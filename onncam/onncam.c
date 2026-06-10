#include <errno.h>
#include <libusb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONN_VID 0x3938
#define ONN_PID 0x1390

#define REQ_SET_CUR 0x01
#define REQ_GET_CUR 0x81
#define REQ_GET_MIN 0x82
#define REQ_GET_MAX 0x83
#define REQ_GET_RES 0x84
#define REQ_GET_LEN 0x85
#define REQ_GET_INFO 0x86
#define REQ_GET_DEF 0x87

#define MAX_XU 4

typedef enum {
    ENTITY_CAMERA,
    ENTITY_PROCESSING
} entity_kind_t;

typedef struct {
    const char *name;
    entity_kind_t entity;
    uint8_t selector;
    uint8_t size;    /* bytes of this field */
    uint8_t payload; /* total transfer size (0 = same as size) */
    uint8_t offset;  /* field offset within the payload */
} control_t;

typedef struct {
    uint8_t unit_id;
    uint8_t guid[16];
    uint8_t num_controls;
    uint8_t control_size;
    uint8_t bm_controls[8];
} xu_info_t;

typedef struct {
    uint8_t vc_interface;
    uint8_t camera_id;
    uint8_t processing_id;
    xu_info_t xus[MAX_XU];
    uint8_t xu_count;
} uvc_entities_t;

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *handle;
    uvc_entities_t entities;
} session_t;

static const control_t controls[] = {
    {"exposure-auto", ENTITY_CAMERA, 0x02, 1, 0, 0},
    {"ae-priority", ENTITY_CAMERA, 0x03, 1, 0, 0},
    {"exposure", ENTITY_CAMERA, 0x04, 4, 0, 0},
    {"focus", ENTITY_CAMERA, 0x06, 2, 0, 0},
    {"focus-auto", ENTITY_CAMERA, 0x08, 1, 0, 0},
    {"zoom", ENTITY_CAMERA, 0x0b, 2, 0, 0},
    /* Pan/tilt share one 8-byte UVC control; sets are read-modify-write. */
    {"pan", ENTITY_CAMERA, 0x0d, 4, 8, 0},
    {"tilt", ENTITY_CAMERA, 0x0d, 4, 8, 4},
    /* The ISP repurposes UVC "roll" as mirror/flip (0..3), matching the
     * hidden slider in the vendor's Windows app. */
    {"mirror", ENTITY_CAMERA, 0x0f, 2, 0, 0},
    {"privacy", ENTITY_CAMERA, 0x11, 1, 0, 0},
    {"backlight", ENTITY_PROCESSING, 0x01, 2, 0, 0},
    {"brightness", ENTITY_PROCESSING, 0x02, 2, 0, 0},
    {"contrast", ENTITY_PROCESSING, 0x03, 2, 0, 0},
    {"gain", ENTITY_PROCESSING, 0x04, 2, 0, 0},
    {"powerline", ENTITY_PROCESSING, 0x05, 1, 0, 0},
    {"hue", ENTITY_PROCESSING, 0x06, 2, 0, 0},
    {"saturation", ENTITY_PROCESSING, 0x07, 2, 0, 0},
    {"sharpness", ENTITY_PROCESSING, 0x08, 2, 0, 0},
    {"gamma", ENTITY_PROCESSING, 0x09, 2, 0, 0},
    {"white", ENTITY_PROCESSING, 0x0a, 2, 0, 0},
    {"white-auto", ENTITY_PROCESSING, 0x0b, 1, 0, 0},
};

static int32_t read_le_signed(const unsigned char *buf, uint8_t size) {
    uint32_t v = 0;
    for (uint8_t i = 0; i < size; i++) v |= ((uint32_t)buf[i]) << (8 * i);
    if (size < 4 && (v & (1u << (size * 8 - 1)))) v |= ~((1u << (size * 8)) - 1);
    return (int32_t)v;
}

static void write_le_signed(unsigned char *buf, uint8_t size, int32_t value) {
    for (uint8_t i = 0; i < size; i++) buf[i] = (unsigned char)((uint32_t)value >> (8 * i));
}

static const control_t *find_control(const char *name) {
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (strcmp(controls[i].name, name) == 0) return &controls[i];
    }
    return NULL;
}

static void parse_vc_extra(const unsigned char *extra, int len, uvc_entities_t *entities) {
    int p = 0;
    while (p + 2 < len) {
        uint8_t blen = extra[p];
        if (blen < 3 || p + blen > len) break;
        uint8_t dtype = extra[p + 1];
        uint8_t subtype = extra[p + 2];
        if (dtype == 0x24 && subtype == 0x02 && blen >= 5) {
            uint16_t terminal_type = (uint16_t)extra[p + 5] << 8 | extra[p + 4];
            if (terminal_type == 0x0201) entities->camera_id = extra[p + 3];
        } else if (dtype == 0x24 && subtype == 0x05 && blen >= 8) {
            entities->processing_id = extra[p + 3];
        } else if (dtype == 0x24 && subtype == 0x06 && blen >= 24 && entities->xu_count < MAX_XU) {
            /* Extension Unit: bUnitID, guid[16], bNumControls, bNrInPins,
             * baSourceID[n], bControlSize, bmControls[bControlSize], iExtension */
            uint8_t npins = extra[p + 21];
            uint8_t csize_off = 22 + npins;
            if (csize_off < blen) {
                xu_info_t *xu = &entities->xus[entities->xu_count];
                memset(xu, 0, sizeof(*xu));
                xu->unit_id = extra[p + 3];
                memcpy(xu->guid, &extra[p + 4], 16);
                xu->num_controls = extra[p + 20];
                xu->control_size = extra[p + csize_off];
                if (xu->control_size > sizeof(xu->bm_controls))
                    xu->control_size = sizeof(xu->bm_controls);
                if (csize_off + 1 + xu->control_size <= blen)
                    memcpy(xu->bm_controls, &extra[p + csize_off + 1], xu->control_size);
                entities->xu_count++;
            }
        }
        p += blen;
    }
}

static void dump_interfaces(libusb_device *dev) {
    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) rc = libusb_get_config_descriptor(dev, 0, &cfg);
    if (rc != 0) return;
    fprintf(stderr, "USB interfaces:\n");
    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            fprintf(stderr, "  if=%u alt=%u class=%02x subclass=%02x protocol=%02x extra=%d\n",
                    alt->bInterfaceNumber, alt->bAlternateSetting, alt->bInterfaceClass,
                    alt->bInterfaceSubClass, alt->bInterfaceProtocol, alt->extra_length);
        }
    }
    libusb_free_config_descriptor(cfg);
}

static int discover_entities(libusb_device *dev, uvc_entities_t *entities) {
    struct libusb_config_descriptor *cfg = NULL;
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) rc = libusb_get_config_descriptor(dev, 0, &cfg);
    if (rc != 0) return rc;

    memset(entities, 0, sizeof(*entities));
    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface *iface = &cfg->interface[i];
        for (int j = 0; j < iface->num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &iface->altsetting[j];
            if (alt->bInterfaceClass == 0x0e && alt->bInterfaceSubClass == 0x01) {
                entities->vc_interface = alt->bInterfaceNumber;
                parse_vc_extra(alt->extra, alt->extra_length, entities);
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return (entities->camera_id && entities->processing_id) ? 0 : -1;
}

static void session_close(session_t *s) {
    if (s->handle) {
        libusb_close(s->handle);
        s->handle = NULL;
    }
}

static int session_open(session_t *s, char *err, size_t errlen) {
    if (s->handle) return 0;
    s->handle = libusb_open_device_with_vid_pid(s->ctx, ONN_VID, ONN_PID);
    if (!s->handle) {
        snprintf(err, errlen, "camera %04x:%04x not found", ONN_VID, ONN_PID);
        return -1;
    }
    if (discover_entities(libusb_get_device(s->handle), &s->entities) != 0) {
        snprintf(err, errlen, "could not discover UVC camera/processing units");
        session_close(s);
        return -1;
    }
    return 0;
}

/* Drop the handle on errors that indicate the device went away so the next
 * command transparently reopens it. A pipe error is just a rejected request;
 * the handle is still good. */
static void note_transfer_error(session_t *s, int rc) {
    if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO || rc == LIBUSB_ERROR_NOT_FOUND) {
        session_close(s);
    }
}

/* Raw class-specific transfer addressed by entity id + selector, usable for
 * the UVC camera/processing/extension units and the UAC audio units alike
 * (the wIndex interface byte differs per target). */
static int xfer_entity(session_t *s, uint8_t interface_num, uint8_t entity_id, uint8_t selector,
                       uint8_t request, unsigned char *data, uint16_t len) {
    uint8_t request_type = request == REQ_SET_CUR ? 0x21 : 0xa1;
    uint16_t value = (uint16_t)selector << 8;
    uint16_t index = ((uint16_t)entity_id << 8) | interface_num;
    int rc = libusb_control_transfer(s->handle, request_type, request, value, index, data, len, 1000);
    if (rc < 0) note_transfer_error(s, rc);
    return rc;
}

static int ctrl_transfer(session_t *s, const control_t *control, uint8_t request,
                         unsigned char *data, uint16_t len) {
    uint8_t entity_id = control->entity == ENTITY_CAMERA ? s->entities.camera_id : s->entities.processing_id;
    return xfer_entity(s, s->entities.vc_interface, entity_id, control->selector, request, data, len);
}

static int get_value(session_t *s, const control_t *control, uint8_t request, int32_t *value) {
    unsigned char buf[8] = {0};
    uint8_t payload = control->payload ? control->payload : control->size;
    int rc = ctrl_transfer(s, control, request, buf, payload);
    if (rc < 0) return rc;
    *value = read_le_signed(buf + control->offset, control->size);
    return 0;
}

static void print_control(session_t *s, const control_t *control) {
    unsigned char info = 0;
    int rc = ctrl_transfer(s, control, REQ_GET_INFO, &info, 1);
    if (rc < 0 || !(info & 0x01)) return;

    int32_t cur = 0, min = 0, max = 0, res = 0, def = 0;
    int got_cur = get_value(s, control, REQ_GET_CUR, &cur) == 0;
    int got_min = get_value(s, control, REQ_GET_MIN, &min) == 0;
    int got_max = get_value(s, control, REQ_GET_MAX, &max) == 0;
    int got_res = get_value(s, control, REQ_GET_RES, &res) == 0;
    int got_def = get_value(s, control, REQ_GET_DEF, &def) == 0;

    printf("%-14s size=%u get=%s set=%s", control->name, control->size,
           (info & 0x01) ? "yes" : "no", (info & 0x02) ? "yes" : "no");
    if (got_cur) printf(" cur=%d", cur);
    if (got_min) printf(" min=%d", min);
    if (got_max) printf(" max=%d", max);
    if (got_res) printf(" step=%d", res);
    if (got_def) printf(" default=%d", def);
    printf("\n");
}

static int cmd_list(session_t *s, char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    printf("onn 4K Webcam %04x:%04x vc_if=%u camera_id=%u processing_id=%u\n",
           ONN_VID, ONN_PID, s->entities.vc_interface, s->entities.camera_id, s->entities.processing_id);
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        print_control(s, &controls[i]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* probe: exhaustive discovery of UVC selectors, extension units, the */
/* UAC microphone feature units, and any vendor HID interfaces.       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t sel;
    uint8_t size;
    const char *name;
} sel_info_t;

/* Every selector defined by UVC 1.5 for the camera terminal. */
static const sel_info_t ct_selectors[] = {
    {0x01, 1, "scanning-mode"},
    {0x02, 1, "ae-mode"},
    {0x03, 1, "ae-priority"},
    {0x04, 4, "exposure-abs"},
    {0x05, 1, "exposure-rel"},
    {0x06, 2, "focus-abs"},
    {0x07, 2, "focus-rel"},
    {0x08, 1, "focus-auto"},
    {0x09, 2, "iris-abs"},
    {0x0a, 1, "iris-rel"},
    {0x0b, 2, "zoom-abs"},
    {0x0c, 3, "zoom-rel"},
    {0x0d, 8, "pantilt-abs"},
    {0x0e, 4, "pantilt-rel"},
    {0x0f, 2, "roll-abs"},
    {0x10, 2, "roll-rel"},
    {0x11, 1, "privacy"},
    {0x12, 1, "focus-simple"},
    {0x13, 12, "digital-window"},
    {0x14, 10, "region-of-interest"},
};

/* Every selector defined by UVC 1.5 for the processing unit. */
static const sel_info_t pu_selectors[] = {
    {0x01, 2, "backlight"},
    {0x02, 2, "brightness"},
    {0x03, 2, "contrast"},
    {0x04, 2, "gain"},
    {0x05, 1, "powerline"},
    {0x06, 2, "hue"},
    {0x07, 2, "saturation"},
    {0x08, 2, "sharpness"},
    {0x09, 2, "gamma"},
    {0x0a, 2, "wb-temperature"},
    {0x0b, 1, "wb-temperature-auto"},
    {0x0c, 4, "wb-component"},
    {0x0d, 1, "wb-component-auto"},
    {0x0e, 2, "digital-multiplier"},
    {0x0f, 2, "digital-multiplier-limit"},
    {0x10, 1, "hue-auto"},
    {0x11, 1, "analog-video-standard"},
    {0x12, 1, "analog-lock-status"},
    {0x13, 1, "contrast-auto"},
};

static int is_mapped_selector(entity_kind_t entity, uint8_t sel) {
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); i++) {
        if (controls[i].entity == entity && controls[i].selector == sel) return 1;
    }
    return 0;
}

static void print_hex(const unsigned char *buf, int len) {
    for (int i = 0; i < len; i++) printf("%02x", buf[i]);
}

static void probe_uvc_request(session_t *s, uint8_t entity_id, uint8_t sel, uint8_t request,
                              const char *tag, uint8_t size) {
    unsigned char buf[64] = {0};
    if (size > sizeof(buf)) size = sizeof(buf);
    int rc = xfer_entity(s, s->entities.vc_interface, entity_id, sel, request, buf, size);
    if (rc < 0) return;
    printf(" %s=", tag);
    if (rc <= 4) {
        printf("%d", read_le_signed(buf, (uint8_t)rc));
    } else {
        printf("0x");
        print_hex(buf, rc);
    }
}

static void probe_uvc_entity(session_t *s, const char *label, uint8_t entity_id,
                             entity_kind_t entity, const sel_info_t *sels, size_t count) {
    printf("probe %s id=%u\n", label, entity_id);
    for (size_t i = 0; i < count; i++) {
        unsigned char info = 0;
        int rc = xfer_entity(s, s->entities.vc_interface, entity_id, sels[i].sel,
                             REQ_GET_INFO, &info, 1);
        if (rc < 0) {
            printf("  sel=0x%02x %-26s unsupported (%s)\n", sels[i].sel, sels[i].name,
                   libusb_error_name(rc));
            continue;
        }
        printf("  sel=0x%02x %-26s info=0x%02x%s%s%s", sels[i].sel, sels[i].name, info,
               (info & 0x01) ? " get" : "", (info & 0x02) ? " set" : "",
               is_mapped_selector(entity, sels[i].sel) ? "" : " NEW");
        if (info & 0x01) {
            probe_uvc_request(s, entity_id, sels[i].sel, REQ_GET_CUR, "cur", sels[i].size);
            probe_uvc_request(s, entity_id, sels[i].sel, REQ_GET_MIN, "min", sels[i].size);
            probe_uvc_request(s, entity_id, sels[i].sel, REQ_GET_MAX, "max", sels[i].size);
            probe_uvc_request(s, entity_id, sels[i].sel, REQ_GET_DEF, "def", sels[i].size);
        }
        printf("\n");
    }
}

static void probe_xu(session_t *s, const xu_info_t *xu) {
    printf("probe extension-unit id=%u guid=", xu->unit_id);
    /* GUID little-endian fields: 4-2-2 then byte order. */
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-",
           xu->guid[3], xu->guid[2], xu->guid[1], xu->guid[0],
           xu->guid[5], xu->guid[4], xu->guid[7], xu->guid[6],
           xu->guid[8], xu->guid[9]);
    print_hex(&xu->guid[10], 6);
    printf(" controls=%u bmControls=0x", xu->num_controls);
    print_hex(xu->bm_controls, xu->control_size ? xu->control_size : 1);
    printf("\n");

    int max_sel = xu->control_size * 8;
    if (max_sel == 0 || max_sel > 32) max_sel = 32;
    for (int sel = 1; sel <= max_sel; sel++) {
        int advertised = ((sel - 1) / 8 < (int)sizeof(xu->bm_controls)) &&
                         (xu->bm_controls[(sel - 1) / 8] & (1 << ((sel - 1) % 8)));
        unsigned char lenbuf[2] = {0};
        int len_rc = xfer_entity(s, s->entities.vc_interface, xu->unit_id, (uint8_t)sel,
                                 REQ_GET_LEN, lenbuf, 2);
        unsigned char info = 0;
        int info_rc = xfer_entity(s, s->entities.vc_interface, xu->unit_id, (uint8_t)sel,
                                  REQ_GET_INFO, &info, 1);
        if (len_rc < 0 && info_rc < 0 && !advertised) continue;

        int payload = (len_rc >= 2) ? (lenbuf[0] | (lenbuf[1] << 8)) : 0;
        printf("  sel=0x%02x%s len=%d", sel, advertised ? " advertised" : "", payload);
        if (info_rc >= 0)
            printf(" info=0x%02x%s%s", info, (info & 0x01) ? " get" : "", (info & 0x02) ? " set" : "");
        if (payload > 0 && payload <= 64 && (info_rc < 0 || (info & 0x01))) {
            unsigned char buf[64] = {0};
            int rc = xfer_entity(s, s->entities.vc_interface, xu->unit_id, (uint8_t)sel,
                                 REQ_GET_CUR, buf, (uint16_t)payload);
            if (rc > 0) {
                printf(" cur=0x");
                print_hex(buf, rc);
            }
            memset(buf, 0, sizeof(buf));
            rc = xfer_entity(s, s->entities.vc_interface, xu->unit_id, (uint8_t)sel,
                             REQ_GET_DEF, buf, (uint16_t)payload);
            if (rc > 0) {
                printf(" def=0x");
                print_hex(buf, rc);
            }
        }
        printf("\n");
    }
}

/* UAC1 feature unit control bits. */
static const char *const uac_fu_controls[] = {
    "mute", "volume", "bass", "mid", "treble", "graphic-eq", "agc", "delay", "bass-boost", "loudness"
};

static void probe_audio_volume(session_t *s, uint8_t ac_interface, uint8_t unit_id, uint8_t channel) {
    /* UAC1 volume: selector 0x02, signed 16-bit in 1/256 dB. The wValue low
     * byte addresses the channel. */
    unsigned char buf[2];
    int16_t cur = 0, vmin = 0, vmax = 0;
    uint16_t value = (uint16_t)(0x02 << 8) | channel;
    uint16_t index = ((uint16_t)unit_id << 8) | ac_interface;
    int rc = libusb_control_transfer(s->handle, 0xa1, 0x81, value, index, buf, 2, 1000);
    if (rc < 2) {
        printf("  volume ch=%u unreadable (%s)\n", channel, rc < 0 ? libusb_error_name(rc) : "short");
        return;
    }
    cur = (int16_t)(buf[0] | (buf[1] << 8));
    if (libusb_control_transfer(s->handle, 0xa1, 0x82, value, index, buf, 2, 1000) == 2)
        vmin = (int16_t)(buf[0] | (buf[1] << 8));
    if (libusb_control_transfer(s->handle, 0xa1, 0x83, value, index, buf, 2, 1000) == 2)
        vmax = (int16_t)(buf[0] | (buf[1] << 8));
    printf("  volume ch=%u cur=%.1fdB min=%.1fdB max=%.1fdB\n",
           channel, cur / 256.0, vmin / 256.0, vmax / 256.0);
}

static void probe_audio_agc(session_t *s, uint8_t ac_interface, uint8_t unit_id, uint8_t channel) {
    unsigned char agc = 0;
    uint16_t value = (uint16_t)(0x07 << 8) | channel;
    uint16_t index = ((uint16_t)unit_id << 8) | ac_interface;
    int rc = libusb_control_transfer(s->handle, 0xa1, 0x81, value, index, &agc, 1, 1000);
    if (rc == 1) printf("  agc ch=%u cur=%u\n", channel, agc);
}

static void probe_audio(session_t *s) {
    struct libusb_config_descriptor *cfg = NULL;
    libusb_device *dev = libusb_get_device(s->handle);
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) rc = libusb_get_config_descriptor(dev, 0, &cfg);
    if (rc != 0) return;

    for (uint8_t i = 0; i < cfg->bNumInterfaces; i++) {
        for (int j = 0; j < cfg->interface[i].num_altsetting; j++) {
            const struct libusb_interface_descriptor *alt = &cfg->interface[i].altsetting[j];

            if (alt->bInterfaceClass == 0x03) {
                /* Vendor HID interface: possible Sonix audio DSP config path. */
                int report_len = 0;
                const unsigned char *e = alt->extra;
                for (int p = 0; p + 1 < alt->extra_length; p += e[p]) {
                    if (e[p] >= 9 && e[p + 1] == 0x21) report_len = e[p + 7] | (e[p + 8] << 8);
                    if (e[p] == 0) break;
                }
                printf("probe hid if=%u subclass=0x%02x protocol=0x%02x report-desc-len=%d\n",
                       alt->bInterfaceNumber, alt->bInterfaceSubClass, alt->bInterfaceProtocol,
                       report_len);
                continue;
            }

            if (alt->bInterfaceClass != 0x01 || alt->bInterfaceSubClass != 0x01) continue;

            printf("probe audio-control if=%u\n", alt->bInterfaceNumber);
            const unsigned char *e = alt->extra;
            int len = alt->extra_length;
            int p = 0;
            while (p + 2 < len) {
                uint8_t blen = e[p];
                if (blen < 3 || p + blen > len) break;
                if (e[p + 1] == 0x24) {
                    uint8_t subtype = e[p + 2];
                    if (subtype == 0x01 && blen >= 5) {
                        printf("  header bcdADC=%02x.%02x\n", e[p + 4], e[p + 3]);
                    } else if (subtype == 0x02 && blen >= 8) {
                        uint16_t ttype = e[p + 4] | (e[p + 5] << 8);
                        printf("  input-terminal id=%u type=0x%04x%s channels=%u\n",
                               e[p + 3], ttype, ttype == 0x0201 ? " (microphone)" : "", e[p + 7]);
                    } else if (subtype == 0x03 && blen >= 8) {
                        uint16_t ttype = e[p + 4] | (e[p + 5] << 8);
                        printf("  output-terminal id=%u type=0x%04x source=%u\n",
                               e[p + 3], ttype, e[p + 7]);
                    } else if (subtype == 0x06 && blen >= 7) {
                        /* UAC1 feature unit: bUnitID, bSourceID, bControlSize, bmaControls... */
                        uint8_t unit_id = e[p + 3];
                        uint8_t csize = e[p + 5];
                        int channels = csize ? (blen - 7) / csize : 0;
                        printf("  feature-unit id=%u source=%u\n", unit_id, e[p + 4]);
                        for (int ch = 0; ch < channels; ch++) {
                            uint32_t bits = 0;
                            for (int b = 0; b < csize && b < 4; b++)
                                bits |= (uint32_t)e[p + 6 + ch * csize + b] << (8 * b);
                            if (!bits) continue;
                            printf("    ch%d:", ch);
                            for (size_t c = 0; c < sizeof(uac_fu_controls) / sizeof(uac_fu_controls[0]); c++) {
                                if (bits & (1u << c)) printf(" %s", uac_fu_controls[c]);
                            }
                            printf("\n");
                            if (bits & 0x02) probe_audio_volume(s, alt->bInterfaceNumber, unit_id, (uint8_t)ch);
                            if (bits & 0x40) probe_audio_agc(s, alt->bInterfaceNumber, unit_id, (uint8_t)ch);
                        }
                    } else {
                        printf("  cs-descriptor subtype=0x%02x len=%u\n", subtype, blen);
                    }
                }
                p += blen;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
}

static int cmd_probe(session_t *s, char *err, size_t errlen) {
    (void)err;
    (void)errlen;
    probe_uvc_entity(s, "camera-terminal", s->entities.camera_id, ENTITY_CAMERA,
                     ct_selectors, sizeof(ct_selectors) / sizeof(ct_selectors[0]));
    probe_uvc_entity(s, "processing-unit", s->entities.processing_id, ENTITY_PROCESSING,
                     pu_selectors, sizeof(pu_selectors) / sizeof(pu_selectors[0]));
    for (uint8_t i = 0; i < s->entities.xu_count; i++) {
        probe_xu(s, &s->entities.xus[i]);
    }
    probe_audio(s);
    return 0;
}

static int cmd_get(session_t *s, const char *name, char *err, size_t errlen) {
    const control_t *control = find_control(name);
    if (!control) {
        snprintf(err, errlen, "unknown control: %s", name);
        return -1;
    }
    int32_t value = 0;
    int rc = get_value(s, control, REQ_GET_CUR, &value);
    if (rc < 0) {
        snprintf(err, errlen, "GET_CUR failed for %s: %s", control->name, libusb_error_name(rc));
        return -1;
    }
    printf("%s=%d\n", control->name, value);
    return 0;
}

static int cmd_set(session_t *s, const char *name, const char *value_str, char *err, size_t errlen) {
    const control_t *control = find_control(name);
    if (!control) {
        snprintf(err, errlen, "unknown control: %s", name);
        return -1;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(value_str, &end, 0);
    if (errno || !end || *end) {
        snprintf(err, errlen, "invalid value: %s", value_str);
        return -1;
    }
    unsigned char buf[8] = {0};
    uint8_t payload = control->payload ? control->payload : control->size;
    if (payload != control->size) {
        /* Multi-field control (pan/tilt): keep the other fields intact. */
        int rc = ctrl_transfer(s, control, REQ_GET_CUR, buf, payload);
        if (rc < 0) {
            snprintf(err, errlen, "GET_CUR failed for %s: %s", control->name, libusb_error_name(rc));
            return -1;
        }
    }
    write_le_signed(buf + control->offset, control->size, (int32_t)value);
    int rc = ctrl_transfer(s, control, REQ_SET_CUR, buf, payload);
    if (rc < 0) {
        snprintf(err, errlen, "SET_CUR failed for %s: %s", control->name, libusb_error_name(rc));
        return -1;
    }
    int32_t readback = 0;
    if (get_value(s, control, REQ_GET_CUR, &readback) == 0) {
        printf("%s=%d\n", control->name, readback);
    }
    return 0;
}

/* Persistent mode: read commands from stdin, one per line, keeping the USB
 * device open between commands. Every command is answered with zero or more
 * data lines followed by exactly one "ok" or "err <message>" line.
 *
 * Commands: list | get <control> | set <control> <value> | ping | quit
 */
static int serve(session_t *s) {
    char line[256];
    setvbuf(stdout, NULL, _IOLBF, 0);

    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!line[0]) continue;

        if (strcmp(line, "quit") == 0) break;
        if (strcmp(line, "ping") == 0) {
            printf("ok\n");
            continue;
        }

        char cmd[32] = "", a1[64] = "", a2[64] = "";
        int n = sscanf(line, "%31s %63s %63s", cmd, a1, a2);
        char err[256] = "";
        int rc;

        if ((rc = session_open(s, err, sizeof(err))) == 0) {
            if (strcmp(cmd, "list") == 0 && n == 1) {
                rc = cmd_list(s, err, sizeof(err));
            } else if (strcmp(cmd, "probe") == 0 && n == 1) {
                rc = cmd_probe(s, err, sizeof(err));
            } else if (strcmp(cmd, "get") == 0 && n == 2) {
                rc = cmd_get(s, a1, err, sizeof(err));
            } else if (strcmp(cmd, "set") == 0 && n == 3) {
                rc = cmd_set(s, a1, a2, err, sizeof(err));
            } else {
                snprintf(err, sizeof(err), "unknown command: %s", cmd);
                rc = -1;
            }
        }

        if (rc == 0) {
            printf("ok\n");
        } else {
            printf("err %s\n", err[0] ? err : "command failed");
        }
    }
    return 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "Usage:\n"
            "  %s list\n"
            "  %s probe\n"
            "  %s get <control>\n"
            "  %s set <control> <value>\n"
            "  %s serve\n",
            argv0, argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    session_t session = {0};
    int rc = libusb_init(&session.ctx);
    if (rc != 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_error_name(rc));
        return 1;
    }

    if (strcmp(argv[1], "serve") == 0) {
        rc = serve(&session);
        session_close(&session);
        libusb_exit(session.ctx);
        return rc;
    }

    char err[256] = "";
    if (session_open(&session, err, sizeof(err)) != 0) {
        fprintf(stderr, "%s\n", err);
        if (session.handle) dump_interfaces(libusb_get_device(session.handle));
        libusb_exit(session.ctx);
        return 1;
    }

    if (strcmp(argv[1], "list") == 0) {
        rc = cmd_list(&session, err, sizeof(err));
    } else if (strcmp(argv[1], "probe") == 0) {
        rc = cmd_probe(&session, err, sizeof(err));
    } else if (strcmp(argv[1], "get") == 0 && argc == 3) {
        rc = cmd_get(&session, argv[2], err, sizeof(err));
    } else if (strcmp(argv[1], "set") == 0 && argc == 4) {
        rc = cmd_set(&session, argv[2], argv[3], err, sizeof(err));
    } else {
        usage(argv[0]);
        rc = 2;
        err[0] = '\0';
    }

    if (rc != 0 && err[0]) fprintf(stderr, "%s\n", err);
    session_close(&session);
    libusb_exit(session.ctx);
    return rc < 0 ? 1 : rc;
}
