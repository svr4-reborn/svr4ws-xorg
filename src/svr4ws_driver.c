#include "xorg-server.h"

/* TEMP: horizontal-scale probe pattern (no slow ISABugger spins).  Remove. */
/* #define SVR4WS_TESTPATTERN 0 */
/* TEMP: ISABugger register readbacks (CR1B, SR07, geometry inputs).  Remove. */
/* #define SVR4WS_DIAGNOSTICS 0 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
#define SVR4WS_HW_COLORS 256
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

/*
 * The graphics-mode memory map is the single 64 KB window at A0000 (graphics
 * register GR06 bits [3:2] = 01, banked_mask = 0xffff).  In single-bank mode
 * (GR0B bit 0 = 0) the whole 64 KB window is addressed by the GR09 offset
 * register, which with GR0B bit 5 set has 16 KB granularity.  So GR09 must
 * advance by four (4 x 16 KB = 64 KB) to step the window by one full aperture,
 * and the bank base is therefore 64 KB-aligned.
 */
#define SVR4WS_BANK_APERTURE (64 * 1024)
#define SVR4WS_BANK_GRANULARITY (16 * 1024)

#define CIRRUS_SR07_BPP_SVGA 0x01
#define CIRRUS_SR07_BPP_16 0x06
/*
 * IMPORTANT: SR07 bits [7:4] are NOT a harmless pixel/aperture tweak -- a
 * nonzero high nibble selects the high-memory *linear* framebuffer aperture
 * and DISABLES the A0000 banked window that this driver writes through.  We
 * drive the chip purely via the A0000 64 KB bank window (GR09), so SR07's high
 * nibble must stay zero.  The full 16bpp value is therefore just
 * SVGA (bit 0) | BPP_16 (bits 2:1) = 0x07.
 *
 * (cirrusfb uses SR07 = 0xa7 because it sets up the linear aperture instead;
 * mixing its SR07 value with banked writes is what left the screen showing
 * un-written VRAM.)
 */
#define CIRRUS_SR07_16BPP_BANKED (CIRRUS_SR07_BPP_SVGA | CIRRUS_SR07_BPP_16)
#define CIRRUS_SR0F_MEMSIZE_1M 0x10
#define CIRRUS_SR0F_BANKSWITCH 0x80

/*
 * Cirrus chip identification.  The chip family is read back from CRTC index
 * 0x27 (the chip-ID register).  These are the values 86Box and real silicon
 * report.  We only need to know whether the part supports 16bpp HiColor.
 */
#define CIRRUS_CRTC_CHIPID 0x27
#define CIRRUS_ID_GD5420 0x8a
#define CIRRUS_ID_GD5422 0x8c
#define CIRRUS_ID_GD5424 0x94
#define CIRRUS_ID_GD5426 0x90
#define CIRRUS_ID_GD5428 0x98
#define CIRRUS_ID_GD5429 0x9c
#define CIRRUS_ID_GD5430 0xa0
#define CIRRUS_ID_GD5434 0xa8
#define CIRRUS_ID_GD5436 0xac
#define CIRRUS_ID_GD5446 0xb8
/* The chip-ID register reports the family in bits [7:2]; mask off the rev. */
#define CIRRUS_CHIPID_MASK 0xfc

/*
 * CRTC extension register 0x1b.  It carries the offset-overflow bit, the
 * display-start overflow bits, and two control bits that must be set for a
 * correct HiColor scanout:
 *   bit 1 (0x02): use the full video-memory display mask (otherwise the
 *                 emulated/real scanout is clamped to 256 KB, which wraps an
 *                 800x600x16 frame into repeating bands).
 *   bit 5 (0x20): extended blanking (required on GD5424 and later).
 * cirrusfb writes 0x22 for every mode; we do the same, ORing in the offset
 * overflow bit only when the pitch exceeds 8 bits.
 */
#define CIRRUS_CRTC_EXT 0x1b
#define CIRRUS_CR1B_BASE 0x22
#define CIRRUS_CR1B_OFFSET8 0x10
/*
 * Hidden DAC (port 0x3c6) value for 16bpp 5-6-5.  The high nibble 0xc0
 * enables the extended/packed-pixel HiColor modes on real GD542x silicon;
 * the low nibble selects the pixel format (0 = 5-5-5, 1 = 5-6-5).  Emulators
 * decode only the low nibble, but real hardware requires the 0xc0 enable.
 */
#define CIRRUS_HIDDEN_DAC_565 0xc1

/*
 * VCLK0 programming for a 40 MHz pixel clock (standard 800x600 @ 60 Hz dot
 * clock, in-spec for the GD5426/5428 HiColor limit of ~45 MHz).
 *
 * The four VCLKs are programmed through sequencer numerator registers
 * SR0B..SR0E and denominator registers SR1B..SR1E, where SR0B/SR1B is VCLK0,
 * ... SR0E/SR1E is VCLK3.  The MISC clock-select bits [3:2] choose the active
 * VCLK; the egatab sets them to 00 = VCLK0, so we must program SR0B/SR1B
 * (NOT SR0E/SR1E, which is VCLK3 and was the cause of the chip falling back to
 * its fixed 25.175 MHz default).
 *
 *   VCLK = 14.31818 MHz * num / (den * mul)
 * The denominator byte is (den << 1) | (mul == 2 ? 1 : 0); num is 7 bits and
 * den is only 5 bits.  num = 42 (0x2a), den = 15, mul = 1 ->
 * 14.31818 * 42 / 15 = 40.09 MHz.
 */
#define CIRRUS_SR0B_VCLK0_NUM 0x2a
#define CIRRUS_SR1B_VCLK0_DEN ((15 << 1) | 0)

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
    OPTION_MODE,
    OPTION_CHIPSET
} SVR4WSOption;

static const OptionInfoRec SVR4WSOptions[] = {
    { OPTION_DEVICE, "Device", OPTV_STRING, {0}, FALSE },
    { OPTION_VT, "VT", OPTV_STRING, {0}, FALSE },
    { OPTION_MODE, "Mode", OPTV_STRING, {0}, FALSE },
    { OPTION_CHIPSET, "Chipset", OPTV_STRING, {0}, FALSE },
    { -1, NULL, OPTV_NONE, {0}, FALSE }
};

/*
 * Per-chip description.  hicolor is FALSE for the GD5420/5422, which are
 * 8bpp-only and cannot drive this driver's hardcoded 16bpp mode.
 */
typedef struct {
    const char *name;
    unsigned char id;       /* CRTC 0x27 value, masked with CIRRUS_CHIPID_MASK */
    Bool hicolor;
} SVR4WSChipInfo;

static const SVR4WSChipInfo svr4ws_chips[] = {
    { "GD5420", CIRRUS_ID_GD5420 & CIRRUS_CHIPID_MASK, FALSE },
    { "GD5422", CIRRUS_ID_GD5422 & CIRRUS_CHIPID_MASK, FALSE },
    { "GD5424", CIRRUS_ID_GD5424 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5426", CIRRUS_ID_GD5426 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5428", CIRRUS_ID_GD5428 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5429", CIRRUS_ID_GD5429 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5430", CIRRUS_ID_GD5430 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5434", CIRRUS_ID_GD5434 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5436", CIRRUS_ID_GD5436 & CIRRUS_CHIPID_MASK, TRUE },
    { "GD5446", CIRRUS_ID_GD5446 & CIRRUS_CHIPID_MASK, TRUE },
};

/* Default assumption when detection is unavailable: a GD5429-class part. */
#define SVR4WS_DEFAULT_CHIP 5

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
    const char *chipset_name;       /* Xorg "Chipset" override, or NULL */
    const SVR4WSChipInfo *chip;     /* selected chip description */
} SVR4WSRec, *SVR4WSPtr;

#define SVR4WSPTR(p) ((SVR4WSPtr)((p)->driverPrivate))

static const unsigned short svr4ws_vga_io_ports[] = {
    0x3c4, 0x3c5, 0x3ce, 0x3cf, 0x3c8, 0x3c9, 0x3c6, 0x3d4, 0x3d5,
    /* 86Box ISABugger diagnostic card (index 0x7a, data 0x7b). */
    0x7a, 0x7b
};

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

/*
 * Diagnostics via the 86Box ISABugger card (index port 0x7a, data port 0x7b).
 * Compiled out unless SVR4WS_DIAGNOSTICS is defined, because the readout spin
 * (and the standalone test pattern) badly slow X startup under 86Box.  Build
 * with -DSVR4WS_DIAGNOSTICS to re-enable them.
 *   - red LEDs   (reg 0x00): a "stage" tag so you know which value is shown
 *   - green LEDs (reg 0x01): the reported value, in binary
 *   - 7-seg displays (reg 0x02 right, 0x04 left): the reported value, in hex
 */
#ifdef SVR4WS_DIAGNOSTICS
#define SVR4WS_BUGGER_INDEX 0x7a
#define SVR4WS_BUGGER_DATA 0x7b

static void
SVR4WSBuggerWrite(unsigned char reg, unsigned char value)
{
    SVR4WSWritePort8(SVR4WS_BUGGER_INDEX, reg);
    SVR4WSWritePort8(SVR4WS_BUGGER_DATA, value);
}

static void
SVR4WSBuggerReport(unsigned char stage, unsigned char value)
{
    volatile unsigned long spin;

    SVR4WSBuggerWrite(0x00, stage);          /* red LEDs: which value this is */
    SVR4WSBuggerWrite(0x01, value);          /* green LEDs: value in binary */
    SVR4WSBuggerWrite(0x02, value);          /* right 7-seg: value low nibble pair */
    SVR4WSBuggerWrite(0x04, value);          /* left 7-seg: same byte */

    /* Hold the value on the displays long enough to read (~1-2s of busy spin). */
    for (spin = 0; spin < 200000000UL; ++spin)
        __asm__ __volatile__("" ::: "memory");
}
#else
#define SVR4WSBuggerReport(stage, value) ((void)0)
#endif

static unsigned char
SVR4WSReadVGARegister(unsigned short index_port, unsigned short data_port, unsigned char index)
{
    SVR4WSWritePort8(index_port, index);
    return SVR4WSReadPort8(data_port);
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

static const SVR4WSChipInfo *
SVR4WSChipByName(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(svr4ws_chips) / sizeof(svr4ws_chips[0]); ++i) {
        if (strcasecmp(name, svr4ws_chips[i].name) == 0)
            return &svr4ws_chips[i];
    }
    return NULL;
}

static const SVR4WSChipInfo *
SVR4WSChipById(unsigned char id)
{
    size_t i;

    id &= CIRRUS_CHIPID_MASK;
    for (i = 0; i < sizeof(svr4ws_chips) / sizeof(svr4ws_chips[0]); ++i) {
        if (svr4ws_chips[i].id == id)
            return &svr4ws_chips[i];
    }
    return NULL;
}

/*
 * Resolve which Cirrus chip we are driving.  A "Chipset" option in the Xorg
 * config always wins; otherwise we read the chip-ID register (CRTC 0x27) and
 * match it against the table.  Requires VGA I/O to already be enabled.
 * Falls back to the GD5429-class default if nothing matches.
 */
static void
SVR4WSDetectChip(ScrnInfoPtr pScrn)
{
    SVR4WSPtr fPtr = SVR4WSPTR(pScrn);
    unsigned char raw;
    const SVR4WSChipInfo *chip;

    if (fPtr->chipset_name) {
        chip = SVR4WSChipByName(fPtr->chipset_name);
        if (chip) {
            fPtr->chip = chip;
            xf86DrvMsg(pScrn->scrnIndex, X_CONFIG,
                "using configured Cirrus chipset %s\n", chip->name);
            return;
        }
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
            "unknown configured chipset '%s'; auto-detecting\n", fPtr->chipset_name);
    }

    /* Unlock the Cirrus extensions so the chip-ID register reads back. */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x06, 0x12);
    raw = SVR4WSReadVGARegister(0x3d4, 0x3d5, CIRRUS_CRTC_CHIPID);
    chip = SVR4WSChipById(raw);
    if (chip) {
        fPtr->chip = chip;
        xf86DrvMsg(pScrn->scrnIndex, X_PROBED,
            "detected Cirrus %s (CR27=0x%02x)\n", chip->name, raw);
        return;
    }

    fPtr->chip = &svr4ws_chips[SVR4WS_DEFAULT_CHIP];
    xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
        "unrecognized Cirrus chip-ID 0x%02x; assuming %s\n",
        raw, fPtr->chip->name);
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
        /* Restore the CRTC extension register to its standard-VGA state */
        SVR4WSWriteVGARegister(0x3d4, 0x3d5, CIRRUS_CRTC_EXT, 0x00);
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
#ifdef SVR4WS_DIAGNOSTICS
        /*
         * Stage 7: highest bank index this refresh reaches.  The last scanline
         * of a full 800x600x16 frame lands at byte 599*1600 -> bank 0x3a, so
         * ~0x3a means banking reaches the whole frame.
         */
        {
            size_t last = (size_t)(SVR4WS_HEIGHT - 1) * (size_t)fPtr->framebuffer_stride;
            SVR4WSBuggerReport(7, (unsigned char)(last / SVR4WS_BANK_GRANULARITY));
        }
#endif
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
                /*
                 * The 64 KB window is anchored on a 64 KB boundary; GR09 holds
                 * that base in 16 KB-granularity units (so it is a multiple of
                 * four).  The byte position inside the window is the low 16
                 * bits of the offset.
                 */
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
    /*
     * This driver hardcodes a 16-bpp HiColor mode.  Only the Cirrus GD5426 and
     * later (GD5428/5429/5434, ...) implement HiColor; the GD5420 and GD5422
     * are 8-bpp only and cannot drive this mode.
     */
    xf86DrvMsg(pScrn->scrnIndex, X_INFO,
        "16-bpp HiColor mode requires a GD5426 or later Cirrus chip\n");
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
    fPtr->chipset_name = xf86GetOptValString(fPtr->options, OPTION_CHIPSET);
    if (fPtr->chipset_name && !SVR4WSChipByName(fPtr->chipset_name)) {
        xf86DrvMsg(pScrn->scrnIndex, X_WARNING,
            "configured Chipset '%s' is not recognized; will auto-detect\n",
            fPtr->chipset_name);
    }
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

    if (!fPtr->chip)
        SVR4WSDetectChip(pScrn);
    if (!fPtr->chip->hicolor) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR,
            "Cirrus %s does not support 16-bpp HiColor; this driver requires "
            "a GD5424 or later part\n", fPtr->chip->name);
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
                /*
                 * MISC: clock-select bits [3:2] = 00 select VCLK0, which we
                 * program below to 40 MHz.  (Was 0x2f, selecting VCLK3 with
                 * its ~50 MHz default -> wrong refresh and an out-of-spec
                 * HiColor dot clock.)
                 */
                0x23,
                {
                    /*
                     * CRTC, indices 0x00-0x18.  Horizontal total (CR00) is
                     * 0x83 -> (0x83 + 5) = 132 char clocks = 1056 px, the
                     * standard 800x600 @ 60 Hz htotal.  Underline location
                     * (CR14, 0x00) clears the doubleword (DW) addressing bit.
                     *
                     * CR17 (0xe3) selects BYTE addressing via bit 6 (0x40).
                     * This is essential: 86Box's svga_recalc_remap_func derives
                     * the scanout address remap from CR17/CR14 and applies it
                     * even to packed-pixel HiColor modes.  With bit 6 clear the
                     * chip is in WORD mode (CR17 bit 5 set -> VAR_WORD_MODE_MA15)
                     * and the linear 16-bpp framebuffer is read with the VGA
                     * word-mode address rotation (addr<<1 | MA15->MA2), which
                     * scrambles the image into the diagonal/sheared "sectioned"
                     * pattern.  QEMU ignores CR17 byte/word mode in SVGA modes
                     * and reads linearly, which is why the bug only shows on
                     * 86Box.  Byte mode (bit 6 = 1) makes remap_required false
                     * -> linear scanout.  Other bits kept: 0x80 sync enable,
                     * 0x20 address wrap, 0x03 compatibility/CMS.
                     */
                    0x83, 0x63, 0x64, 0x9e, 0x69, 0x92, 0x6f, 0xf0,
                    0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x58, 0x8a, 0x57, 0xc8, 0x00, 0x58, 0x6f, 0xe3, 0xff
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
    /*
     * Force SR01 (clocking mode) to 8 dots per character clock.  Bit 3 selects
     * 16 dots/clock; if it is left set, the chip generates twice as many pixels
     * per scanline as the CRTC width implies, so an 800-px line is scanned out
     * as 1600 px and the image tiles twice horizontally with a staircase shear.
     * Bit 0 = 8-dot characters; all other bits clear for a plain SVGA line.
     */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x01, 0x01);
    /*
     * Program VCLK0 to 40 MHz so the MISC clock-select (set to VCLK0 in the
     * egatab) drives the correct 800x600 @ 60 Hz dot clock.  This dot clock is
     * within the GD5426/5428 16-bpp limit of ~45 MHz.
     */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x0b, CIRRUS_SR0B_VCLK0_NUM);
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x1b, CIRRUS_SR1B_VCLK0_DEN);
    /* Enable 16-bpp packed-pixel SVGA mode, keeping the A0000 banked window. */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x07, CIRRUS_SR07_16BPP_BANKED);
    SVR4WSSetCirrusHiddenDAC(CIRRUS_HIDDEN_DAC_565);
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x0f,
        CIRRUS_SR0F_MEMSIZE_1M | CIRRUS_SR0F_BANKSWITCH);
    /*
     * Program the CRTC extension register.  Without bit 1 the display scanout
     * is clamped to 256 KB and an 800x600x16 (937 KB) frame wraps into
     * repeating bands; bit 5 selects the extended blanking required on the
     * GD5424 and later.  Our pitch (CR13 = 200) fits in 8 bits, so the offset
     * overflow bit is not needed.
     */
    SVR4WSWriteVGARegister(0x3d4, 0x3d5, CIRRUS_CRTC_EXT, CIRRUS_CR1B_BASE);
    /*
     * Force CR17 byte addressing (bit 6).  The egatab already sets CR17 = 0xe3,
     * but re-assert it here so the scanout address remap stays linear no matter
     * what the kernel mode-switch left behind.  In word mode (bit 6 clear) the
     * 16-bpp framebuffer is read with the VGA word-mode rotation and shears; see
     * the egatab CRTC comment and 86Box svga_recalc_remap_func for the detail.
     */
    SVR4WSWriteVGARegister(0x3d4, 0x3d5, 0x17, 0xe3);
    /*
     * Single-bank mode (GR0B bit 0 = 0) with 16 KB granularity (bit 5 = 1).
     * The A0000 window is the 64 KB aperture driven by GR09.
     */
    SVR4WSWriteVGARegister(0x3ce, 0x3cf, 0x0b, 0x20);
    /*
     * Re-assert SR01 = 8 dots/clock immediately before the final SR07 write.
     * 86Box recomputes the display geometry on every SR07 write
     * (gd54xx_recalctimings) and from it derives hdisp = (CR01+1) * (SR01 bit 3
     * ? 16 : 8) and a linedbl heuristic (dispend*9/10 >= hdisp).  If SR01 bit 3
     * were set here, hdisp would read as 1600, linedbl would flip, and 86Box
     * would select the line-doubled 16bpp renderer (svga_render_16bpp_lowres):
     * each scanline drawn twice, half the content, producing the staircase
     * shear with vertical duplication.  QEMU has no such heuristic and renders
     * the packed framebuffer linearly regardless, which is why this only bites
     * on 86Box.  Re-writing SR01 here guarantees the geometry latched by the
     * final recalc below uses 8 dots/clock no matter what the kernel egatab or
     * an intervening SR07/SR0F write left behind.
     */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x01, 0x01);
    /*
     * Re-write SR07 last so the chip re-evaluates the memory map and display
     * timings with every register now in its final state (an SR07 write forces
     * that re-evaluation; CR1B and GR0B do not).  This keeps the A0000 banked
     * window mapped now that GR0B's granularity bit is set.
     */
    SVR4WSWriteVGARegister(0x3c4, 0x3c5, 0x07, CIRRUS_SR07_16BPP_BANKED);

    /*
     * ISABugger diagnostics: read back the registers that actually drive the
     * pixel format so we can see what stuck.  Stage tag is on the red LEDs.
     *   1: CR27 chip id   (expect 0x9c on a GD5429)
     *   2: SR07 readback  (expect 0x07: SVGA + 16bpp, banked window kept)
     *   3: CR1B readback  (expect 0x22: full vram mask + ext blanking)
     *   4: hidden DAC readback (expect 0xc1: HiColor 565)
     */
#ifdef SVR4WS_DIAGNOSTICS
    SVR4WSBuggerReport(1, SVR4WSReadVGARegister(0x3d4, 0x3d5, CIRRUS_CRTC_CHIPID));
    SVR4WSBuggerReport(2, SVR4WSReadVGARegister(0x3c4, 0x3c5, 0x07));
    SVR4WSBuggerReport(3, SVR4WSReadVGARegister(0x3d4, 0x3d5, CIRRUS_CRTC_EXT));
    {
        unsigned char dac;
        (void)SVR4WSReadPort8(0x3c6);
        (void)SVR4WSReadPort8(0x3c6);
        (void)SVR4WSReadPort8(0x3c6);
        dac = SVR4WSReadPort8(0x3c6);
        SVR4WSBuggerReport(4, dac);
    }
    /*
     * Geometry inputs 86Box feeds into its hdisp/dispend/linedbl heuristic.
     * If the staircase+duplication persists, these reveal which one is wrong:
     *   8: SR01 (expect 0x01: bit 3 clear -> 8 dots/clock -> hdisp = 800)
     *   9: CR01 horizontal display end (expect 0x63 -> (0x63+1)*8 = 800)
     *  10: CR07 overflow   (expect 0xf0: bit 6 set -> dispend |= 0x200 -> 600)
     *  11: CR12 vertical display end low byte (expect 0x57 -> 87, +512+1 = 600)
     */
    SVR4WSBuggerReport(8,  SVR4WSReadVGARegister(0x3c4, 0x3c5, 0x01));
    SVR4WSBuggerReport(9,  SVR4WSReadVGARegister(0x3d4, 0x3d5, 0x01));
    SVR4WSBuggerReport(10, SVR4WSReadVGARegister(0x3d4, 0x3d5, 0x07));
    SVR4WSBuggerReport(11, SVR4WSReadVGARegister(0x3d4, 0x3d5, 0x12));
    /*
     * Pitch (CR13/CR1B) and interlace (CR1A) — the scanout stride.  86Box uses
     * rowoffset = CR13 | ((CR1B & 0x10) << 4), then displayed stride =
     * rowoffset << 3.  For 800x600x16 we need rowoffset = 200 (0xc8) ->
     * stride 1600.  A wrong CR13 here is a pure horizontal shear.
     *  12: CR13 offset/pitch  (expect 0xc8 = 200)
     *  13: CR1A               (expect 0x00: bit 0 clear -> not interlaced)
     */
    SVR4WSBuggerReport(12, SVR4WSReadVGARegister(0x3d4, 0x3d5, 0x13));
    SVR4WSBuggerReport(13, SVR4WSReadVGARegister(0x3d4, 0x3d5, 0x1a));
    /*
     * 14: CR17 (expect 0xe3: bit 6 set -> byte addressing -> linear scanout).
     * If this reads back without bit 6, 86Box selects a word/dword address
     * remap and the framebuffer shears.
     */
    SVR4WSBuggerReport(14, SVR4WSReadVGARegister(0x3d4, 0x3d5, 0x17));
#endif

    if (ioctl(fPtr->kd_fd, KDSETMODE, KD_GRAPHICS) < 0) {
        xf86DrvMsg(pScrn->scrnIndex, X_ERROR, "KDSETMODE KD_GRAPHICS failed: %s\n", strerror(errno));
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
    if (!SVR4WSMapDisplay(pScrn)) {
        SVR4WSRestoreTextMode(pScrn);
        return FALSE;
    }
#ifdef SVR4WS_DIAGNOSTICS
    /* Stage 5/6: low/high byte of the kernel-reported aperture size. */
    SVR4WSBuggerReport(5, (unsigned char)(fPtr->framebuffer_size & 0xff));
    SVR4WSBuggerReport(6, (unsigned char)((fPtr->framebuffer_size >> 8) & 0xff));
#endif

#ifdef SVR4WS_TESTPATTERN
    /*
     * Diagnostic test pattern, written straight into VRAM through the same
     * banked path RefreshArea uses, bypassing X/ShadowFB entirely.  It is a
     * horizontal-scale probe: each row is split into quarters by x, coloured
     *   x  0-199 : red      x 200-399 : green
     *   x 400-599: blue     x 600-799 : white
     * If the horizontal scale is 1:1 we see four equal vertical stripes
     * red|green|blue|white across the full width.  If the image tiles 2x we
     * see those four stripes twice (eight stripes); the pattern thus reads off
     * the exact horizontal scale factor directly.
     */
    if (fPtr->framebuffer) {
        static const unsigned short quarters[4] = { 0xf800, 0x07e0, 0x001f, 0xffff };
        int current_bank = -1;
        int row;

        for (row = 0; row < SVR4WS_HEIGHT; ++row) {
            size_t offset = (size_t)row * (size_t)fPtr->framebuffer_stride;
            int col;

            for (col = 0; col < SVR4WS_WIDTH; ++col, offset += SVR4WS_BYTES_PER_PIXEL) {
                unsigned short colour = quarters[(col * 4) / SVR4WS_WIDTH];
                size_t bank_base = offset & ~((size_t)SVR4WS_BANK_APERTURE - 1U);
                int bank = (int)(bank_base / SVR4WS_BANK_GRANULARITY);
                size_t bank_offset = offset - bank_base;

                if (bank != current_bank) {
                    SVR4WSSetCirrusBank(bank);
                    current_bank = bank;
                }
                *(volatile unsigned short *)(fPtr->framebuffer + bank_offset) = colour;
            }
        }
    }
#endif

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
