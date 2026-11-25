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
static QueueHandle_t s_txq;                   /* mic packets held while (re)connecting */
static volatile bool s_listening;             /* "listen start" has been sent on this connection */
static volatile bool s_want_listen;           /* the button is held */
static volatile bool s_lost;                  /* server closed an idle connection: reconnect on the next press */
static TaskHandle_t s_xl;                     /* 翻译 worker */
static char s_xl_text[240];
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
        if (s_mode == 1 && t && t[0] && s_xl) {          /* 翻译: we only want the transcript */
            set_txt(s_xl_text, sizeof(s_xl_text), t);
            xTaskNotifyGive(s_xl);
        }
    } else if (!strcmp(type, "tts") && s_mode == 1) {
        /* 翻译 ignores 小智's own reply (the translator task aborts it) */
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
            if (!s_play || s_mode == 1) break;                                   /* aborted / translator: drop */
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
static void send_listen_start(void)
{
    send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"manual\"%s}", NULL);
    /* audio recorded while connecting goes out first */
    pkt_t *p;
    for (int pass = 0; pass < 2; pass++) {             /* again after the flag: the mic may queue one meanwhile */
        while (xQueueReceive(s_txq, &p, 0) == pdTRUE) {
            if (s_ws) esp_websocket_client_send_bin(s_ws, (const char *)p->d, p->len, pdMS_TO_TICKS(500));
            free(p);
        }
        s_listening = true;
    }
}

/* The server closes a connection that sits idle, so it is only a convenience to keep one open:
 * when it goes away we just wait for the next press and reconnect then, keeping what the user
 * says meanwhile in s_txq. */
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
        while (s_want) {
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
            if (!ok) break;                                   /* fail() already said why */
            s_lost = false;
            s_listening = false;
            if (s_want_listen) { send_listen_start(); s_st = XZ_LISTEN; }
            else if (!s_mic_on && s_listening == false && uxQueueMessagesWaiting(s_txq)) {
                /* released before we got connected: send what was said, then stop */
                send_listen_start();
                send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"%s}", NULL);
                s_listening = false;
                s_st = XZ_THINK;
            } else s_st = XZ_READY;
            while (s_want) {
                EventBits_t b = xEventGroupWaitBits(s_ev, EV_CLOSED, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
                if (b & EV_CLOSED) break;
            }
            ws_close();
            s_play = false;
            s_listening = false;
            if (!s_want) break;
            /* closed by the server: no error, reconnect on the next press */
            s_lost = true;
            if (s_st != XZ_LISTEN) s_st = XZ_READY;
            ESP_LOGI(TAG, "connection closed; will reconnect on the next press");
            while (s_want && !s_want_listen) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
            if (!s_want) break;
            s_st = XZ_LISTEN;                                 /* keep showing "listening" while we reconnect */
        }
        ws_close();
        s_play = false;
        s_lost = false;
        net_wifi_use(false);
        pkt_t *p;
        while (s_txq && xQueueReceive(s_txq, &p, 0) == pdTRUE) free(p);
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
                    if (len > 0) {
                        if (s_listening && s_ws && esp_websocket_client_is_connected(s_ws))
                            esp_websocket_client_send_bin(s_ws, (const char *)opus, len, pdMS_TO_TICKS(500));
                        else {                               /* still connecting: hold it (up to ~6 s) */
                            pkt_t *pk = heap_caps_malloc(sizeof(pkt_t) + len, MALLOC_CAP_SPIRAM);
                            if (pk) { pk->len = len; memcpy(pk->d, opus, len); if (xQueueSend(s_txq, &pk, 0) != pdTRUE) free(pk); }
                        }
                    }
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

/* ---------------- 翻译 ----------------
 * The xiaozhi server doesn't take instructions (a "detect" text is treated as a wake word and
 * gets a greeting), so 小智 is used only for speech recognition: on the transcript we abort its
 * reply, translate with Google Translate (zh -> en, anything else -> zh-CN) and speak the result
 * with Google's TTS (MP3, decoded by Helix like the radio). Both are the free, keyless endpoints
 * the Translate web page uses. */
#include "mp3dec.h"

static void urlenc(const char *in, char *out, int n)
{
    static const char hex[] = "0123456789ABCDEF";
    int o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o < n - 4; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) out[o++] = *p;
        else { out[o++] = '%'; out[o++] = hex[*p >> 4]; out[o++] = hex[*p & 15]; }
    }
    out[o] = 0;
}

static int http_get(const char *url, uint8_t *out, int outlen)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
                                     .buffer_size = 4096, .buffer_size_tx = 2048 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 Chrome/120 Mobile Safari/537.36");
    int len = -1;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        if (esp_http_client_get_status_code(c) == 200) {
            len = 0;
            int r;
            while (len < outlen - 1 && (r = esp_http_client_read(c, (char *)out + len, outlen - 1 - len)) > 0) len += r;
            out[len] = 0;
        }
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return len;
}

static void play_mp3(const uint8_t *buf, int size)
{
    HMP3Decoder dec = MP3InitDecoder();
    int16_t *pcm = heap_caps_malloc(2304 * 2, MALLOC_CAP_SPIRAM), *out = heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
    if (dec && pcm && out) {
        const unsigned char *p = buf;
        size_t left = size;
        uint32_t phase = 0;
        int16_t last = 0;
        while (left > 0 && s_play) {
            int sync = MP3FindSyncWord(p, left);
            if (sync < 0) break;
            p += sync; left -= sync;
            const unsigned char *before = p;
            if (MP3Decode(dec, &p, &left, (short *)pcm, 0)) { if (p == before) { p++; left--; } continue; }
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (fi.samprate <= 0 || fi.outputSamps <= 0) continue;
            int n = fi.outputSamps / fi.nChans;
            if (fi.nChans == 2) for (int i = 0; i < n; i++) pcm[i] = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
            uint32_t step = ((uint32_t)fi.samprate << 16) / 24000;
            int o = 0;
            while (o < 4096 && (phase >> 16) < (uint32_t)n) {
                uint32_t idx = phase >> 16;
                int16_t a = idx ? pcm[idx - 1] : last, b = pcm[idx];
                out[o++] = (int16_t)(a + (((int32_t)(b - a) * (int32_t)(phase & 0xFFFF)) >> 16));
                phase += step;
            }
            phase -= (uint32_t)n << 16;
            last = pcm[n - 1];
            sound_play_pcm(out, o, 24000, &s_play);
        }
    }
    if (dec) MP3FreeDecoder(dec);
    free(pcm);
    free(out);
}

static bool has_cjk(const char *t)
{
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) if (*p >= 0xE4 && *p <= 0xE9) return true;
    return false;
}

static void xl_task(void *arg)
{
    char *q = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM), *url = heap_caps_malloc(1400, MALLOC_CAP_SPIRAM);
    uint8_t *buf = heap_caps_malloc(160 * 1024, MALLOC_CAP_SPIRAM);
    char text[240], tr[480];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!q || !url || !buf) continue;
        send_json("{\"session_id\":\"%s\",\"type\":\"abort\"%s}", NULL);   /* stop 小智 answering */
        taskENTER_CRITICAL(&s_txt_mux);
        memcpy(text, s_xl_text, sizeof(text));
        taskEXIT_CRITICAL(&s_txt_mux);
        const char *tl = has_cjk(text) ? "en" : "zh-CN";
        urlenc(text, q, 1024);
        snprintf(url, 1400, "https://translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=%s&dt=t&q=%s", tl, q);
        s_st = XZ_THINK;
        int n = http_get(url, buf, 160 * 1024);
        tr[0] = 0;
        cJSON *j = n > 0 ? cJSON_Parse((char *)buf) : NULL;
        cJSON *segs = j ? cJSON_GetArrayItem(j, 0) : NULL;            /* [[["translated","original",...],...],...] */
        cJSON *seg;
        cJSON_ArrayForEach(seg, segs) {
            const char *part = cJSON_GetStringValue(cJSON_GetArrayItem(seg, 0));
            if (part && strlen(tr) + strlen(part) < sizeof(tr) - 1) strcat(tr, part);
        }
        cJSON_Delete(j);
        if (!tr[0]) { set_txt(s_bot, sizeof(s_bot), "翻译失败（连不上翻译服务）"); s_st = XZ_READY; continue; }
        set_txt(s_bot, sizeof(s_bot), tr);
        /* speak it: TTS takes up to ~200 characters per request */
        char say[200];
        snprintf(say, sizeof(say), "%s", tr);
        urlenc(say, q, 1024);
        snprintf(url, 1400, "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=%s&q=%s", tl, q);
        n = http_get(url, buf, 160 * 1024);
        if (n > 0) { s_st = XZ_SPEAK; s_play = true; play_mp3(buf, n); s_play = false; }
        if (s_st == XZ_SPEAK || s_st == XZ_THINK) s_st = XZ_READY;
    }
}

static bool xz_tasks(void)
{
    if (s_ctl) return true;
    s_ev = xEventGroupCreate();
    s_rxq = xQueueCreateWithCaps(40, sizeof(pkt_t *), MALLOC_CAP_SPIRAM);
    s_txq = xQueueCreateWithCaps(100, sizeof(pkt_t *), MALLOC_CAP_SPIRAM);
    if (!s_ev || !s_rxq || !s_txq) return false;
    if (xTaskCreateWithCaps(ctl_task, "xz_ctl", 8192, NULL, 4, &s_ctl, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    if (xTaskCreateWithCaps(mic_task, "xz_mic", 32768, NULL, 5, &s_mic, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    if (xTaskCreateWithCaps(spk_task, "xz_spk", 16384, NULL, 5, &s_spk, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    if (xTaskCreateWithCaps(xl_task, "xz_tr", 12288, NULL, 4, &s_xl, MALLOC_CAP_SPIRAM) != pdPASS) return false;
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
    s_want_listen = false;
    s_mic_on = false;
    s_play = false;
}

static void talk_down(void)
{
    if (s_st == XZ_ERROR || s_st == XZ_OFF) { xz_open(s_mode); return; }   /* tap to retry */
    if (s_st < XZ_READY && !s_lost) return;                                 /* still setting up */
    if (s_st == XZ_SPEAK || s_st == XZ_THINK) {                             /* interrupt 小智 */
        s_play = false;
        send_json("{\"session_id\":\"%s\",\"type\":\"abort\"%s}", NULL);
    }
    s_want_listen = true;
    s_mic_on = true;
    xTaskNotifyGive(s_mic);                                                 /* record right away */
    if (s_lost || !s_ws) xTaskNotifyGive(s_ctl);                            /* reconnect; audio is held meanwhile */
    else send_listen_start();
    s_st = XZ_LISTEN;
    hw_haptic(20);
}

static void talk_up(void)
{
    if (!s_want_listen) return;
    s_want_listen = false;
    s_mic_on = false;
    if (s_listening) {
        send_json("{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"%s}", NULL);
        s_listening = false;
        s_st = XZ_THINK;
    }
    /* not connected yet: the control task sends the held audio + stop once it is */
}

/* ---------------- UI (shared by 小智 and 翻译) ---------------- */
/* 小智's replies carry emoji; show only characters the font has (no boxes) */
static void font_clean(char *s, const lv_font_t *f)
{
    char *w = s;
    for (const char *r = s; *r;) {
        const unsigned char *u = (const unsigned char *)r;
        uint32_t i, cp;
        if (u[0] < 0x80) { cp = u[0]; i = 1; }
        else if ((u[0] & 0xE0) == 0xC0 && u[1]) { cp = (u[0] & 0x1F) << 6 | (u[1] & 0x3F); i = 2; }
        else if ((u[0] & 0xF0) == 0xE0 && u[1] && u[2]) { cp = (u[0] & 0x0F) << 12 | (u[1] & 0x3F) << 6 | (u[2] & 0x3F); i = 3; }
        else if ((u[0] & 0xF8) == 0xF0 && u[1] && u[2] && u[3]) { cp = (u[0] & 0x07) << 18 | (u[1] & 0x3F) << 12 | (u[2] & 0x3F) << 6 | (u[3] & 0x3F); i = 4; }
        else { r++; continue; }                         /* broken byte: drop */
        lv_font_glyph_dsc_t g;
        bool keep = cp == '\n' || (cp >= 0x20 && cp != 0xFE0F && cp != 0x200D && lv_font_get_glyph_dsc(f, &g, cp, 0) && g.box_w);
        if (cp == ' ') keep = true;
        if (keep) { memmove(w, r, i); w += i; }
        r += i;
    }
    *w = 0;
    /* tidy leading punctuation left behind by a removed emoji, e.g. "~你好" */
    for (;;) {
        if (*s == '~' || *s == ' ') memmove(s, s + 1, strlen(s));
        else if (!strncmp(s, "\xEF\xBD\x9E", 3)) memmove(s, s + 3, strlen(s) - 2);      /* fullwidth ～ */
        else break;
    }
}
static lv_obj_t *s_ui_st, *s_ui_user, *s_ui_bot, *s_ui_btn, *s_ui_qr;
static bool s_qr_forced;                     /* the user tapped the link: show the QR until tapped again */
static lv_timer_t *s_ui_tm;

static void ui_paint(lv_timer_t *t)
{
    xz_save_if_needed();
    if (!s_ui_st) return;
    static const char *ST[] = {"已关闭", "正在打开 WiFi…", "连接小智服务器…", "", "建立对话…", "按住下面说话", "在听…放开就送出", "想一想…", "小智在说话（按住可以打断）", ""};
    char line[120];
    if (s_st == XZ_ACTIVATE) snprintf(line, sizeof(line), "激活码 %s\n扫码打开 xiaozhi.me → 添加设备 → 输入激活码", s_code);
    else if (s_st == XZ_ERROR) snprintf(line, sizeof(line), "%s\n（点按钮重试）", s_err);
    else snprintf(line, sizeof(line), "%s", ST[s_st]);
    lv_label_set_text(s_ui_st, line);
    char u[240], b[480];
    taskENTER_CRITICAL(&s_txt_mux);
    memcpy(u, s_user, sizeof(u));
    memcpy(b, s_bot, sizeof(b));
    taskEXIT_CRITICAL(&s_txt_mux);
    /* QR for xiaozhi.me: always while waiting for activation, or when the link was tapped */
    bool qr = s_qr_forced || s_st == XZ_ACTIVATE;
    if (qr) { lv_obj_remove_flag(s_ui_qr, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_ui_bot, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_ui_user, LV_OBJ_FLAG_HIDDEN); }
    else { lv_obj_add_flag(s_ui_qr, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_ui_bot, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(s_ui_user, LV_OBJ_FLAG_HIDDEN); }
    font_clean(u, &font_cn16);
    font_clean(b, &font_cn20);
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

static void qr_tap(lv_event_t *e) { s_qr_forced = !s_qr_forced; ui_paint(NULL); }

static void ui_close(void)
{
    s_qr_forced = false;
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
    s_ui_qr = lv_qrcode_create(root);
    lv_qrcode_set_size(s_ui_qr, 168);
    lv_qrcode_set_dark_color(s_ui_qr, lv_color_black());
    lv_qrcode_set_light_color(s_ui_qr, lv_color_white());
    lv_qrcode_update(s_ui_qr, "https://xiaozhi.me", strlen("https://xiaozhi.me"));
    lv_obj_set_style_border_color(s_ui_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_ui_qr, 8, 0);          /* quiet zone so phones read it */
    lv_obj_align(s_ui_qr, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_add_flag(s_ui_qr, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_ui_qr, qr_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *n = ui_label(root, &font_cn16, 0x5EEAD4, hint);
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_flag(n, LV_OBJ_FLAG_CLICKABLE);              /* tap: show / hide the xiaozhi.me QR */
    lv_obj_set_ext_click_area(n, 16);
    lv_obj_add_event_cb(n, qr_tap, LV_EVENT_CLICKED, NULL);
    ui_paint(NULL);
    s_ui_tm = lv_timer_create(ui_paint, 250, NULL);
    power_keep_awake(true);
    g_app_close = ui_close;
    xz_open(mode);
}

void app_xiaozhi(lv_obj_t *root) { build(root, "小智", 0, 0x14B8A6, "点这里显示 xiaozhi.me 网址二维码"); }
void app_translate(lv_obj_t *root) { build(root, "翻译", 1, 0x14B8A6, "说中文翻成英文，说英文翻成中文"); }
