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
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
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
};

struct key_event_msg {
    uint8_t len;
    uint8_t data[5];
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

ZMK_LISTENER(key_events_gatt, key_events_listener);
ZMK_SUBSCRIPTION(key_events_gatt, zmk_position_state_changed);
ZMK_SUBSCRIPTION(key_events_gatt, zmk_layer_state_changed);
