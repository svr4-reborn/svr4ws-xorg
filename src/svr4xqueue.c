/*
 * xf86-input-svr4xqueue: X.org input driver for the historical SVR4
 * workstation "Xqueue" interface.
 *
 * The SVR4 kernel exposes one shared, memory-mapped event ring that carries
 * BOTH keyboard and mouse events. The ring is enabled with the KDQUEMODE ioctl
 * on the workstation device (the same /dev/kd/kdvm00 the video driver opens).
 * Keyboard scan codes arrive as XQ_KEY events once the console is put in
 * K_RAW translation mode (KDSKBMODE); mouse motion/button changes arrive as
 * XQ_MOTION / XQ_BUTTON events (these require the in-kernel PS/2 mouse driver
 * to be configured -- if it is absent, keyboard still works and the pointer
 * device simply sees no events).
 *
 * Because there is a single shared ring per server process, this driver uses a
 * process-global singleton for the device/queue and is instantiated twice by
 * the X server: once as the keyboard device and once as the pointer device.
 * The first instance to initialise opens the device and enables the queue; the
 * read_input handler drains the ring and dispatches each event to the
 * appropriate X device based on xq_type.
 */

#include "xorg-server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stropts.h>
#include <bits/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xproto.h>
#include <X11/keysym.h>
#include <X11/extensions/XI.h>

#include "xf86.h"
#include "xf86Xinput.h"
#include "xf86_OSproc.h"
#include "xf86Module.h"
#include "exevents.h"
#include "input.h"
#include "inputstr.h"
#include "mipointer.h"
#include "windowstr.h"
#include "xserver-properties.h"

/* ------------------------------------------------------------------------- */
/* Kernel interface definitions (mirrors uts/i386/sys/kd.h and xque.h).      */
/* ------------------------------------------------------------------------- */

#define KIOC ('K' << 8)
#define KDGKBMODE (KIOC | 6)  /* get keyboard translation mode */
#define KDSKBMODE (KIOC | 7)  /* set keyboard translation mode */
#define KDQUEMODE (KIOC | 15) /* enable/disable shared event queue */

#define K_RAW 0x00   /* raw up/down scan codes */
#define K_XLATE 0x01 /* translate scan codes to ASCII */

#define KBD_BREAK 0x80 /* scan-code make/break bit (break == 1) */

/* struct kd_quemode -- argument to KDQUEMODE (see uts/i386/sys/kd.h). */
struct kd_quemode {
    int qsize;   /* desired number of queue elements (set by caller) */
    int signo;   /* signal raised when an empty queue goes non-empty */
    char *qaddr; /* mapped user address of the queue (set by kernel) */
};

/* xqEvent / xqEventQueue -- the mapped ring (see uts/i386/sys/xque.h). */
typedef unsigned char unchar;

typedef struct xqEvent {
    unchar xq_type; /* XQ_BUTTON / XQ_MOTION / XQ_KEY */
    unchar xq_code; /* scan code, or active-low mouse button bitmask */
    char xq_x;      /* relative mouse delta x (motion only) */
    char xq_y;      /* relative mouse delta y (motion only) */
    long xq_time;   /* event timestamp in milliseconds */
} xqEvent;

typedef struct xqEventQueue {
    char xq_sigenable; /* non-zero => signal when queue goes non-empty */
    int xq_head;       /* index of next event to dequeue (owned by us) */
    int xq_tail;       /* index of next slot to fill (owned by kernel) */
    long xq_curtime;   /* current time in ms, refreshed by the kernel */
    int xq_size;       /* number of elements in xq_events */
    xqEvent xq_events[1];
} xqEventQueue;

#define XQ_BUTTON 0 /* button state change only */
#define XQ_MOTION 1 /* mouse movement (and maybe button change) */
#define XQ_KEY 2    /* key pressed or released */

/*
 * Mouse button encoding in xq_code is ACTIVE LOW (see xque.h):
 *   bit 0 clear => right button down
 *   bit 1 clear => middle button down
 *   bit 2 clear => left button down
 */
#define XQ_BUT_RIGHT 0x01
#define XQ_BUT_MIDDLE 0x02
#define XQ_BUT_LEFT 0x04

#define SVR4XQ_QUEUE_ELEMENTS 256

/* X core keyboard codes are scan code + this offset (the classic evdev/X +8). */
#define SVR4XQ_KEYCODE_OFFSET 8

static const char *const svr4xq_default_device_paths[] = {
    "/dev/kd/kdvm00", "/dev/vt00", "/dev/syscon", "/dev/console"
};

/*
 * The PS/2 mouse driver (m320) produces CH_MSE STREAMS messages on its own
 * stream. For those to reach the console's char module (which converts them to
 * Xqueue XQ_MOTION/XQ_BUTTON events), the mouse stream must be I_PLINK'd
 * underneath the VT/console stream -- exactly what the historical mousemgr
 * daemon did. We do that here so the driver is self-contained.
 */
static const char *const svr4xq_default_mouse_paths[] = { "/dev/mouse" };
static const char *const svr4xq_default_vt_paths[] = {
    "/dev/vt00", "/dev/syscon", "/dev/console"
};

/* ------------------------------------------------------------------------- */
/* Process-global shared state: one device, one queue, two X devices.        */
/* ------------------------------------------------------------------------- */

typedef struct {
    int fd;                       /* fd of the workstation device */
    int refcount;                 /* number of inited devices using the queue */
    int queue_enabled;            /* KDQUEMODE succeeded */
    int saved_kbmode;             /* keyboard mode to restore (-1 if unknown) */
    int kbmode_saved;             /* whether saved_kbmode is valid */
    volatile xqEventQueue *queue; /* the mapped ring */
    size_t queue_map_size;        /* mapped size, for munmap */

    int devices_on;        /* number of devices currently DEVICE_ON */

    int vt_fd;             /* VT/console stream the mouse is linked under (-1) */
    int mouse_fd;          /* /dev/mouse fd held open for the persistent link */
    int mouse_linkid;      /* I_PLINK link id, or -1 if not linked */

    InputInfoPtr keyboard; /* keyboard device, if registered */
    InputInfoPtr pointer;  /* pointer device, if registered */

    unsigned char prev_buttons; /* previous mouse button bitmask (active low) */
    int pointer_x;              /* absolute screen position synthesized from deltas */
    int pointer_y;
    int pointer_pos_valid;
    int prefix_e0;              /* pending 0xE0 extended scan-code prefix */
} SVR4XQShared;

static SVR4XQShared svr4xq_shared = {
    .fd = -1,
    .saved_kbmode = -1,
    .vt_fd = -1,
    .mouse_fd = -1,
    .mouse_linkid = -1,
    .prev_buttons = 0xff,
};

/* Per-device private: just the device class so read_input can route. */
typedef enum { SVR4XQ_DEV_KEYBOARD, SVR4XQ_DEV_POINTER } SVR4XQDevClass;

typedef struct {
    SVR4XQDevClass dev_class;
} SVR4XQPrivate;

static void svr4xq_dbg(const char *msg);
static void svr4xq_dbgf(const char *fmt, ...);

/* ------------------------------------------------------------------------- */
/* Device open / queue enable / teardown (shared singleton).                 */
/* ------------------------------------------------------------------------- */

static int
svr4xq_open_device(InputInfoPtr pInfo)
{
    const char *configured;
    size_t index;
    int fd;

    if (svr4xq_shared.fd >= 0)
        return svr4xq_shared.fd;

    configured = xf86SetStrOption(pInfo->options, "Device", NULL);
    svr4xq_dbgf("svr4xq: open_device configured=%s\n",
                configured ? configured : "(none)");
    if (configured) {
        fd = open(configured, O_RDWR);
        if (fd >= 0) {
            svr4xq_dbgf("svr4xq: opened configured device %s fd=%d\n",
                        configured, fd);
            svr4xq_shared.fd = fd;
            return fd;
        }
        xf86IDrvMsg(pInfo, X_WARNING, "cannot open configured device %s: %s\n",
                    configured, strerror(errno));
    }

    for (index = 0;
         index < sizeof(svr4xq_default_device_paths) / sizeof(svr4xq_default_device_paths[0]);
         ++index) {
        fd = open(svr4xq_default_device_paths[index], O_RDWR);
        if (fd >= 0) {
            svr4xq_dbgf("svr4xq: opened fallback device %s fd=%d\n",
                        svr4xq_default_device_paths[index], fd);
            svr4xq_shared.fd = fd;
            return fd;
        }
    }

    xf86IDrvMsg(pInfo, X_ERROR, "unable to open any workstation device\n");
    return -1;
}

static int
svr4xq_open_first(const char *configured, const char *const *fallbacks,
                  size_t fallback_count)
{
    size_t index;
    int fd;

    if (configured) {
        svr4xq_dbgf("svr4xq: open_first trying configured=%s\n", configured);
        fd = open(configured, O_RDWR);
        if (fd >= 0)
            return fd;
    }
    for (index = 0; index < fallback_count; ++index) {
        svr4xq_dbgf("svr4xq: open_first trying fallback=%s\n", fallbacks[index]);
        fd = open(fallbacks[index], O_RDWR);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

/*
 * Open /dev/mouse and I_PLINK it underneath the VT/console stream, so the PS/2
 * mouse driver's CH_MSE messages reach the console's char module and get
 * converted into Xqueue pointer events. Best-effort: if any step fails (e.g. the
 * kernel mouse driver isn't present), we log and continue -- the keyboard half
 * of the queue still works.
 */
static void
svr4xq_link_mouse(InputInfoPtr pInfo)
{
    const char *mouse_opt;
    const char *vt_opt;

    if (svr4xq_shared.mouse_linkid >= 0)
        return; /* already linked */

    mouse_opt = xf86SetStrOption(pInfo->options, "MouseDevice", NULL);
    svr4xq_dbgf("svr4xq: link_mouse mouse_opt=%s shared_fd=%d\n",
                mouse_opt ? mouse_opt : "(none)", svr4xq_shared.fd);
    svr4xq_shared.mouse_fd = svr4xq_open_first(mouse_opt, svr4xq_default_mouse_paths,
        sizeof(svr4xq_default_mouse_paths) / sizeof(svr4xq_default_mouse_paths[0]));
    if (svr4xq_shared.mouse_fd < 0) {
        xf86IDrvMsg(pInfo, X_WARNING,
                    "cannot open mouse device; pointer disabled: %s\n",
                    strerror(errno));
        return;
    }

    vt_opt = xf86SetStrOption(pInfo->options, "VT", NULL);
    svr4xq_shared.vt_fd = svr4xq_open_first(vt_opt, svr4xq_default_vt_paths,
        sizeof(svr4xq_default_vt_paths) / sizeof(svr4xq_default_vt_paths[0]));
    if (svr4xq_shared.vt_fd >= 0) {
        svr4xq_dbgf("svr4xq: trying I_PLINK mouse_fd=%d under VT fd=%d\n",
                    svr4xq_shared.mouse_fd, svr4xq_shared.vt_fd);
        svr4xq_shared.mouse_linkid = ioctl(svr4xq_shared.vt_fd, I_PLINK,
                                           svr4xq_shared.mouse_fd);
        if (svr4xq_shared.mouse_linkid >= 0) {
            xf86IDrvMsg(pInfo, X_INFO, "mouse stream linked under VT (linkid %d)\n",
                        svr4xq_shared.mouse_linkid);
            svr4xq_dbgf("svr4xq: VT I_PLINK success linkid=%d\n",
                        svr4xq_shared.mouse_linkid);
            return;
        }
        svr4xq_dbgf("svr4xq: I_PLINK under VT failed errno=%d\n", errno);
        close(svr4xq_shared.vt_fd);
        svr4xq_shared.vt_fd = -1;
    } else {
        xf86IDrvMsg(pInfo, X_WARNING,
                    "no VT stream to link mouse under, trying Xqueue device: %s\n",
                    strerror(errno));
    }

    if (svr4xq_shared.fd >= 0) {
        svr4xq_shared.vt_fd = dup(svr4xq_shared.fd);
        if (svr4xq_shared.vt_fd >= 0) {
            svr4xq_dbgf("svr4xq: trying fallback I_PLINK mouse_fd=%d under dup fd=%d\n",
                        svr4xq_shared.mouse_fd, svr4xq_shared.vt_fd);
            svr4xq_shared.mouse_linkid = ioctl(svr4xq_shared.vt_fd, I_PLINK,
                                               svr4xq_shared.mouse_fd);
            if (svr4xq_shared.mouse_linkid >= 0) {
                xf86IDrvMsg(pInfo, X_INFO,
                            "mouse stream linked under Xqueue device (linkid %d)\n",
                            svr4xq_shared.mouse_linkid);
                svr4xq_dbgf("svr4xq: fallback Xqueue I_PLINK success linkid=%d\n",
                            svr4xq_shared.mouse_linkid);
                return;
            }
            svr4xq_dbgf("svr4xq: fallback I_PLINK under shared fd failed errno=%d\n",
                        errno);
            close(svr4xq_shared.vt_fd);
            svr4xq_shared.vt_fd = -1;
        }
    }

    if (svr4xq_shared.mouse_linkid < 0) {
        xf86IDrvMsg(pInfo, X_WARNING,
                    "I_PLINK of mouse failed; pointer disabled: %s\n",
                    strerror(errno));
        close(svr4xq_shared.mouse_fd);
        svr4xq_shared.mouse_fd = -1;
        return;
    }
}

static void
svr4xq_unlink_mouse(void)
{
    if (svr4xq_shared.mouse_linkid >= 0 && svr4xq_shared.vt_fd >= 0)
        (void)ioctl(svr4xq_shared.vt_fd, I_PUNLINK, svr4xq_shared.mouse_linkid);
    svr4xq_dbgf("svr4xq: unlink_mouse linkid=%d vt_fd=%d mouse_fd=%d\n",
                svr4xq_shared.mouse_linkid, svr4xq_shared.vt_fd,
                svr4xq_shared.mouse_fd);
    svr4xq_shared.mouse_linkid = -1;
    if (svr4xq_shared.mouse_fd >= 0)
        close(svr4xq_shared.mouse_fd);
    svr4xq_shared.mouse_fd = -1;
    if (svr4xq_shared.vt_fd >= 0)
        close(svr4xq_shared.vt_fd);
    svr4xq_shared.vt_fd = -1;
}

static Bool
svr4xq_enable_queue(InputInfoPtr pInfo)
{
    struct kd_quemode qmode;
    long page_size;
    size_t map_size;
    int mode;

    if (svr4xq_shared.queue_enabled)
        return TRUE;
    if (svr4xq_shared.fd < 0)
        return FALSE;
    svr4xq_dbgf("svr4xq: enable_queue fd=%d\n", svr4xq_shared.fd);

    /* Put the keyboard into raw scan-code mode, saving the old mode first. */
    if (ioctl(svr4xq_shared.fd, KDGKBMODE, &mode) >= 0) {
        svr4xq_shared.saved_kbmode = mode;
        svr4xq_shared.kbmode_saved = 1;
        svr4xq_dbgf("svr4xq: saved keyboard mode=%d\n", mode);
    }
    if (ioctl(svr4xq_shared.fd, KDSKBMODE, K_RAW) < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "KDSKBMODE K_RAW failed: %s\n", strerror(errno));
        return FALSE;
    }

    memset(&qmode, 0, sizeof(qmode));
    qmode.qsize = SVR4XQ_QUEUE_ELEMENTS;
    qmode.signo = 0; /* poll-driven; we drain from read_input each cycle */
    if (ioctl(svr4xq_shared.fd, KDQUEMODE, &qmode) < 0) {
        xf86IDrvMsg(pInfo, X_ERROR, "KDQUEMODE enable failed: %s\n", strerror(errno));
        if (svr4xq_shared.kbmode_saved)
            (void)ioctl(svr4xq_shared.fd, KDSKBMODE, svr4xq_shared.saved_kbmode);
        return FALSE;
    }

    svr4xq_shared.queue = (volatile xqEventQueue *)qmode.qaddr;
    svr4xq_dbgf("svr4xq: KDQUEMODE returned qaddr=%p\n", qmode.qaddr);
    if (!svr4xq_shared.queue) {
        xf86IDrvMsg(pInfo, X_ERROR, "KDQUEMODE returned a NULL queue address\n");
        if (svr4xq_shared.kbmode_saved)
            (void)ioctl(svr4xq_shared.fd, KDSKBMODE, svr4xq_shared.saved_kbmode);
        return FALSE;
    }

    /*
     * The kernel mapped the ring for us; record its page-rounded size so we can
     * munmap on teardown. The element count chosen by the kernel lives in
     * xq_size and may exceed our request (it rounds up to a page).
     */
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;
    map_size = sizeof(xqEventQueue) +
               (size_t)(svr4xq_shared.queue->xq_size - 1) * sizeof(xqEvent);
    map_size = (map_size + (size_t)page_size - 1U) & ~((size_t)page_size - 1U);
    svr4xq_shared.queue_map_size = map_size;

    svr4xq_shared.queue_enabled = 1;
    svr4xq_shared.prev_buttons = 0xff; /* all buttons up (active low) */
    svr4xq_shared.prefix_e0 = 0;
    xf86IDrvMsg(pInfo, X_INFO, "Xqueue enabled, %d elements\n",
                svr4xq_shared.queue->xq_size);
    svr4xq_dbgf("svr4xq: queue enabled size=%d head=%d tail=%d map=%lu\n",
                svr4xq_shared.queue->xq_size, svr4xq_shared.queue->xq_head,
                svr4xq_shared.queue->xq_tail,
                (unsigned long)svr4xq_shared.queue_map_size);

    return TRUE;
}

static void
svr4xq_disable_queue(void)
{
    if (!svr4xq_shared.queue_enabled)
        return;
    svr4xq_dbg("svr4xq: disable_queue\n");

    svr4xq_unlink_mouse();

    if (svr4xq_shared.queue)
        svr4xq_shared.queue->xq_sigenable = 0;
    if (svr4xq_shared.fd >= 0) {
        (void)ioctl(svr4xq_shared.fd, KDQUEMODE, 0);
        if (svr4xq_shared.kbmode_saved)
            (void)ioctl(svr4xq_shared.fd, KDSKBMODE, svr4xq_shared.saved_kbmode);
    }
    if (svr4xq_shared.queue && svr4xq_shared.queue_map_size)
        (void)munmap((void *)svr4xq_shared.queue, svr4xq_shared.queue_map_size);

    svr4xq_shared.queue = NULL;
    svr4xq_shared.queue_map_size = 0;
    svr4xq_shared.queue_enabled = 0;
}

static void
svr4xq_close_device(void)
{
    if (svr4xq_shared.fd >= 0)
        close(svr4xq_shared.fd);
    svr4xq_shared.fd = -1;
    svr4xq_shared.kbmode_saved = 0;
    svr4xq_shared.saved_kbmode = -1;
}

/* ------------------------------------------------------------------------- */
/* Event dispatch.                                                           */
/* ------------------------------------------------------------------------- */

/*
 * Debug logging to the QEMU debug console (same SYS_CLOCAL path the video
 * driver uses). Enabled when the environment or a build flag requests it, so
 * we can see exactly what the kernel queue delivers without flooding the X log.
 */
#ifndef SVR4XQ_SYS_CLOCAL
#define SVR4XQ_SYS_CLOCAL 127
#endif
#ifndef SVR4XQ_CLOCAL_DEBUGCON_WRITE
#define SVR4XQ_CLOCAL_DEBUGCON_WRITE 1
#endif
#ifndef SVR4XQ_DEBUG
#define SVR4XQ_DEBUG 0
#endif

static int svr4xq_debug = SVR4XQ_DEBUG;

static void
svr4xq_dbg(const char *msg)
{
    size_t len = strlen(msg);

    if (!svr4xq_debug || !len)
        return;
    (void)syscall(SVR4XQ_SYS_CLOCAL, SVR4XQ_CLOCAL_DEBUGCON_WRITE, msg, len, 0, 0);
}

static void
svr4xq_dbgf(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    if (!svr4xq_debug)
        return;

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svr4xq_dbg(buf);
}

static void
svr4xq_dispatch_key(unsigned char code)
{
    InputInfoPtr kbd = svr4xq_shared.keyboard;
    int is_down;
    unsigned int keycode;

    /*
     * 0xE0 is an extended-key prefix (right ctrl/alt, arrows, etc.). It arrives
     * as a standalone 0xE0 byte (the high bit is part of the prefix value, not
     * the make/break bit). The set-1 scan codes after the prefix overlap with
     * the base set, so the server-side XKB map needs distinct keycodes. We fold
     * the prefix into the high range by adding 0x60 to the base scan code,
     * matching the conventional console "e0 table" offset; XKB can then map them
     * via an appropriate keymap. The rarer 0xE1 (Pause) prefix is ignored.
     */
    if (code == 0xE0) {
        svr4xq_shared.prefix_e0 = 1;
        return;
    }
    if (code == 0xE1)
        return;

    if (!kbd || !kbd->dev)
        return;

    is_down = (code & KBD_BREAK) ? 0 : 1;
    keycode = (unsigned int)(code & ~KBD_BREAK);
    if (svr4xq_shared.prefix_e0) {
        keycode += 0x60;
        svr4xq_shared.prefix_e0 = 0;
    }
    keycode += SVR4XQ_KEYCODE_OFFSET;

    xf86PostKeyboardEvent(kbd->dev, keycode, is_down);
}

static void
svr4xq_post_motion(DeviceIntPtr dev, int dx, int dy)
{
    DeviceIntPtr master;
    ScreenPtr screen;
    SpritePtr sprite;
    WindowPtr root;
    BoxPtr root_extents;
    int x;
    int y;

    master = GetMaster(dev, MASTER_POINTER);
    screen = miPointerGetScreen(dev);
    sprite = dev->spriteInfo ? dev->spriteInfo->sprite : NULL;
    root = screen ? screen->root : NULL;
    root_extents = root ? RegionExtents(&root->borderSize) : NULL;
    miPointerGetPosition(dev, &x, &y);

    if (!svr4xq_shared.pointer_pos_valid) {
        svr4xq_shared.pointer_x = x;
        svr4xq_shared.pointer_y = y;
        svr4xq_shared.pointer_pos_valid = 1;
    }
    svr4xq_shared.pointer_x += dx;
    svr4xq_shared.pointer_y += dy;
    if (screen) {
        if (svr4xq_shared.pointer_x < 0)
            svr4xq_shared.pointer_x = 0;
        if (svr4xq_shared.pointer_x >= screen->width)
            svr4xq_shared.pointer_x = screen->width - 1;
        if (svr4xq_shared.pointer_y < 0)
            svr4xq_shared.pointer_y = 0;
        if (svr4xq_shared.pointer_y >= screen->height)
            svr4xq_shared.pointer_y = screen->height - 1;
    }

    svr4xq_dbgf("svr4xq: before rel motion dx=%d dy=%d expected=%d,%d sprite=%d,%d slave_last=%d,%d "
                "master=%p master_last=%d,%d screen=%d %dx%d screenInfo=%d,%d %dx%d "
                "axis0=%d..%d mode=%d axis1=%d..%d mode=%d enabled=%d floating=%d\n",
                dx, dy, svr4xq_shared.pointer_x, svr4xq_shared.pointer_y, x, y,
                (int)dev->last.valuators[0], (int)dev->last.valuators[1],
                master,
                master ? (int)master->last.valuators[0] : -1,
                master ? (int)master->last.valuators[1] : -1,
                screen ? screen->myNum : -1,
                screen ? screen->width : -1, screen ? screen->height : -1,
                screenInfo.x, screenInfo.y, screenInfo.width, screenInfo.height,
                dev->valuator->axes[0].min_value, dev->valuator->axes[0].max_value,
                dev->valuator->axes[0].mode,
                dev->valuator->axes[1].min_value, dev->valuator->axes[1].max_value,
                dev->valuator->axes[1].mode, dev->enabled, IsFloating(dev));
    svr4xq_dbgf("svr4xq: sprite constraints hot=%d,%d hotPhys=%d,%d hotLimits=%d,%d-%d,%d "
                "physLimits=%d,%d-%d,%d root=%d,%d-%d,%d sprite=%p rootwin=%p\n",
                sprite ? sprite->hot.x : -1, sprite ? sprite->hot.y : -1,
                sprite ? sprite->hotPhys.x : -1, sprite ? sprite->hotPhys.y : -1,
                sprite ? sprite->hotLimits.x1 : -1,
                sprite ? sprite->hotLimits.y1 : -1,
                sprite ? sprite->hotLimits.x2 : -1,
                sprite ? sprite->hotLimits.y2 : -1,
                sprite ? sprite->physLimits.x1 : -1,
                sprite ? sprite->physLimits.y1 : -1,
                sprite ? sprite->physLimits.x2 : -1,
                sprite ? sprite->physLimits.y2 : -1,
                root_extents ? root_extents->x1 : -1,
                root_extents ? root_extents->y1 : -1,
                root_extents ? root_extents->x2 : -1,
                root_extents ? root_extents->y2 : -1,
                sprite, root);
    xf86PostMotionEvent(dev, 0, 0, 2, dx, dy);
    miPointerGetPosition(dev, &x, &y);
    svr4xq_dbgf("svr4xq: after rel motion sprite x=%d y=%d slave_last=%d,%d "
                "master=%p master_last=%d,%d\n",
                x, y, (int)dev->last.valuators[0], (int)dev->last.valuators[1],
                master,
                master ? (int)master->last.valuators[0] : -1,
                master ? (int)master->last.valuators[1] : -1);
}

static void
svr4xq_sync_pointer_position(DeviceIntPtr dev)
{
    DeviceIntPtr master;
    ScreenPtr screen;
    int x;
    int y;

    if (!dev || !dev->valuator)
        return;

    screen = miPointerGetScreen(dev);
    miPointerGetPosition(dev, &x, &y);
    if (screen) {
        if (x < 0 || x >= screen->width || y < 0 || y >= screen->height) {
            x = screen->width / 2;
            y = screen->height / 2;
        }
        miPointerSetScreen(dev, screen->myNum, x, y);
    }
    dev->valuator->axisVal[0] = x;
    dev->valuator->axisVal[1] = y;
    dev->last.valuators[0] = x;
    dev->last.valuators[1] = y;
    svr4xq_shared.pointer_x = x;
    svr4xq_shared.pointer_y = y;
    svr4xq_shared.pointer_pos_valid = 1;

    master = GetMaster(dev, MASTER_POINTER);
    if (master) {
        master->last.valuators[0] = x;
        master->last.valuators[1] = y;
    }

    svr4xq_dbgf("svr4xq: synced pointer position x=%d y=%d screen=%d %dx%d master=%p\n",
                x, y, screen ? screen->myNum : -1,
                screen ? screen->width : -1, screen ? screen->height : -1,
                master);
}

static void
svr4xq_dispatch_mouse(const xqEvent *ev)
{
    InputInfoPtr ptr = svr4xq_shared.pointer;
    unsigned char buttons = ev->xq_code & 0x07;
    unsigned char changed;

    if (!ptr || !ptr->dev) {
        svr4xq_dbg("svr4xq: mouse event dropped (no pointer device)\n");
        return;
    }

    if (ev->xq_type == XQ_MOTION && (ev->xq_x || ev->xq_y)) {
        svr4xq_dbgf("svr4xq: dispatch motion code=0x%02x dx=%d dy=%d\n",
                    buttons, (int)ev->xq_x, (int)ev->xq_y);
        svr4xq_post_motion(ptr->dev, (int)ev->xq_x, (int)ev->xq_y);
    }

    /* Buttons are active low; a transition from prev_buttons drives an event. */
    changed = (unsigned char)(buttons ^ (svr4xq_shared.prev_buttons & 0x07));
    if (changed)
        svr4xq_dbgf("svr4xq: dispatch buttons old=0x%02x new=0x%02x changed=0x%02x\n",
                    svr4xq_shared.prev_buttons & 0x07, buttons, changed);
    if (changed & XQ_BUT_LEFT) {
        svr4xq_dbgf("svr4xq: post button1 down=%d\n", !(buttons & XQ_BUT_LEFT));
        xf86PostButtonEvent(ptr->dev, 0, 1, !(buttons & XQ_BUT_LEFT), 0, 0);
    }
    if (changed & XQ_BUT_MIDDLE) {
        svr4xq_dbgf("svr4xq: post button2 down=%d\n", !(buttons & XQ_BUT_MIDDLE));
        xf86PostButtonEvent(ptr->dev, 0, 2, !(buttons & XQ_BUT_MIDDLE), 0, 0);
    }
    if (changed & XQ_BUT_RIGHT) {
        svr4xq_dbgf("svr4xq: post button3 down=%d\n", !(buttons & XQ_BUT_RIGHT));
        xf86PostButtonEvent(ptr->dev, 0, 3, !(buttons & XQ_BUT_RIGHT), 0, 0);
    }

    svr4xq_shared.prev_buttons = (unsigned char)((svr4xq_shared.prev_buttons & ~0x07) | buttons);
}

static void
svr4xq_drain(void)
{
    volatile xqEventQueue *q = svr4xq_shared.queue;
    int size;

    if (!q)
        return;
    size = q->xq_size;
    if (size <= 0)
        return;

    while (q->xq_head != q->xq_tail) {
        xqEvent ev = ((xqEvent *)q->xq_events)[q->xq_head];
        q->xq_head = (q->xq_head + 1) % size;

        if (svr4xq_debug && ev.xq_type != XQ_KEY) {
            svr4xq_dbgf(
                "svr4xq: ev type=%d code=0x%02x x=%d y=%d (head=%d tail=%d)\n",
                ev.xq_type, ev.xq_code, (int)ev.xq_x, (int)ev.xq_y,
                q->xq_head, q->xq_tail);
        }

        switch (ev.xq_type) {
        case XQ_KEY:
            svr4xq_dispatch_key(ev.xq_code);
            break;
        case XQ_MOTION:
        case XQ_BUTTON:
            svr4xq_dispatch_mouse(&ev);
            break;
        default:
            break;
        }
    }
}

/*
 * The Xqueue is drained from shared memory, not by reading the device fd. The
 * workstation device is a STREAMS special file that the server's input thread
 * cannot epoll() (it returns ENODEV), so we must NOT register the fd with
 * xf86AddEnabledDevice. Instead we drain from a server block/wakeup handler that
 * runs on the main thread every iteration of the dispatch loop, and we cap the
 * server's poll timeout so it wakes frequently enough to feel responsive.
 */
#define SVR4XQ_POLL_INTERVAL_MS 16 /* ~60 Hz drain cadence */

static int svr4xq_handlers_registered;

static void
svr4xq_block_handler(void *blockData, void *timeout)
{
    svr4xq_drain();
    /* Ensure the server wakes again soon to drain the next batch of events. */
    AdjustWaitForDelay(timeout, SVR4XQ_POLL_INTERVAL_MS);
}

static void
svr4xq_wakeup_handler(void *blockData, int result)
{
    svr4xq_drain();
}

static void
svr4xq_register_handlers(void)
{
    if (svr4xq_handlers_registered)
        return;
    if (RegisterBlockAndWakeupHandlers(svr4xq_block_handler,
                                       svr4xq_wakeup_handler, NULL))
        svr4xq_handlers_registered = 1;
}

static void
svr4xq_unregister_handlers(void)
{
    if (!svr4xq_handlers_registered)
        return;
    RemoveBlockAndWakeupHandlers(svr4xq_block_handler,
                                 svr4xq_wakeup_handler, NULL);
    svr4xq_handlers_registered = 0;
}

/* Kept for the InputInfoRec slot; never invoked since no fd is registered. */
static void
svr4xq_read_input(InputInfoPtr pInfo)
{
    svr4xq_drain();
}

/* ------------------------------------------------------------------------- */
/* Device control (init/on/off/close) for each X device.                     */
/* ------------------------------------------------------------------------- */

static int
svr4xq_keyboard_control(DeviceIntPtr device, int what)
{
    InputInfoPtr pInfo = device->public.devicePrivate;
    svr4xq_dbgf("svr4xq: keyboard_control what=%d on=%d devices_on=%d\n",
                what, device->public.on, svr4xq_shared.devices_on);

    switch (what) {
    case DEVICE_INIT: {
        XkbRMLVOSet rmlvo;

        memset(&rmlvo, 0, sizeof(rmlvo));
        rmlvo.rules = xf86SetStrOption(pInfo->options, "xkb_rules", "evdev");
        rmlvo.model = xf86SetStrOption(pInfo->options, "xkb_model", "pc105");
        rmlvo.layout = xf86SetStrOption(pInfo->options, "xkb_layout", "us");
        rmlvo.variant = xf86SetStrOption(pInfo->options, "xkb_variant", NULL);
        rmlvo.options = xf86SetStrOption(pInfo->options, "xkb_options", NULL);

        if (!InitKeyboardDeviceStruct(device, &rmlvo, NULL, NULL)) {
            xf86IDrvMsg(pInfo, X_ERROR, "InitKeyboardDeviceStruct failed\n");
            return BadValue;
        }
        return Success;
    }
    case DEVICE_ON:
        if (!svr4xq_enable_queue(pInfo))
            return BadValue;
        svr4xq_register_handlers();
        svr4xq_shared.devices_on++;
        device->public.on = TRUE;
        return Success;
    case DEVICE_OFF:
    case DEVICE_CLOSE:
        if (device->public.on) {
            if (svr4xq_shared.devices_on > 0)
                svr4xq_shared.devices_on--;
            if (svr4xq_shared.devices_on == 0) {
                svr4xq_unregister_handlers();
                svr4xq_disable_queue();
            }
        }
        device->public.on = FALSE;
        return Success;
    default:
        return BadValue;
    }
}

static int
svr4xq_pointer_control(DeviceIntPtr device, int what)
{
    InputInfoPtr pInfo = device->public.devicePrivate;
    /* 3-button mouse map: identity (button N -> N). */
    static CARD8 map[] = { 0, 1, 2, 3 };
    Atom btn_labels[3];
    Atom axis_labels[2];
    svr4xq_dbgf("svr4xq: pointer_control what=%d on=%d devices_on=%d\n",
                what, device->public.on, svr4xq_shared.devices_on);

    switch (what) {
    case DEVICE_INIT:
        btn_labels[0] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_LEFT);
        btn_labels[1] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_MIDDLE);
        btn_labels[2] = XIGetKnownProperty(BTN_LABEL_PROP_BTN_RIGHT);
        axis_labels[0] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_X);
        axis_labels[1] = XIGetKnownProperty(AXIS_LABEL_PROP_REL_Y);

        if (!InitPointerDeviceStruct((DevicePtr)device, map, 3, btn_labels,
                                     (PtrCtrlProcPtr)NoopDDA,
                                     GetMotionHistorySize(), 2, axis_labels)) {
            xf86IDrvMsg(pInfo, X_ERROR, "InitPointerDeviceStruct failed\n");
            return BadValue;
        }
        xf86InitValuatorDefaults(device, 0);
        xf86InitValuatorDefaults(device, 1);
        return Success;
    case DEVICE_ON:
        if (!svr4xq_enable_queue(pInfo))
            return BadValue;
        /*
         * The queue singleton is often enabled by the keyboard device first.
         * Do not open and enable the PS/2 mouse until the X pointer device is
         * actually on; otherwise timing during server startup can leave the
         * mouse streaming into a half-initialized input stack.
         */
        svr4xq_link_mouse(pInfo);
        svr4xq_register_handlers();
        svr4xq_sync_pointer_position(device);
        svr4xq_shared.devices_on++;
        device->public.on = TRUE;
        return Success;
    case DEVICE_OFF:
    case DEVICE_CLOSE:
        if (device->public.on) {
            if (svr4xq_shared.devices_on > 0)
                svr4xq_shared.devices_on--;
            if (svr4xq_shared.devices_on == 0) {
                svr4xq_unregister_handlers();
                svr4xq_disable_queue();
            }
        }
        device->public.on = FALSE;
        return Success;
    default:
        return BadValue;
    }
}

/* ------------------------------------------------------------------------- */
/* Driver entry points.                                                      */
/* ------------------------------------------------------------------------- */

static int
SVR4XQPreInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    SVR4XQPrivate *priv;
    const char *type;
    const char *debug_env;

    priv = calloc(1, sizeof(SVR4XQPrivate));
    if (!priv)
        return BadAlloc;

    debug_env = getenv("SVR4XQ_DEBUG");
    if (debug_env)
        svr4xq_debug = atoi(debug_env) != 0;
    svr4xq_debug = xf86SetBoolOption(pInfo->options, "Debug", svr4xq_debug);
    svr4xq_dbgf("svr4xq: PreInit name=%s debug=%d\n",
                pInfo->name ? pInfo->name : "(unnamed)", svr4xq_debug);

    /*
     * Decide whether this instance is the keyboard or the pointer. Honour an
     * explicit "DeviceClass"/"Type" option; otherwise infer from the device's
     * type_name as set by the server's input matching.
     */
    type = xf86SetStrOption(pInfo->options, "DeviceClass", NULL);
    if (!type)
        type = pInfo->type_name;

    if (type && (strcasecmp(type, "pointer") == 0 ||
                 strcasecmp(type, XI_MOUSE) == 0)) {
        priv->dev_class = SVR4XQ_DEV_POINTER;
        pInfo->type_name = XI_MOUSE;
        pInfo->device_control = svr4xq_pointer_control;
    } else {
        priv->dev_class = SVR4XQ_DEV_KEYBOARD;
        pInfo->type_name = XI_KEYBOARD;
        pInfo->device_control = svr4xq_keyboard_control;
    }
    svr4xq_dbgf("svr4xq: PreInit class=%s type=%s\n",
                priv->dev_class == SVR4XQ_DEV_POINTER ? "pointer" : "keyboard",
                type ? type : "(none)");

    pInfo->private = priv;
    pInfo->read_input = svr4xq_read_input;
    pInfo->fd = -1;

    if (svr4xq_open_device(pInfo) < 0) {
        free(priv);
        pInfo->private = NULL;
        return BadValue;
    }

    if (priv->dev_class == SVR4XQ_DEV_POINTER)
        svr4xq_shared.pointer = pInfo;
    else
        svr4xq_shared.keyboard = pInfo;
    svr4xq_shared.refcount++;
    svr4xq_dbgf("svr4xq: PreInit complete refcount=%d keyboard=%p pointer=%p\n",
                svr4xq_shared.refcount, svr4xq_shared.keyboard,
                svr4xq_shared.pointer);

    return Success;
}

static void
SVR4XQUnInit(InputDriverPtr drv, InputInfoPtr pInfo, int flags)
{
    SVR4XQPrivate *priv = pInfo->private;

    if (svr4xq_shared.keyboard == pInfo)
        svr4xq_shared.keyboard = NULL;
    if (svr4xq_shared.pointer == pInfo)
        svr4xq_shared.pointer = NULL;

    if (svr4xq_shared.refcount > 0)
        svr4xq_shared.refcount--;
    svr4xq_dbgf("svr4xq: UnInit refcount=%d\n", svr4xq_shared.refcount);

    if (svr4xq_shared.refcount == 0) {
        svr4xq_unregister_handlers();
        svr4xq_shared.devices_on = 0;
        svr4xq_disable_queue();
        svr4xq_close_device();
    }

    free(priv);
    pInfo->private = NULL;
    xf86DeleteInput(pInfo, flags);
}

static InputDriverRec SVR4XQ = {
    1,              /* driverVersion */
    "svr4xqueue",   /* driverName */
    NULL,           /* Identify */
    SVR4XQPreInit,  /* PreInit */
    SVR4XQUnInit,   /* UnInit */
    NULL,           /* module */
    NULL,           /* default_options */
    0               /* capabilities */
};

static pointer
svr4xqPlug(pointer module, pointer options, int *errmaj, int *errmin)
{
    xf86AddInputDriver(&SVR4XQ, module, 0);
    return module;
}

static void
svr4xqUnplug(pointer p)
{
}

static XF86ModuleVersionInfo svr4xqVersRec = {
    "svr4xqueue",
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    1, 0, 0,
    ABI_CLASS_XINPUT,
    ABI_XINPUT_VERSION,
    MOD_CLASS_XINPUT,
    { 0, 0, 0, 0 }
};

_X_EXPORT XF86ModuleData svr4xqueueModuleData = {
    &svr4xqVersRec,
    svr4xqPlug,
    svr4xqUnplug
};
