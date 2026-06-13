#include "xorg-server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bits/syscall.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <X11/X.h>
#include <X11/Xproto.h>

#include "fb.h"
#include "micmap.h"
#include "mipointer.h"
#include "scrnintstr.h"
#include "servermd.h"
#include "shadowfb.h"
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86cmap.h"
#include "xf86Module.h"
#include "xf86Opt.h"

#define SCRN_ARG_TYPE ScrnInfoPtr
#define SCRN_INFO_PTR(arg1) ScrnInfoPtr pScrn = (arg1)
#define SCREEN_INIT_ARGS_DECL ScreenPtr pScreen, int argc, char **argv
#define CLOSE_SCREEN_ARGS_DECL ScreenPtr pScreen
#define CLOSE_SCREEN_ARGS pScreen
#define FREE_SCREEN_ARGS_DECL ScrnInfoPtr arg
#define VT_FUNC_ARGS_DECL ScrnInfoPtr arg

#define SVR4WS_VERSION 1000
#define SVR4WS_NAME "SVR4WS"
#define SVR4WS_DRIVER_NAME "svr4ws"
#define SVR4WS_CHIP 0

#define SVR4WS_WIDTH 800
#define SVR4WS_HEIGHT 600
#define SVR4WS_DEPTH 16
#define SVR4WS_BPP 16
#define SVR4WS_BYTES_PER_PIXEL (SVR4WS_BPP / 8)
#define SVR4WS_FRAMEBUFFER_STRIDE (SVR4WS_WIDTH * SVR4WS_BYTES_PER_PIXEL)
#define SVR4WS_FRAMEBUFFER_SIZE (SVR4WS_HEIGHT * SVR4WS_FRAMEBUFFER_STRIDE)
#define SVR4WS_PLANES 4
#define SVR4WS_HW_COLORS 0
#define SVR4WS_COLORMAP_ENTRIES 256

#define SYS_CLOCAL 127
#define CLOCAL_DEBUGCON_WRITE 1

#define KIOC ('K' << 8)
#define KDMAPDISP (KIOC | 2)
#define KDUNMAPDISP (KIOC | 3)
#define KDGETMODE (KIOC | 9)
#define KDSETMODE (KIOC | 10)
#define KDADDIO (KIOC | 11)
#define KDDELIO (KIOC | 12)
#define KDDISPINFO (KIOC | 18)
#define KDENABIO (KIOC | 60)
#define KDDISABIO (KIOC | 61)
#define KD_SET_CUSTOM_MODE (('K' << 8) | 120)
#define KD_GET_CUSTOM_MODE (('K' << 8) | 121)

#define CONSIOC ('c' << 8)
#define CONS_GET (CONSIOC | 2)

#define MODESWITCH ('x' << 8)
#define DM_VDC800x600E 39
#define SW_VDC800x600E (MODESWITCH | DM_VDC800x600E)

#define KD_GRAPHICS 1

#define SVR4WS_BANK_APERTURE (64 * 1024)
#define SVR4WS_BANK_GRANULARITY (16 * 1024)

#define CIRRUS_SR07_BPP_SVGA 0x01
#define CIRRUS_SR07_BPP_16 0x06
#define CIRRUS_SR07_ISAADDR_A0000 0x80
#define CIRRUS_SR0F_MEMSIZE_1M 0x10
#define CIRRUS_SR0F_BANKSWITCH 0x80
#define CIRRUS_HIDDEN_DAC_565 0x01

#ifndef HW_SKIP_CONSOLE
#define HW_SKIP_CONSOLE 4
#endif

struct kd_dispinfo {
    char *vaddr;
    unsigned long physaddr;
    unsigned long size;
};

struct kd_memloc {
    char *vaddr;
    char *physaddr;
    long length;
    long ioflg;
};

struct egainit {
    unsigned char  ei_hortot;
    unsigned char  ei_hde;
    unsigned char  ei_shb;
    unsigned char  ei_ehb;
    unsigned char  ei_shr;
    unsigned char  ei_ehr;
    unsigned char  ei_vertot;
    unsigned char  ei_ovflow;
    unsigned char  ei_prs;
    unsigned char  ei_maxscn;
    unsigned char  ei_curbeg;
    unsigned char  ei_curend;
    unsigned char  ei_stadh;
    unsigned char  ei_stadl;
    unsigned char  ei_cursh;
    unsigned char  ei_cursl;
    unsigned char  ei_vrs;
    unsigned char  ei_vre;
    unsigned char  ei_vde;
    unsigned char  ei_offset;
    unsigned char  ei_undloc;
    unsigned char  ei_svb;
    unsigned char  ei_evb;
    unsigned char  ei_mode;
    unsigned char  ei_lcomp;
};

struct b_param {
    unsigned char  fill[5];
    unsigned char  seqtab[4];
    unsigned char  miscreg;
    struct egainit egatab;
    unsigned char  attrtab[20];
    unsigned char  graphtab[9];
};

struct kd_custom_mode {
    unsigned short xpix;
    unsigned short ypix;
    unsigned short colors;
    unsigned long  buf_size;
    unsigned long  map_size;
    unsigned short slbytes;
    unsigned char  ramdac;
    unsigned char  reserved;
    struct b_param regs;
};

typedef enum {
    OPTION_DEVICE,
    OPTION_VT,
    OPTION_MODE
} SVR4WSOption;

static const OptionInfoRec SVR4WSOptions[] = {
    { OPTION_DEVICE, "Device", OPTV_STRING, {0}, FALSE },
    { OPTION_VT, "VT", OPTV_STRING, {0}, FALSE },
    { OPTION_MODE, "Mode", OPTV_STRING, {0}, FALSE },
    { -1, NULL, OPTV_NONE, {0}, FALSE }
};

typedef struct {
    int vt_fd;
    int kd_fd;
    int original_console_mode;
    int original_kd_mode;
    int io_enabled;
    int display_mapped;
    void *mapping_base;
    size_t mapping_size;
    volatile unsigned char *framebuffer;
    size_t framebuffer_size;
    unsigned char *shadow;
    size_t shadow_size;
    int shadow_stride;
    int framebuffer_stride;
    int custom_mode_number;
    OptionInfoPtr options;
    CloseScreenProcPtr CloseScreen;
    Bool (*ShadowFBInit)(ScreenPtr pScreen, RefreshAreaFuncPtr refreshArea);
    const char *device_path;
    const char *vt_path;
} SVR4WSRec, *SVR4WSPtr;

#define SVR4WSPTR(p) ((SVR4WSPtr)((p)->driverPrivate))

static const unsigned short svr4ws_vga_io_ports[] = { 0x3c4, 0x3c5, 0x3ce, 0x3cf, 0x3c8, 0x3c9 };

static const char *const svr4ws_default_vt_paths[] = { "/dev/vt00", "/dev/tty", "/dev/syscon", "/dev/console" };
static const char *const svr4ws_default_device_paths[] = { "/dev/kd/kdvm00", "/dev/video" };

static const OptionInfoRec *SVR4WSAvailableOptions(int chipid, int busid);
static void SVR4WSIdentify(int flags);
static Bool SVR4WSProbe(DriverPtr drv, int flags);
static Bool SVR4WSPreInit(ScrnInfoPtr pScrn, int flags);
static Bool SVR4WSScreenInit(SCREEN_INIT_ARGS_DECL);
static Bool SVR4WSEnterVT(VT_FUNC_ARGS_DECL);
static void SVR4WSLeaveVT(VT_FUNC_ARGS_DECL);
static Bool SVR4WSCloseScreen(CLOSE_SCREEN_ARGS_DECL);
static void SVR4WSFreeScreen(FREE_SCREEN_ARGS_DECL);
static ModeStatus SVR4WSValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode, Bool verbose, int flags);
static Bool SVR4WSSaveScreen(ScreenPtr pScreen, int mode);
static Bool SVR4WSDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr);
static void SVR4WSLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices, LOCO *colors, VisualPtr pVisual);

static void
SVR4WSDebug(const char *message)
{
    size_t length;

    length = strlen(message);
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r'))
        --length;
    if (!length)
        return;

    (void)syscall(SYS_CLOCAL, CLOCAL_DEBUGCON_WRITE, message, length, 0, 0);
}

static void
SVR4WSDebugRefresh(int count, BoxPtr boxes)
{
    static unsigned int calls;
    char message[160];

    ++calls;
    if (calls > 32 && (calls & (calls - 1)) != 0)
        return;

    if (count > 0 && boxes) {
        (void)snprintf(message, sizeof(message),
            "svr4ws: RefreshArea call=%u count=%d first=%d,%d-%d,%d",
            calls, count, boxes[0].x1, boxes[0].y1, boxes[0].x2, boxes[0].y2);
    } else {
        (void)snprintf(message, sizeof(message),
            "svr4ws: RefreshArea call=%u count=%d", calls, count);
    }
    SVR4WSDebug(message);
}

_X_EXPORT DriverRec SVR4WS = {
    SVR4WS_VERSION,
    SVR4WS_DRIVER_NAME,
    SVR4WSIdentify,
    SVR4WSProbe,
    SVR4WSAvailableOptions,
    NULL,
    0,
    SVR4WSDriverFunc
};

static SymTabRec SVR4WSChipsets[] = {
    { SVR4WS_CHIP, "svr4ws" },
    { -1, NULL }
};

static XF86ModuleVersionInfo svr4wsVersRec = {
    SVR4WS_DRIVER_NAME,
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    0, 1, 0,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    { 0, 0, 0, 0 }
};

static MODULESETUPPROTO(svr4wsSetup);

_X_EXPORT XF86ModuleData svr4wsModuleData = { &svr4wsVersRec, svr4wsSetup, NULL };

static pointer
svr4wsSetup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool setupDone = FALSE;

    if (setupDone) {
        if (errmaj)
            *errmaj = LDR_ONCEONLY;
        return NULL;
    }

    setupDone = TRUE;
    xf86AddDriver(&SVR4WS, module, HaveDriverFuncs);
    return (pointer)1;
}

static Bool
SVR4WSGetRec(ScrnInfoPtr pScrn)
{
    if (pScrn->driverPrivate)
        return TRUE;

    pScrn->driverPrivate = xnfcalloc(sizeof(SVR4WSRec), 1);
    if (!pScrn->driverPrivate)
        return FALSE;

    SVR4WSPTR(pScrn)->vt_fd = -1;
    SVR4WSPTR(pScrn)->kd_fd = -1;
    SVR4WSPTR(pScrn)->original_console_mode = -1;
    SVR4WSPTR(pScrn)->original_kd_mode = -1;
    SVR4WSPTR(pScrn)->custom_mode_number = -1;
    return TRUE;
}

static void
SVR4WSFreeRec(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr;

    if (!pScrn->driverPrivate)
        return;

    fPtr = SVR4WSPTR(pScrn);
    free(fPtr->options);
    free(pScrn->driverPrivate);
    pScrn->driverPrivate = NULL;
}

static int
SVR4WSOpenFirst(const char *configured_path, const char *const *fallbacks, size_t fallback_count)
{
    size_t index;
    int fd;

    if (configured_path) {
        fd = open(configured_path, O_RDWR);
        if (fd >= 0)
            return fd;
    }

    for (index = 0; index < fallback_count; ++index) {
        fd = open(fallbacks[index], O_RDWR);
        if (fd >= 0)
            return fd;
    }

    return -1;
}

static void
SVR4WSCloseFd(int *fd)
{
    if (*fd >= 0)
        close(*fd);
    *fd = -1;
}

static size_t
SVR4WSRoundUpPageSize(size_t size)
{
    long page_size;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;
    return (size + (size_t)page_size - 1U) & ~((size_t)page_size - 1U);
}

static void *
SVR4WSReserveMapping(size_t size)
{
    void *mapping;

    mapping = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED)
        return NULL;
    return mapping;
}

static inline void
SVR4WSWritePort8(unsigned short port, unsigned char value)
{
    __asm__ __volatile__("outb %0, %w1" : : "a"(value), "Nd"(port) : "memory");
}

static inline unsigned char
SVR4WSReadPort8(unsigned short port)
{
    unsigned char value;

    __asm__ __volatile__("inb %w1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

static void
SVR4WSWriteVGARegister(unsigned short index_port, unsigned short data_port, unsigned char index, unsigned char value)
{
    SVR4WSWritePort8(index_port, index);
    SVR4WSWritePort8(data_port, value);
}

static void
SVR4WSSetVGAPlaneMask(unsigned char plane_mask)
{
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x02, plane_mask);
}

static void
SVR4WSSetVGAByteMask(unsigned char byte_mask)
{
    SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x08, byte_mask);
}

static void
SVR4WSSetCirrusBank(int bank)
{
    SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x09, (unsigned char)bank);
}

static void
SVR4WSSetCirrusHiddenDAC(unsigned char value)
{
    (void)SVR4WSReadPort8(0x3c6);
    (void)SVR4WSReadPort8(0x3c6);
    (void)SVR4WSReadPort8(0x3c6);
    (void)SVR4WSReadPort8(0x3c6);
    SVR4WSWritePort8(0x3c6, value);
}

static Bool
SVR4WSEnableVGAIO(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr;
    size_t index;

    fPtr = SVR4WSPTR(pScrn);
    if (fPtr->io_enabled)
        return TRUE;

    for (index = 0; index < sizeof(svr4ws_vga_io_ports) / sizeof(svr4ws_vga_io_ports[0]); ++index) {
        if (ioctl(fPtr->kd_fd, KDADDIO, (unsigned int)svr4ws_vga_io_ports[index]) < 0)
            goto fail;
    }

    if (ioctl(fPtr->kd_fd, KDENABIO, 0) < 0)
        goto fail;

    fPtr->io_enabled = 1;
    return TRUE;

fail:
    xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "unable to enable VGA I/O: %s\n", strerror(errno));
    (void)ioctl(fPtr->kd_fd, KDDISABIO, 0);
    while (index > 0) {
        --index;
        (void)ioctl(fPtr->kd_fd, KDDELIO, (unsigned int)svr4ws_vga_io_ports[index]);
    }
    return FALSE;
}

static void
SVR4WSDisableVGAIO(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr;
    size_t index;

    fPtr = SVR4WSPTR(pScrn);
    if (!fPtr->io_enabled)
        return;

    SVR4WSSetVGAPlaneMask(0x0f);
    SVR4WSSetVGAByteMask(0xff);
    (void)ioctl(fPtr->kd_fd, KDDISABIO, 0);
    for (index = 0; index < sizeof(svr4ws_vga_io_ports) / sizeof(svr4ws_vga_io_ports[0]); ++index)
        (void)ioctl(fPtr->kd_fd, KDDELIO, (unsigned int)svr4ws_vga_io_ports[index]);
    fPtr->io_enabled = 0;
}

static Bool
SVR4WSMapDisplay(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr;
    struct kd_dispinfo dispinfo;
    struct kd_memloc memloc;
    void *mapping_base;
    size_t mapping_size;

    fPtr = SVR4WSPTR(pScrn);
    if (fPtr->display_mapped)
        return TRUE;

    memset(&dispinfo, 0, sizeof(dispinfo));
    if (ioctl(fPtr->kd_fd, KDDISPINFO, &dispinfo) < 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KDDISPINFO failed: %s\n", strerror(errno));
        return FALSE;
    }

    mapping_size = SVR4WSRoundUpPageSize((size_t)dispinfo.size);
    mapping_base = SVR4WSReserveMapping(mapping_size);
    if (!mapping_base) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "unable to reserve framebuffer mapping: %s\n", strerror(errno));
        return FALSE;
    }

    memset(&memloc, 0, sizeof(memloc));
    memloc.vaddr = mapping_base;
    memloc.physaddr = (char *)(uintptr_t)dispinfo.physaddr;
    memloc.length = (long)dispinfo.size;
    memloc.ioflg = 1;

    if (ioctl(fPtr->kd_fd, KDMAPDISP, &memloc) < 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KDMAPDISP failed: %s\n", strerror(errno));
        (void)munmap(mapping_base, mapping_size);
        return FALSE;
    }

    fPtr->framebuffer = (volatile unsigned char *)memloc.vaddr;
    fPtr->framebuffer_size = (size_t)dispinfo.size;
    fPtr->mapping_base = mapping_base;
    fPtr->mapping_size = mapping_size;
    fPtr->display_mapped = 1;
    xf86DrvMsg(pScrn->scrnIndex, X_INFO, "mapped display phys=0x%08lx size=%lu vaddr=%p\n",
        dispinfo.physaddr, dispinfo.size, memloc.vaddr);
    return TRUE;
}

static void
SVR4WSUnmapDisplay(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr;

    fPtr = SVR4WSPTR(pScrn);
    if (!fPtr->display_mapped)
        return;

    (void)ioctl(fPtr->kd_fd, KDUNMAPDISP, 0);
    if (fPtr->mapping_base && fPtr->mapping_size)
        (void)munmap(fPtr->mapping_base, fPtr->mapping_size);
    fPtr->framebuffer = NULL;
    fPtr->framebuffer_size = 0;
    fPtr->mapping_base = NULL;
    fPtr->mapping_size = 0;
    fPtr->display_mapped = 0;
}

static void
SVR4WSRestoreTextMode(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr;

    fPtr = SVR4WSPTR(pScrn);
    if (fPtr->io_enabled) {
        /* Unlock extensions */
        SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x06, 0x12);
        /* Disable packed-pixel mode */
        SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x07, 0x00);
        /* Disable extensions control */
        SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x0b, 0x00);
        /* Clear bank offsets */
        SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x09, 0x00);
        SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x0a, 0x00);
        /* Lock extensions */
        SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x06, 0x00);
    }
    SVR4WSUnmapDisplay(pScrn);
    SVR4WSDisableVGAIO(pScrn);
    if (fPtr->kd_fd >= 0) {
        if (fPtr->original_console_mode >= 0)
            (void)ioctl(fPtr->kd_fd, MODESWITCH | (fPtr->original_console_mode & 0xff), 0);
        if (fPtr->original_kd_mode >= 0)
            (void)ioctl(fPtr->kd_fd, KDSETMODE, fPtr->original_kd_mode);
    }
}

static DisplayModePtr
SVR4WSCreateFixedMode(void)
{
    DisplayModePtr mode;

    mode = xnfcalloc(sizeof(DisplayModeRec), 1);
    mode->name = xnfstrdup("800x600");
    mode->status = MODE_OK;
    mode->type = M_T_DRIVER | M_T_PREFERRED;
    mode->Clock = 40000;
    mode->HDisplay = 800;
    mode->HSyncStart = 840;
    mode->HSyncEnd = 968;
    mode->HTotal = 1056;
    mode->VDisplay = 600;
    mode->VSyncStart = 601;
    mode->VSyncEnd = 605;
    mode->VTotal = 628;
    mode->next = mode;
    mode->prev = mode;
    return mode;
}

static void
SVR4WSRefreshArea(ScrnInfoPtr pScrn, int count, BoxPtr boxes)
{
    SVR4WSPtr fPtr;
    static int logged;
    int box_index;

    fPtr = SVR4WSPTR(pScrn);
    if (!fPtr->shadow || !fPtr->framebuffer || !fPtr->display_mapped) {
        static int logged_skip;

        if (!logged_skip) {
            SVR4WSDebug("svr4ws: RefreshArea skipped no shadow/fb/map");
            logged_skip = 1;
        }
        return;
    }

    if (!logged) {
        SVR4WSDebug("svr4ws: RefreshArea");
        logged = 1;
    }
    SVR4WSDebugRefresh(count, boxes);

    int current_bank = -1;

    for (box_index = 0; box_index < count; ++box_index) {
        int x1 = boxes[box_index].x1;
        int y1 = boxes[box_index].y1;
        int x2 = boxes[box_index].x2;
        int y2 = boxes[box_index].y2;

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 > SVR4WS_WIDTH) x2 = SVR4WS_WIDTH;
        if (y2 > SVR4WS_HEIGHT) y2 = SVR4WS_HEIGHT;
        if (x1 >= x2 || y1 >= y2)
            continue;

        int y;
        for (y = y1; y < y2; ++y) {
            const unsigned char *src;
            size_t offset;
            size_t remaining;

            src = fPtr->shadow + ((size_t)y * (size_t)fPtr->shadow_stride)
                + ((size_t)x1 * SVR4WS_BYTES_PER_PIXEL);
            offset = ((size_t)y * (size_t)fPtr->framebuffer_stride)
                + ((size_t)x1 * SVR4WS_BYTES_PER_PIXEL);
            remaining = (size_t)(x2 - x1) * SVR4WS_BYTES_PER_PIXEL;

            while (remaining) {
                size_t bank_base = offset & ~((size_t)SVR4WS_BANK_APERTURE - 1U);
                int bank = (int)(bank_base / SVR4WS_BANK_GRANULARITY);
                size_t bank_offset = offset - bank_base;
                size_t bank_remaining = (size_t)SVR4WS_BANK_APERTURE - bank_offset;
                size_t chunk = remaining < bank_remaining ? remaining : bank_remaining;

                if (bank != current_bank) {
                    SVR4WSSetCirrusBank(bank);
                    current_bank = bank;
                }

                memcpy((void *)(fPtr->framebuffer + bank_offset), src, chunk);
                src += chunk;
                offset += chunk;
                remaining -= chunk;
            }
        }
    }
}

static const OptionInfoRec *
SVR4WSAvailableOptions(int chipid, int busid)
{
    return SVR4WSOptions;
}

static void
SVR4WSIdentify(int flags)
{
    xf86PrintChipsets(SVR4WS_NAME, "SVR4 workstation display driver", SVR4WSChipsets);
}

static Bool
SVR4WSProbe(DriverPtr drv, int flags)
{
    Bool found_screen;
    GDevPtr *dev_sections;
    int num_dev_sections;
    int index;

    if (flags & PROBE_DETECT)
        return FALSE;

    num_dev_sections = xf86MatchDevice(SVR4WS_DRIVER_NAME, &dev_sections);
    if (num_dev_sections <= 0)
        return FALSE;

    found_screen = FALSE;
    for (index = 0; index < num_dev_sections; ++index) {
        ScrnInfoPtr pScrn;
        int entity;

        entity = xf86ClaimNoSlot(drv, SVR4WS_CHIP, dev_sections[index], TRUE);
        pScrn = xf86AllocateScreen(drv, 0);
        if (!pScrn)
            continue;

        xf86AddEntityToScreen(pScrn, entity);
        pScrn->driverVersion = SVR4WS_VERSION;
        pScrn->driverName = SVR4WS_DRIVER_NAME;
        pScrn->name = SVR4WS_NAME;
        pScrn->Probe = SVR4WSProbe;
        pScrn->PreInit = SVR4WSPreInit;
        pScrn->ScreenInit = SVR4WSScreenInit;
        pScrn->EnterVT = SVR4WSEnterVT;
        pScrn->LeaveVT = SVR4WSLeaveVT;
        pScrn->FreeScreen = SVR4WSFreeScreen;
        pScrn->ValidMode = SVR4WSValidMode;
        found_screen = TRUE;
    }

    free(dev_sections);
    return found_screen;
}

static Bool
SVR4WSPreInit(ScrnInfoPtr pScrn, int flags)
{
    SVR4WSPtr fPtr;
    EntityInfoPtr entity;
    Gamma zeros = { 0.0, 0.0, 0.0 };
    rgb default_weight = { 5, 6, 5 };
    rgb default_mask = { 0, 0, 0 };

    if (flags & PROBE_DETECT)
        return TRUE;

    SVR4WSDebug("svr4ws: PreInit\n");

    if (!SVR4WSGetRec(pScrn))
        return FALSE;

    fPtr = SVR4WSPTR(pScrn);
    entity = xf86GetEntityInfo(pScrn->entityList[0]);
    pScrn->chipset = (char *)xf86TokenToString(SVR4WSChipsets, SVR4WS_CHIP);
    pScrn->monitor = pScrn->confScreen->monitor;
    pScrn->progClock = TRUE;
    pScrn->rgbBits = 6;

    if (!xf86SetDepthBpp(pScrn, SVR4WS_DEPTH, SVR4WS_DEPTH, SVR4WS_BPP, 0))
        return FALSE;
    if (pScrn->depth != SVR4WS_DEPTH || pScrn->bitsPerPixel != SVR4WS_BPP) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "only depth 16 / bpp 16 is supported\n");
        return FALSE;
    }
    if (!xf86SetWeight(pScrn, default_weight, default_mask))
        return FALSE;

    xf86PrintDepthBpp(pScrn);
    if (!xf86SetDefaultVisual(pScrn, TrueColor))
        return FALSE;
    if (!xf86SetGamma(pScrn, zeros))
        return FALSE;

    xf86CollectOptions(pScrn, entity->device->options);
    fPtr->options = malloc(sizeof(SVR4WSOptions));
    if (!fPtr->options)
        return FALSE;
    memcpy(fPtr->options, SVR4WSOptions, sizeof(SVR4WSOptions));
    xf86ProcessOptions(pScrn->scrnIndex, pScrn->options, fPtr->options);
    fPtr->device_path = xf86GetOptValString(fPtr->options, OPTION_DEVICE);
    fPtr->vt_path = xf86GetOptValString(fPtr->options, OPTION_VT);
    {
        const char *configured_mode = xf86GetOptValString(fPtr->options, OPTION_MODE);
        if (configured_mode && strcmp(configured_mode, "cirrus800x600x16") != 0) {
            xf86DrvMsg(pScrn->scrnIndex, X_WARNING, "configured mode '%s' is not supported; falling back to cirrus800x600x16\n", configured_mode);
        }
    }

    pScrn->videoRam = 1024;
    pScrn->virtualX = SVR4WS_WIDTH;
    pScrn->virtualY = SVR4WS_HEIGHT;
    pScrn->displayWidth = SVR4WS_WIDTH;
    pScrn->modes = SVR4WSCreateFixedMode();
    pScrn->currentMode = pScrn->modes;
    xf86SetDpi(pScrn, 0, 0);

    if (!xf86LoadSubModule(pScrn, "fb"))
        return FALSE;
    {
        void *module;

        module = xf86LoadSubModule(pScrn, "shadowfb");
        if (!module)
            return FALSE;
        fPtr->ShadowFBInit = LoaderSymbolFromModule(module, "ShadowFBInit");
        if (!fPtr->ShadowFBInit) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "shadowfb module does not export ShadowFBInit\n");
            return FALSE;
        }
    }

    if (!fPtr->ShadowFBInit)
        return FALSE;

    pScrn->memPhysBase = 0;
    pScrn->fbOffset = 0;
    SVR4WSDebug("svr4ws: PreInit done\n");
    return TRUE;
}

static Bool
SVR4WSEnterVT(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);
    SVR4WSPtr fPtr;

    fPtr = SVR4WSPTR(pScrn);
    if (fPtr->kd_fd < 0) {
        fPtr->vt_fd = SVR4WSOpenFirst(fPtr->vt_path, svr4ws_default_vt_paths,
            sizeof(svr4ws_default_vt_paths) / sizeof(svr4ws_default_vt_paths[0]));
        fPtr->kd_fd = SVR4WSOpenFirst(fPtr->device_path, svr4ws_default_device_paths,
            sizeof(svr4ws_default_device_paths) / sizeof(svr4ws_default_device_paths[0]));
        if (fPtr->kd_fd < 0) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "unable to open workstation display device: %s\n", strerror(errno));
            SVR4WSCloseFd(&fPtr->vt_fd);
            return FALSE;
        }
        fPtr->original_console_mode = ioctl(fPtr->kd_fd, CONS_GET, 0);
        if (ioctl(fPtr->kd_fd, KDGETMODE, &fPtr->original_kd_mode) < 0)
            fPtr->original_kd_mode = -1;
    }

    if (!SVR4WSEnableVGAIO(pScrn)) {
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
    {
        struct kd_custom_mode custom_mode = {
            SVR4WS_WIDTH, SVR4WS_HEIGHT, 0, SVR4WS_FRAMEBUFFER_SIZE,
            SVR4WS_BANK_APERTURE, SVR4WS_FRAMEBUFFER_STRIDE, 3, 0,
            {
                {0x00, 0x00, 0x00, 0x00, 0x00},
                {0x01, 0x0f, 0x00, 0x0e},
                0x2f,
                {
                    0x7b, 0x63, 0x64, 0x9e, 0x69, 0x92, 0x6f, 0xf0,
                    0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x58, 0x8a, 0x57, 0xc8, 0x40, 0x58, 0x6f, 0xa3, 0xff
                },
                {
                    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x41, 0x00, 0x0f, 0x00
                },
                {
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0f, 0xff
                }
            }
        };

        fPtr->framebuffer_stride = custom_mode.slbytes;
        if (ioctl(fPtr->kd_fd, KD_SET_CUSTOM_MODE, &custom_mode) < 0) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KD_SET_CUSTOM_MODE failed: %s\n", strerror(errno));
            SVR4WSRestoreTextMode(pScrn);
            return FALSE;
        }
        if (ioctl(fPtr->kd_fd, KD_GET_CUSTOM_MODE, &fPtr->custom_mode_number) < 0) {
            xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KD_GET_CUSTOM_MODE failed: %s\n", strerror(errno));
            SVR4WSRestoreTextMode(pScrn);
            return FALSE;
        }
    }
    if (fPtr->custom_mode_number < 0 || fPtr->custom_mode_number > 255) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "kernel returned invalid custom mode %d\n", fPtr->custom_mode_number);
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
    if (ioctl(fPtr->kd_fd, MODESWITCH | fPtr->custom_mode_number, 0) < 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "SW_CUSTOM_MODE failed: %s\n", strerror(errno));
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
    /* Unlock Cirrus Logic extensions */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x06, 0x12);
    /* Enable 16-bpp packed-pixel SVGA mode at the A0000 aperture. */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x07,
        CIRRUS_SR07_ISAADDR_A0000 | CIRRUS_SR07_BPP_SVGA | CIRRUS_SR07_BPP_16);
    SVR4WSSetCirrusHiddenDAC(CIRRUS_HIDDEN_DAC_565);
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x0f,
        CIRRUS_SR0F_MEMSIZE_1M | CIRRUS_SR0F_BANKSWITCH);
    /* Configure single bank mode with 16KB granularity */
    SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x0b, 0x20);

    if (ioctl(fPtr->kd_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KDSETMODE KD_GRAPHICS failed: %s\n", strerror(errno));
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
    if (!SVR4WSMapDisplay(pScrn)) {
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
    pScrn->vtSema = TRUE;
    return TRUE;
}

static void
SVR4WSLeaveVT(VT_FUNC_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);

    SVR4WSRestoreTextMode(pScrn);
    pScrn->vtSema = FALSE;
}

static Bool
SVR4WSScreenInit(SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn;
    SVR4WSPtr fPtr;
    VisualPtr visual;

    pScrn = xf86ScreenToScrn(pScreen);
    fPtr = SVR4WSPTR(pScrn);
    SVR4WSDebug("svr4ws: ScreenInit\n");

    fPtr->shadow_stride = SVR4WS_FRAMEBUFFER_STRIDE;
    fPtr->framebuffer_stride = SVR4WS_FRAMEBUFFER_STRIDE;
    fPtr->shadow_size = (size_t)SVR4WS_HEIGHT * (size_t)fPtr->shadow_stride;
    fPtr->shadow = calloc(1, fPtr->shadow_size);
    if (!fPtr->shadow)
        return FALSE;

    if (!SVR4WSEnterVT(pScrn))
        return FALSE;
    SVR4WSDebug("svr4ws: EnterVT done\n");

    miClearVisualTypes();
    if (!miSetVisualTypes(pScrn->depth, miGetDefaultVisualMask(pScrn->depth), pScrn->rgbBits, pScrn->defaultVisual))
        return FALSE;
    if (!miSetPixmapDepths())
        return FALSE;
    if (!fbScreenInit(pScreen, fPtr->shadow, pScrn->virtualX, pScrn->virtualY,
            pScrn->xDpi, pScrn->yDpi, pScrn->displayWidth, pScrn->bitsPerPixel))
        return FALSE;

    visual = pScreen->visuals + pScreen->numVisuals;
    while (--visual >= pScreen->visuals) {
        if ((visual->class | DynamicClass) == DirectColor) {
            visual->offsetRed = pScrn->offset.red;
            visual->offsetGreen = pScrn->offset.green;
            visual->offsetBlue = pScrn->offset.blue;
            visual->redMask = pScrn->mask.red;
            visual->greenMask = pScrn->mask.green;
            visual->blueMask = pScrn->mask.blue;
        }
    }

    fbPictureInit(pScreen, 0, 0);
    xf86SetBlackWhitePixels(pScreen);
    xf86SetBackingStore(pScreen);
    xf86SetSilkenMouse(pScreen);
    miDCInitialize(pScreen, xf86GetPointerScreenFuncs());

    if (!miCreateDefColormap(pScreen))
        return FALSE;
    if (pScrn->depth <= 8) {
        SVR4WSDebug("svr4ws: HandleColormaps\n");
        if (!xf86HandleColormaps(pScreen, SVR4WS_COLORMAP_ENTRIES, pScrn->rgbBits, SVR4WSLoadPalette, NULL, CMAP_PALETTED_TRUECOLOR))
            return FALSE;
    }
    SVR4WSDebug("svr4ws: ShadowFBInit\n");
    if (!fPtr->ShadowFBInit(pScreen, SVR4WSRefreshArea))
        return FALSE;

    pScreen->SaveScreen = SVR4WSSaveScreen;
    fPtr->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = SVR4WSCloseScreen;

    if (serverGeneration == 1)
        xf86ShowUnusedOptions(pScrn->scrnIndex, pScrn->options);

    SVR4WSRefreshArea(pScrn, 1, &(BoxRec){ 0, 0, SVR4WS_WIDTH, SVR4WS_HEIGHT });
    SVR4WSDebug("svr4ws: ScreenInit done\n");
    return TRUE;
}

static Bool
SVR4WSCloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn;
    SVR4WSPtr fPtr;

    pScrn = xf86ScreenToScrn(pScreen);
    fPtr = SVR4WSPTR(pScrn);
    SVR4WSRestoreTextMode(pScrn);
    free(fPtr->shadow);
    fPtr->shadow = NULL;
    fPtr->shadow_size = 0;
    SVR4WSCloseFd(&fPtr->kd_fd);
    SVR4WSCloseFd(&fPtr->vt_fd);
    pScrn->vtSema = FALSE;
    pScreen->CloseScreen = fPtr->CloseScreen;
    return (*pScreen->CloseScreen)(CLOSE_SCREEN_ARGS);
}

static void
SVR4WSFreeScreen(FREE_SCREEN_ARGS_DECL)
{
    SCRN_INFO_PTR(arg);

    SVR4WSFreeRec(pScrn);
}

static ModeStatus
SVR4WSValidMode(SCRN_ARG_TYPE arg, DisplayModePtr mode, Bool verbose, int flags)
{
    if (mode->HDisplay == SVR4WS_WIDTH && mode->VDisplay == SVR4WS_HEIGHT)
        return MODE_OK;
    return MODE_BAD;
}

static Bool
SVR4WSSaveScreen(ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static void
SVR4WSLoadPalette(ScrnInfoPtr pScrn, int numColors, int *indices, LOCO *colors, VisualPtr pVisual)
{
    int i;
    static int logged;

    if (!logged) {
        SVR4WSDebug("svr4ws: LoadPalette");
        logged = 1;
    }

    if (!pScrn->vtSema)
        return;

    for (i = 0; i < numColors; ++i) {
        int index;
        int hw_index;

        index = indices[i];
        if (index < 0 || index >= SVR4WS_COLORMAP_ENTRIES)
            continue;

        hw_index = index & (SVR4WS_HW_COLORS - 1);

        SVR4WSWritePort8(0x3c8, (unsigned char)hw_index);
        SVR4WSWritePort8(0x3c9, (unsigned char)(colors[index].red & 0x3f));
        SVR4WSWritePort8(0x3c9, (unsigned char)(colors[index].green & 0x3f));
        SVR4WSWritePort8(0x3c9, (unsigned char)(colors[index].blue & 0x3f));
    }
}

static Bool
SVR4WSDriverFunc(ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr)
{
    CARD32 *flag;

    switch (op) {
    case GET_REQUIRED_HW_INTERFACES:
        flag = (CARD32 *)ptr;
        *flag = HW_SKIP_CONSOLE;
        return TRUE;
    default:
        return FALSE;
    }
}
