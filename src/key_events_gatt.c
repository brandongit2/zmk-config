/*
 * Key events GATT service.
 *
 * Notifies subscribed hosts of physical key position changes and of the
 * active layer bitmask, so a companion app can show exactly which key was
 * pressed, including keys that send nothing to the host (layers, Bluetooth,
 * lighting).
 *
 * Message format (little-endian):
 *   0x01 <position> <pressed>      key position pressed (1) or released (0)
 *   0x02 <mask: 4 bytes>           active layers, one bit per layer id
 *   0x03 <src> <dx: i16> <dy: i16> <reports>
 *                                  raw trackball counts summed over ~16 ms,
 *                                  before any input processor (src 0 = left
 *                                  sensor, 1 = right sensor via split, 2 = other)
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

#include <zmk/event_manager.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define KEY_EVENTS_UUID(num) BT_UUID_128_ENCODE(num, 0x4b45, 0x5945, 0x8a1e, 0x1d2c3b4a5f60)
#define KEY_EVENTS_SERVICE_UUID KEY_EVENTS_UUID(0x6b6b0000)
#define KEY_EVENTS_CHRC_UUID KEY_EVENTS_UUID(0x6b6b0001)

enum key_event_type {
    KEY_EVENT_POSITION = 0x01,
    KEY_EVENT_LAYERS = 0x02,
    KEY_EVENT_TRACKBALL = 0x03,
};

struct key_event_msg {
    uint8_t len;
    uint8_t data[7];
};

K_MSGQ_DEFINE(key_event_msgq, sizeof(struct key_event_msg), 32, 4);

static atomic_t subscribed;

static void queue_layers(void) {
    struct key_event_msg msg = {.len = 5, .data = {KEY_EVENT_LAYERS}};
    sys_put_le32(zmk_keymap_layer_state(), &msg.data[1]);
    k_msgq_put(&key_event_msgq, &msg, K_NO_WAIT);
}

static void key_events_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value);

BT_GATT_SERVICE_DEFINE(key_events_svc,
                       BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(KEY_EVENTS_SERVICE_UUID)),
                       BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(KEY_EVENTS_CHRC_UUID),
                                              BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL, NULL,
                                              NULL),
                       BT_GATT_CCC(key_events_ccc_changed,
                                   BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

static void key_events_send(struct k_work *work) {
    struct key_event_msg msg;

    while (k_msgq_get(&key_event_msgq, &msg, K_NO_WAIT) == 0) {
        if (!atomic_get(&subscribed)) {
            continue;
        }

        int err = bt_gatt_notify(NULL, &key_events_svc.attrs[1], msg.data, msg.len);
        if (err < 0 && err != -ENOTCONN) {
            LOG_WRN("Key events notify failed: %d", err);
        }
    }
}

static K_WORK_DEFINE(key_events_work, key_events_send);

static void key_events_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    atomic_set(&subscribed, value == BT_GATT_CCC_NOTIFY);

    if (value == BT_GATT_CCC_NOTIFY) {
        // Give the new subscriber the current layer state right away.
        queue_layers();
        k_work_submit(&key_events_work);
    }
}

static int key_events_listener(const zmk_event_t *eh) {
    if (!atomic_get(&subscribed)) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos != NULL) {
        struct key_event_msg msg = {
            .len = 3, .data = {KEY_EVENT_POSITION, (uint8_t)pos->position, pos->state}};
        k_msgq_put(&key_event_msgq, &msg, K_NO_WAIT);
    } else if (as_zmk_layer_state_changed(eh) != NULL) {
        queue_layers();
    }

    k_work_submit(&key_events_work);
    return ZMK_EV_EVENT_BUBBLE;
}

#if IS_ENABLED(CONFIG_INPUT)

#define TRACKBALL_SOURCES 3
#define TRACKBALL_FLUSH_MS 16

struct trackball_accum {
    int32_t dx;
    int32_t dy;
    uint8_t reports;
};

static struct trackball_accum trackball_accum[TRACKBALL_SOURCES];
static struct k_spinlock trackball_lock;

static void trackball_flush(struct k_work *work) {
    for (uint8_t src = 0; src < TRACKBALL_SOURCES; src++) {
        k_spinlock_key_t key = k_spin_lock(&trackball_lock);
        struct trackball_accum acc = trackball_accum[src];
        trackball_accum[src] = (struct trackball_accum){0};
        k_spin_unlock(&trackball_lock, key);

        if (acc.reports == 0) {
            continue;
        }

        struct key_event_msg msg = {.len = 7, .data = {KEY_EVENT_TRACKBALL, src}};
        sys_put_le16((uint16_t)CLAMP(acc.dx, INT16_MIN, INT16_MAX), &msg.data[2]);
        sys_put_le16((uint16_t)CLAMP(acc.dy, INT16_MIN, INT16_MAX), &msg.data[4]);
        msg.data[6] = acc.reports;
        k_msgq_put(&key_event_msgq, &msg, K_NO_WAIT);
    }
    key_events_send(NULL);
}

static K_WORK_DELAYABLE_DEFINE(trackball_flush_work, trackball_flush);

static uint8_t trackball_source(const struct device *dev) {
#if DT_NODE_HAS_STATUS(DT_NODELABEL(trackball_central), okay)
    if (dev == DEVICE_DT_GET(DT_NODELABEL(trackball_central))) {
        return 0;
    }
#endif
#if DT_NODE_HAS_STATUS(DT_NODELABEL(trackball_peripheral_split), okay)
    if (dev == DEVICE_DT_GET(DT_NODELABEL(trackball_peripheral_split))) {
        return 1;
    }
#endif
    return 2;
}

// Input callbacks run in name order and share the event, so the "aaa" prefix
// places this one before the input listeners that apply input processors.
static void aaa_key_events_raw_input(struct input_event *evt) {
    if (!atomic_get(&subscribed) || evt->type != INPUT_EV_REL) {
        return;
    }

    struct trackball_accum *acc = &trackball_accum[trackball_source(evt->dev)];
    k_spinlock_key_t key = k_spin_lock(&trackball_lock);
    if (evt->code == INPUT_REL_X) {
        acc->dx += evt->value;
    } else if (evt->code == INPUT_REL_Y) {
        acc->dy += evt->value;
    }
    if (evt->sync && acc->reports < UINT8_MAX) {
        acc->reports++;
    }
    k_spin_unlock(&trackball_lock, key);

    k_work_schedule(&trackball_flush_work, K_MSEC(TRACKBALL_FLUSH_MS));
}

INPUT_CALLBACK_DEFINE(NULL, aaa_key_events_raw_input);

#endif // IS_ENABLED(CONFIG_INPUT)

ZMK_LISTENER(key_events_gatt, key_events_listener);
ZMK_SUBSCRIPTION(key_events_gatt, zmk_position_state_changed);
ZMK_SUBSCRIPTION(key_events_gatt, zmk_layer_state_changed);
