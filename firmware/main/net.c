#include "esp_pm.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app.h"

static const char *TAG = "net";
static bool s_time_valid;
static bool s_wifi_up;
static bool s_wifi_started;
static bool s_wifi_enabled = true;
static volatile bool s_ntp_done;
static char s_err[64];
static volatile int s_last_reason;      /* reason of the latest STA disconnect */
const char *net_last_error(void) { return s_err; }

void net_apply_tz(void)
{
    int m = g_set.tz_min;
    char tz[24];
    char sign = m >= 0 ? '-' : '+';     /* POSIX TZ uses the inverted sign */
    m = abs(m);
    snprintf(tz, sizeof(tz), "UTC%c%d:%02d", sign, m / 60, m % 60);
    setenv("TZ", tz, 1);
    tzset();
}

bool net_time_valid(void) { return s_time_valid; }
bool net_wifi_connected(void) { return s_wifi_up; }
static volatile int s_users;
bool net_wifi_busy(void) { return s_wifi_up && s_users > 0; }   /* something is using the network now */
const char *net_wifi_ssid(void) { return g_set.ssid; }

static void status_async(void *arg) { ui_status_changed(); }
static void notify_ui(void)
{
    if (ui_lock(200)) { lv_async_call(status_async, NULL); ui_unlock(); }
    ble_notify_status();
}

void net_set_time(int64_t epoch, int16_t tz_min, bool from_phone)
{
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    if (from_phone) g_set.tz_min = tz_min;
    net_apply_tz();
    hw_rtc_write((time_t)epoch);
    s_time_valid = true;
    ESP_LOGI(TAG, "time set (%s)", from_phone ? "phone" : "ntp/rtc");
    notify_ui();
}

static void sntp_cb(struct timeval *tv)
{
    s_ntp_done = true;
    net_set_time(tv->tv_sec, g_set.tz_min, false);
}

/* ---------------- WiFi: remembers up to 6 networks, joins the strongest known one ---------------- */
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#define MAX_NETS 6
typedef struct { char ssid[33]; char pass[65]; } net_t;
static net_t s_nets[MAX_NETS];
static int s_nnets;
static EventGroupHandle_t s_ev;
static TaskHandle_t s_mgr;
/* s_users (declared above): jobs that need WiFi right now */
static esp_timer_handle_t s_off_timer;
static volatile bool s_radio_on;
/* s_users is changed from many tasks (ntp, weather, light, BLE host): keep it atomic.
 * s_radio_lock serialises switching the radio on/off against the idle-off timer. */
static portMUX_TYPE s_users_mux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_radio_lock;
/* set while the watch itself drops the link (idle-off, WiFi disabled, network deleted),
 * so that disconnect is not reported as an error */
static volatile bool s_leaving;

/* A network sent from the phone is first tried, and only saved if it really connects. */
static volatile bool s_test_pending;
static net_t s_test;
static net_test_cb_t s_test_cb;
static int users_add(int d)
{
    taskENTER_CRITICAL(&s_users_mux);
    int v = s_users + d;
    if (v < 0) v = 0;
    s_users = v;
    taskEXIT_CRITICAL(&s_users_mux);
    return v;
}
static void radio_lock(void) { if (s_radio_lock) xSemaphoreTake(s_radio_lock, portMAX_DELAY); }
static void radio_unlock(void) { if (s_radio_lock) xSemaphoreGive(s_radio_lock); }

/* Writes `in` as the inside of a JSON string ("" not included). Always NUL-terminates. */
int json_esc(char *out, int len, const char *in)
{
    int n = 0;
    if (len <= 0) return 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        char t[8];
        int k;
        if (*p == '"' || *p == '\\') { t[0] = '\\'; t[1] = (char)*p; k = 2; }
        else if (*p < 0x20) k = snprintf(t, sizeof(t), "\\u%04x", *p);
        else { t[0] = (char)*p; k = 1; }
        if (n + k >= len) break;
        memcpy(out + n, t, k);
        n += k;
    }
    out[n] = 0;
    return n;
}
#define EV_GOT_IP BIT0
#define EV_DISC   BIT1

static void nets_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_nets, sizeof(net_t) * s_nnets);
    nvs_commit(h);
    nvs_close(h);
}

/* Where we last got online: joining that AP directly (known channel + BSSID) skips the
 * ~2 s all-channel scan. Kept in NVS so the first join after a reboot is fast too. */
typedef struct { char ssid[33]; uint8_t bssid[6]; uint8_t ch; } last_ap_t;
static last_ap_t s_last;

static void last_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "last", &s_last, sizeof(s_last));
    nvs_commit(h);
    nvs_close(h);
}

static void last_forget(const char *ssid)
{
    if (s_last.ssid[0] && (!ssid || !strcmp(s_last.ssid, ssid))) { memset(&s_last, 0, sizeof(s_last)); last_save(); }
}

/* called on the WiFi manager task right after we got an IP */
static void last_record(void)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return;
    if (!strcmp(s_last.ssid, g_set.ssid) && !memcmp(s_last.bssid, ap.bssid, 6) && s_last.ch == ap.primary) return;
    memset(&s_last, 0, sizeof(s_last));
    strncpy(s_last.ssid, g_set.ssid, 32);
    memcpy(s_last.bssid, ap.bssid, 6);
    s_last.ch = ap.primary;
    last_save();
}

static void nets_load(void)
{
    nvs_handle_t h;
    s_nnets = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_nets);
        if (nvs_get_blob(h, "nets", s_nets, &len) == ESP_OK) s_nnets = len / sizeof(net_t);
        nvs_close(h);
    }
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_last);
        if (nvs_get_blob(h, "last", &s_last, &len) != ESP_OK || len != sizeof(s_last)) memset(&s_last, 0, sizeof(s_last));
        nvs_close(h);
    }
    if (s_nnets == 0 && g_set.ssid[0]) {           /* migrate the single network from v0.1-v0.5 */
        strncpy(s_nets[0].ssid, g_set.ssid, 32);
        strncpy(s_nets[0].pass, g_set.pass, 64);
        s_nnets = 1;
        nets_save();
    }
}

void net_wifi_add(const char *ssid, const char *pass)
{
    if (!ssid[0]) return;
    int i;
    for (i = 0; i < s_nnets && strcmp(s_nets[i].ssid, ssid); i++) {}
    if (i == s_nnets) {
        if (s_nnets == MAX_NETS) { memmove(&s_nets[0], &s_nets[1], sizeof(net_t) * (MAX_NETS - 1)); i = MAX_NETS - 1; }
        else s_nnets++;
    }
    last_forget(ssid);                              /* new password: don't reuse the old AP hint blindly */
    memset(&s_nets[i], 0, sizeof(net_t));
    strncpy(s_nets[i].ssid, ssid, 32);
    strncpy(s_nets[i].pass, pass, 64);
    nets_save();
    ESP_LOGI(TAG, "saved network '%s' (%d known)", ssid, s_nnets);
    net_time_sync();                      /* try it right away (also syncs the clock) */
}

void net_wifi_del(const char *ssid)
{
    for (int i = 0; i < s_nnets; i++) {
        if (strcmp(s_nets[i].ssid, ssid)) continue;
        last_forget(ssid);
        memmove(&s_nets[i], &s_nets[i + 1], sizeof(net_t) * (s_nnets - i - 1));
        s_nnets--;
        nets_save();
        if (s_wifi_up && !strcmp(g_set.ssid, ssid)) { s_leaving = true; esp_wifi_disconnect(); }
        return;
    }
}

/* JSON array of the saved SSIDs, escaped; drops trailing names rather than overflow. */
int net_wifi_list_json(char *buf, int len)
{
    if (len < 3) { if (len > 0) buf[0] = 0; return 0; }
    int n = snprintf(buf, len, "[");
    char e[6 * 32 + 1];
    for (int i = 0, first = 1; i < s_nnets; i++) {
        int k = json_esc(e, sizeof(e), s_nets[i].ssid);
        if (n + k + 4 >= len) break;                  /* room for ,"..." and the closing ] */
        n += snprintf(buf + n, len - n, "%s\"%s\"", first ? "" : ",", e);
        first = 0;
    }
    n += snprintf(buf + n, len - n, "]");
    return n;
}

int net_wifi_count(void) { return s_nnets; }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        int r = d ? d->reason : 0;
        s_last_reason = r;
        if (r == 15 || r == 204 || r == 202 || r == 2) snprintf(s_err, sizeof(s_err), "%s 密码可能不对", g_set.ssid);
        else if (r == 201) snprintf(s_err, sizeof(s_err), "找不到 %s", g_set.ssid);
        /* 8 ASSOC_LEAVE / 36 STA_LEAVING: we left on purpose (the radio idles off 20 s after
         * the last job). Not an error: keep "记住 N 个 · 平时关着" on the phone-settings screen. */
        else if (!s_leaving && r != 8 && r != 36) snprintf(s_err, sizeof(s_err), "%s 断开（原因 %d）", g_set.ssid, r);
        ESP_LOGW(TAG, "wifi disconnected, reason %d", r);
        bool was = s_wifi_up;
        s_wifi_up = false;
        xEventGroupSetBits(s_ev, EV_DISC);
        if (was) notify_ui();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
        s_err[0] = 0;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
        ESP_LOGI(TAG, "wifi connected to %s", g_set.ssid);
        notify_ui();
    }
}

static bool join_hint(int k, int rssi, int timeout_ms, const last_ap_t *hint)
{
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s_nets[k].ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, s_nets[k].pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s_nets[k].pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    if (hint) {                                     /* straight to the AP we used last time */
        wc.sta.channel = hint->ch;
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, hint->bssid, 6);
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(g_set.ssid, 0, sizeof(g_set.ssid));
    strncpy(g_set.ssid, s_nets[k].ssid, sizeof(g_set.ssid) - 1);
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISC);
    s_leaving = false;                          /* from here on a drop is real */
    ESP_LOGI(TAG, "joining %s (%d dBm)", g_set.ssid, rssi);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (b & EV_GOT_IP) { last_record(); return true; }
    if (!(b & EV_DISC)) {
        if (!hint) snprintf(s_err, sizeof(s_err), "%s 连接超时（拿不到 IP）", g_set.ssid);
        s_leaving = true;                           /* stop the half-done attempt, or the next scan fails */
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, EV_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    }
    return false;
}
static bool join(int k, int rssi, int timeout_ms) { return join_hint(k, rssi, timeout_ms, NULL); }

static bool try_join(void)
{
    if (!s_nnets) return false;
    if (s_last.ssid[0]) {
        for (int k = 0; k < s_nnets; k++)
            if (!strcmp(s_nets[k].ssid, s_last.ssid)) {
                ESP_LOGI(TAG, "fast join %s on ch %d", s_last.ssid, s_last.ch);
                if (join_hint(k, 0, 6000, &s_last)) return true;
                break;                              /* moved / AP changed: fall back to a scan */
            }
    }
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return false;
    uint16_t n = 20;
    wifi_ap_record_t *ap = calloc(n, sizeof(wifi_ap_record_t));
    if (!ap) return false;
    esp_wifi_scan_get_ap_records(&n, ap);
    int best = -1, rssi = -127;
    bool seen[MAX_NETS] = {0}, hidden = false;
    for (int i = 0; i < n; i++) {
        if (!ap[i].ssid[0]) hidden = true;            /* hidden networks scan with an empty name */
        for (int k = 0; k < s_nnets; k++)
            if (!strcmp((char *)ap[i].ssid, s_nets[k].ssid)) {
                seen[k] = true;
                if (ap[i].rssi > rssi) { rssi = ap[i].rssi; best = k; }
            }
    }
    free(ap);
    if (best >= 0) return join(best, rssi, 15000);
    /* None of ours is visible by name, but a hidden network is around: it could be one of
     * ours. Connecting with the SSID set sends a directed probe, which hidden APs answer. */
    if (hidden)
        for (int k = 0; k < s_nnets; k++)
            if (!seen[k] && join(k, 0, 10000)) return true;
    ESP_LOGI(TAG, "no known network in range (%d seen)", n);
    snprintf(s_err, sizeof(s_err), "附近没有记住的 WiFi（看到 %d 个）", n);
    return false;
}

/* WiFi is only switched on while something needs it (time sync, weather, office
 * light, adding a network) and switched off 45 s after the last job finishes. */
static volatile bool s_keep;               /* "WiFi 常开": never switch off when idle */
static void radio_off_cb(void *arg)
{
    bool off = false;
    if (s_keep) return;
    radio_lock();
    if (s_users == 0 && s_radio_on) {          /* re-checked under the lock: a job may have just started */
        s_leaving = true;
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_radio_on = false;
        s_wifi_up = false;
        off = true;
    }
    radio_unlock();
    if (off) { ESP_LOGI(TAG, "wifi off (idle)"); notify_ui(); }
}

static void radio_up(void)
{
    if (!s_wifi_enabled || !s_wifi_started) return;
    radio_lock();
    if (!s_radio_on) { s_radio_on = true; esp_wifi_start(); ESP_LOGI(TAG, "wifi on"); }
    radio_unlock();
    net_wifi_reconnect();
}

/* While a feature is actively using the network (radio, 小智, lights): no WiFi modem sleep and
 * CPU at 240 MHz. Modem sleep + 80 MHz made streams stutter and TLS handshakes time out. */
static esp_pm_lock_handle_t s_pm_net;
static void perf(bool on)
{
    if (!s_pm_net) esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "net", &s_pm_net);
    /* WiFi keeps modem sleep (needed when Bluetooth is on too); only the CPU stays at full speed */
    if (on) { if (s_pm_net) esp_pm_lock_acquire(s_pm_net); }
    else if (s_pm_net) esp_pm_lock_release(s_pm_net);
}

void net_wifi_use(bool on)
{
    if (on) {
        if (users_add(1) == 1) perf(true);
        if (s_off_timer) esp_timer_stop(s_off_timer);
        radio_up();
    } else if (s_users > 0 && users_add(-1) == 0) {
        perf(false);
        if (!s_off_timer) return;
        esp_timer_stop(s_off_timer);
        esp_timer_start_once(s_off_timer, 45 * 1000000);     /* stays on a while: the next tap needn't wait */
    }
}

bool net_wait_connected(int ms)
{
    if (s_wifi_up) return true;
    if (!s_nnets || !s_wifi_enabled) return false;
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(ms));
    return (b & EV_GOT_IP) != 0;
}

static void run_test(void);

static void mgr_task(void *arg)
{
    int fails = 0;
    for (;;) {
        if (s_test_pending && s_radio_on) {
            run_test();
            fails = 0;
            continue;
        }
        if (s_radio_on && s_wifi_enabled && !s_wifi_up && s_nnets && (s_users > 0 || s_keep)) {
            if (try_join()) fails = 0;
            else if (++fails >= 3) { fails = 0; ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000)); }
            else ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));      /* a failed scan returns at once: don't spin */
            continue;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
    }
}

static void test_done(bool ok, const char *why)
{
    net_test_cb_t cb = s_test_cb;
    char ssid[33];
    memcpy(ssid, s_test.ssid, sizeof(ssid));
    s_test_pending = false;
    if (ok) net_wifi_add(s_test.ssid, s_test.pass);   /* saves, and starts a time sync on the live link */
    memset(&s_test, 0, sizeof(s_test));
    ESP_LOGI(TAG, "test '%s': %s", ssid, ok ? "ok" : why);
    if (cb) cb(ok, ssid, why);
    net_wifi_use(false);                                /* the reference taken in net_wifi_test() */
    net_wifi_reconnect();
    notify_ui();
}

/* Runs on the WiFi manager task. */
static void run_test(void)
{
    char prev_ssid[sizeof(g_set.ssid)];
    memcpy(prev_ssid, g_set.ssid, sizeof(prev_ssid));
    if (s_wifi_up) {                                    /* drop the current link first */
        s_leaving = true;
        xEventGroupClearBits(s_ev, EV_DISC);
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, EV_DISC, pdTRUE, pdFALSE, pdMS_TO_TICKS(3000));
        s_wifi_up = false;
    }
    wifi_scan_config_t sc = { .show_hidden = true };
    uint16_t n = 20;
    wifi_ap_record_t *ap = calloc(n, sizeof(wifi_ap_record_t));
    bool found = false, hidden = false;
    if (ap && esp_wifi_scan_start(&sc, true) == ESP_OK) {
        esp_wifi_scan_get_ap_records(&n, ap);
        for (int i = 0; i < n; i++) {
            if (!ap[i].ssid[0]) hidden = true;
            if (!strcmp((char *)ap[i].ssid, s_test.ssid)) found = true;
        }
    } else n = 0;
    free(ap);
    if (!found && !hidden) { memcpy(g_set.ssid, prev_ssid, sizeof(prev_ssid)); test_done(false, "附近找不到这个 WiFi（手表只能连 2.4 GHz）"); return; }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, s_test.ssid, sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, s_test.pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = s_test.pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    memset(g_set.ssid, 0, sizeof(g_set.ssid));
    strncpy(g_set.ssid, s_test.ssid, sizeof(g_set.ssid) - 1);
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_DISC);
    s_leaving = false;
    s_last_reason = 0;
    ESP_LOGI(TAG, "testing %s%s", s_test.ssid, found ? "" : " (hidden?)");
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP, pdTRUE, pdFALSE, pdMS_TO_TICKS(20000));
    if (b & EV_GOT_IP) { last_record(); test_done(true, NULL); return; }

    int r = s_last_reason;
    s_leaving = true;                                   /* our own give-up is not an error */
    esp_wifi_disconnect();
    s_err[0] = 0;                                       /* it was never saved: don't show it as the watch's WiFi error */
    memcpy(g_set.ssid, prev_ssid, sizeof(prev_ssid));
    if (r == 15 || r == 204 || r == 202 || r == 2) test_done(false, "密码不对");
    else if (r == 201 || !found) test_done(false, "附近找不到这个 WiFi（手表只能连 2.4 GHz）");
    else test_done(false, "连不上（拿不到 IP）");
}

bool net_wifi_test(const char *ssid, const char *pass, net_test_cb_t cb)
{
    if (!ssid[0] || s_test_pending) return false;
    if (!s_wifi_enabled || !s_wifi_started) { if (cb) cb(false, ssid, "省电模式下 WiFi 关着"); return true; }
    memset(&s_test, 0, sizeof(s_test));
    strncpy(s_test.ssid, ssid, 32);
    strncpy(s_test.pass, pass, 64);
    s_test_cb = cb;
    s_test_pending = true;
    net_wifi_use(true);                                 /* radio on; released in test_done() */
    return true;
}

/* time sync job: every 6 h (or at boot), WiFi on just long enough for NTP */
static void ntp_job(void *arg)
{
    net_wifi_use(true);
    s_ntp_done = false;
    if (net_wait_connected(25000)) {
        esp_sntp_restart();
        for (int i = 0; i < 40 && !s_ntp_done; i++) vTaskDelay(pdMS_TO_TICKS(250));
    }
    net_wifi_use(false);
    vTaskDelete(NULL);
}

void net_time_sync(void)
{
    if (s_nnets && s_wifi_enabled) xTaskCreate(ntp_job, "ntp", 3072, NULL, 3, NULL);
}

void net_wifi_reconnect(void)
{
    if (s_mgr) xTaskNotifyGive(s_mgr);
}

void net_wifi_keep(bool on)
{
    s_keep = on;
    if (on) { if (s_off_timer) esp_timer_stop(s_off_timer); radio_up(); }
    else if (s_users == 0 && s_off_timer) { esp_timer_stop(s_off_timer); esp_timer_start_once(s_off_timer, 45 * 1000000); }
    notify_ui();
}

void net_wifi_enable(bool on)
{
    if (on == s_wifi_enabled) return;
    s_wifi_enabled = on;
    if (!on) {
        radio_lock();
        if (s_radio_on) { s_leaving = true; esp_wifi_disconnect(); esp_wifi_stop(); s_radio_on = false; s_wifi_up = false; }
        radio_unlock();
    } else if (s_users > 0 || s_keep) radio_up();
}

void net_init(void)
{
    net_apply_tz();
    time_t t;
    if (hw_rtc_read(&t)) {
        struct timeval tv = { .tv_sec = t };
        settimeofday(&tv, NULL);
        s_time_valid = true;
        ESP_LOGI(TAG, "time restored from RTC");
    }
    s_ev = xEventGroupCreate();
    s_radio_lock = xSemaphoreCreateMutex();
    nets_load();

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   /* we keep our own list; don't rewrite flash on every connect attempt */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sc.start = false;
    sc.sync_cb = sntp_cb;
    esp_netif_sntp_init(&sc);
    esp_netif_sntp_start();

    esp_timer_create_args_t ta = { .callback = radio_off_cb, .name = "wifioff" };
    esp_timer_create(&ta, &s_off_timer);
    s_wifi_started = true;
    if (g_set.power_save) s_wifi_enabled = false;
    xTaskCreate(mgr_task, "wifi_mgr", 4096, NULL, 3, &s_mgr);
    if (g_set.wifi_keep) net_wifi_keep(true);
    net_time_sync();
}

/* ---------------- weather (Open-Meteo, no key needed) ---------------- */
#include "esp_http_client.h"
#include "cJSON.h"
weather_t g_weather;
static volatile bool s_wx_busy;
static int64_t s_wx_phone_us;

const char *weather_text(int c)
{
    if (c == 0) return "晴";
    if (c <= 2) return "少云";
    if (c == 3) return "阴";
    if (c == 45 || c == 48) return "雾";
    if (c >= 51 && c <= 57) return "毛毛雨";
    if (c >= 61 && c <= 67) return "雨";
    if (c >= 71 && c <= 77) return "雪";
    if (c >= 80 && c <= 82) return "阵雨";
    if (c >= 95) return "雷雨";
    return "多云";
}

static void wx_task(void *arg)
{
    net_wifi_use(true);
    if (!net_wait_connected(25000)) {
        ESP_LOGW(TAG, "weather: no wifi");
        net_wifi_use(false);
        s_wx_busy = false;
        notify_ui();
        vTaskDelete(NULL);
        return;
    }
    char url[256];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f"
             "&current=temperature_2m,relative_humidity_2m,weather_code&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=1", g_set.lat, g_set.lon);
    char *buf = calloc(1, 4096);
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int len = 0;
    if (buf && c && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        int r;
        while ((r = esp_http_client_read(c, buf + len, 4095 - len)) > 0) len += r;
    }
    if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
    if (len > 0) {
        cJSON *j = cJSON_Parse(buf);
        cJSON *cur = j ? cJSON_GetObjectItem(j, "current") : NULL;
        cJSON *day = j ? cJSON_GetObjectItem(j, "daily") : NULL;
        if (cur) {
            weather_t w = {0};
            w.temp = cJSON_GetObjectItem(cur, "temperature_2m") ? cJSON_GetObjectItem(cur, "temperature_2m")->valuedouble : 0;
            w.hum = cJSON_GetObjectItem(cur, "relative_humidity_2m") ? cJSON_GetObjectItem(cur, "relative_humidity_2m")->valueint : 0;
            w.code = cJSON_GetObjectItem(cur, "weather_code") ? cJSON_GetObjectItem(cur, "weather_code")->valueint : 0;
            if (day) {
                cJSON *mx = cJSON_GetObjectItem(day, "temperature_2m_max"), *mn = cJSON_GetObjectItem(day, "temperature_2m_min");
                if (mx && cJSON_GetArraySize(mx)) w.tmax = cJSON_GetArrayItem(mx, 0)->valuedouble;
                if (mn && cJSON_GetArraySize(mn)) w.tmin = cJSON_GetArrayItem(mn, 0)->valuedouble;
            }
            time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
            snprintf(w.upd, sizeof(w.upd), "%02d:%02d", tm.tm_hour, tm.tm_min);
            w.ok = true;
            g_weather = w;
            ESP_LOGI(TAG, "weather %.1f C code %d", w.temp, w.code);
        }
        cJSON_Delete(j);
    } else ESP_LOGW(TAG, "weather fetch failed");
    free(buf);
    net_wifi_use(false);
    notify_ui();
    s_wx_busy = false;
    vTaskDelete(NULL);
}

/* weather pushed by the phone over Bluetooth: no WiFi needed on the watch */
void net_weather_from_phone(float t, int code, float tmax, float tmin, int hum)
{
    weather_t w = { .ok = true, .temp = t, .code = code, .tmax = tmax, .tmin = tmin, .hum = hum };
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    snprintf(w.upd, sizeof(w.upd), "%02d:%02d", tm.tm_hour, tm.tm_min);
    g_weather = w;
    s_wx_phone_us = esp_timer_get_time();
    notify_ui();
}

void net_weather_refresh(void)
{
    if (s_wx_phone_us && esp_timer_get_time() - s_wx_phone_us < 50LL * 60 * 1000000) return;   /* phone gave us fresh data */
    if (!s_nnets || !s_wifi_enabled || s_wx_busy) return;
    s_wx_busy = true;
    if (xTaskCreate(wx_task, "wx", 6144, NULL, 3, NULL) != pdPASS) s_wx_busy = false;
}

#include "esp_tls_errors.h"
#include "esp_heap_caps.h"
void net_err_text(struct esp_http_client *c, int err, char *out, int n)
{
    int code = 0, flags = 0;
    esp_err_t tls = c ? esp_http_client_get_and_clear_last_tls_error(c, &code, &flags) : ESP_OK;
    const char *why;
    switch (tls) {
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME: why = "找不到服务器网址（DNS）"; break;
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:  why = "服务器不回应"; break;
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:      why = "连线超时"; break;
    case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:    why = "开不了连线"; break;
    case ESP_ERR_MBEDTLS_SSL_SETUP_FAILED:
    case ESP_ERR_MBEDTLS_SSL_CONFIG_DEFAULTS_FAILED: why = "加密准备失败（内存？）"; break;
    case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:    why = "加密握手失败"; break;
    case ESP_ERR_MBEDTLS_X509_CRT_PARSE_FAILED:
    case ESP_ERR_MBEDTLS_CERT_PARTLY_OK:          why = "证书不对（手表时间对吗？）"; break;
    default:                                      why = esp_err_to_name(err); break;
    }
    snprintf(out, n, "%s · 内存 %uK/%uK", why,
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    if (code) { int l = strlen(out); snprintf(out + l, n - l, " (-0x%x)", -code); }
}
