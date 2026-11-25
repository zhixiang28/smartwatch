#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_store.h"
void ble_store_config_init(void);
#include "esp_app_desc.h"
#include "app.h"

static const char *TAG = "ble";

/* f15a000X-6c2b-4b9a-9d3e-2a1c5e7b8d00 (bytes are little-endian) */
#define FW_UUID(x) BLE_UUID128_INIT(0x00, 0x8d, 0x7b, 0x5e, 0x1c, 0x2a, 0x3e, 0x9d, 0x9a, 0x4b, 0x2b, 0x6c, x, 0x00, 0x5a, 0xf1)
static const ble_uuid128_t UUID_SVC = FW_UUID(0x01);
static const ble_uuid128_t UUID_AUTH = FW_UUID(0x02);
static const ble_uuid128_t UUID_TIME = FW_UUID(0x03);
static const ble_uuid128_t UUID_WIFI = FW_UUID(0x04);
static const ble_uuid128_t UUID_STATUS = FW_UUID(0x05);
static const ble_uuid128_t UUID_CMD = FW_UUID(0x06);
static const ble_uuid128_t UUID_LOC = FW_UUID(0x07);
static const ble_uuid128_t UUID_DATA = FW_UUID(0x08);

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

/* ---- Remote shutter: standard HID over GATT (consumer control). Android's camera takes a
 * photo on Volume Up from any keyboard / remote, so no app is needed. HID needs a bonded,
 * encrypted link; the bond is kept in NVS (CONFIG_BT_NIMBLE_NVS_PERSIST). ---- */
static const uint8_t HID_REPORT_MAP[] = {
    0x05, 0x0C,        /* Usage Page (Consumer) */
    0x09, 0x01,        /* Usage (Consumer Control) */
    0xA1, 0x01,        /* Collection (Application) */
    0x85, 0x01,        /*   Report ID 1 */
    0x15, 0x00, 0x26, 0xFF, 0x03,   /* Logical 0..1023 */
    0x19, 0x00, 0x2A, 0xFF, 0x03,   /* Usage 0..1023 */
    0x75, 0x10, 0x95, 0x01,         /* 1 x 16 bit */
    0x81, 0x00,        /*   Input (Data, Array) */
    0xC0               /* End Collection */
};
static const uint8_t HID_INFO[4] = {0x11, 0x01, 0x00, 0x02};      /* bcdHID 1.11, country 0, normally connectable */
static const uint8_t HID_REPORT_REF[2] = {0x01, 0x01};             /* report id 1, input */
static const uint8_t PNP_ID[7] = {0x02, 0xE5, 0x02, 0x01, 0x00, 0x01, 0x00};   /* USB VID 0x02E5 (Espressif), PID 1, v1 */
static uint16_t s_hid_report_h, s_batt_h;
static bool s_hid_sub;                /* the host subscribed to the input report */
static bool s_encrypted;

static int hid_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int which = (int)(intptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR || ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        static const uint8_t zero[2] = {0, 0};
        uint8_t b;
        switch (which) {
        case 1: return os_mbuf_append(ctxt->om, HID_INFO, sizeof(HID_INFO)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 2: return os_mbuf_append(ctxt->om, HID_REPORT_MAP, sizeof(HID_REPORT_MAP)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 3: return os_mbuf_append(ctxt->om, zero, 2) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 4: return os_mbuf_append(ctxt->om, HID_REPORT_REF, 2) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 5: b = 1; return os_mbuf_append(ctxt->om, &b, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;   /* report protocol */
        case 6: b = hw_battery_percent() < 0 ? 100 : hw_battery_percent(); return os_mbuf_append(ctxt->om, &b, 1) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 7: return os_mbuf_append(ctxt->om, PNP_ID, sizeof(PNP_ID)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        case 8: return os_mbuf_append(ctxt->om, "FishWatch", 9) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        }
        return 0;
    }
    return 0;                         /* control point / protocol mode writes: nothing to do */
}

bool ble_hid_ready(void) { return ble_connected() && s_encrypted && s_hid_sub; }
bool ble_hid_bonded(void)
{
    int n = 0;
    ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
    return n > 0;
}

/* press + release of a consumer key, e.g. 0xE9 Volume Up = camera shutter */
bool ble_hid_tap(uint16_t usage)
{
    if (!ble_hid_ready()) return false;
    uint8_t down[2] = {usage & 0xFF, usage >> 8}, up[2] = {0, 0};
    struct os_mbuf *om = ble_hs_mbuf_from_flat(down, 2);
    if (!om || ble_gatts_notify_custom(s_conn, s_hid_report_h, om)) return false;
    vTaskDelay(pdMS_TO_TICKS(40));
    om = ble_hs_mbuf_from_flat(up, 2);
    return om && ble_gatts_notify_custom(s_conn, s_hid_report_h, om) == 0;
}

static uint16_t s_status_h, s_cmd_h, s_data_h;
static volatile bool s_syncing;
static bool s_authed;
static uint16_t s_code;
static uint8_t s_addr_type;
static bool s_inited, s_synced, s_on;
static int64_t s_idle_since;

bool ble_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }
bool ble_authed(void) { return s_authed && ble_connected(); }
uint16_t ble_pair_code(void) { return s_code; }
void ble_new_code(void) { s_code = 1000 + esp_random() % 9000; s_authed = false; }

static void status_async(void *arg) { ui_status_changed(); }
static void ui_kick(void) { if (ui_lock(200)) { lv_async_call(status_async, NULL); ui_unlock(); } }

#define STATUS_MAX 1800     /* 6 escaped SSIDs at worst + the rest; heap, not the NimBLE stack */

static int status_json(char *buf, int len)
{
    char *nets = malloc(1300);
    char ssid[6 * 32 + 1];
    if (!nets) return snprintf(buf, len, "{}");
    net_wifi_list_json(nets, 1300);
    json_esc(ssid, sizeof(ssid), g_set.ssid);
    int n = snprintf(buf, len,
        "{\"ver\":\"%s\",\"auth\":%s,\"batt\":%d,\"chg\":%s,\"wifi\":%s,\"ssid\":\"%s\",\"time\":%s,\"tz\":%d,"
        "\"hunger\":%d,\"dirt\":%d,\"workouts\":%u,\"total\":%lu,\"steps\":%lu,\"nets\":%s,\"rs\":%d,\"rw\":%d,\"lk\":%s,\"la\":%s,\"rst\":\"%s\",\"dump\":%s,\"xzs\":%d,\"xzc\":\"%s\"}",
        esp_app_get_description()->version, s_authed ? "true" : "false", hw_battery_percent(), hw_charging() ? "true" : "false",
        net_wifi_connected() ? "true" : "false", ssid, net_time_valid() ? "true" : "false", g_set.tz_min,
        (int)g_set.hunger, (int)g_set.dirt, g_set.workouts, (unsigned long)g_set.total_kg, (unsigned long)g_set.steps, nets, g_set.remind_sit, g_set.remind_water, g_set.light_key[0] ? "true" : "false",
        g_set.light_person[0] && g_set.light_pin[0] ? "true" : "false",
        diag_reset_reason(), diag_has_dump() ? "true" : "false", xz_state(), xz_code());
    free(nets);
    return n < len ? n : len - 1;
}

void ble_notify_status(void)
{
    if (!ble_connected() || !s_authed) return;
    /* Longer than MTU-3 gets cut off by the stack; the phone page then reads the full value. */
    char *buf = malloc(STATUS_MAX);
    if (!buf) return;
    int n = status_json(buf, STATUS_MAX);
    int max = ble_att_mtu(s_conn) - 3;              /* the stack would cut it here anyway */
    if (max > 0 && n > max) n = max;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, n);
    free(buf);
    if (om) ble_gatts_notify_custom(s_conn, s_status_h, om);
}

void ble_find_phone(bool on)
{
    if (!ble_connected()) return;
    const char *m = on ? "find" : "find_stop";
    struct os_mbuf *om = ble_hs_mbuf_from_flat(m, strlen(m));
    if (om) ble_gatts_notify_custom(s_conn, s_cmd_h, om);
}

/* Result of trying a network the phone sent: "wifi_ok:<ssid>" or "wifi_fail:<ssid>\n<why>" */
static void toast_async(void *arg) { ui_toast((const char *)arg); free(arg); }
static void wifi_test_done(bool ok, const char *ssid, const char *why)
{
    char m[200];
    if (ok) snprintf(m, sizeof(m), "wifi_ok:%s", ssid);
    else snprintf(m, sizeof(m), "wifi_fail:%s\n%s", ssid, why ? why : "");
    if (ble_connected()) {
        int n = strlen(m), max = ble_att_mtu(s_conn) - 3;
        if (max > 0 && n > max) n = max;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(m, n);
        if (om) ble_gatts_notify_custom(s_conn, s_cmd_h, om);
    }
    char *t = malloc(120);
    if (t) {
        if (ok) snprintf(t, 120, "已加入 %.32s", ssid);
        else snprintf(t, 120, "%.32s：%s", ssid, why ? why : "连不上");
        if (ui_lock(200)) { lv_async_call(toast_async, t); ui_unlock(); } else free(t);
    }
    ui_kick();
}

/* ---- health data export, only while a phone is connected ---- */
static bool emit_line(const char *line)
{
    int len = strlen(line);
    int chunk = ble_att_mtu(s_conn) - 3;
    if (chunk < 20) chunk = 20;
    for (int off = 0; off < len; off += chunk) {
        int k = len - off > chunk ? chunk : len - off;
        for (int tries = 0;; tries++) {
            if (!ble_connected()) return false;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(line + off, k);
            if (om && ble_gatts_notify_custom(s_conn, s_data_h, om) == 0) break;
            if (tries > 50) return false;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    return true;
}

static void dump_task(void *arg)
{
    bool ok = diag_export(emit_line);
    ESP_LOGI(TAG, "dump export %s", ok ? "done" : "aborted");
    s_syncing = false;
    vTaskDelete(NULL);
}

static void sync_task(void *arg)
{
    health_save();
    bool ok = health_export(emit_line);
    ESP_LOGI(TAG, "health sync %s", ok ? "done" : "aborted");
    s_syncing = false;
    vTaskDelete(NULL);
}

static int flat(struct ble_gatt_access_ctxt *ctxt, char *buf, int max)
{
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, max - 1, &len) != 0) return -1;
    buf[len] = 0;
    return len;
}

static int access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int which = (int)(intptr_t)arg;
    char buf[200];
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (which == 5) {
            char *js = malloc(STATUS_MAX);
            if (!js) return BLE_ATT_ERR_INSUFFICIENT_RES;
            int n = status_json(js, STATUS_MAX);
            int rc = os_mbuf_append(ctxt->om, js, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
            free(js);
            return rc;
        }
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    int n = flat(ctxt, buf, sizeof(buf));
    if (n < 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    if (which == 2) {                                  /* AUTH: "1234" */
        s_authed = atoi(buf) == s_code;
        ESP_LOGI(TAG, "auth %s", s_authed ? "ok" : "failed");
        ui_kick();
        return s_authed ? 0 : BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    if (!s_authed) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;

    if (which == 3) {                                  /* TIME: "epoch,tzmin" */
        long long epoch = 0; int tz = 480;
        if (sscanf(buf, "%lld,%d", &epoch, &tz) < 1) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        net_set_time(epoch, (int16_t)tz, true);
        store_save();
    } else if (which == 4) {                           /* WIFI: "ssid\npassword" */
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;
        if (!net_wifi_test(buf, nl ? nl + 1 : "", wifi_test_done)) wifi_test_done(false, buf, "上一个 WiFi 还在连，等一下再加");
        ui_kick();
    } else if (which == 7) {                           /* LOC: "lat,lon" from the phone's GPS */
        float lat, lon;
        if (sscanf(buf, "%f,%f", &lat, &lon) == 2) { g_set.lat = lat; g_set.lon = lon; store_save(); net_weather_refresh(); }
    } else if (which == 6) {                           /* CMD */
        if (!strcmp(buf, "find_stop")) ble_find_phone(false);
        else if (!strcmp(buf, "sync") && !s_syncing) {
            s_syncing = true;
            if (xTaskCreate(sync_task, "sync", 4096, NULL, 3, NULL) != pdPASS) s_syncing = false;
        }
        else if (!strcmp(buf, "dump") && !s_syncing) {          /* crash report -> phone, over DATA */
            s_syncing = true;
            if (xTaskCreate(dump_task, "dump", 4096, NULL, 3, NULL) != pdPASS) s_syncing = false;
        }
        else if (!strcmp(buf, "dump_clear")) diag_clear();
        else if (!strncmp(buf, "wifi_del:", 9)) { net_wifi_del(buf + 9); ui_kick(); }
        else if (!strncmp(buf, "wx:", 3)) {
            float t, mx, mn; int code, hum;
            if (sscanf(buf + 3, "%f,%d,%f,%f,%d", &t, &code, &mx, &mn, &hum) == 5) net_weather_from_phone(t, code, mx, mn, hum);
        }
        else if (!strncmp(buf, "url:", 4)) { snprintf(g_set.page_url, sizeof(g_set.page_url), "%s", buf + 4); store_save(); ui_kick(); }
        else if (!strncmp(buf, "lightkey:", 9)) { snprintf(g_set.light_key, sizeof(g_set.light_key), "%s", buf + 9); store_save(); }
        else if (!strncmp(buf, "lightacct:", 10)) {             /* "lightacct:<person uuid>\n<pin>", already checked by the phone */
            char *nl = strchr(buf + 10, '\n');
            if (nl) {
                *nl = 0;
                snprintf(g_set.light_person, sizeof(g_set.light_person), "%s", buf + 10);
                snprintf(g_set.light_pin, sizeof(g_set.light_pin), "%s", nl + 1);
                store_save();
            }
        }
        else if (!strncmp(buf, "xz_ota:", 7)) {                 /* 小智 server; "" = official. A new server needs new credentials */
            snprintf(g_set.xz_ota, sizeof(g_set.xz_ota), "%s", buf + 7);
            g_set.xz_url[0] = 0; g_set.xz_token[0] = 0;
            store_save();
        }
        else if (!strncmp(buf, "radio_url:", 10)) { snprintf(g_set.radio_url, sizeof(g_set.radio_url), "%s", buf + 10); store_save(); }
        else if (!strcmp(buf, "lightout")) { g_set.light_person[0] = 0; g_set.light_pin[0] = 0; store_save(); }
        else if (!strncmp(buf, "remind:", 7)) { g_set.remind_sit = buf[7] == '1'; g_set.remind_water = buf[8] == '1'; store_save(); }
        else if (!strncmp(buf, "goal:", 5)) { int g = atoi(buf + 5); if (g >= 1000 && g <= 50000) { g_set.step_goal = g; store_save(); } }
        else if (!strcmp(buf, "buzz")) hw_haptic(400);
    }
    ble_notify_status();
    return 0;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &UUID_AUTH.u, .access_cb = access_cb, .arg = (void *)2, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_TIME.u, .access_cb = access_cb, .arg = (void *)3, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_WIFI.u, .access_cb = access_cb, .arg = (void *)4, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_STATUS.u, .access_cb = access_cb, .arg = (void *)5, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_status_h },
            { .uuid = &UUID_CMD.u, .access_cb = access_cb, .arg = (void *)6, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_cmd_h },
            { .uuid = &UUID_LOC.u, .access_cb = access_cb, .arg = (void *)7, .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &UUID_DATA.u, .access_cb = access_cb, .arg = (void *)8, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_data_h },
            { 0 }
        },
    },
    {   /* HID: reading the report map / report needs an encrypted (bonded) link */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x1812),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A4A), .access_cb = hid_access, .arg = (void *)1, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4B), .access_cb = hid_access, .arg = (void *)2, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4C), .access_cb = hid_access, .arg = (void *)0, .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4E), .access_cb = hid_access, .arg = (void *)5, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = BLE_UUID16_DECLARE(0x2A4D), .access_cb = hid_access, .arg = (void *)3,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC, .val_handle = &s_hid_report_h,
              .descriptors = (struct ble_gatt_dsc_def[]) {
                  { .uuid = BLE_UUID16_DECLARE(0x2908), .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC, .access_cb = hid_access, .arg = (void *)4 },
                  { 0 } } },
            { 0 }
        },
    },
    {   /* battery level (Android shows it for HID devices) */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A19), .access_cb = hid_access, .arg = (void *)6, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_batt_h },
            { 0 }
        },
    },
    {   /* device information: PnP ID is what HID hosts look for */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = BLE_UUID16_DECLARE(0x2A50), .access_cb = hid_access, .arg = (void *)7, .flags = BLE_GATT_CHR_F_READ },
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = hid_access, .arg = (void *)8, .flags = BLE_GATT_CHR_F_READ },
            { 0 }
        },
    },
    { 0 }
};

static void advertise(void);

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) { s_conn = ev->connect.conn_handle; s_authed = false; s_encrypted = false; s_hid_sub = false; }
        else advertise();
        ui_kick();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_authed = false;
        s_encrypted = false;
        s_hid_sub = false;
        s_idle_since = esp_timer_get_time();
        if (s_on) advertise();
        ui_kick();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_on) advertise();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        s_encrypted = ev->enc_change.status == 0;
        ESP_LOGI(TAG, "encryption %s", s_encrypted ? "on" : "failed");
        ui_kick();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_hid_report_h) { s_hid_sub = ev->subscribe.cur_notify; ui_kick(); }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* the phone forgot us and pairs again: drop our old bond and accept */
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields f = {0};
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    f.uuids16 = &hid_uuid;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 0;
    f.appearance = 0x0180;            /* generic remote control */
    f.appearance_is_present = 1;
    ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN,
                                    .itvl_min = 160, .itvl_max = 320 };
    ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_cb, NULL);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    s_synced = true;
    if (s_on) advertise();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* Bluetooth is off unless the user turns it on (pairing, find phone).
 * It switches itself off after 3 minutes without a phone connected. */
static void ble_lazy_init(void)
{
    if (s_inited) return;
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;   /* "just works" pairing from Android settings */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(s_svcs);
    ble_gatts_add_svcs(s_svcs);
    ble_svc_gap_device_name_set("FishWatch");
    nimble_port_freertos_init(host_task);
    s_inited = true;
}

bool ble_is_on(void) { return s_on; }

void ble_start(void)
{
    s_on = true;
    s_idle_since = esp_timer_get_time();
    ble_lazy_init();
    if (s_synced && !ble_gap_adv_active() && !ble_connected()) advertise();
    ESP_LOGI(TAG, "bluetooth on");
    ui_kick();
}

void ble_stop(void)
{
    s_on = false;
    if (!s_inited) return;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    if (ble_connected()) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ESP_LOGI(TAG, "bluetooth off");
    ui_kick();
}

void ble_poll(void)
{
    if (s_on && !ble_connected() && esp_timer_get_time() - s_idle_since > 180LL * 1000000) ble_stop();
}

void ble_init(void)
{
    ble_new_code();
}
