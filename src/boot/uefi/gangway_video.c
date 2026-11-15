/*
 * Gangway — the screen: the GOP that is the operator's display, a mode chosen
 * by the monitor's own preferred timing (EDID) where it can be had, and the
 * framebuffer the kernel is handed, said in full because a wrong one shows as
 * inverted colours or diagonal text with nothing on screen to explain it.
 */

#include "gangway.h"

typedef struct {
    EFI_HANDLE handle;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
} GopSelection;

typedef struct {
    uint32_t width;
    uint32_t height;
    int      valid;
    int      native;
} EdidPreferredTiming;

typedef enum {
    GopChoiceCurrent,
    GopChoiceEdid,
    GopChoiceLargest,
    GopChoiceSmallest,
    GopChoiceEdidTooLarge,
    GopChoiceEdidUnavailable,
    GopChoiceUnavailable
} GopModeChoice;

typedef struct {
    uint32_t selected_mode;
    uint32_t largest_mode;
    uint32_t smallest_mode;
    uint32_t aspect_mode;
    uint64_t largest_area;
    uint64_t smallest_area;
    uint64_t aspect_area;
    uint32_t invalid_modes;
    int      found_selected;
    int      selected_matches_format;
    int      found_largest;
    int      found_smallest;
    int      found_aspect;
} GopModeCandidates;

/*
 * ── what one pixel of a GOP mode is, in the only two orders this loader can
 *    tell the kernel about
 *
 * UEFI 2.10 §12.9.2: PixelFormat says how a pixel is laid out, and only the
 * two 8-bit-per-colour formats are a fixed four bytes. PixelBitMask means
 * "read PixelInformation" — "This bit-mask is only valid if PixelFormat is
 * set to PixelPixelBitMask. A bit being set defines what bits are used for
 * what purpose such as Red, Green, Blue, or Reserved." PixelBltOnly means
 * there is no linear framebuffer at all, and anything at or past
 * PixelFormatMax is not a format.
 *
 * The kernel's framebuffer backend writes uint32_t pixels, so this contract
 * intentionally accepts only the two 32-bit layouts it can encode. Other
 * valid GOP modes remain usable by firmware, but are refused here rather than
 * published with a false stride or pixel format.
 */
static int GopDescribePixels(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             uint32_t *out_format, uint32_t *out_pixel_bytes)
{
    if (!info) return 0;

    switch (info->pixel_format) {
        case PixelRedGreenBlueReserved8BitPerColor:
            *out_format = 0; *out_pixel_bytes = 4; return 1;
        case PixelBlueGreenRedReserved8BitPerColor:
            *out_format = 1; *out_pixel_bytes = 4; return 1;
        case PixelBitMask:
            break;
        default:
            return 0;   /* PixelBltOnly, and anything >= PixelFormatMax */
    }

    /* Masks are over the 32-bit pixel value; written little-endian, bits
     * 0..7 are byte 0. The reserved byte is required because the kernel
     * stores one complete uint32_t for every pixel. */
    const EFI_PIXEL_BITMASK *m = &info->pixel_info;
    if (m->green_mask != 0x0000FF00U || m->reserved_mask != 0xFF000000U)
        return 0;
    if (m->red_mask == 0x00FF0000U && m->blue_mask == 0x000000FFU) {
        *out_format = 1; *out_pixel_bytes = 4; return 1;   /* B,G,R,X in memory */
    }
    if (m->red_mask == 0x000000FFU && m->blue_mask == 0x00FF0000U) {
        *out_format = 0; *out_pixel_bytes = 4; return 1;   /* R,G,B,X in memory */
    }
    return 0;
}

static int GopModeInfoUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             uint32_t *out_format, uint32_t *out_pixel_bytes);

/* EFI_CONSOLE_OUT_DEVICE_GUID — UEFI 2.10 §12.3, the tag the firmware puts on
 * the handle its own console output goes to. Defined here rather than in
 * uefi.h because this is the only question in this loader that needs it. */
#define EFI_CONSOLE_OUT_DEVICE_GUID \
    EFI_GUID_INIT(0xd3b36f2c,0xd551,0x11d4, 0x9a,0x46,0x00,0x90,0x27,0x3f,0xc1,0x4d)

/* Has this adapter ANY mode whose pixels this loader can hand over? The
 * current one first, because that is the one it will usually keep. */
static int GopHasUsableMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop)
{
    if (!gop || !gop->mode) return 0;

    uint32_t fmt = 0, pixel_bytes = 0;
    if (gop->mode->info && GopModeInfoUsable(gop->mode->info, &fmt, &pixel_bytes))
        return 1;

    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->query_mode(gop, m, &size_of_info, &info))) continue;
        if (!info) continue;
        /* QueryMode's buffer is the caller's to release (UEFI 2.10 §12.9.2.1).
         * Before ExitBootServices every pool block left behind is one more
         * descriptor in the map this loader is about to convert. */
        int usable = GopModeInfoUsable(info, &fmt, &pixel_bytes);
        g_bs->free_pool(info);
        if (usable) return 1;
    }
    return 0;
}

/*
 * ── which GOP, on a machine that has more than one
 *
 * locate_protocol returns ONE handle and the firmware picks which. On
 * EDK2-derived firmware the ConSplitter installs a virtual GOP on the console
 * handle by default (PcdConOutGopSupport), and every mode of that GOP is
 * PixelBltOnly with FrameBufferBase == 0 in two states that exist on shipping
 * hardware: no physical GOP connected, and two or more of them — a server with
 * BMC video beside an add-in card, a desktop with iGPU and dGPU both attached.
 * Taking that handle means refusing a framebuffer the machine really has and
 * bringing the kernel up on VGA text, which on a UEFI class-3 board is a black
 * screen with nothing said.
 *
 * So every handle is asked, one with no describable mode at all is passed
 * over, and one that also carries EFI_CONSOLE_OUT_DEVICE — the device the
 * firmware's own console writes to, and therefore the display in front of the
 * operator — wins outright. This keeps the output identity explicit instead
 * of trusting whichever GOP a firmware happens to return first.
 */
static int GopFindPrimary(GopSelection *out)
{
    if (!out) return 0;
    out->handle = NULL;
    out->gop = NULL;

    EFI_GUID gop_guid    = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GUID conout_guid = EFI_CONSOLE_OUT_DEVICE_GUID;

    UINTN       handle_count = 0;
    EFI_HANDLE *handles      = NULL;
    EFI_STATUS  status = g_bs->locate_handle_buffer(EFI_BY_PROTOCOL, &gop_guid,
                                                    NULL, &handle_count, &handles);
    if (EFI_ERROR(status) || !handles || handle_count == 0) {
        /* The firmware will not enumerate its GOPs. One question is left. */
        EFI_GRAPHICS_OUTPUT_PROTOCOL *only = NULL;
        if (EFI_ERROR(g_bs->locate_protocol(&gop_guid, NULL, (void **)&only)))
            return 0;
        out->gop = only;
        return 1;
    }

    EFI_HANDLE first_handle = NULL;
    EFI_HANDLE conout_handle = NULL;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *first  = NULL;   /* first usable adapter */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *conout = NULL;   /* ...that is the console */

    for (UINTN i = 0; i < handle_count && !conout; i++) {
        EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = NULL;
        if (EFI_ERROR(g_bs->handle_protocol(handles[i], &gop_guid, (void **)&gop)))
            continue;
        if (!GopHasUsableMode(gop)) continue;

        void *console = NULL;
        if (!EFI_ERROR(g_bs->handle_protocol(handles[i], &conout_guid, &console))) {
            conout = gop;
            conout_handle = handles[i];
        } else if (!first) {
            first = gop;
            first_handle = handles[i];
        }
    }

    g_bs->free_pool(handles);

    if (conout) {
        out->handle = conout_handle;
        out->gop = conout;
        return 1;
    }
    if (first) {
        out->handle = first_handle;
        out->gop = first;
        return 1;
    }

    /* Not one adapter here has a mode this loader can hand over. Answer with
     * whatever locate_protocol names anyway, so QueryGopFramebuffer prints
     * WHICH format it is refusing — that line is the whole diagnosis on a
     * board that comes up black, and it is worth more than a bare "none". */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *any = NULL;
    if (EFI_ERROR(g_bs->locate_protocol(&gop_guid, NULL, (void **)&any)))
        return 0;
    out->gop = any;
    return 1;
}

#define MIN_FB_WIDTH   640U
#define MIN_FB_HEIGHT  400U
#define MAX_FB_WIDTH   3840U
#define MAX_FB_HEIGHT  2160U

static int FbModeInBand(uint32_t w, uint32_t h)
{
    return (w >= MIN_FB_WIDTH  && w <= MAX_FB_WIDTH  &&
            h >= MIN_FB_HEIGHT && h <= MAX_FB_HEIGHT);
}

static int GopModeInfoUsable(const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info,
                             uint32_t *out_format, uint32_t *out_pixel_bytes)
{
    if (!info || !GopDescribePixels(info, out_format, out_pixel_bytes)) return 0;
    if (info->horizontal_resolution == 0 || info->vertical_resolution == 0 ||
        info->pixels_per_scan_line < info->horizontal_resolution)
        return 0;
    return 1;
}

static uint64_t GopModeArea(uint32_t width, uint32_t height)
{
    return (uint64_t)width * (uint64_t)height;
}

static int GopModeMatchesAspect(uint32_t width, uint32_t height,
                                const EdidPreferredTiming *preferred)
{
    if (!preferred || !preferred->valid || width == 0 || height == 0) return 0;
    return (uint64_t)width * preferred->height ==
           (uint64_t)height * preferred->width;
}

static int ParseEdidPreferredTiming(const uint8_t *edid, uint32_t size,
                                    EdidPreferredTiming *out)
{
    if (!out) return 0;
    MemZero(out, sizeof(*out));
    if (!edid || size < 128) return 0;

    static const uint8_t header[8] = { 0x00, 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0x00 };
    if (!MemEqual(edid, header, sizeof(header))) return 0;

    uint32_t checksum = 0;
    for (uint32_t i = 0; i < 128; i++) checksum += edid[i];
    if ((checksum & 0xFFU) != 0) return 0;
    if (edid[18] != 1 || edid[19] < 3) return 0;

    /* The first detailed timing descriptor is at byte 54. A zero pixel clock
     * denotes a monitor descriptor rather than a video timing. */
    uint16_t pixel_clock = (uint16_t)edid[54] | ((uint16_t)edid[55] << 8);
    if (pixel_clock == 0) return 0;

    uint32_t width = (uint32_t)edid[56] |
                     ((uint32_t)(edid[58] & 0xF0U) << 4);
    uint32_t height = (uint32_t)edid[59] |
                      ((uint32_t)(edid[61] & 0xF0U) << 4);
    if (!width || !height) return 0;

    out->width = width;
    out->height = height;
    out->valid = 1;
    /* EDID 1.4 bit 1 says that the preferred timing includes native format
     * and refresh. Earlier revisions still define the first timing as the
     * preferred timing, but do not carry that native-confidence bit. */
    out->native = (edid[19] >= 4) ? ((edid[24] & 0x02U) != 0) : 1;
    return 1;
}

static int GopReadPreferredTiming(EFI_HANDLE handle, EdidPreferredTiming *out)
{
    if (!out) return 0;
    MemZero(out, sizeof(*out));
    if (!handle) return 0;

    EFI_GUID active_guid = EFI_EDID_ACTIVE_PROTOCOL_GUID;
    EFI_GUID discovered_guid = EFI_EDID_DISCOVERED_PROTOCOL_GUID;
    EFI_EDID_PROTOCOL *protocol = NULL;

    if (!EFI_ERROR(g_bs->handle_protocol(handle, &active_guid,
                                         (void **)&protocol)) && protocol &&
        ParseEdidPreferredTiming(protocol->edid, protocol->size_of_edid, out))
        return 1;

    protocol = NULL;
    if (!EFI_ERROR(g_bs->handle_protocol(handle, &discovered_guid,
                                         (void **)&protocol)) && protocol &&
        ParseEdidPreferredTiming(protocol->edid, protocol->size_of_edid, out))
        return 1;

    MemZero(out, sizeof(*out));
    return 0;
}

static const char *GopChoiceName(GopModeChoice choice)
{
    switch (choice) {
        case GopChoiceCurrent:    return "current-fallback";
        case GopChoiceEdid:       return "edid";
        case GopChoiceLargest:    return "largest-fallback";
        case GopChoiceSmallest:   return "smallest-fallback";
        case GopChoiceEdidTooLarge:return "edid-preferred-too-large";
        case GopChoiceEdidUnavailable:return "edid-preferred-unavailable";
        case GopChoiceUnavailable:return "unavailable";
        default:                  return "unknown";
    }
}

static void GopScanModes(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                         const EdidPreferredTiming *preferred,
                         uint32_t current_format,
                         GopModeCandidates *out)
{
    MemZero(out, sizeof(*out));
    out->selected_mode = gop->mode->mode;
    out->smallest_area = ~0ULL;

    for (uint32_t m = 0; m < gop->mode->max_mode; m++) {
        UINTN size_of_info = 0;
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = NULL;
        if (EFI_ERROR(gop->query_mode(gop, m, &size_of_info, &info)) || !info) {
            out->invalid_modes++;
            continue;
        }

        uint32_t format = 0, bytes = 0;
        if (!GopModeInfoUsable(info, &format, &bytes)) {
            out->invalid_modes++;
            g_bs->free_pool(info);
            continue;
        }

        uint32_t width = info->horizontal_resolution;
        uint32_t height = info->vertical_resolution;
        uint64_t area = GopModeArea(width, height);

        if (preferred && preferred->valid &&
            width == preferred->width && height == preferred->height &&
            FbModeInBand(width, height)) {
            int matches_format = format == current_format;
            if (!out->found_selected ||
                (matches_format && !out->selected_matches_format)) {
                out->selected_mode = m;
                out->found_selected = 1;
                out->selected_matches_format = matches_format;
            }
        }

        if (area < out->smallest_area) {
            out->smallest_area = area;
            out->smallest_mode = m;
            out->found_smallest = 1;
        }
        if (FbModeInBand(width, height)) {
            if (!out->found_largest || area > out->largest_area) {
                out->largest_area = area;
                out->largest_mode = m;
                out->found_largest = 1;
            }
            if (GopModeMatchesAspect(width, height, preferred) &&
                (!out->found_aspect || area > out->aspect_area)) {
                out->aspect_area = area;
                out->aspect_mode = m;
                out->found_aspect = 1;
            }
        }
        g_bs->free_pool(info);
    }
}

static GopModeChoice SelectGopBestMode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop,
                                       const EdidPreferredTiming *preferred,
                                       uint32_t *out_invalid_modes)
{
    if (out_invalid_modes) *out_invalid_modes = 0;
    if (!gop || !gop->mode) return GopChoiceUnavailable;

    uint32_t current_format = 0, current_bytes = 0;
    int current_usable = GopModeInfoUsable(gop->mode->info,
                                           &current_format, &current_bytes);
    GopModeCandidates candidates;
    GopScanModes(gop, preferred, current_format, &candidates);
    if (out_invalid_modes) *out_invalid_modes = candidates.invalid_modes;

    if (candidates.found_selected) {
        if (candidates.selected_mode == gop->mode->mode) return GopChoiceEdid;
        EFI_STATUS status = gop->set_mode(gop, candidates.selected_mode);
        int verified = !EFI_ERROR(status) && gop->mode &&
                       gop->mode->mode == candidates.selected_mode &&
                       GopModeInfoUsable(gop->mode->info, &current_format,
                                         &current_bytes);
        if (verified)
            return GopChoiceEdid;

        Print("Gangway: GOP EDID mode change ");
        Print(EFI_ERROR(status) ? "failed status=" : "verification failed status=");
        PrintHex64((uint64_t)status);
        Print(" — keeping the firmware mode\r\n");
        return current_usable ? GopChoiceCurrent : GopChoiceUnavailable;
    }

    if (!preferred || !preferred->valid) {
        if (current_usable && FbModeInBand(gop->mode->info->horizontal_resolution,
                                           gop->mode->info->vertical_resolution))
            return GopChoiceCurrent;
    }

    uint32_t fallback_mode;
    GopModeChoice choice;
    if (candidates.found_aspect) {
        fallback_mode = candidates.aspect_mode;
        choice = GopChoiceLargest;
    } else if (candidates.found_largest) {
        fallback_mode = candidates.largest_mode;
        choice = GopChoiceLargest;
    } else if (candidates.found_smallest) {
        fallback_mode = candidates.smallest_mode;
        choice = (preferred && preferred->valid)
                   ? GopChoiceEdidUnavailable : GopChoiceSmallest;
    } else {
        return GopChoiceUnavailable;
    }

    if (preferred && preferred->valid) {
        if (!FbModeInBand(preferred->width, preferred->height))
            choice = GopChoiceEdidTooLarge;
        else if (choice == GopChoiceLargest || choice == GopChoiceSmallest)
            choice = GopChoiceEdidUnavailable;
    }

    if (fallback_mode == gop->mode->mode) return current_usable ? choice : GopChoiceUnavailable;
    EFI_STATUS status = gop->set_mode(gop, fallback_mode);
    int verified = !EFI_ERROR(status) && gop->mode &&
                  gop->mode->mode == fallback_mode &&
                  GopModeInfoUsable(gop->mode->info, &current_format,
                                    &current_bytes);
    if (!verified) {
        Print("Gangway: GOP fallback mode change ");
        Print(EFI_ERROR(status) ? "failed status=" : "verification failed status=");
        PrintHex64((uint64_t)status);
        Print(" — keeping the firmware mode\r\n");
        return current_usable ? GopChoiceCurrent : GopChoiceUnavailable;
    }
    return choice;
}
FbInfo QueryGopFramebuffer(void)
{
    FbInfo fb;
    MemZero(&fb, sizeof(fb));

    GopSelection selection;
    if (!GopFindPrimary(&selection) || !selection.gop || !selection.gop->mode) {
        Print("Gangway: no display adapter here offers a linear framebuffer — "
              "the kernel comes up on text\r\n");
        return fb;
    }

    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = selection.gop;
    uint32_t current_mode = gop->mode->mode;
    EdidPreferredTiming preferred;
    int have_edid = GopReadPreferredTiming(selection.handle, &preferred);
    uint32_t invalid_modes = 0;
    GopModeChoice choice = SelectGopBestMode(gop, have_edid ? &preferred : NULL,
                                             &invalid_modes);

    Print("Gangway: GOP modes=");
    PrintDec(gop->mode->max_mode);
    Print(" current=");
    PrintDec(current_mode);
    Print(" selected=");
    PrintDec(gop->mode->mode);
    Print(" edid=");
    if (have_edid) {
        PrintDec(preferred.width);
        Print("x");
        PrintDec(preferred.height);
        Print(preferred.native ? " native" : " preferred");
    } else {
        Print("none");
    }
    Print(" choice=");
    Print(GopChoiceName(choice));
    Print(" invalid=");
    PrintDec(invalid_modes);
    Print("\r\n");

    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *mode = gop->mode;
    if (!mode->info) return fb;

    uint32_t format      = 0;
    uint32_t pixel_bytes = 0;
    if (!GopModeInfoUsable(mode->info, &format, &pixel_bytes)) {
        Print("Gangway: GOP pixel format ");
        PrintDec((uint64_t)mode->info->pixel_format);
        Print(" (masks r=");
        PrintHex64(mode->info->pixel_info.red_mask);
        Print(" g=");
        PrintHex64(mode->info->pixel_info.green_mask);
        Print(" b=");
        PrintHex64(mode->info->pixel_info.blue_mask);
        Print(" x=");
        PrintHex64(mode->info->pixel_info.reserved_mask);
        Print(") is not a layout this loader can hand over — no framebuffer, "
              "the kernel comes up on text\r\n");
        return fb;
    }

    uint64_t stride = (uint64_t)mode->info->pixels_per_scan_line * pixel_bytes;
    if (stride > 0xFFFFFFFFULL ||
        stride > (~0ULL / (uint64_t)mode->info->vertical_resolution)) {
        Print("Gangway: GOP scan-line geometry overflows the framebuffer contract — "
              "the kernel comes up on text\r\n");
        return fb;
    }
    uint64_t span = stride * (uint64_t)mode->info->vertical_resolution;

    if (mode->frame_buffer_base == 0 || span == 0) {
        Print("Gangway: GOP reports no framebuffer for its own mode — "
              "the kernel comes up on text\r\n");
        return fb;
    }

    /*
     * UEFI 2.10 §12.9.2 defines FrameBufferSize as PixelsPerScanLine ×
     * VerticalResolution × PixelElementSize — the one number the firmware
     * publishes that can catch a pixel size this loader derived wrongly.
     *
     * A non-zero FrameBufferSize is authoritative for every pixel format.
     * Zero is permitted by older firmware, so it cannot be used as a reason
     * to reject an otherwise valid mode.
     */
    if (mode->frame_buffer_size != 0 &&
        span > (uint64_t)mode->frame_buffer_size) {
        Print("Gangway: GOP's aperture is smaller than the scan lines it "
              "describes — refusing the framebuffer\r\n");
        return fb;
    }
    if (mode->frame_buffer_base > (~0ULL - span)) {
        Print("Gangway: GOP framebuffer address overflows its visible span — "
              "the kernel comes up on text\r\n");
        return fb;
    }

    fb.addr   = mode->frame_buffer_base;
    fb.width  = mode->info->horizontal_resolution;
    fb.height = mode->info->vertical_resolution;
    fb.stride = (uint32_t)stride;
    fb.format = format;

    /* The order and the scan line go on the line as well. They are what this
     * loader decided rather than read, and a wrong one shows as inverted
     * colours or diagonal text with nothing else on screen to explain it. */
    Print("Gangway: GOP ");
    PrintDec(fb.width);
    Print("x");
    PrintDec(fb.height);
    Print(" fb=");
    PrintHex64(fb.addr);
    Print(" stride=");
    PrintDec(fb.stride);
    Print(fb.format == 0 ? " order=RGBX\r\n" : " order=BGRX\r\n");

    return fb;
}
