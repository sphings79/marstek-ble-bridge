#include "ble_central.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "ble";

// The Marstek service and its two characteristics, as used by the storages: 0xFF00 with 0xFF01 to
// write to and 0xFF02 delivering notifications.
#define SVC_UUID 0xFF00
#define TX_UUID  0xFF01
#define RX_UUID  0xFF02
// Third characteristic the module exposes; its role is unknown, so it is only read and logged.
#define AUX_UUID 0xFF06

// Only Marstek devices are of interest, and the advertised name is what identifies them.
#define NAME_PREFIX "MST_"

// Standard GAP characteristic carrying the device name. Read after connecting so the bridge does
// not depend on having scanned first - after a reboot it connects straight to the bound address,
// and the name is what selects the dashboard and guards firmware updates against the wrong model.
#define GAP_DEVICE_NAME_UUID 0x2A00

#define MAX_SCAN_RESULTS 16

/**
 * Everything that belongs to one storage. A slot is a place for one connection; which device sits
 * in it is decided by the caller, the radio code only keeps the link and what was learnt on it.
 */
typedef struct {
    uint16_t conn_handle;
    uint16_t tx_handle;
    uint16_t rx_handle;
    uint16_t rx_cccd_handle;
    uint16_t tx_cccd_handle;

    char name[BLE_NAME_MAX];
    /** Name from the binding or the scan; the device's own GAP name replaces `name` after connecting. */
    char known_name[BLE_NAME_MAX];
    char address[BLE_ADDR_STR_LEN];
    int8_t rssi;
    uint16_t mtu;
    bool subscribed;
    bool subscribed_tx;
    uint8_t tx_props;
    uint8_t rx_props;
    uint16_t cccd_written;
    bool encrypted;
    uint16_t last_enc_status;
    ble_stats_t stats;

    /** Last notification, or the moment the link came up. Tells a talking link from a mute one. */
    int64_t last_notify_us;
    int64_t last_write_us;
    /** Subscription done and name read: from here on the storage answers requests. */
    bool ready;

    ble_addr_t target;
    bool have_target;
    /** A connection attempt is running or waiting its turn. */
    bool connect_pending;
    /** Asked for while another slot was still connecting; started when that one is done. */
    bool queued;

    /** Handle of the module's third characteristic (0xFF06), 0 if the device has none. */
    uint16_t aux_handle;
    uint8_t aux_props;
    int64_t aux_read_at_us;
    /** Mute recovery (see the supervisor at the end of the file). */
    uint8_t mute_step;
    bool supervised;
    int64_t mute_next_us;
    int64_t mute_probe_us;
} slot_t;

static slot_t s_slots[BLE_MAX_SLOTS];

static ble_notify_cb_t s_on_notify;
static ble_state_cb_t s_on_state;
static ble_scan_cb_t s_on_scan;

static bool s_host_ready = false;

/** Slot whose ble_gap_connect() is outstanding, or -1. The controller starts one at a time. */
static int s_connecting = -1;

static ble_device_t s_scan_results[MAX_SCAN_RESULTS];
static size_t s_scan_count = 0;

static int on_gap_event_slot(struct ble_gap_event *event, void *arg);
static void supervisor_start(void);

/* ----------------------------------------------------------------------------- experiments */

#define EXP_RANDOM_ADDRESS 1
#define EXP_MTU_512 2
#define EXP_APP_READS 4
#define EXP_APP_HEARTBEAT 8
#define EXP_APP_REPLAY 16
#define EXP_SMALL_PACKETS 64

static uint8_t s_experiment = 0;
static bool s_random_address_ready = false;

void ble_central_set_experiment(uint8_t bits)
{
    s_experiment = bits;
}

uint8_t ble_central_experiment(void)
{
    return s_experiment;
}

/* ------------------------------------------------------------------------------------ event log */

#define LOG_ENTRIES 320

typedef struct {
    uint32_t ms;
    uint8_t type;
    uint8_t slot;
    uint16_t a;
    uint16_t b;
} log_entry_t;

static log_entry_t s_log[LOG_ENTRIES];
static uint32_t s_log_count = 0;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;

void ble_central_log(ble_event_t type, uint8_t slot, uint16_t a, uint16_t b)
{
    // Three tasks write here: the NimBLE host, the HTTP server and the timers.
    portENTER_CRITICAL(&s_log_lock);
    log_entry_t *e = &s_log[s_log_count % LOG_ENTRIES];
    e->ms = (uint32_t) (esp_timer_get_time() / 1000);
    e->type = (uint8_t) type;
    e->slot = slot;
    e->a = a;
    e->b = b;
    s_log_count++;
    portEXIT_CRITICAL(&s_log_lock);
}

static const char *event_name(uint8_t type)
{
    switch (type) {
        case BLE_EV_CONNECT_START:   return "connect-start";
        case BLE_EV_LINK_UP:         return "link-up";
        case BLE_EV_CONNECT_FAILED:  return "connect-failed";
        case BLE_EV_READY:           return "ready";
        case BLE_EV_DISCONNECT:      return "disconnect";
        case BLE_EV_ENC:             return "encryption";
        case BLE_EV_MTU:             return "mtu";
        case BLE_EV_WRITE:           return "write";
        case BLE_EV_NOTIFY:          return "notify";
        case BLE_EV_KEEPALIVE:       return "keepalive";
        case BLE_EV_KEEPALIVE_MISS:  return "keepalive-miss";
        case BLE_EV_CYCLE:           return "cycle";
        case BLE_EV_GAP_OTHER:       return "gap-event";
        case BLE_EV_CONN_PARAMS:     return "conn-params";
        case BLE_EV_SUBSCRIBE:       return "subscribe";
        case BLE_EV_RECOVERY:        return "mute-recovery";
        case BLE_EV_AUX:             return "ff06";
        default:                     return "?";
    }
}

void ble_central_log_dump(ble_log_line_cb_t cb, void *ctx)
{
    uint32_t count;
    portENTER_CRITICAL(&s_log_lock);
    count = s_log_count;
    portEXIT_CRITICAL(&s_log_lock);

    const uint32_t shown = count < LOG_ENTRIES ? count : LOG_ENTRIES;
    const uint32_t first = count - shown;

    for (uint32_t i = 0; i < shown; i++) {
        // One entry at a time under the lock. The log may move on while it is being read, which is
        // fine for a diagnostic listing and keeps this off the stack and out of RAM.
        log_entry_t entry;
        portENTER_CRITICAL(&s_log_lock);
        entry = s_log[(first + i) % LOG_ENTRIES];
        portEXIT_CRITICAL(&s_log_lock);

        const log_entry_t *e = &entry;
        char line[96];

        switch (e->type) {
            case BLE_EV_WRITE:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s %u bytes, cmd 0x%02x%s\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->a,
                         (unsigned) (e->b & 0xff), (e->b & 0x8000) ? " (FAILED)" : "");
                break;
            case BLE_EV_NOTIFY:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s %u bytes, cmd 0x%02x, handle %u\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->a,
                         (unsigned) (e->b & 0xff), (unsigned) (e->b >> 8));
                break;
            case BLE_EV_CONN_PARAMS:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s interval %.2f ms, supervision %u ms\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), e->a * 1.25,
                         (unsigned) e->b * 10);
                break;
            case BLE_EV_SUBSCRIBE:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s %s characteristic, status %u\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type),
                         e->a == 2 ? "write (skipped)" : (e->a ? "write" : "notify"), (unsigned) e->b);
                break;
            case BLE_EV_RECOVERY: {
                static const char *const what[] = {
                    "mute detected", "probe only", "re-write the notify descriptor",
                    "notify off, then on", "connection parameter update", "read FF06",
                    "reconnect", "supervision active" };
                const unsigned step = e->a < 8 ? e->a : 0;
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s step %u (%s): %s\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), step, what[step],
                         e->b == 1 ? "ANSWERED AFTER THIS" : (e->b == 2 ? "link ended" : (step == 7 ? "Venus D recognised" : "tried")));
                break;
            }
            case BLE_EV_AUX:
                if (e->a == 0xF006) {
                    snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s found, properties 0x%02x\n",
                             e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->b);
                } else if (e->a == 0xFFFF) {
                    snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s read failed, status %u\n",
                             e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->b);
                } else {
                    snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s read: %02x %02x %02x %02x\n",
                             e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (e->a >> 8) & 0xff,
                             e->a & 0xff, (e->b >> 8) & 0xff, e->b & 0xff);
                }
                break;
            case BLE_EV_GAP_OTHER:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s type %u (%u)\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->a, (unsigned) e->b);
                break;
            case BLE_EV_DISCONNECT:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s reason 0x%x\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->a);
                break;
            default:
                snprintf(line, sizeof(line), "%9.3f s  slot %u  %-14s %u %u\n",
                         e->ms / 1000.0, (unsigned) e->slot, event_name(e->type), (unsigned) e->a,
                         (unsigned) e->b);
        }
        cb(line, ctx);
    }
}

static void log_conn_params(uint8_t slot, uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        ble_central_log(BLE_EV_CONN_PARAMS, slot, desc.conn_itvl, desc.supervision_timeout);
    }
}

static void reset_link(slot_t *s)
{
    s->conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s->tx_handle = s->rx_handle = s->rx_cccd_handle = s->tx_cccd_handle = 0;
    s->aux_handle = 0;
    s->aux_props = 0;
    s->aux_read_at_us = 0;
    s->mute_step = 0;
    s->subscribed = s->subscribed_tx = false;
    s->encrypted = false;
    s->ready = false;
    s->mtu = 23;
}

/** The slot a callback was registered for travels as its argument. */
static inline uint8_t slot_of(void *arg)
{
    return (uint8_t) (uintptr_t) arg;
}

static inline void *arg_of(uint8_t slot)
{
    return (void *) (uintptr_t) slot;
}

static inline bool valid_slot(uint8_t slot)
{
    return slot < BLE_MAX_SLOTS;
}

static void set_state(uint8_t slot, ble_state_t state, const char *msg)
{
    if (s_on_state) {
        s_on_state(slot, state, msg);
    }
}

/**
 * The vendor app, right after connecting, asks for a 7.5 ms interval and about a second later for
 * 48.75 ms (both with a 5 s supervision timeout). Whether the Venus D cares is not known - it keeps
 * answering the app for minutes and stops answering this bridge after about 28 s - so the bridge
 * does the same.
 */
#define PARAM_RESTORE_DELAY_MS 900

static esp_timer_handle_t s_param_timer[BLE_MAX_SLOTS];

static void request_params(uint16_t conn_handle, uint16_t itvl)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = itvl,
        .itvl_max = itvl,
        .latency = 0,
        .supervision_timeout = 500,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    const int rc = ble_gap_update_params(conn_handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "Connection update to interval %u failed: %d", (unsigned) itvl, rc);
    }
}

static void param_restore(void *arg)
{
    const uint8_t slot = slot_of(arg);
    if (s_slots[slot].conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        request_params(s_slots[slot].conn_handle, 0x0027);
    }
}

static void start_param_dance(uint8_t slot, uint16_t conn_handle)
{
    if (!s_param_timer[slot]) {
        const esp_timer_create_args_t args = { .callback = &param_restore, .arg = arg_of(slot), .name = "ble_params" };
        if (esp_timer_create(&args, &s_param_timer[slot]) != ESP_OK) {
            return;
        }
    }

    request_params(conn_handle, 0x0006);
    esp_timer_stop(s_param_timer[slot]);
    esp_timer_start_once(s_param_timer[slot], (uint64_t) PARAM_RESTORE_DELAY_MS * 1000);
}

static void addr_to_str(const ble_addr_t *addr, char *out)
{
    const uint8_t *v = addr->val;   /* NimBLE stores the address little-endian */
    snprintf(out, BLE_ADDR_STR_LEN, "%02x:%02x:%02x:%02x:%02x:%02x",
             v[5], v[4], v[3], v[2], v[1], v[0]);
}

static bool str_to_addr(const char *str, uint8_t type, ble_addr_t *out)
{
    unsigned v[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }

    for (int i = 0; i < 6; i++) {
        out->val[i] = (uint8_t) v[5 - i];
    }
    out->type = type;

    return true;
}

/* ------------------------------------------------------------------ notifications & discovery */

/**
 * Ask the storage for a bigger ATT MTU.
 *
 * NimBLE does not do this on its own, and the default of 23 leaves 20 usable bytes per
 * notification. Browsers negotiate ~185 without being asked, which is why the same commands work
 * over Web Bluetooth and produced silence here: a hundred-byte response has to survive the trip
 * in one or two pieces rather than six.
 */
static int on_mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t mtu, void *arg)
{
    (void) conn_handle;
    slot_t *s = &s_slots[slot_of(arg)];

    if (error->status == 0) {
        s->mtu = mtu;
        ESP_LOGI(TAG, "[%u] MTU negotiated: %u bytes", slot_of(arg), (unsigned) mtu);
    } else {
        ESP_LOGW(TAG, "[%u] MTU exchange refused (%d), staying at %u", slot_of(arg),
                 error->status, (unsigned) s->mtu);
    }

    return 0;
}

/**
 * The vendor app's first requests after connecting, besides the state: modem version, VID and
 * vid/xid, all reads. Frames as seen in a capture of the app.
 */
static const uint8_t s_app_reads[][8] = {
    { 0x73, 0x08, 0x23, 0x11, 0x0a, 0x0b, 0x0c, 0x44 },
    { 0x73, 0x07, 0x23, 0x24, 0x0a, 0x0b, 0x72, 0x00 },
    { 0x73, 0x06, 0x23, 0x51, 0x0c, 0x0b, 0x00, 0x00 },
};
static const uint8_t s_app_reads_len[] = { 8, 7, 6 };

static esp_timer_handle_t s_reads_timer[BLE_MAX_SLOTS];
static uint8_t s_reads_next[BLE_MAX_SLOTS];

static void send_app_read(void *arg)
{
    const uint8_t slot = slot_of(arg);
    const uint8_t i = s_reads_next[slot];
    if (i >= sizeof(s_app_reads_len) || !ble_central_is_connected(slot)) {
        return;
    }

    ble_central_write(slot, s_app_reads[i], s_app_reads_len[i], false);
    s_reads_next[slot] = i + 1;

    if (i + 1 < sizeof(s_app_reads_len)) {
        esp_timer_start_once(s_reads_timer[slot], 700 * 1000);
    }
}

/**
 * The vendor app's steady polling, as seen in a capture: the state (0x03) and the setting (0x0a),
 * each as a 6-byte frame with the payload byte 0x01, one of them every 3 s. This bridge and the web
 * app ask with a 5-byte frame without payload, and the Venus D stops answering about 28 s after
 * connecting; the app, which keeps its connection for minutes, never sends that form after the first
 * seconds.
 */
static const uint8_t s_heartbeat[][6] = {
    { 0x73, 0x06, 0x23, 0x03, 0x01, 0x54 },
    { 0x73, 0x06, 0x23, 0x0a, 0x01, 0x5d },
};

static esp_timer_handle_t s_beat_timer[BLE_MAX_SLOTS];
static uint8_t s_beat_next[BLE_MAX_SLOTS];

static void send_heartbeat(void *arg)
{
    const uint8_t slot = slot_of(arg);
    if (!(s_experiment & EXP_APP_HEARTBEAT) || !ble_central_is_ready(slot)) {
        return;
    }

    ble_central_write_quiet(slot, s_heartbeat[s_beat_next[slot]], sizeof(s_heartbeat[0]), false);
    s_beat_next[slot] = (uint8_t) ((s_beat_next[slot] + 1) % 2);
    esp_timer_start_once(s_beat_timer[slot], 3000 * 1000);
}

static void start_heartbeat(uint8_t slot)
{
    if (!s_beat_timer[slot]) {
        const esp_timer_create_args_t args = { .callback = &send_heartbeat, .arg = arg_of(slot), .name = "ble_beat" };
        if (esp_timer_create(&args, &s_beat_timer[slot]) != ESP_OK) {
            return;
        }
    }
    s_beat_next[slot] = 0;
    esp_timer_stop(s_beat_timer[slot]);
    esp_timer_start_once(s_beat_timer[slot], 1000 * 1000);
}

/**
 * The vendor app's whole session, replayed. The sequence and the timing are those of the longest
 * connection in a capture of the app (every write of it): state and setting first, then modem
 * version, VID, the state again, VID, the setting, vid/xid, and from there the state and the setting
 * alternately every 3 s. Delays are from the previous frame, starting at the moment the bridge is
 * ready, which is about when the app wrote its notification descriptor.
 */
typedef struct {
    uint16_t delay_ms;
    uint8_t frame;   /* index into s_replay_frames */
} replay_step_t;

static const uint8_t s_replay_frames[][8] = {
    { 0x73, 0x06, 0x23, 0x03, 0x01, 0x54, 0x00, 0x00 },   /* 0: state */
    { 0x73, 0x06, 0x23, 0x0a, 0x01, 0x5d, 0x00, 0x00 },   /* 1: setting */
    { 0x73, 0x08, 0x23, 0x11, 0x0a, 0x0b, 0x0c, 0x44 },   /* 2: modem version */
    { 0x73, 0x07, 0x23, 0x24, 0x0a, 0x0b, 0x72, 0x00 },   /* 3: VID */
    { 0x73, 0x06, 0x23, 0x51, 0x0c, 0x0b, 0x00, 0x00 },   /* 4: vid/xid */
};
static const uint8_t s_replay_len[] = { 6, 6, 8, 7, 6 };

static const replay_step_t s_replay_steps[] = {
    { 650, 0 }, { 490, 1 }, { 1010, 2 }, { 2000, 0 }, { 1610, 3 }, { 1390, 1 },
    { 2020, 4 }, { 980, 0 }, { 3000, 1 },
};

static esp_timer_handle_t s_replay_timer[BLE_MAX_SLOTS];
static uint16_t s_replay_pos[BLE_MAX_SLOTS];

static void replay_step(void *arg)
{
    const uint8_t slot = slot_of(arg);
    if (!(s_experiment & EXP_APP_REPLAY) || !ble_central_is_ready(slot)) {
        return;
    }

    const size_t steps = sizeof(s_replay_steps) / sizeof(s_replay_steps[0]);
    const uint16_t pos = s_replay_pos[slot];
    uint8_t frame;
    uint32_t next_ms;

    if (pos < steps) {
        frame = s_replay_steps[pos].frame;
    } else {
        // Steady state: the state and the setting in turn, every 3 s, the state first.
        frame = (uint8_t) ((pos - steps) % 2 == 0 ? 0 : 1);
    }
    ble_central_write(slot, s_replay_frames[frame], s_replay_len[frame], false);

    s_replay_pos[slot] = pos + 1;
    next_ms = (pos + 1 < steps) ? s_replay_steps[pos + 1].delay_ms : 3000;
    esp_timer_start_once(s_replay_timer[slot], (uint64_t) next_ms * 1000);
}

static void start_replay(uint8_t slot)
{
    if (!s_replay_timer[slot]) {
        const esp_timer_create_args_t args = { .callback = &replay_step, .arg = arg_of(slot), .name = "ble_replay" };
        if (esp_timer_create(&args, &s_replay_timer[slot]) != ESP_OK) {
            return;
        }
    }
    s_replay_pos[slot] = 0;
    esp_timer_stop(s_replay_timer[slot]);
    esp_timer_start_once(s_replay_timer[slot], (uint64_t) s_replay_steps[0].delay_ms * 1000);
}

static void start_app_reads(uint8_t slot)
{
    if (!s_reads_timer[slot]) {
        const esp_timer_create_args_t args = { .callback = &send_app_read, .arg = arg_of(slot), .name = "ble_reads" };
        if (esp_timer_create(&args, &s_reads_timer[slot]) != ESP_OK) {
            return;
        }
    }
    s_reads_next[slot] = 0;
    esp_timer_stop(s_reads_timer[slot]);
    esp_timer_start_once(s_reads_timer[slot], 400 * 1000);
}

static void announce_connected(uint8_t slot)
{
    ESP_LOGI(TAG, "[%u] Connected to %s (%s)", slot, s_slots[slot].name, s_slots[slot].address);
    s_slots[slot].ready = true;
    ble_central_log(BLE_EV_READY, slot, 0, 0);
    if (s_slots[slot].aux_handle) {
        s_slots[slot].aux_read_at_us = esp_timer_get_time() + 1000 * 1000;
    }
    if (s_experiment & EXP_APP_REPLAY) {
        // The whole session replaces the smaller experiments.
        start_replay(slot);
    } else {
        if (s_experiment & EXP_APP_READS) {
            start_app_reads(slot);
        }
        if (s_experiment & EXP_APP_HEARTBEAT) {
            start_heartbeat(slot);
        }
    }
    set_state(slot, BLE_STATE_CONNECTED, NULL);
}

static int on_name_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    const uint8_t slot = slot_of(arg);
    slot_t *s = &s_slots[slot];

    if (error->status == 0 && attr && attr->om) {
        const uint16_t len = OS_MBUF_PKTLEN(attr->om);
        const uint16_t copy = len < BLE_NAME_MAX - 1 ? len : BLE_NAME_MAX - 1;
        if (ble_hs_mbuf_to_flat(attr->om, s->name, copy, NULL) == 0) {
            s->name[copy] = '\0';
            ESP_LOGI(TAG, "[%u] Device reports its name as \"%s\"", slot, s->name);
        }
        return 0;
    }

    // Nothing read, or the read finished: report the link either way. A device that will not tell
    // us its name still works; the name kept from the scan or the binding stands in.
    if (error->status == BLE_HS_EDONE || error->status != 0) {
        if (s->name[0] == '\0') {
            ESP_LOGW(TAG, "[%u] Device did not report a name", slot);
        }
        announce_connected(slot);
    }

    return 0;
}

static int on_tx_subscribe_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                                struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    (void) attr;
    const uint8_t slot = slot_of(arg);

    s_slots[slot].subscribed_tx = (error->status == 0);
    ble_central_log(BLE_EV_SUBSCRIBE, slot, 1, (uint16_t) error->status);
    ESP_LOGI(TAG, "[%u] Second subscription on the write characteristic: %s", slot,
             s_slots[slot].subscribed_tx ? "accepted" : "refused");
    return 0;
}

static int on_tx_descriptors(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void) chr_val_handle;
    slot_t *s = &s_slots[slot_of(arg)];

    if (error->status == 0 && dsc && s->tx_cccd_handle == 0 &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s->tx_cccd_handle = dsc->handle;
        return 0;
    }

    if (error->status == BLE_HS_EDONE && s->tx_cccd_handle != 0) {
        static const uint8_t enable[2] = { 0x01, 0x00 };
        ble_gattc_write_flat(conn_handle, s->tx_cccd_handle, enable, sizeof(enable),
                             on_tx_subscribe_done, arg);
    }

    return 0;
}

/**
 * Whether to leave the write characteristic's notifications alone.
 *
 * Browsers subscribe to the notify characteristic only, and the Venus D answers on it. The extra
 * subscription on the write characteristic exists for storages that answer there instead, and on
 * a Venus D the replies stopped about 25 s after it - which no other difference in the connection
 * set-up explains. The name comes from the binding; a slot without one keeps the old behaviour.
 */
static bool skip_tx_subscription(const slot_t *s)
{
    return strncmp(s->name, "MST_VNSD", 8) == 0;
}

static int on_subscribe_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void) attr;
    const uint8_t slot = slot_of(arg);
    slot_t *s = &s_slots[slot];

    if (error->status != 0) {
        ESP_LOGE(TAG, "[%u] Subscribing to notifications failed: %d", slot, error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(slot, BLE_STATE_ERROR, "Could not subscribe to notifications");
        return 0;
    }

    s->subscribed = true;
    ble_central_log(BLE_EV_SUBSCRIBE, slot, 0, 0);

    // The write characteristic advertises notify as well. Subscribing to both costs one more
    // descriptor write and removes the guess about which one the device actually uses.
    if (skip_tx_subscription(s)) {
        ble_central_log(BLE_EV_SUBSCRIBE, slot, 2, 0);
    } else {
        ble_gattc_disc_all_dscs(conn_handle, s->tx_handle, s->rx_handle, on_tx_descriptors, arg);
    }

    const ble_uuid16_t name_uuid = BLE_UUID16_INIT(GAP_DEVICE_NAME_UUID);
    if (ble_gattc_read_by_uuid(conn_handle, 1, 0xffff, &name_uuid.u, on_name_read, arg) != 0) {
        announce_connected(slot);
    }

    return 0;
}

static int on_descriptors(uint16_t conn_handle, const struct ble_gatt_error *error,
                          uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void) chr_val_handle;
    const uint8_t slot = slot_of(arg);
    slot_t *s = &s_slots[slot];

    // Keep the FIRST one found. Discovery runs from the RX value handle upwards to the end of the
    // table, so later characteristics have configuration descriptors too - overwriting would leave
    // us enabling notifications on some unrelated characteristic, and nothing would ever arrive.
    if (error->status == 0 && dsc && s->rx_cccd_handle == 0 &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s->rx_cccd_handle = dsc->handle;
        ESP_LOGI(TAG, "[%u] Notification descriptor at handle %u", slot, (unsigned) dsc->handle);
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        return 0;
    }

    if (s->rx_cccd_handle == 0) {
        ESP_LOGE(TAG, "[%u] Notification descriptor not found", slot);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(slot, BLE_STATE_ERROR, "Notification descriptor not found");
        return 0;
    }

    // Which bit to set depends on what the characteristic actually offers. Notify and indicate are
    // different values in the same descriptor, and a device that only indicates will accept the
    // notify bit without complaint and then never send anything - which looks exactly like a
    // working subscription that produces no data.
    const bool can_notify = (s->rx_props & BLE_GATT_CHR_PROP_NOTIFY) != 0;
    const bool can_indicate = (s->rx_props & BLE_GATT_CHR_PROP_INDICATE) != 0;

    s->cccd_written = can_notify ? 0x0001 : (can_indicate ? 0x0002 : 0x0001);

    ESP_LOGI(TAG, "[%u] RX properties 0x%02x (notify=%d, indicate=%d), enabling 0x%04x", slot,
             s->rx_props, can_notify, can_indicate, s->cccd_written);

    const uint8_t enable[2] = { (uint8_t) (s->cccd_written & 0xff), (uint8_t) (s->cccd_written >> 8) };
    ble_gattc_write_flat(conn_handle, s->rx_cccd_handle, enable, sizeof(enable),
                         on_subscribe_done, arg);
    return 0;
}

static int on_characteristics(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_chr *chr, void *arg)
{
    const uint8_t slot = slot_of(arg);
    slot_t *s = &s_slots[slot];

    if (error->status == 0 && chr) {
        const uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        if (uuid == TX_UUID) {
            s->tx_handle = chr->val_handle;
            s->tx_props = chr->properties;
        } else if (uuid == RX_UUID) {
            s->rx_handle = chr->val_handle;
            s->rx_props = chr->properties;
        } else if (uuid == AUX_UUID) {
            s->aux_handle = chr->val_handle;
            s->aux_props = chr->properties;
            // 0xF006 marks "properties of 0xFF06" in the log.
            ble_central_log(BLE_EV_AUX, slot, 0xF006, chr->properties);
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        return 0;
    }

    if (s->tx_handle == 0 || s->rx_handle == 0) {
        ESP_LOGE(TAG, "[%u] Device does not expose the expected characteristics", slot);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(slot, BLE_STATE_ERROR, "Not a Marstek storage");
        return 0;
    }

    // Descriptors live between the value handle and the end of the service; asking from the value
    // handle upwards is enough to find this characteristic's own configuration descriptor.
    ble_gattc_disc_all_dscs(conn_handle, s->rx_handle, 0xffff, on_descriptors, arg);
    return 0;
}

static int on_service(uint16_t conn_handle, const struct ble_gatt_error *error,
                      const struct ble_gatt_svc *service, void *arg)
{
    const uint8_t slot = slot_of(arg);

    if (error->status == 0 && service) {
        if (s_slots[slot].mtu <= 23) {
            ble_gattc_exchange_mtu(conn_handle, on_mtu_exchanged, arg);
        }
        ble_gattc_disc_all_chrs(conn_handle, service->start_handle, service->end_handle,
                                on_characteristics, arg);
        return 0;
    }

    if (error->status == BLE_HS_EDONE && s_slots[slot].tx_handle == 0) {
        ESP_LOGE(TAG, "[%u] Marstek service not found", slot);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        set_state(slot, BLE_STATE_ERROR, "Marstek service not found");
    }

    return 0;
}

/* -------------------------------------------------------------------------------- GAP events */

static void remember_scan_result(const struct ble_gap_disc_desc *desc)
{
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
        return;
    }
    if (fields.name == NULL || fields.name_len < strlen(NAME_PREFIX)) {
        return;
    }
    if (strncmp((const char *) fields.name, NAME_PREFIX, strlen(NAME_PREFIX)) != 0) {
        return;
    }

    char address[BLE_ADDR_STR_LEN];
    addr_to_str(&desc->addr, address);

    // Advertisements repeat; keep one entry per device and refresh its signal strength.
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_results[i].address, address) == 0) {
            s_scan_results[i].rssi = desc->rssi;
            return;
        }
    }

    if (s_scan_count >= MAX_SCAN_RESULTS) {
        return;
    }

    ble_device_t *entry = &s_scan_results[s_scan_count++];
    entry->addr_type = desc->addr.type;
    size_t name_len = fields.name_len < BLE_NAME_MAX - 1 ? fields.name_len : BLE_NAME_MAX - 1;
    memcpy(entry->name, fields.name, name_len);
    entry->name[name_len] = '\0';
    strlcpy(entry->address, address, sizeof(entry->address));
    entry->rssi = desc->rssi;

    ESP_LOGI(TAG, "Found %s (%s) at %d dBm", entry->name, entry->address, entry->rssi);
}

/**
 * Scanning is the radio's business, not one slot's: a scan can run next to any number of healthy
 * connections, so nothing here touches a slot's state. Reporting "idle" or "scanning" for a slot
 * that is happily connected would tell the app the storage had gone away.
 */
static int on_scan_event(struct ble_gap_event *event, void *arg)
{
    (void) arg;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            remember_scan_result(&event->disc);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            ESP_LOGI(TAG, "Scan finished, %u device(s)", (unsigned) s_scan_count);
            if (s_on_scan) {
                s_on_scan(s_scan_results, s_scan_count);
                s_on_scan = NULL;
            }
            break;

        default:
            break;
    }

    return 0;
}

static esp_err_t start_connect(uint8_t slot)
{
    slot_t *s = &s_slots[slot];

    // A scan in progress would block the connection attempt.
    ble_gap_disc_cancel();

    uint8_t own_addr_type;
    if (s_experiment & EXP_RANDOM_ADDRESS) {
        if (!s_random_address_ready) {
            ble_addr_t addr;
            if (ble_hs_id_gen_rnd(0, &addr) == 0 && ble_hs_id_set_rnd(addr.val) == 0) {
                s_random_address_ready = true;
            }
        }
        own_addr_type = s_random_address_ready ? BLE_OWN_ADDR_RANDOM : BLE_OWN_ADDR_PUBLIC;
    } else if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        s->connect_pending = false;
        set_state(slot, BLE_STATE_ERROR, "Could not choose a Bluetooth address");
        return ESP_FAIL;
    }

    // The vendor app's connection never extends the data length: the capture has no Data Length
    // Change event and no command for it, so its packets stay at 27 bytes. This bridge's controller
    // negotiates up to 251 by itself right after connecting (gap event 34 in the log). The
    // suggested default is what it starts from.
    ble_gap_write_sugg_def_data_len((s_experiment & EXP_SMALL_PACKETS) ? 27 : 251,
                                    (s_experiment & EXP_SMALL_PACKETS) ? 328 : 2120);

    // Takes effect when the peer's MTU request is answered, so it has to be set before the link is up.
    ble_att_set_preferred_mtu((s_experiment & EXP_MTU_512) ? 512 : 256);

    s_connecting = slot;
    s->connect_pending = true;
    {
        uint16_t sugg_octets = 0, sugg_time = 0;
        ble_gap_read_sugg_def_data_len(&sugg_octets, &sugg_time);
        ble_central_log(BLE_EV_CONNECT_START, slot, s_experiment, sugg_octets);
    }

    // The parameters the vendor app asks for (read from a Bluetooth capture of it): 30-50 ms
    // interval, no latency, and a 5 s supervision timeout - NimBLE's default is 2.56 s.
    const struct ble_gap_conn_params conn_params = {
        .scan_itvl = 0x0060,
        .scan_window = 0x0030,
        .itvl_min = 0x0018,
        .itvl_max = 0x0028,
        .latency = 0,
        .supervision_timeout = 500,
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_connect(own_addr_type, &s->target, 10000, &conn_params, on_gap_event_slot, arg_of(slot));
    if (rc != 0) {
        s_connecting = -1;
        s->connect_pending = false;
        ESP_LOGE(TAG, "[%u] Connection could not be started: %d", slot, rc);

        // The numeric reason travels to the browser too. Without it the only place it exists is a
        // serial console, which is exactly what a bridge in a cellar does not have.
        char msg[80];
        snprintf(msg, sizeof(msg), "Could not start connecting (NimBLE error %d, address type %u)",
                 rc, (unsigned) s->target.type);
        set_state(slot, BLE_STATE_ERROR, msg);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void start_next_queued(void)
{
    if (s_connecting >= 0) {
        return;
    }

    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
        if (s_slots[i].queued) {
            s_slots[i].queued = false;
            if (start_connect(i) == ESP_OK) {
                return;
            }
            // That one failed outright and already said so; the next in line still gets its turn.
        }
    }
}

static int on_gap_event_slot(struct ble_gap_event *event, void *arg)
{
    const uint8_t slot = slot_of(arg);
    slot_t *s = &s_slots[slot];

    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (s_connecting == slot) {
                s_connecting = -1;
            }
            s->connect_pending = false;

            if (event->connect.status != 0) {
                ESP_LOGE(TAG, "[%u] Connection failed: %d", slot, event->connect.status);
                ble_central_log(BLE_EV_CONNECT_FAILED, slot, (uint16_t) event->connect.status, 0);
                s->conn_handle = BLE_HS_CONN_HANDLE_NONE;
                set_state(slot, BLE_STATE_ERROR, "Could not connect to the storage");
                start_next_queued();
                break;
            }

            reset_link(s);
            s->conn_handle = event->connect.conn_handle;
            s->last_notify_us = esp_timer_get_time();
            ble_central_log(BLE_EV_LINK_UP, slot, s->conn_handle, 0);
            log_conn_params(slot, s->conn_handle);

            // No MTU request of our own yet: the Venus D asks for one itself right after it
            // connects (the vendor app's capture shows its request for 512), and answering it is
            // all the app does. Our own request would cross it; it is only a fallback now, sent
            // once the service has been found if nothing has been agreed by then.
            start_param_dance(slot, s->conn_handle);

            // No pairing is started here. An earlier version did, on the theory that a storage
            // might stay silent on an unencrypted link. The Venus D never answers the request,
            // and 30 s later - when the pairing times out - it stops answering anything on that
            // link: the event log showed every connection going quiet at exactly that moment, and
            // coming back with the next connection. Without the request the link stays usable.
            ESP_LOGI(TAG, "[%u] Link up, discovering services", slot);
            {
                const ble_uuid16_t svc = BLE_UUID16_INIT(SVC_UUID);
                ble_gattc_disc_svc_by_uuid(s->conn_handle, &svc.u, on_service, arg);
            }

            // The controller is free again; the next slot in line can start.
            start_next_queued();
            break;

        case BLE_GAP_EVENT_DISCONNECT: {
            // A link that never became usable is not the same thing as one that ended. The
            // storage regularly accepts a connection and drops it again within a second - the
            // next attempt then works - and reporting that as a plain disconnect makes pressing
            // connect look like it did nothing at all.
            const bool was_usable = s->subscribed;

            if (s_connecting == slot) {
                s_connecting = -1;
            }
            s->connect_pending = false;
            ESP_LOGI(TAG, "[%u] Disconnected, reason 0x%x", slot, event->disconnect.reason);
            ble_central_log(BLE_EV_DISCONNECT, slot, (uint16_t) event->disconnect.reason, 0);
            reset_link(s);
            // The name stays: it came from the binding, and the next connection needs it before
            // the device has been asked (see skip_tx_subscription). A new name is read on connect.

            if (was_usable) {
                set_state(slot, BLE_STATE_DISCONNECTED, NULL);
            } else {
                set_state(slot, BLE_STATE_ERROR,
                          "dropped_early|The storage dropped the connection before it was "
                          "usable. It usually accepts the next attempt.");
            }
            start_next_queued();
            break;
        }

        case BLE_GAP_EVENT_ENC_CHANGE:
            s->last_enc_status = event->enc_change.status;
            ble_central_log(BLE_EV_ENC, slot, (uint16_t) event->enc_change.status, 0);
            {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(s->conn_handle, &desc) == 0) {
                    s->encrypted = desc.sec_state.encrypted;
                }
            }
            ESP_LOGI(TAG, "[%u] Encryption change: status %d, encrypted=%d", slot,
                     event->enc_change.status, s->encrypted);
            break;

        case BLE_GAP_EVENT_MTU:
            s->mtu = event->mtu.value;
            ble_central_log(BLE_EV_MTU, slot, event->mtu.value, 0);
            ESP_LOGI(TAG, "[%u] MTU is now %u bytes", slot, (unsigned) s->mtu);
            break;

        case BLE_GAP_EVENT_NOTIFY_RX: {
            // Hand the payload up exactly as it arrived. One notification is one frame; merging or
            // splitting here would break the reassembler in the web app.
            const uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            uint8_t buf[512];
            if (len <= sizeof(buf) &&
                ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), NULL) == 0 &&
                s_on_notify) {
                s->last_notify_us = esp_timer_get_time();
                ble_central_log(BLE_EV_NOTIFY, slot, len,
                                (uint16_t) ((len > 3 ? buf[3] : 0) | ((event->notify_rx.attr_handle & 0xff) << 8)));
                s->stats.notifications++;
                s->stats.notify_bytes += len;
                ESP_LOGD(TAG, "[%u] Notification, %u bytes", slot, (unsigned) len);
                s_on_notify(slot, buf, len);
            } else {
                ESP_LOGW(TAG, "[%u] Dropped a %u byte notification", slot, (unsigned) len);
            }
            break;
        }

        case BLE_GAP_EVENT_DATA_LEN_CHG:
            ble_central_log(BLE_EV_GAP_OTHER, slot, (uint16_t) event->type,
                            (uint16_t) ((event->data_len_chg.max_tx_octets << 8) | (event->data_len_chg.max_rx_octets & 0xff)));
            break;

        case BLE_GAP_EVENT_CONN_UPDATE:
            ble_central_log(BLE_EV_GAP_OTHER, slot, (uint16_t) event->type, (uint16_t) event->conn_update.status);
            log_conn_params(slot, s->conn_handle);
            break;

        default:
            // Everything not handled above is worth a line: the silence of a storage is a question
            // about what happened on the link, and a parameter or PHY change is the kind of thing
            // that would be the answer.
            ble_central_log(BLE_EV_GAP_OTHER, slot, (uint16_t) event->type, 0);
            break;
    }

    return 0;
}

/* -------------------------------------------------------------------------------- host plumbing */

static void on_host_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_host_ready = true;
    ESP_LOGI(TAG, "Bluetooth host ready");
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth host reset, reason %d", reason);
    s_host_ready = false;
}

static void host_task(void *param)
{
    (void) param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_central_init(ble_notify_cb_t on_notify, ble_state_cb_t on_state)
{
    s_on_notify = on_notify;
    s_on_state = on_state;

    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
        reset_link(&s_slots[i]);
    }

    supervisor_start();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_host_sync;
    ble_hs_cfg.reset_cb = on_host_reset;

    nimble_port_freertos_init(host_task);

    return ESP_OK;
}

/* -------------------------------------------------------------------------------------- API */

esp_err_t ble_central_scan(uint32_t seconds, ble_scan_cb_t on_result)
{
    if (!s_host_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_count = 0;
    s_on_scan = on_result;

    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        return ESP_FAIL;
    }

    struct ble_gap_disc_params params = {
        // Active scanning asks for the scan response, which is where the storages put their name.
        .passive = 0,
        .filter_duplicates = 0,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
    };

    int rc = ble_gap_disc(own_addr_type, seconds * 1000, &params, on_scan_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Scan could not be started: %d", rc);
        s_on_scan = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

uint8_t ble_central_addr_type_for(const char *address)
{
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_results[i].address, address) == 0) {
            return s_scan_results[i].addr_type;
        }
    }
    return BLE_ADDR_TYPE_UNKNOWN;
}

esp_err_t ble_central_connect(uint8_t slot, const char *address, uint8_t addr_type)
{
    if (!valid_slot(slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    slot_t *s = &s_slots[slot];

    if (!s_host_ready) {
        ESP_LOGW(TAG, "Cannot connect: Bluetooth host not ready yet");
        return ESP_ERR_INVALID_STATE;
    }

    // Pressing connect twice is the normal thing to do when the first press seems to do nothing.
    // A second ble_gap_connect while one is outstanding just fails, so treat the repeat as a
    // no-op rather than turning it into an error the user has to interpret - and answer with the
    // state rather than with silence, which is exactly what makes it seem like nothing happened.
    // The same goes for a link that is up and still being set up: a second ble_gap_connect to a
    // device we are already connected to would fail and be reported as an error.
    if (s->connect_pending || s->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "[%u] Connection attempt already running, repeating the state", slot);
        set_state(slot, BLE_STATE_CONNECTING, NULL);
        return ESP_OK;
    }

    if (ble_central_is_connected(slot)) {
        ESP_LOGI(TAG, "[%u] Already connected", slot);
        set_state(slot, BLE_STATE_CONNECTED, NULL);
        return ESP_OK;
    }

    if (addr_type == BLE_ADDR_TYPE_UNKNOWN) {
        addr_type = ble_central_addr_type_for(address);
    }
    if (addr_type == BLE_ADDR_TYPE_UNKNOWN) {
        // Nothing better to go on. Public is the common case, but say so - if the connection
        // fails, this is the first thing to suspect.
        ESP_LOGW(TAG, "[%u] No address type known for %s, assuming public", slot, address);
        addr_type = BLE_ADDR_PUBLIC;
    }

    if (!str_to_addr(address, addr_type, &s->target)) {
        return ESP_ERR_INVALID_ARG;
    }
    s->have_target = true;
    strlcpy(s->address, address, sizeof(s->address));

    set_state(slot, BLE_STATE_CONNECTING, NULL);

    // The controller connects to one device at a time. Another slot is mid-attempt, so this one
    // waits its turn instead of failing - a page that reconnects all its slots in a row would
    // otherwise lose every one but the first.
    if (s_connecting >= 0) {
        s->queued = true;
        s->connect_pending = true;
        ESP_LOGI(TAG, "[%u] Waiting for slot %d to finish connecting", slot, s_connecting);
        return ESP_OK;
    }

    return start_connect(slot);
}

void ble_central_disconnect(uint8_t slot)
{
    if (!valid_slot(slot)) {
        return;
    }
    slot_t *s = &s_slots[slot];

    // Still waiting for its turn: just forget the request.
    if (s->queued) {
        s->queued = false;
        s->connect_pending = false;
        set_state(slot, BLE_STATE_IDLE, NULL);
        return;
    }

    if (s->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else if (s_connecting == slot) {
        // Mid-attempt, so there is no link to terminate yet. The cancel ends the attempt with a
        // connect event, which then starts whoever was waiting.
        ble_gap_conn_cancel();
    }
}

void ble_central_disconnect_all(void)
{
    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
        ble_central_disconnect(i);
    }
}

bool ble_central_is_connected(uint8_t slot)
{
    if (!valid_slot(slot)) {
        return false;
    }
    return s_slots[slot].conn_handle != BLE_HS_CONN_HANDLE_NONE && s_slots[slot].tx_handle != 0;
}

int64_t ble_central_last_notify_us(uint8_t slot)
{
    return valid_slot(slot) ? s_slots[slot].last_notify_us : 0;
}

bool ble_central_is_busy(uint8_t slot)
{
    if (!valid_slot(slot)) {
        return false;
    }
    return s_slots[slot].connect_pending || s_slots[slot].conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

const char *ble_central_device_name(uint8_t slot)
{
    return valid_slot(slot) ? s_slots[slot].name : "";
}

const char *ble_central_device_address(uint8_t slot)
{
    return valid_slot(slot) && s_slots[slot].have_target ? s_slots[slot].address : "";
}

uint16_t ble_central_mtu(uint8_t slot)
{
    return valid_slot(slot) ? s_slots[slot].mtu : 23;
}

void ble_central_stats(uint8_t slot, ble_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!valid_slot(slot)) {
        return;
    }
    const slot_t *s = &s_slots[slot];

    *out = s->stats;
    out->mtu = s->mtu;
    out->tx_handle = s->tx_handle;
    out->rx_handle = s->rx_handle;
    out->cccd_handle = s->rx_cccd_handle;
    out->tx_props = s->tx_props;
    out->rx_props = s->rx_props;
    out->cccd_written = s->cccd_written;
    out->subscribed = s->subscribed;
    out->subscribed_tx = s->subscribed_tx;
    out->encrypted = s->encrypted;
    out->last_enc_status = s->last_enc_status;
    out->connected = ble_central_is_connected(slot);
}

int8_t ble_central_rssi(uint8_t slot)
{
    if (!valid_slot(slot)) {
        return 0;
    }
    slot_t *s = &s_slots[slot];

    if (s->conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        int8_t rssi;
        if (ble_gap_conn_rssi(s->conn_handle, &rssi) == 0) {
            s->rssi = rssi;
        }
    }
    return s->rssi;
}

/** Tells us what the device did with a write, which the queueing return value does not. */
static int on_write_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    (void) attr;
    const uint8_t slot = slot_of(arg);
    slot_t *s = &s_slots[slot];

    if (error->status == 0) {
        s->stats.write_acks++;
    } else {
        s->stats.write_rejects++;
        s->stats.last_write_error = error->status;
        ESP_LOGW(TAG, "[%u] Device rejected a write: status %d", slot, error->status);
    }

    return 0;
}

static esp_err_t write_to_slot(uint8_t slot, const uint8_t *data, size_t len, bool with_response, bool quiet)
{
    if (!ble_central_is_connected(slot)) {
        return ESP_ERR_INVALID_STATE;
    }
    slot_t *s = &s_slots[slot];
    s->last_write_us = esp_timer_get_time();

    // Ask for an acknowledged write only if the characteristic actually offers one. Browsers pick
    // this way too - Chrome's writeValue() looks at the properties and silently uses the
    // unacknowledged form when that is all there is. Sending a write request to a characteristic
    // that only accepts write-without-response gets it refused, and without a callback that
    // refusal is invisible: the queueing call still returns success.
    const bool supports_ack = (s->tx_props & BLE_GATT_CHR_PROP_WRITE) != 0;
    const bool supports_no_ack = (s->tx_props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
    const bool acknowledged = with_response ? supports_ack : (supports_no_ack ? false : supports_ack);

    int rc = acknowledged
        ? ble_gattc_write_flat(s->conn_handle, s->tx_handle, data, len, on_write_done, arg_of(slot))
        : ble_gattc_write_no_rsp_flat(s->conn_handle, s->tx_handle, data, len);

    if (!quiet) {
        ble_central_log(BLE_EV_WRITE, slot, (uint16_t) len,
                        (uint16_t) ((len > 3 ? data[3] : 0) | (rc != 0 ? 0x8000 : 0)));
    }

    if (rc != 0) {
        s->stats.writes_failed++;
        ESP_LOGW(TAG, "[%u] Write of %u bytes failed: %d", slot, (unsigned) len, rc);
        return ESP_FAIL;
    }

    s->stats.writes_ok++;
    return ESP_OK;
}

esp_err_t ble_central_write(uint8_t slot, const uint8_t *data, size_t len, bool with_response)
{
    return write_to_slot(slot, data, len, with_response, false);
}

esp_err_t ble_central_write_quiet(uint8_t slot, const uint8_t *data, size_t len, bool with_response)
{
    return write_to_slot(slot, data, len, with_response, true);
}

int64_t ble_central_last_write_us(uint8_t slot)
{
    return valid_slot(slot) ? s_slots[slot].last_write_us : 0;
}

bool ble_central_is_ready(uint8_t slot)
{
    return valid_slot(slot) && s_slots[slot].ready && ble_central_is_connected(slot);
}

/** Remember the name a scan saw, so the app can identify the model without a round trip. */
void ble_central_set_device_name(uint8_t slot, const char *name)
{
    if (valid_slot(slot)) {
        strlcpy(s_slots[slot].name, name, sizeof(s_slots[slot].name));
        strlcpy(s_slots[slot].known_name, name, sizeof(s_slots[slot].known_name));
    }
}


/* ----------------------------------------------------------------------- mute recovery */

/*
 * The Venus D stops answering about 28 s after the link came up while the link itself stays up.
 * Neither the capture of the vendor app nor the firmware explains it, so this does not try to
 * prevent it: it notices a request that stays unanswered and then works through everything that
 * could wake the device, one step at a time, probing after each. The log says which step (if any)
 * got an answer, and whatever is left ends with a fresh connection.
 */

#define MUTE_DETECT_US   (4 * 1000 * 1000)
#define MUTE_STEP_US     (3 * 1000 * 1000)
#define MUTE_LAST_STEP   6

static esp_timer_handle_t s_super_timer;

static bool is_venus_d(const slot_t *s)
{
    // The device's own GAP name need not be the advertised one, so look at every name there is:
    // the one it reports, the one from the binding, and the one the last scan saw for its address.
    // The module's third characteristic (0xFF06) is the other tell: the bridge may know no name at
    // all (a binding made without one, no scan since the reboot).
    if (s->aux_handle || strstr(s->name, "VNSD") || strstr(s->known_name, "VNSD")) {
        return true;
    }
    for (size_t i = 0; i < s_scan_count; i++) {
        if (strcmp(s_scan_results[i].address, s->address) == 0) {
            return strstr(s_scan_results[i].name, "VNSD") != NULL;
        }
    }
    return false;
}

static int on_aux_read(uint16_t conn_handle, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg)
{
    (void) conn_handle;
    const uint8_t slot = slot_of(arg);
    uint8_t buf[4] = { 0 };

    if (error->status == 0 && attr && attr->om) {
        uint16_t got = 0;
        ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &got);
        ble_central_log(BLE_EV_AUX, slot, (uint16_t) ((buf[0] << 8) | buf[1]),
                        (uint16_t) ((buf[2] << 8) | buf[3]));
    } else if (error->status != BLE_HS_EDONE) {
        ble_central_log(BLE_EV_AUX, slot, 0xFFFF, (uint16_t) error->status);
    }
    return 0;
}

static void read_aux(uint8_t slot)
{
    slot_t *s = &s_slots[slot];
    if (s->aux_handle && ble_central_is_connected(slot)) {
        ble_gattc_read(s->conn_handle, s->aux_handle, on_aux_read, arg_of(slot));
    }
}

static int on_cccd_off_done(uint16_t conn_handle, const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr, void *arg)
{
    (void) attr;
    (void) error;
    const uint8_t slot = slot_of(arg);
    const uint8_t on[2] = { 0x01, 0x00 };
    if (s_slots[slot].rx_cccd_handle) {
        ble_gattc_write_flat(conn_handle, s_slots[slot].rx_cccd_handle, on, sizeof(on), NULL, NULL);
    }
    return 0;
}

static void mute_action(uint8_t slot, uint8_t step)
{
    slot_t *s = &s_slots[slot];
    const uint8_t on[2] = { 0x01, 0x00 };
    const uint8_t off[2] = { 0x00, 0x00 };

    switch (step) {
        case 2:
            if (s->rx_cccd_handle) {
                ble_gattc_write_flat(s->conn_handle, s->rx_cccd_handle, on, sizeof(on), NULL, NULL);
            }
            break;
        case 3:
            if (s->rx_cccd_handle) {
                ble_gattc_write_flat(s->conn_handle, s->rx_cccd_handle, off, sizeof(off),
                                     on_cccd_off_done, arg_of(slot));
            }
            break;
        case 4:
            request_params(s->conn_handle, 0x0018);
            break;
        case 5:
            read_aux(slot);
            break;
        default:
            break;
    }
}

static void supervise_slot(uint8_t slot, int64_t now)
{
    slot_t *s = &s_slots[slot];
    if (!is_venus_d(s) || !ble_central_is_ready(slot)) {
        s->mute_step = 0;
        s->supervised = false;
        return;
    }

    if (!s->supervised) {
        s->supervised = true;
        ble_central_log(BLE_EV_RECOVERY, slot, 7, 0);
    }

    if (s->aux_read_at_us && now >= s->aux_read_at_us) {
        s->aux_read_at_us = 0;
        read_aux(slot);
    }

    if (s->mute_step == 0) {
        // A request went out and nothing has come back for a while, after the link had been
        // answering. Writes that never get a reply by design are caught by the probe below.
        if (s->last_write_us > s->last_notify_us && now - s->last_write_us > MUTE_DETECT_US) {
            s->mute_step = 1;
            ble_central_log(BLE_EV_RECOVERY, slot, 0, 0);
            read_aux(slot);
            s->mute_next_us = now;
        } else {
            return;
        }
    }

    if (now < s->mute_next_us) {
        return;
    }

    // Did the last probe get an answer?
    if (s->mute_probe_us && s->last_notify_us >= s->mute_probe_us) {
        ble_central_log(BLE_EV_RECOVERY, slot, (uint16_t) (s->mute_step - 1 > 0 ? s->mute_step - 1 : 1), 1);
        s->mute_step = 0;
        s->mute_probe_us = 0;
        return;
    }

    if (s->mute_step > MUTE_LAST_STEP) {
        s->mute_step = 0;
        return;
    }

    const uint8_t step = s->mute_step++;
    ble_central_log(BLE_EV_RECOVERY, slot, step, 0);

    if (step == MUTE_LAST_STEP) {
        ble_central_log(BLE_EV_RECOVERY, slot, step, 2);
        s->mute_probe_us = 0;
        s->mute_step = 0;
        ble_gap_terminate(s->conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    mute_action(slot, step);

    // The probe is the same state request the vendor app sends: it is answered at once on a link
    // that works.
    static const uint8_t probe[6] = { 0x73, 0x06, 0x23, 0x03, 0x01, 0x54 };
    s->mute_probe_us = esp_timer_get_time();
    ble_central_write_quiet(slot, probe, sizeof(probe), false);
    s->mute_next_us = now + MUTE_STEP_US;
}

static void supervisor_tick(void *arg)
{
    (void) arg;
    const int64_t now = esp_timer_get_time();
    for (uint8_t i = 0; i < BLE_MAX_SLOTS; i++) {
        supervise_slot(i, now);
    }
}

static void supervisor_start(void)
{
    const esp_timer_create_args_t args = { .callback = &supervisor_tick, .name = "ble_super" };
    if (esp_timer_create(&args, &s_super_timer) == ESP_OK) {
        esp_timer_start_periodic(s_super_timer, 1000 * 1000);
    }
}
