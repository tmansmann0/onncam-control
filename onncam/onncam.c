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
#define REQ_GET_INFO 0x86
#define REQ_GET_DEF 0x87

typedef enum {
    ENTITY_CAMERA,
    ENTITY_PROCESSING
} entity_kind_t;

typedef struct {
    const char *name;
    entity_kind_t entity;
    uint8_t selector;
    uint8_t size;
} control_t;

typedef struct {
    uint8_t vc_interface;
    uint8_t camera_id;
    uint8_t processing_id;
} uvc_entities_t;

typedef struct {
    libusb_context *ctx;
    libusb_device_handle *handle;
    uvc_entities_t entities;
} session_t;

static const control_t controls[] = {
    {"exposure-auto", ENTITY_CAMERA, 0x02, 1},
    {"exposure", ENTITY_CAMERA, 0x04, 4},
    {"focus", ENTITY_CAMERA, 0x06, 2},
    {"focus-auto", ENTITY_CAMERA, 0x08, 1},
    {"zoom", ENTITY_CAMERA, 0x0b, 2},
    {"backlight", ENTITY_PROCESSING, 0x01, 2},
    {"brightness", ENTITY_PROCESSING, 0x02, 2},
    {"contrast", ENTITY_PROCESSING, 0x03, 2},
    {"gain", ENTITY_PROCESSING, 0x04, 2},
    {"powerline", ENTITY_PROCESSING, 0x05, 1},
    {"hue", ENTITY_PROCESSING, 0x06, 2},
    {"saturation", ENTITY_PROCESSING, 0x07, 2},
    {"sharpness", ENTITY_PROCESSING, 0x08, 2},
    {"gamma", ENTITY_PROCESSING, 0x09, 2},
    {"white", ENTITY_PROCESSING, 0x0a, 2},
    {"white-auto", ENTITY_PROCESSING, 0x0b, 1},
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

static int ctrl_transfer(session_t *s, const control_t *control, uint8_t request,
                         unsigned char *data, uint16_t len) {
    uint8_t entity_id = control->entity == ENTITY_CAMERA ? s->entities.camera_id : s->entities.processing_id;
    uint8_t request_type = request == REQ_SET_CUR ? 0x21 : 0xa1;
    uint16_t value = (uint16_t)control->selector << 8;
    uint16_t index = ((uint16_t)entity_id << 8) | s->entities.vc_interface;
    int rc = libusb_control_transfer(s->handle, request_type, request, value, index, data, len, 1000);
    if (rc < 0) note_transfer_error(s, rc);
    return rc;
}

static int get_value(session_t *s, const control_t *control, uint8_t request, int32_t *value) {
    unsigned char buf[4] = {0};
    int rc = ctrl_transfer(s, control, request, buf, control->size);
    if (rc < 0) return rc;
    *value = read_le_signed(buf, control->size);
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
    unsigned char buf[4] = {0};
    write_le_signed(buf, control->size, (int32_t)value);
    int rc = ctrl_transfer(s, control, REQ_SET_CUR, buf, control->size);
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
            "  %s get <control>\n"
            "  %s set <control> <value>\n"
            "  %s serve\n",
            argv0, argv0, argv0, argv0);
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
