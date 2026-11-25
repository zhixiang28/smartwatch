/* 小智 (xiaozhi.me) voice assistant, push-to-talk, following the official xiaozhi-esp32
 * protocol (github.com/78/xiaozhi-esp32: ota.cc, protocols/websocket_protocol.cc):
 *
 *  1. POST device info to the OTA URL. The reply holds the websocket URL + token, and, until
 *     the device is bound to an account, an "activation" code. The user adds the device on
 *     xiaozhi.me with that 6-digit code (shown on the watch and the phone page) while we poll
 *     <ota>/activate (202 = still waiting, 200 = done).
 *  2. Open the websocket (Authorization: Bearer <token>, Device-Id, Client-Id, Protocol-Version 1),
 *     send "hello", wait for the server hello (session_id, its audio params).
 *  3. Holding the button: listen start (manual), mic 24 kHz -> 16 kHz mono -> Opus 60 ms frames as
 *     binary messages; releasing: listen stop. The server answers with stt / llm / tts JSON and
 *     Opus audio, decoded and played on the speaker.
 *
 * Tasks: control (HTTP, websocket lifecycle), mic (Opus encoder: big alloca stack), speaker
 * (Opus decoder). All three stacks are in PSRAM, so none of them may touch flash: settings are
 * saved later from the UI task (xz_save_if_needed). The websocket task stack is in PSRAM too
 * (CONFIG_ESP_WS_CLIENT_TASK_STACK_IN_EXT_RAM); its event handler only copies data. */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "cJSON.h"
#include "opus.h"
#include "app.h"

extern void (*g_app_refresh)(void);
extern void (*g_app_close)(void);

static const char *TAG = "xiaozhi";
#define XZ_OTA_DEFAULT "https://api.tenclass.net/xiaozhi/ota/"

enum { XZ_OFF, XZ_WIFI, XZ_CHECK, XZ_ACTIVATE, XZ_CONNECT, XZ_READY, XZ_LISTEN, XZ_THINK, XZ_SPEAK, XZ_ERROR };
static volatile int s_st;
static char s_err[96];
static char s_code[16];                       /* activation code while not yet bound */
static char s_user[240], s_bot[480];          /* last thing said by the user / by 小智 */
static portMUX_TYPE s_txt_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_want;                  /* an app wants the session */
static volatile int s_mode;                   /* 0 chat, 1 translate */
static volatile bool s_need_save;
static volatile bool s_mic_on, s_play;
static esp_websocket_client_handle_t s_ws;
static char s_session[80];
static int s_srv_rate = 24000, s_srv_frame = 60;
static EventGroupHandle_t s_ev;
#define EV_CONNECTED BIT0
#define EV_HELLO     BIT1
#define EV_CLOSED    BIT2
static QueueHandle_t s_rxq;                   /* opus packets from the server (pkt_t *) */
static TaskHandle_t s_ctl, s_mic, s_spk;
typedef struct { uint16_t len; uint8_t d[]; } pkt_t;

int xz_state(void) { return s_st; }
const char *xz_code(void) { return s_st == XZ_ACTIVATE ? s_code : ""; }
void xz_save_if_needed(void) { if (s_need_save) { s_need_save = false; store_save(); } }

static void set_txt(char *dst, size_t n, const char *src)
{
    taskENTER_CRITICAL(&s_txt_mux);
    snprintf(dst, n, "%s", src);
    taskEXIT_CRITICAL(&s_txt_mux);
}
static void fail(const char *m) { snprintf(s_err, sizeof(s_err), "%s", m); s_st = XZ_ERROR; ESP_LOGW(TAG, "%s", m); }

static void mac_str(char *out)
{
    uint8_t m[6];
    esp_read_mac(m, ESP_MAC_WIFI_STA);
    sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ensure_uuid(void)
{
    if (g_set.xz_uuid[0]) return;
    uint8_t r[16];
    esp_fill_random(r, 16);
    r[6] = (r[6] & 0x0F) | 0x40;
    r[8] = (r[8] & 0x3F) | 0x80;
    snprintf(g_set.xz_uuid, sizeof(g_set.xz_uuid),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
    s_need_save = true;
}

static const char *ota_url(void) { return g_set.xz_ota[0] ? g_set.xz_ota : XZ_OTA_DEFAULT; }

/* POST to the OTA server; returns HTTP status, body in out (NUL-terminated). */
static int ota_post(const char *url, const char *body, char *out, int outlen)
{
    char mac[20], ua[48];
    mac_str(mac);
    snprintf(ua, sizeof(ua), "fishwatch-s3-amoled-2.06/%s", esp_app_get_description()->version);
    esp_http_client_config_t cfg = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 10000,
                                     .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048, .buffer_size_tx = 1024 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -2;
    esp_http_client_set_header(c, "Activation-Version", "1");
    esp_http_client_set_header(c, "Device-Id", mac);
    esp_http_client_set_header(c, "Client-Id", g_set.xz_uuid);
    esp_http_client_set_header(c, "User-Agent", ua);
    esp_http_client_set_header(c, "Accept-Language", "zh-CN");
    esp_http_client_set_header(c, "Content-Type", "application/json");
    int st = -1, len = 0;
    if (esp_http_client_open(c, strlen(body)) == ESP_OK) {
        esp_http_client_write(c, body, strlen(body));
        esp_http_client_fetch_headers(c);
        st = esp_http_client_get_status_code(c);
        int r;
        while (len < outlen - 1 && (r = esp_http_client_read(c, out + len, outlen - 1 - len)) > 0) len += r;
    }
    out[len] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return st;
}

/* Step 1. Returns 1 = have websocket config, 0 = activation needed (s_code set), -1 = error. */
static int check_server(void)
{
    char mac[20];
    mac_str(mac);
    const esp_app_desc_t *app = esp_app_get_description();
    char *body = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM), *resp = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!body || !resp) { free(body); free(resp); fail("内存不足"); return -1; }
    snprintf(body, 1024,
             "{\"version\":2,\"language\":\"zh-CN\",\"flash_size\":16777216,\"mac_address\":\"%s\",\"uuid\":\"%s\","
             "\"chip_model_name\":\"esp32s3\",\"chip_info\":{\"model\":9,\"cores\":2,\"revision\":0,\"features\":18},"
             "\"application\":{\"name\":\"fishwatch\",\"version\":\"%s\",\"compile_time\":\"%sT%sZ\",\"idf_version\":\"%s\",\"elf_sha256\":\"\"},"
             "\"partition_table\":[],\"ota\":{\"label\":\"factory\"},"
             "\"board\":{\"type\":\"waveshare-esp32-s3-touch-amoled-2.06\",\"name\":\"FishWatch\",\"mac\":\"%s\"},"
             "\"protocol\":{\"features\":{\"mcp\":false,\"aec\":false}}}",
             mac, g_set.xz_uuid, app->version, app->date, app->time, app->idf_ver, mac);
    int st = ota_post(ota_url(), body, resp, 4096);
    int ret = -1;
    if (st != 200) {
        char m[64]; snprintf(m, sizeof(m), "小智服务器连不上（%d）", st); fail(m);
    } else {
        cJSON *j = cJSON_Parse(resp);
        cJSON *act = j ? cJSON_GetObjectItem(j, "activation") : NULL;
        cJSON *ws = j ? cJSON_GetObjectItem(j, "websocket") : NULL;
        if (cJSON_IsObject(ws)) {
            cJSON *u = cJSON_GetObjectItem(ws, "url"), *t = cJSON_GetObjectItem(ws, "token");
            if (cJSON_IsString(u) && strcmp(u->valuestring, g_set.xz_url)) { snprintf(g_set.xz_url, sizeof(g_set.xz_url), "%s", u->valuestring); s_need_save = true; }
            if (cJSON_IsString(t) && strcmp(t->valuestring, g_set.xz_token)) { snprintf(g_set.xz_token, sizeof(g_set.xz_token), "%s", t->valuestring); s_need_save = true; }
        }
        cJSON *code = cJSON_IsObject(act) ? cJSON_GetObjectItem(act, "code") : NULL;
        if (cJSON_IsString(code)) { snprintf(s_code, sizeof(s_code), "%s", code->valuestring); ret = 0; }
        else if (g_set.xz_url[0]) ret = 1;
        else fail("服务器没有给连线地址");
        /* the reply may also offer a firmware update: ignored, this is not a xiaozhi firmware */
        cJSON_Delete(j);
    }
    free(body);
    free(resp);
    return ret;
}

/* waits for the user to bind the device on xiaozhi.me; true when activated */
static bool wait_activation(void)
{
    char url[160], resp[256];
    snprintf(url, sizeof(url), "%s%sactivate", ota_url(), ota_url()[strlen(ota_url()) - 1] == '/' ? "" : "/");
    s_st = XZ_ACTIVATE;
    for (int i = 0; i < 200 && s_want; i++) {             /* ~10 min */
        int st = ota_post(url, "{}", resp, sizeof(resp));
        if (st == 200) return true;
        if (st != 202 && st > 0 && i > 2) ESP_LOGW(TAG, "activate: %d %s", st, resp);
        for (int k = 0; k < 30 && s_want; k++) vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}

/* ---------------- websocket ---------------- */
static char *s_txt;                     /* reassembly of a fragmented text frame */
static size_t s_txt_len;

static void on_json(const char *data, size_t len)
{
    cJSON *j = cJSON_ParseWithLength(data, len);
    if (!j) return;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
    if (!type) { cJSON_Delete(j); return; }
    if (!strcmp(type, "hello")) {
        const char *sid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "session_id"));
        if (sid) snprintf(s_session, sizeof(s_session), "%s", sid);
        cJSON *ap = cJSON_GetObjectItem(j, "audio_params");
        if (cJSON_IsObject(ap)) {
            cJSON *sr = cJSON_GetObjectItem(ap, "sample_rate"), *fd = cJSON_GetObjectItem(ap, "frame_duration");
            if (cJSON_IsNumber(sr)) s_srv_rate = sr->valueint;
            if (cJSON_IsNumber(fd)) s_srv_frame = fd->valueint;
        }
        xEventGroupSetBits(s_ev, EV_HELLO);
    } else if (!strcmp(type, "stt")) {
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
        if (t) set_txt(s_user, sizeof(s_user), t);
        if (s_st == XZ_LISTEN || s_st == XZ_READY) s_st = XZ_THINK;
    } else if (!strcmp(type, "tts")) {
        const char *state = cJSON_GetStringValue(cJSON_GetObjectItem(j, "state"));
        if (state && !strcmp(state, "start")) { s_play = true; if (s_st != XZ_LISTEN) s_st = XZ_SPEAK; }
        else if (state && !strcmp(state, "sentence_start")) {
            const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(j, "text"));
            if (t) set_txt(s_bot, sizeof(s_bot), t);
        } else if (state && !strcmp(state, "stop")) {
            if (s_st == XZ_SPEAK || s_st == XZ_THINK) s_st = XZ_READY;
        }
    }
    /* llm (emotion), mcp, system...: not used */
    cJSON_Delete(j);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: xEventGroupSetBits(s_ev, EV_CONNECTED); break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED: xEventGroupSetBits(s_ev, EV_CLOSED); break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 || (d->op_code == 0x0 && s_txt_len)) {          /* text (maybe fragmented) */
            if (d->payload_offset == 0) s_txt_len = 0;
            if (!s_txt) s_txt = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
            if (s_txt && s_txt_len + d->data_len < 8192) { memcpy(s_txt + s_txt_len, d->data_ptr, d->data_len); s_txt_len += d->data_len; }
            if (s_txt && d->payload_offset + d->data_len >= d->payload_len) { on_json(s_txt, s_txt_len); s_txt_len = 0; }
        } else if (d->op_code == 0x2 && d->payload_offset == 0 && d->data_len == d->payload_len && d->data_len > 0) {
            if (!s_play) break;                                                  /* aborted: drop */
            pkt_t *p = heap_caps_malloc(sizeof(pkt_t) + d->data_len, MALLOC_CAP_SPIRAM);
            if (!p) break;
            p->len = d->data_len;
            memcpy(p->d, d->data_ptr, d->data_len);
            if (xQueueSend(s_rxq, &p, 0) != pdTRUE) free(p);
        }
        break;
    default: break;
    }
}

static void ws_close(void)
{
    if (!s_ws) return;
    s_mic_on = false;
    esp_websocket_client_close(s_ws, pdMS_TO_TICKS(2000));
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
}

static bool ws_open(void)
{
    char mac[20], hdr[400];
    mac_str(mac);
    const char *tok = g_set.xz_token;
    snprintf(hdr, sizeof(hdr), "Authorization: %s%s\r\nProtocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
             strchr(tok, ' ') ? "" : "Bearer ", tok, mac, g_set.xz_uuid);
    esp_websocket_client_config_t cfg = {
        .uri = g_set.xz_url, .headers = hdr, .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096, .task_stack = 6144, .task_prio = 5, .network_timeout_ms = 10000,
        .disable_auto_reconnect = true, .reconnect_timeout_ms = 10000,
    };
    xEventGroupClearBits(s_ev, EV_CONNECTED | EV_HELLO | EV_CLOSED);
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) { fail("开不了连线（内存不足）"); return false; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    if (esp_websocket_client_start(s_ws) != ESP_OK) { ws_close(); fail("开不了连线"); return false; }
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_CONNECTED | EV_CLOSED, pdFALSE, pdFALSE, pdMS_TO_TICKS(12000));
    if (!(b & EV_CONNECTED)) { ws_close(); fail("连不上小智服务器"); return false; }
    char hello[200];
    int n = snprintf(hello, sizeof(hello), "{\"type\":\"hello\",\"version\":1,\"features\":{\"mcp\":false},\"transport\":\"websocket\","
                     "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_duration\":60}}");
    esp_websocket_client_send_text(s_ws, hello, n, pdMS_TO_TICKS(3000));
    b = xEventGroupWaitBits(s_ev, EV_HELLO | EV_CLOSED, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(b & EV_HELLO)) {
        ws_close();
        /* a stale token is the usual cause: forget it so the next try asks the server again */
        g_set.xz_url[0] = 0; g_set.xz_token[0] = 0; s_need_save = true;
        fail("小智没有回应，再试一次");
        return false;
    }
    ESP_LOGI(TAG, "session %s, server audio %d Hz / %d ms", s_session, s_srv_rate, s_srv_frame);
    return true;
}

static void send_json(const char *fmt, const char *a)
{
    if (!s_ws || !esp_websocket_client_is_connected(s_ws)) return;
    char m[600];
    int n = snprintf(m, sizeof(m), fmt, s_session, a ? a : "");
    esp_websocket_client_send_text(s_ws, m, n, pdMS_TO_TICKS(2000));
}

/* ---------------- control task ---------------- */
static void ctl_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_want) continue;
        s_err[0] = 0;
        s_st = XZ_WIFI;
        net_wifi_use(true);
        if (!net_wait_connected(30000)) { fail("WiFi 连不上"); net_wifi_use(false); continue; }
        ensure_uuid();
        bool ok = false;
        for (int round = 0; round < 3 && s_want && !ok; round++) {
            if (!g_set.xz_url[0] || round) {
                s_st = XZ_CHECK;
                int r = check_server();
                if (r < 0) break;
                if (r == 0) { if (!wait_activation()) { if (s_want) fail("激活等太久了，再打开试试"); break; } continue; }
            }
            s_st = XZ_CONNECT;
            ok = ws_open();
        }
        if (ok && s_mode == 1) {
            /* translator: one instruction at the start of the session, the server keeps the context */
            send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"detect\",\"text\":\"%s\"}",
                      "接下来你只做口译：我说中文，你就只说英文译文；我说英文，你就只说中文译文。不要解释，不要回答问题。现在回答：好的");
        }
        if (ok) s_st = XZ_READY;
        /* stay connected while the app is open */
        while (s_want && ok) {
            EventBits_t b = xEventGroupWaitBits(s_ev, EV_CLOSED, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
            if (b & EV_CLOSED) { ws_close(); if (s_want) fail("连线断了，再按一下重连"); break; }
        }
        ws_close();
        s_play = false;
        net_wifi_use(false);
        if (!s_want && s_st != XZ_ERROR) s_st = XZ_OFF;
    }
}

/* ---------------- mic: 24 kHz 4-ch TDM -> mic 1 -> 16 kHz -> Opus 60 ms ---------------- */
static void mic_task(void *arg)
{
    int err;
    OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    if (enc) { opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(3)); opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000)); }
    int16_t *frame = heap_caps_malloc(256 * 4 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *pcm16 = heap_caps_malloc(960 * 2, MALLOC_CAP_SPIRAM);
    uint8_t *opus = heap_caps_malloc(600, MALLOC_CAP_SPIRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_mic_on || !enc || !frame || !pcm16 || !opus) continue;
        esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
        esp_codec_dev_sample_info_t fs = { .sample_rate = 24000, .channel = 4, .bits_per_sample = 16, .channel_mask = 0x0F };
        if (!mic || esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "mic open failed"); continue; }
        esp_codec_dev_set_in_gain(mic, 30.0f);
        int n16 = 0;
        uint32_t phase = 0;                                  /* 24k -> 16k: step 1.5 in 16.16 */
        int16_t prev = 0;
        while (s_mic_on) {
            if (esp_codec_dev_read(mic, frame, 256 * 4 * 2) != ESP_CODEC_DEV_OK) break;
            for (;;) {
                uint32_t idx = phase >> 16;
                if (idx >= 256) break;
                int16_t a = idx ? frame[(idx - 1) * 4] : prev, b = frame[idx * 4];
                pcm16[n16++] = (int16_t)(a + (((int32_t)(b - a) * (int32_t)(phase & 0xFFFF)) >> 16));
                phase += 0x18000;
                if (n16 == 960) {
                    int len = opus_encode(enc, pcm16, 960, opus, 600);
                    if (len > 0 && s_ws) esp_websocket_client_send_bin(s_ws, (const char *)opus, len, pdMS_TO_TICKS(500));
                    n16 = 0;
                }
            }
            phase -= 256u << 16;
            prev = frame[255 * 4];
        }
        esp_codec_dev_close(mic);
    }
}

/* ---------------- speaker: Opus at the server's rate -> 24 kHz ---------------- */
static void spk_task(void *arg)
{
    OpusDecoder *dec = NULL;
    int dec_rate = 0;
    int16_t *pcm = heap_caps_malloc(2880 * 2, MALLOC_CAP_SPIRAM);           /* 120 ms at 24 kHz */
    int16_t *out = heap_caps_malloc(4320 * 2, MALLOC_CAP_SPIRAM);
    for (;;) {
        pkt_t *p;
        if (xQueueReceive(s_rxq, &p, portMAX_DELAY) != pdTRUE) continue;
        if (!s_play || !pcm || !out) { free(p); continue; }
        if (!dec || dec_rate != s_srv_rate) {
            if (dec) opus_decoder_destroy(dec);
            int err;
            dec_rate = s_srv_rate;
            dec = opus_decoder_create(dec_rate, 1, &err);
        }
        int n = dec ? opus_decode(dec, p->d, p->len, pcm, 2880, 0) : 0;
        free(p);
        if (n <= 0) continue;
        if (dec_rate == 24000) sound_play_pcm(pcm, n, 24000, &s_play);
        else {
            uint32_t step = ((uint32_t)dec_rate << 16) / 24000, ph = 0;
            int o = 0;
            while ((ph >> 16) < (uint32_t)n && o < 4320) { out[o++] = pcm[ph >> 16]; ph += step; }
            sound_play_pcm(out, o, 24000, &s_play);
        }
    }
}

static bool xz_tasks(void)
{
    if (s_ctl) return true;
    s_ev = xEventGroupCreate();
    s_rxq = xQueueCreateWithCaps(40, sizeof(pkt_t *), MALLOC_CAP_SPIRAM);
    if (!s_ev || !s_rxq) return false;
    if (xTaskCreateWithCaps(ctl_task, "xz_ctl", 8192, NULL, 4, &s_ctl, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    if (xTaskCreateWithCaps(mic_task, "xz_mic", 32768, NULL, 5, &s_mic, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    if (xTaskCreateWithCaps(spk_task, "xz_spk", 16384, NULL, 5, &s_spk, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    return true;
}

static void xz_open(int mode)
{
    set_txt(s_user, sizeof(s_user), "");
    set_txt(s_bot, sizeof(s_bot), "");
    if (!xz_tasks()) { fail("内存不足"); return; }
    if (radio_playing()) radio_stop();                     /* one thing on the speaker at a time */
    s_mode = mode;
    s_want = true;
    xTaskNotifyGive(s_ctl);
}

static void xz_shut(void)
{
    s_want = false;
    s_mic_on = false;
    s_play = false;
}

static void talk_down(void)
{
    if (s_st == XZ_ERROR || s_st == XZ_OFF) { xz_open(s_mode); return; }   /* tap to retry */
    if (s_st < XZ_READY) return;
    if (s_st == XZ_SPEAK || s_st == XZ_THINK) {            /* interrupt 小智 */
        s_play = false;
        send_json("{\"session_id\":\"%s\",\"type\":\"abort\"%s}", NULL);
    }
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"%s}", NULL);
    s_st = XZ_LISTEN;
    s_mic_on = true;
    xTaskNotifyGive(s_mic);
    hw_haptic(20);
}

static void talk_up(void)
{
    if (s_st != XZ_LISTEN) return;
    s_mic_on = false;
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"%s}", NULL);
    s_st = XZ_THINK;
}

/* ---------------- UI (shared by 小智 and 翻译) ---------------- */
static lv_obj_t *s_ui_st, *s_ui_user, *s_ui_bot, *s_ui_btn;
static lv_timer_t *s_ui_tm;

static void ui_paint(lv_timer_t *t)
{
    xz_save_if_needed();
    if (!s_ui_st) return;
    static const char *ST[] = {"已关闭", "正在打开 WiFi…", "连接小智服务器…", "", "建立对话…", "按住下面说话", "在听…放开就送出", "想一想…", "小智在说话（按住可以打断）", ""};
    char line[120];
    if (s_st == XZ_ACTIVATE) snprintf(line, sizeof(line), "激活码 %s\n到 xiaozhi.me 添加设备，手机设定网页也看得到", s_code);
    else if (s_st == XZ_ERROR) snprintf(line, sizeof(line), "%s\n（点按钮重试）", s_err);
    else snprintf(line, sizeof(line), "%s", ST[s_st]);
    lv_label_set_text(s_ui_st, line);
    char u[240], b[480];
    taskENTER_CRITICAL(&s_txt_mux);
    memcpy(u, s_user, sizeof(u));
    memcpy(b, s_bot, sizeof(b));
    taskEXIT_CRITICAL(&s_txt_mux);
    lv_label_set_text(s_ui_user, u[0] ? u : "");
    lv_label_set_text(s_ui_bot, b);
    bool live = s_st == XZ_LISTEN;
    lv_obj_set_style_bg_color(s_ui_btn, lv_color_hex(live ? 0xF43F5E : s_st >= XZ_READY && s_st <= XZ_SPEAK ? 0x14B8A6 : 0x3A3A3C), 0);
}

static void btn_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) talk_down();
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) talk_up();
    ui_paint(NULL);
}

static void ui_close(void)
{
    if (s_ui_tm) { lv_timer_delete(s_ui_tm); s_ui_tm = NULL; }
    s_ui_st = NULL;
    xz_shut();
    xz_save_if_needed();
    power_keep_awake(false);
}

static void build(lv_obj_t *root, const char *title, int mode, uint32_t tint, const char *hint)
{
    ui_header(root, title, tint);
    s_ui_st = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_width(s_ui_st, 366);
    lv_label_set_long_mode(s_ui_st, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ui_st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_ui_st, LV_ALIGN_TOP_MID, 0, 66);
    s_ui_user = ui_label(root, &font_cn16, 0x8E8E93, "");
    lv_obj_set_width(s_ui_user, 366);
    lv_label_set_long_mode(s_ui_user, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_ui_user, 44);
    lv_obj_set_pos(s_ui_user, 22, 116);
    s_ui_bot = ui_label(root, &font_cn20, C_TEXT, "");
    lv_obj_set_width(s_ui_bot, 366);
    lv_label_set_long_mode(s_ui_bot, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_ui_bot, 150);
    lv_obj_set_pos(s_ui_bot, 22, 164);
    s_ui_btn = lv_button_create(root);
    lv_obj_set_size(s_ui_btn, 366, 92);
    lv_obj_set_pos(s_ui_btn, 22, 330);
    lv_obj_set_style_radius(s_ui_btn, 46, 0);
    lv_obj_set_style_shadow_width(s_ui_btn, 0, 0);
    lv_obj_add_event_cb(s_ui_btn, btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui_btn, btn_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_ui_btn, btn_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_t *l = ui_label(s_ui_btn, &font_cn20, 0x0B1F1C, "按住说话");
    lv_obj_center(l);
    lv_obj_t *n = ui_label(root, &font_cn16, 0x6B7280, hint);
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -30);
    ui_paint(NULL);
    s_ui_tm = lv_timer_create(ui_paint, 250, NULL);
    power_keep_awake(true);
    g_app_close = ui_close;
    xz_open(mode);
}

void app_xiaozhi(lv_obj_t *root) { build(root, "小智", 0, 0x14B8A6, "要联网；第一次要在 xiaozhi.me 激活"); }
void app_translate(lv_obj_t *root) { build(root, "翻译", 1, 0x14B8A6, "说中文翻成英文，说英文翻成中文"); }
