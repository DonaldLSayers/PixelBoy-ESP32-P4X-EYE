/* USB descriptors for PIXELBOY's composite device: Mass Storage (photos,
 * see app_usb.c) + a USB Video Class (UVC) "webcam" interface, both always
 * enumerated - which one actually does anything is picked on-device (see
 * usb_webcam_accept()/usb_msc_accept() in app_usb.c), not by re-enumerating
 * with a different descriptor (TinyUSB's descriptor set is fixed once
 * tinyusb_driver_install() runs, so there's no cheap way to swap it live).
 *
 * There's no high-level "just give me a webcam" helper in esp_tinyusb like
 * there is for MSC (tinyusb_msc_storage_init_*) - UVC needs a hand-built
 * descriptor, same as TinyUSB's own examples/device/video_capture. This
 * follows that example's struct layout, with an MSC interface prepended and
 * the isochronous/uncompressed-YUY2 paths dropped (bulk + MJPEG only - bulk
 * avoids needing an alternate-setting interface, MJPEG keeps frames small
 * enough for Full Speed bulk to carry at a usable frame rate).
 *
 * "Mirror mode": streams whatever's on the device's own 240x240 screen, not
 * a camera-only capture path - see app_usb.h's WEBCAM_FRAME_W/H comment. */
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "tusb.h"

#include "app_usb.h"
#include "app_usb_video_desc.h"

#define USB_VID 0x303A /* Espressif's - matches the board's existing debug-port VID so it's a known-good/signed value */
#define USB_PID 0x8123 /* arbitrary, distinct from Espressif's other demo PIDs */
#define UVC_CLOCK_FREQUENCY 27000000

enum { ITF_MSC, ITF_VIDEO_CONTROL, ITF_VIDEO_STREAMING, ITF_TOTAL };

#define EP_MSC_OUT   0x01
#define EP_MSC_IN    0x81
#define EP_VIDEO_IN  0x82

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_MSC,
    STRID_VIDEO_CONTROL,
    STRID_VIDEO_STREAMING,
    STRID_COUNT
};

static char const *s_strings[STRID_COUNT] = {
    (const char[]){0x09, 0x04},
    "PIXELBOY",
    "PIXELBOY",
    NULL, /* unique chip ID, filled at runtime - see usb_video_desc_init() */
    "Photos",
    "Webcam Control",
    "Webcam Stream",
};

static const tusb_desc_device_t s_device_desc = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    /* IAD required: video's a multi-interface function, MISC/IAD is what
     * lets a composite device with it still enumerate as one device. */
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

typedef struct TU_ATTR_PACKED {
    tusb_desc_interface_t itf;
    tusb_desc_endpoint_t ep_out;
    tusb_desc_endpoint_t ep_in;
} msc_desc_t;

typedef struct TU_ATTR_PACKED {
    tusb_desc_interface_t itf;
    tusb_desc_video_control_header_1itf_t header;
    tusb_desc_video_control_camera_terminal_t camera_terminal;
    tusb_desc_video_control_output_terminal_t output_terminal;
} video_control_desc_t;

typedef struct TU_ATTR_PACKED {
    tusb_desc_interface_t itf;
    tusb_desc_video_streaming_input_header_1byte_t header;
    tusb_desc_video_format_mjpeg_t format;
    tusb_desc_video_frame_mjpeg_continuous_t frame;
    tusb_desc_video_streaming_color_matching_t color;
    tusb_desc_endpoint_t ep;
} video_streaming_desc_t;

typedef struct TU_ATTR_PACKED {
    tusb_desc_configuration_t config;
    msc_desc_t msc;
    tusb_desc_interface_assoc_t iad;
    video_control_desc_t video_control;
    video_streaming_desc_t video_streaming;
} cfg_desc_t;

#define UVC_ENTITY_CAMERA 0x01
#define UVC_ENTITY_OUTPUT 0x02

static const cfg_desc_t s_cfg_desc = {
    .config = {
        .bLength = sizeof(tusb_desc_configuration_t),
        .bDescriptorType = TUSB_DESC_CONFIGURATION,
        .wTotalLength = sizeof(cfg_desc_t),
        .bNumInterfaces = ITF_TOTAL,
        .bConfigurationValue = 1,
        .iConfiguration = 0,
        .bmAttributes = TU_BIT(7),
        .bMaxPower = 250, /* 500mA - a live JPEG encode + USB TX loop is more current-hungry than MSC's idle drive */
    },
    .msc = {
        .itf = {
            .bLength = sizeof(tusb_desc_interface_t), .bDescriptorType = TUSB_DESC_INTERFACE,
            .bInterfaceNumber = ITF_MSC, .bAlternateSetting = 0, .bNumEndpoints = 2,
            .bInterfaceClass = TUSB_CLASS_MSC, .bInterfaceSubClass = MSC_SUBCLASS_SCSI,
            .bInterfaceProtocol = MSC_PROTOCOL_BOT, .iInterface = STRID_MSC,
        },
        .ep_out = {
            .bLength = sizeof(tusb_desc_endpoint_t), .bDescriptorType = TUSB_DESC_ENDPOINT,
            .bEndpointAddress = EP_MSC_OUT, .bmAttributes = {.xfer = TUSB_XFER_BULK},
            .wMaxPacketSize = 64, .bInterval = 0,
        },
        .ep_in = {
            .bLength = sizeof(tusb_desc_endpoint_t), .bDescriptorType = TUSB_DESC_ENDPOINT,
            .bEndpointAddress = EP_MSC_IN, .bmAttributes = {.xfer = TUSB_XFER_BULK},
            .wMaxPacketSize = 64, .bInterval = 0,
        },
    },
    .iad = {
        .bLength = sizeof(tusb_desc_interface_assoc_t), .bDescriptorType = TUSB_DESC_INTERFACE_ASSOCIATION,
        .bFirstInterface = ITF_VIDEO_CONTROL, .bInterfaceCount = 2,
        .bFunctionClass = TUSB_CLASS_VIDEO, .bFunctionSubClass = VIDEO_SUBCLASS_INTERFACE_COLLECTION,
        .bFunctionProtocol = VIDEO_ITF_PROTOCOL_UNDEFINED, .iFunction = 0,
    },
    .video_control = {
        .itf = {
            .bLength = sizeof(tusb_desc_interface_t), .bDescriptorType = TUSB_DESC_INTERFACE,
            .bInterfaceNumber = ITF_VIDEO_CONTROL, .bAlternateSetting = 0, .bNumEndpoints = 0,
            .bInterfaceClass = TUSB_CLASS_VIDEO, .bInterfaceSubClass = VIDEO_SUBCLASS_CONTROL,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15, .iInterface = STRID_VIDEO_CONTROL,
        },
        .header = {
            .bLength = sizeof(tusb_desc_video_control_header_1itf_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_HEADER, .bcdUVC = VIDEO_BCD_1_50,
            .wTotalLength = sizeof(video_control_desc_t) - sizeof(tusb_desc_interface_t),
            .dwClockFrequency = UVC_CLOCK_FREQUENCY, .bInCollection = 1,
            .baInterfaceNr = {ITF_VIDEO_STREAMING},
        },
        .camera_terminal = {
            .bLength = sizeof(tusb_desc_video_control_camera_terminal_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_INPUT_TERMINAL, .bTerminalID = UVC_ENTITY_CAMERA,
            .wTerminalType = VIDEO_ITT_CAMERA, .bAssocTerminal = 0, .iTerminal = 0,
            .wObjectiveFocalLengthMin = 0, .wObjectiveFocalLengthMax = 0, .wOcularFocalLength = 0,
            .bControlSize = 3, .bmControls = {0, 0, 0},
        },
        .output_terminal = {
            .bLength = sizeof(tusb_desc_video_control_output_terminal_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VC_OUTPUT_TERMINAL, .bTerminalID = UVC_ENTITY_OUTPUT,
            .wTerminalType = VIDEO_TT_STREAMING, .bAssocTerminal = 0, .bSourceID = UVC_ENTITY_CAMERA, .iTerminal = 0,
        },
    },
    .video_streaming = {
        .itf = {
            .bLength = sizeof(tusb_desc_interface_t), .bDescriptorType = TUSB_DESC_INTERFACE,
            .bInterfaceNumber = ITF_VIDEO_STREAMING, .bAlternateSetting = 0, .bNumEndpoints = 1, /* bulk: no alt-setting needed */
            .bInterfaceClass = TUSB_CLASS_VIDEO, .bInterfaceSubClass = VIDEO_SUBCLASS_STREAMING,
            .bInterfaceProtocol = VIDEO_ITF_PROTOCOL_15, .iInterface = STRID_VIDEO_STREAMING,
        },
        .header = {
            .bLength = sizeof(tusb_desc_video_streaming_input_header_1byte_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_INPUT_HEADER, .bNumFormats = 1,
            .wTotalLength = sizeof(video_streaming_desc_t) - sizeof(tusb_desc_interface_t) - sizeof(tusb_desc_endpoint_t),
            .bEndpointAddress = EP_VIDEO_IN, .bmInfo = 0, .bTerminalLink = UVC_ENTITY_OUTPUT,
            .bStillCaptureMethod = 0, .bTriggerSupport = 0, .bTriggerUsage = 0,
            .bControlSize = 1, .bmaControls = {0},
        },
        .format = {
            .bLength = sizeof(tusb_desc_video_format_mjpeg_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_FORMAT_MJPEG, .bFormatIndex = 1, .bNumFrameDescriptors = 1,
            .bmFlags = 0, .bDefaultFrameIndex = 1, .bAspectRatioX = 0, .bAspectRatioY = 0,
            .bmInterlaceFlags = 0, .bCopyProtect = 0,
        },
        .frame = {
            .bLength = sizeof(tusb_desc_video_frame_mjpeg_continuous_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_FRAME_MJPEG, .bFrameIndex = 1, .bmCapabilities = 0,
            .wWidth = WEBCAM_FRAME_W, .wHeight = WEBCAM_FRAME_H,
            .dwMinBitRate = WEBCAM_FRAME_W * WEBCAM_FRAME_H * 16 * 1,
            .dwMaxBitRate = WEBCAM_FRAME_W * WEBCAM_FRAME_H * 16 * WEBCAM_FRAME_RATE,
            .dwMaxVideoFrameBufferSize = WEBCAM_FRAME_W * WEBCAM_FRAME_H * 2, /* generous MJPEG upper bound, see WEBCAM_JPEG_MAX_BYTES */
            .dwDefaultFrameInterval = 10000000 / WEBCAM_FRAME_RATE, .bFrameIntervalType = 0,
            .dwFrameInterval = {10000000 / WEBCAM_FRAME_RATE, 10000000, 10000000 / WEBCAM_FRAME_RATE},
        },
        .color = {
            .bLength = sizeof(tusb_desc_video_streaming_color_matching_t), .bDescriptorType = TUSB_DESC_CS_INTERFACE,
            .bDescriptorSubType = VIDEO_CS_ITF_VS_COLORFORMAT,
            .bColorPrimaries = VIDEO_COLOR_PRIMARIES_BT709, .bTransferCharacteristics = VIDEO_COLOR_XFER_CH_BT709,
            .bMatrixCoefficients = VIDEO_COLOR_COEF_SMPTE170M,
        },
        .ep = {
            .bLength = sizeof(tusb_desc_endpoint_t), .bDescriptorType = TUSB_DESC_ENDPOINT,
            .bEndpointAddress = EP_VIDEO_IN, .bmAttributes = {.xfer = TUSB_XFER_BULK},
            .wMaxPacketSize = 64, /* Full Speed bulk cap - see desc_hs_cfg for the High Speed copy */
            .bInterval = 0,
        },
    },
};

/* The P4-EYE's OTG PHY is UTMI (High Speed capable, confirmed by this
 * board's own boot log: "Using UTMI PHY instead of requested internal PHY"),
 * so esp_tinyusb's TUD_OPT_HIGH_SPEED is compiled in unconditionally for
 * this target - which means a custom configuration_descriptor needs an
 * hs_configuration_descriptor alongside it too (descriptors_control.c
 * hard-requires both once you're not using the Kconfig-generated default,
 * and requires them to be the same wTotalLength). Content's identical
 * except bulk endpoints' wMaxPacketSize, which HS bumps to 512. */
static cfg_desc_t s_cfg_desc_hs;

static char s_serial[13];

void usb_video_desc_init(void)
{
    s_cfg_desc_hs = s_cfg_desc;
    s_cfg_desc_hs.msc.ep_out.wMaxPacketSize = 512;
    s_cfg_desc_hs.msc.ep_in.wMaxPacketSize = 512;
    s_cfg_desc_hs.video_streaming.ep.wMaxPacketSize = 512;

    uint8_t mac[6];
    /* Not Wi-Fi/BT identity - just a stable per-chip byte source for a
     * unique-enough USB serial string (Windows keys driver state off VID+PID
     * +serial, so a fixed string across boards would collide). ESP_MAC_BASE,
     * not ESP_MAC_WIFI_STA - this board has no Wi-Fi MAC efuse burned. */
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_serial, sizeof s_serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    s_strings[STRID_SERIAL] = s_serial;
}

static const tusb_desc_device_qualifier_t s_qualifier_desc = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 1,
    .bReserved = 0,
};

const tinyusb_config_t *usb_video_tinyusb_config(void)
{
    static tinyusb_config_t cfg;
    memset(&cfg, 0, sizeof cfg);
#if TUD_OPT_HIGH_SPEED
    cfg.hs_configuration_descriptor = (const uint8_t *)&s_cfg_desc_hs;
    cfg.qualifier_descriptor = &s_qualifier_desc;
#endif
    cfg.device_descriptor = &s_device_desc;
    cfg.configuration_descriptor = (const uint8_t *)&s_cfg_desc;
    cfg.string_descriptor = s_strings;
    cfg.string_descriptor_count = STRID_COUNT;
    return &cfg;
}
