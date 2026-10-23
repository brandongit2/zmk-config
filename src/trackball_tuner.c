/*
 * Trackball tuner: an input processor whose settings are changed at runtime
 * from the Imprint companion app over BLE, and saved in flash.
 *
 * Each trackball has its own settings slot (the processor's parameter):
 * cursor or scroll mode, speed, per-axis inversion and swap, scroll factors
 * and a speed-based acceleration curve.
 *
 * GATT characteristic (read/write, encrypted). Slot encoding, little-endian:
 *   u8  mode           0 = cursor, 1 = scroll
 *   u8  flags          bit 0 invert X, bit 1 invert Y, bit 2 swap X/Y
 *   u16 speed          cursor multiplier × 1000
 *   u16 scroll_v       counts → vertical scroll notches × 1000
 *   u16 scroll_h       counts → horizontal scroll notches × 1000 (0 = off)
 *   u16 accel          extra gain per 1000 counts/s above threshold × 1000
 *   u16 threshold      counts/s where acceleration starts
 *   u16 accel_max      maximum gain × 100
 * Read returns both slots (left then right). A write is either
 * <slot index> <slot encoding>, or the single byte 0xFF to restore defaults.
 */

#define DT_DRV_COMPAT imprint_input_processor_tuner

#include <math.h>
#include <string.h>

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <drivers/input_processor.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define TUNER_UUID(num) BT_UUID_128_ENCODE(num, 0x4b45, 0x5945, 0x8a1e, 0x1d2c3b4a5f60)
#define TUNER_SERVICE_UUID TUNER_UUID(0x6b6b0100)
#define TUNER_CHRC_UUID TUNER_UUID(0x6b6b0101)

#define TUNER_SLOTS 2
#define TUNER_SLOT_WIRE_LEN 14
#define TUNER_VERSION 1

#define FLAG_INVERT_X BIT(0)
#define FLAG_INVERT_Y BIT(1)
#define FLAG_SWAP_XY BIT(2)

enum tuner_mode {
    TUNER_MODE_CURSOR = 0,
    TUNER_MODE_SCROLL = 1,
};

struct tuner_slot {
    uint8_t mode;
    uint8_t flags;
    float speed;
    float scroll_v;
    float scroll_h;
    float accel;
    float threshold;
    float accel_max;
};

struct tuner_settings {
    uint8_t version;
    struct tuner_slot slots[TUNER_SLOTS];
};

// Defaults reproduce the stock Cyboard keymap: the left ball scrolls at 1/3
// with the vertical axis inverted, the right ball moves the cursor 1:1.
static const struct tuner_settings tuner_defaults = {
    .version = TUNER_VERSION,
    .slots =
        {
            {.mode = TUNER_MODE_SCROLL,
             .flags = FLAG_INVERT_Y,
             .speed = 1.0f,
             .scroll_v = 1.0f / 3,
             .scroll_h = 1.0f / 3,
             .accel = 0.0f,
             .threshold = 0.0f,
             .accel_max = 4.0f},
            {.mode = TUNER_MODE_CURSOR,
             .flags = 0,
             .speed = 1.0f,
             .scroll_v = 1.0f / 3,
             .scroll_h = 1.0f / 3,
             .accel = 0.0f,
             .threshold = 0.0f,
             .accel_max = 4.0f},
        },
};

static struct tuner_settings tuner = tuner_defaults;

struct tuner_state {
    float remainder[4];
    int32_t frame_x;
    int32_t frame_y;
    int64_t last_sync;
    float speed;
};

static struct tuner_state states[TUNER_SLOTS];

// MARK: Processing

static float accel_gain(const struct tuner_slot *slot, float speed) {
    if (slot->accel <= 0.0f || speed <= slot->threshold) {
        return 1.0f;
    }
    float gain = 1.0f + slot->accel * (speed - slot->threshold) / 1000.0f;
    return MIN(gain, MAX(slot->accel_max, 1.0f));
}

static void track_speed(struct tuner_state *st, const struct input_event *event) {
    if (event->code == INPUT_REL_X) {
        st->frame_x += event->value;
    } else {
        st->frame_y += event->value;
    }
    if (!event->sync) {
        return;
    }

    int64_t now = k_uptime_get();
    int64_t dt = now - st->last_sync;
    st->last_sync = now;
    if (dt > 100) {
        // Ball was at rest: start over without stale speed or remainders.
        st->speed = 0.0f;
        memset(st->remainder, 0, sizeof(st->remainder));
    } else {
        float dist = sqrtf((float)(st->frame_x * st->frame_x + st->frame_y * st->frame_y));
        st->speed = dist * 1000.0f / (float)MAX(dt, 4);
    }
    st->frame_x = 0;
    st->frame_y = 0;
}

static int tuner_handle_event(const struct device *dev, struct input_event *event,
                              uint32_t param1, uint32_t param2,
                              struct zmk_input_processor_state *state) {
    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t index = MIN(param1, TUNER_SLOTS - 1);
    const struct tuner_slot *slot = &tuner.slots[index];
    struct tuner_state *st = &states[index];

    // Acceleration uses the speed of the previous report (one report of lag).
    float gain = accel_gain(slot, st->speed);
    track_speed(st, event);

    bool is_x = event->code == INPUT_REL_X;
    if (slot->flags & FLAG_SWAP_XY) {
        is_x = !is_x;
    }

    float value = (float)event->value;
    if ((is_x && (slot->flags & FLAG_INVERT_X)) || (!is_x && (slot->flags & FLAG_INVERT_Y))) {
        value = -value;
    }

    uint8_t r;
    if (slot->mode == TUNER_MODE_SCROLL) {
        value *= (is_x ? slot->scroll_h : slot->scroll_v) * gain;
        event->code = is_x ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
        r = is_x ? 2 : 3;
    } else {
        value *= slot->speed * gain;
        event->code = is_x ? INPUT_REL_X : INPUT_REL_Y;
        r = is_x ? 0 : 1;
    }

    float total = value + st->remainder[r];
    int32_t out = (int32_t)total;
    st->remainder[r] = total - (float)out;
    event->value = out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static const struct zmk_input_processor_driver_api tuner_driver_api = {
    .handle_event = tuner_handle_event,
};

#define TUNER_INST(n)                                                                              \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tuner_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TUNER_INST)

// MARK: Persistence

static void tuner_save(struct k_work *work) {
    int err = settings_save_one("imprint_tuner/cfg", &tuner, sizeof(tuner));
    if (err < 0) {
        LOG_WRN("Failed to save trackball settings: %d", err);
    }
}

// Saving is debounced so dragging a slider in the app doesn't wear the flash.
static K_WORK_DELAYABLE_DEFINE(tuner_save_work, tuner_save);

static int tuner_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                              void *cb_arg) {
    if (strcmp(name, "cfg") != 0 || len != sizeof(struct tuner_settings)) {
        return -ENOENT;
    }
    struct tuner_settings loaded;
    int err = read_cb(cb_arg, &loaded, sizeof(loaded));
    if (err < 0) {
        return err;
    }
    if (loaded.version == TUNER_VERSION) {
        tuner = loaded;
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(imprint_tuner, "imprint_tuner", NULL, tuner_settings_set, NULL,
                               NULL);

// MARK: GATT

static uint16_t to_fixed(float value, float scale) {
    return (uint16_t)CLAMP(lroundf(value * scale), 0, UINT16_MAX);
}

static void encode_slot(const struct tuner_slot *slot, uint8_t *buf) {
    buf[0] = slot->mode;
    buf[1] = slot->flags;
    sys_put_le16(to_fixed(slot->speed, 1000), &buf[2]);
    sys_put_le16(to_fixed(slot->scroll_v, 1000), &buf[4]);
    sys_put_le16(to_fixed(slot->scroll_h, 1000), &buf[6]);
    sys_put_le16(to_fixed(slot->accel, 1000), &buf[8]);
    sys_put_le16(to_fixed(slot->threshold, 1), &buf[10]);
    sys_put_le16(to_fixed(slot->accel_max, 100), &buf[12]);
}

static void decode_slot(const uint8_t *buf, struct tuner_slot *slot) {
    slot->mode = buf[0] == TUNER_MODE_SCROLL ? TUNER_MODE_SCROLL : TUNER_MODE_CURSOR;
    slot->flags = buf[1] & (FLAG_INVERT_X | FLAG_INVERT_Y | FLAG_SWAP_XY);
    slot->speed = sys_get_le16(&buf[2]) / 1000.0f;
    slot->scroll_v = sys_get_le16(&buf[4]) / 1000.0f;
    slot->scroll_h = sys_get_le16(&buf[6]) / 1000.0f;
    slot->accel = sys_get_le16(&buf[8]) / 1000.0f;
    slot->threshold = (float)sys_get_le16(&buf[10]);
    slot->accel_max = sys_get_le16(&buf[12]) / 100.0f;
}

static ssize_t tuner_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t len, uint16_t offset) {
    uint8_t value[TUNER_SLOTS * TUNER_SLOT_WIRE_LEN];
    for (int i = 0; i < TUNER_SLOTS; i++) {
        encode_slot(&tuner.slots[i], &value[i * TUNER_SLOT_WIRE_LEN]);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t tuner_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                           uint16_t len, uint16_t offset, uint8_t flags) {
    const uint8_t *data = buf;
    if (offset != 0) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }

    if (len == 1 && data[0] == 0xFF) {
        tuner = tuner_defaults;
    } else if (len == 1 + TUNER_SLOT_WIRE_LEN && data[0] < TUNER_SLOTS) {
        decode_slot(&data[1], &tuner.slots[data[0]]);
    } else {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }

    k_work_reschedule(&tuner_save_work, K_SECONDS(2));
    return len;
}

BT_GATT_SERVICE_DEFINE(tuner_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(TUNER_SERVICE_UUID)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(TUNER_CHRC_UUID),
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_READ_ENCRYPT |
                                                  BT_GATT_PERM_WRITE_ENCRYPT,
                                              tuner_read, tuner_write, NULL));
