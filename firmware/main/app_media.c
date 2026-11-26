/* Media apps: remote camera shutter (BLE HID). */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app.h"

extern void (*g_app_refresh)(void);
extern void (*g_app_close)(void);

/* ======================= Remote shutter ======================= */
static lv_obj_t *s_cam_btn, *s_cam_st, *s_cam_flash;
static lv_timer_t *s_cam_tm;

static void cam_paint(lv_timer_t *t)
{
    if (!s_cam_st) return;
    const char *m;
    bool ready = ble_hid_ready();
    if (ready) m = "已连上手机，打开相机后按下快门";
    else if (ble_connected()) m = "连接中…";
    else if (ble_hid_bonded()) m = "等手机连上…\n手机要开着蓝牙";
    else m = "第一次使用：手机「设置 → 蓝牙 → 配对新设备」\n选 FishWatch，配对一次就好";
    lv_label_set_text(s_cam_st, m);
    lv_obj_set_style_bg_color(s_cam_btn, lv_color_hex(ready ? 0xFFFFFF : 0x3A3A3C), 0);
}

static void flash_end(lv_timer_t *t) { if (s_cam_flash) lv_obj_add_flag(s_cam_flash, LV_OBJ_FLAG_HIDDEN); }

static void cam_shoot(lv_event_t *e)
{
    if (!ble_hid_tap(0xE9)) {          /* Volume Up: the stock camera's shutter */
        hw_haptic(80);
        ui_toast(ble_connected() ? "手机还没准备好，等一下" : "手机没有连上");
        return;
    }
    hw_haptic(30);
    sound_play(SND_CLICK);
    lv_obj_remove_flag(s_cam_flash, LV_OBJ_FLAG_HIDDEN);
    lv_timer_t *t = lv_timer_create(flash_end, 120, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void cam_close(void)
{
    if (s_cam_tm) { lv_timer_delete(s_cam_tm); s_cam_tm = NULL; }
    s_cam_st = NULL; s_cam_btn = NULL; s_cam_flash = NULL;
    power_keep_awake(false);
}

void app_camera(lv_obj_t *root)
{
    ui_header(root, "遥控拍照", 0xEC4899);
    if (!ble_is_on()) ble_start();
    s_cam_btn = lv_button_create(root);
    lv_obj_set_size(s_cam_btn, 190, 190);
    lv_obj_align(s_cam_btn, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_radius(s_cam_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_cam_btn, 10, 0);
    lv_obj_set_style_border_color(s_cam_btn, lv_color_hex(0xEC4899), 0);
    lv_obj_set_style_shadow_width(s_cam_btn, 0, 0);
    lv_obj_add_event_cb(s_cam_btn, cam_shoot, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = ui_icon(s_cam_btn, &img_icon_cm, 0x1C1C1E);
    lv_image_set_scale(ic, 520);
    lv_obj_center(ic);
    s_cam_st = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_width(s_cam_st, 366);
    lv_label_set_long_mode(s_cam_st, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_cam_st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cam_st, LV_ALIGN_TOP_MID, 0, 310);
    lv_obj_t *n = ui_label(root, &font_cn16, 0x6B7280, "原理：手表当蓝牙遥控器，按的是「音量 +」");
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -30);
    s_cam_flash = lv_obj_create(lv_obj_get_parent(root));
    lv_obj_set_size(s_cam_flash, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_cam_flash, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_cam_flash, 0, 0);
    lv_obj_remove_flag(s_cam_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cam_flash, LV_OBJ_FLAG_HIDDEN);
    cam_paint(NULL);
    s_cam_tm = lv_timer_create(cam_paint, 500, NULL);
    power_keep_awake(true);
    g_app_close = cam_close;
}

/* ======================= Network radio =======================
 * reader task: HTTP stream -> 64 KB stream buffer (PSRAM)
 * decoder task: Helix MP3 -> mono -> resample to the speaker's 24 kHz -> speaker
 * Both stacks are in PSRAM (no flash access in either), so internal RAM isn't touched.
 * Playback keeps going after leaving the app, like a phone; open it again to stop. */
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/stream_buffer.h"
#include "freertos/idf_additions.h"
#include "mp3dec.h"
#include "aacdec.h"
#include "cJSON.h"

/* q/cc: looked up by name in the Radio Browser directory when played (stream addresses change);
 * url is the fallback if that lookup fails. */
typedef struct { const char *name; const char *url; const char *q; const char *cc; } station_t;
#define STW "http://playerservices.streamtheworld.com/api/livestream-redirect/"
static const station_t STATIONS[] = {
    {"YES 933", STW "YES933AAC.aac", "YES 933", "SG"},
    {"Capital 958", STW "CAPITAL958FMAAC.aac", "958", "SG"},
    {"Love 972", STW "LOVE972FMAAC.aac", "972", "SG"},
    {"MY FM", STW "MY_FMAAC.aac", "MY FM", "MY"},
    {"988", STW "988_FMAAC.aac", "988", "MY"},
    {"BFM 89.9", STW "BFM899_AAC.aac", "BFM", "MY"},
    {"Radio Paradise", "http://stream.radioparadise.com/mp3-128"},
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Lush", "http://ice1.somafm.com/lush-128-mp3"},
    {"BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
};
#define N_BUILTIN (int)(sizeof(STATIONS) / sizeof(STATIONS[0]))
#define RD_BUF (192 * 1024)      /* ~12 s at 128 kbps: rides out WiFi hiccups */
#define RD_PREBUF (48 * 1024)    /* ~3 s before it starts (and again after running dry) */

static const char *RTAG = "radio";
static StreamBufferHandle_t s_rd_sb;
static TaskHandle_t s_rd_reader, s_rd_decoder;
static volatile bool s_rd_play;          /* user wants it playing */
static volatile int s_rd_gen;            /* bumps on every start/stop: old loops notice and quit */
static int s_rd_drops, s_rd_started = -1;
static volatile bool s_rd_aac;          /* this stream is AAC / HE-AAC (ADTS), else MP3 */                 /* reconnects in a row */
static volatile int s_rd_state;          /* 0 idle 1 wifi 2 connecting 3 buffering 4 playing 5 error */
static char s_rd_err[64];
static char s_rd_why[96];
static lv_obj_t *s_rd_name, *s_rd_st, *s_rd_vol, *s_rd_btn;
static lv_timer_t *s_rd_tm;

static int rd_count(void) { return N_BUILTIN + (g_set.radio_url[0] ? 1 : 0); }
static const char *rd_url(int i) { return i < N_BUILTIN ? STATIONS[i].url : g_set.radio_url; }
static const char *rd_name(int i) { return i < N_BUILTIN ? STATIONS[i].name : g_set.radio_name[0] ? g_set.radio_name : "自订电台"; }
bool radio_playing(void) { return s_rd_play; }

static void rd_fail(const char *m) { snprintf(s_rd_err, sizeof(s_rd_err), "%s", m); s_rd_state = 5; ESP_LOGW(RTAG, "%s", m); }

static char s_resolved[16][128];         /* built-in stations: address found in the directory this boot */
static int rb_get(const char *path, char *buf, int cap);
static bool rb_playable(void *station_json, const char **url);
static void rb_lookup(const char *q, const char *cc, char *out, int outlen);
static void reader_task(void *arg)
{
    char *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int gen = s_rd_gen;
        if (!s_rd_play || !buf) continue;
        s_rd_state = 1;
        net_wifi_use(true);
        if (!net_wait_connected(30000)) { rd_fail("WiFi 连不上"); net_wifi_use(false); continue; }
        s_rd_state = 2;
        char url[160];
        int st_i = g_set.radio_station;
        snprintf(url, sizeof(url), "%s", rd_url(st_i));
        if (st_i < N_BUILTIN && STATIONS[st_i].q) {
            if (!s_resolved[st_i][0]) rb_lookup(STATIONS[st_i].q, STATIONS[st_i].cc, s_resolved[st_i], sizeof(s_resolved[st_i]));
            if (s_resolved[st_i][0]) snprintf(url, sizeof(url), "%s", s_resolved[st_i]);
        }
        esp_http_client_handle_t c = NULL;
        int status = 0;
        for (int hop = 0; hop < 4 && gen == s_rd_gen; hop++) {        /* follow up to 3 redirects */
            esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000, .buffer_size = 2048,
                                             .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true };
            c = esp_http_client_init(&cfg);
            if (!c) break;
            esp_http_client_set_header(c, "Icy-MetaData", "0");
            esp_err_t oe = ESP_FAIL;
            for (int t = 0; t < 3 && gen == s_rd_gen; t++) {
                if ((oe = esp_http_client_open(c, 0)) == ESP_OK) break;
                net_err_text(c, oe, s_rd_why, sizeof(s_rd_why));
                esp_http_client_close(c);
                vTaskDelay(pdMS_TO_TICKS(800 * (t + 1)));
            }
            if (oe != ESP_OK) { esp_http_client_cleanup(c); c = NULL; break; }
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
            if (status == 200) {
                char *ct = NULL;
                esp_http_client_get_header(c, "Content-Type", &ct);
                s_rd_aac = (ct && strstr(ct, "aac")) || strstr(url, ".aac") || strstr(url, "AAC");
                ESP_LOGI(RTAG, "stream %s (%s)", s_rd_aac ? "AAC" : "MP3", ct ? ct : "?");
            }
            if (status >= 301 && status <= 308) {
                char *loc = NULL;
                esp_http_client_get_header(c, "Location", &loc);
                if (loc) snprintf(url, sizeof(url), "%s", loc);
                esp_http_client_close(c); esp_http_client_cleanup(c); c = NULL;
                if (!loc) break;
                continue;
            }
            break;
        }
        if (!c || status != 200) {
            char m[64];
            if (status) snprintf(m, sizeof(m), "电台连不上（%d）", status);
            else snprintf(m, sizeof(m), "电台连不上：%s", s_rd_why);
            rd_fail(m);
            if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
            net_wifi_use(false);
            continue;
        }
        if (s_rd_started != gen) {                          /* fresh start (not a reconnect mid-play) */
            s_rd_started = gen;
            s_rd_state = 3;
            xStreamBufferReset(s_rd_sb);
            xTaskNotifyGive(s_rd_decoder);
        }
        while (s_rd_play && gen == s_rd_gen) {
            int r = esp_http_client_read(c, buf, 4096);
            if (r <= 0) break;                          /* dropped: reconnect below while the buffer plays */
            for (int off = 0; off < r && s_rd_play && gen == s_rd_gen;)
                off += xStreamBufferSend(s_rd_sb, buf + off, r - off, pdMS_TO_TICKS(500));
            s_rd_drops = 0;
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        net_wifi_use(false);
        if (s_rd_play && gen == s_rd_gen) {               /* still wanted: connect again */
            if (++s_rd_drops <= 8) { vTaskDelay(pdMS_TO_TICKS(500)); xTaskNotifyGive(s_rd_reader); continue; }
            rd_fail("电台一直断线，换一台试试");
        }
        if (gen == s_rd_gen && s_rd_state != 5) s_rd_state = 0;
    }
}

static void decoder_task(void *arg)
{
    HMP3Decoder mp3 = MP3InitDecoder();
    HAACDecoder aac = NULL;                                             /* made on the first AAC stream (~60 KB PSRAM) */
    uint8_t *in = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);       /* AAC+SBR stereo: 2 x 2048 */
    int16_t *out = heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int gen = s_rd_gen;
        bool is_aac = s_rd_aac;
        if (is_aac && !aac) aac = AACInitDecoder();
        else if (is_aac) AACFlushCodec(aac);
        if (!mp3 || !in || !pcm || !out || (is_aac && !aac)) { rd_fail("内存不足"); continue; }
        while (s_rd_play && gen == s_rd_gen && xStreamBufferBytesAvailable(s_rd_sb) < RD_PREBUF && s_rd_state != 5)
            vTaskDelay(pdMS_TO_TICKS(50));                              /* pre-buffer */
        int have = 0;
        uint32_t phase = 0;                                             /* resampler position, 16.16 */
        int16_t last = 0;
        while (s_rd_play && gen == s_rd_gen) {
            have += xStreamBufferReceive(s_rd_sb, in + have, 8192 - have, pdMS_TO_TICKS(have < 2048 ? 2000 : 0));
            if (have < 1024) {
                if (s_rd_state == 5 || !s_rd_play || gen != s_rd_gen) break;
                s_rd_state = 3;                                         /* starved: fill up again, then play smoothly */
                while (s_rd_play && gen == s_rd_gen && s_rd_state != 5 && xStreamBufferBytesAvailable(s_rd_sb) < RD_PREBUF / 2)
                    vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            int rate, ch, nout;
            if (is_aac) {
                int sync = AACFindSyncWord(in, have);
                if (sync < 0) { have = 0; continue; }
                unsigned char *p = in + sync;
                int left = have - sync;
                int err = AACDecode(aac, &p, &left, pcm);
                memmove(in, p, left);
                have = left;
                if (err == ERR_AAC_INDATA_UNDERFLOW) continue;
                if (err) { if (have) memmove(in, in + 1, --have); continue; }    /* skip a bad frame */
                AACFrameInfo fi;
                AACGetLastFrameInfo(aac, &fi);
                rate = fi.sampRateOut; ch = fi.nChans; nout = fi.outputSamps;
            } else {
                int sync = MP3FindSyncWord(in, have);
                if (sync < 0) { have = 0; continue; }
                const unsigned char *p = in + sync;
                size_t left = have - sync;
                int err = MP3Decode(mp3, &p, &left, (short *)pcm, 0);
                int used = p - in;
                memmove(in, p, left);
                have = (int)left;
                if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) continue;
                if (err) { if (used == sync) { if (have) { memmove(in, in + 1, --have); } } continue; }
                MP3FrameInfo fi;
                MP3GetLastFrameInfo(mp3, &fi);
                rate = fi.samprate; ch = fi.nChans; nout = fi.outputSamps;
            }
            if (rate <= 0 || nout <= 0 || ch <= 0) continue;
            int n = nout / ch;
            if (ch == 2) for (int i = 0; i < n; i++) pcm[i] = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
            /* linear resample to the speaker's 24 kHz */
            uint32_t step = ((uint32_t)rate << 16) / 24000;
            int o = 0;
            while (o < 4096) {
                uint32_t idx = phase >> 16;
                if (idx >= (uint32_t)n) break;
                int16_t a = idx ? pcm[idx - 1] : last, b = pcm[idx];
                uint32_t fr = phase & 0xFFFF;
                out[o++] = (int16_t)(a + (((int32_t)(b - a) * (int32_t)fr) >> 16));
                phase += step;
            }
            phase -= (uint32_t)n << 16;
            last = pcm[n - 1];
            s_rd_state = 4;
            sound_play_pcm(out, o, 24000, (volatile bool *)&s_rd_play);
        }
    }
}

static bool rd_tasks(void)
{
    if (s_rd_decoder) return true;
    s_rd_sb = xStreamBufferCreateWithCaps(RD_BUF, 1, MALLOC_CAP_SPIRAM);
    if (!s_rd_sb) return false;
    if (xTaskCreateWithCaps(reader_task, "radio_rd", 6144, NULL, 4, &s_rd_reader, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    if (xTaskCreateWithCaps(decoder_task, "radio_dec", 12288, NULL, 5, &s_rd_decoder, MALLOC_CAP_SPIRAM) != pdPASS) return false;
    return true;
}

void radio_stop(void)
{
    s_rd_play = false;
    s_rd_gen++;
    s_rd_state = 0;
}

static void radio_start(void)
{
    if (!rd_tasks()) { rd_fail("内存不足"); return; }
    s_rd_gen++;
    s_rd_play = true;
    s_rd_err[0] = 0;
    s_rd_state = 1;
    xTaskNotifyGive(s_rd_reader);
}

static void rd_paint(lv_timer_t *t)
{
    if (!s_rd_st) return;
    if (g_set.radio_station >= rd_count()) g_set.radio_station = 0;
    lv_label_set_text(s_rd_name, rd_name(g_set.radio_station));
    static const char *ST[] = {"已停止", "正在打开 WiFi…", "连接电台…", "缓冲中…", "播放中", ""};
    lv_label_set_text(s_rd_st, s_rd_state == 5 ? s_rd_err : ST[s_rd_state]);
    lv_label_set_text_fmt(s_rd_vol, "音量 %d", g_set.volume);
    lv_label_set_text(lv_obj_get_child(s_rd_btn, 0), s_rd_play ? "停止" : "播放");
}

static void rd_toggle(lv_event_t *e) { if (s_rd_play) radio_stop(); else radio_start(); rd_paint(NULL); }
static void rd_next(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e), n = rd_count();
    g_set.radio_station = (g_set.radio_station + d + n) % n;
    if (s_rd_play) { radio_stop(); radio_start(); }
    rd_paint(NULL);
}
static void rd_vol(lv_event_t *e)
{
    int v = g_set.volume + (int)(intptr_t)lv_event_get_user_data(e);
    g_set.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    sound_set_volume(g_set.volume);
    rd_paint(NULL);
}
static void find_close(lv_event_t *e);
static void rd_close(void)
{
    find_close(NULL);
    if (s_rd_tm) { lv_timer_delete(s_rd_tm); s_rd_tm = NULL; }
    s_rd_st = NULL;
    store_save();                       /* station + volume */
}

/* ---------------- 找电台: Radio Browser directory (free, no key) ----------------
 * MP3 and AAC / HE-AAC streams (what the watch decodes); HLS is skipped. A station picked here becomes
 * the custom station (same slot the phone page fills) and starts playing. */
#define FIND_MAX 20
typedef struct { char name[48]; char url[128]; int kbps; } found_t;
static found_t *s_found;
static int s_nfound;
static volatile int s_find_state;       /* 0 idle 1 searching 2 done 3 failed */
static char s_find_cc[3] = "SG";
static lv_obj_t *s_find, *s_find_list, *s_find_st, *s_find_tab[2];
static lv_timer_t *s_find_tm;

/* GET https://<mirror>/json/... into buf; returns length or -1 */
static int rb_get(const char *path, char *buf, int cap)
{
    static const char *HOSTS[] = {"de1.api.radio-browser.info", "fi1.api.radio-browser.info", "nl1.api.radio-browser.info"};
    char url[260];
    int len = -1;
    for (int h = 0; h < 3 && len <= 0; h++) {
        snprintf(url, sizeof(url), "https://%s%s", HOSTS[h], path);
        esp_http_client_config_t cfg = { .url = url, .timeout_ms = 10000, .crt_bundle_attach = esp_crt_bundle_attach,
                                         .buffer_size = 4096, .buffer_size_tx = 1024 };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) continue;
        esp_http_client_set_header(c, "User-Agent", "FishWatch/1.0");
        if (esp_http_client_open(c, 0) == ESP_OK) {
            esp_http_client_fetch_headers(c);
            if (esp_http_client_get_status_code(c) == 200) {
                len = 0;
                int r;
                while (len < cap - 1 && (r = esp_http_client_read(c, buf + len, cap - 1 - len)) > 0) len += r;
                buf[len] = 0;
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    return len;
}

/* MP3 or AAC (incl. HE-AAC), not HLS / playlists, and short enough to store */
static bool rb_playable(void *js, const char **url)
{
    cJSON *it = js;
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(it, "url_resolved"));
    const char *codec = cJSON_GetStringValue(cJSON_GetObjectItem(it, "codec"));
    cJSON *hls = cJSON_GetObjectItem(it, "hls");
    if (!u || !u[0] || strlen(u) >= 128 || strstr(u, ".m3u8") || strstr(u, ".pls") || strstr(u, ".m3u")) return false;
    if (cJSON_IsNumber(hls) && hls->valueint) return false;
    if (codec && codec[0] && strcmp(codec, "MP3") && strcmp(codec, "AAC") && strcmp(codec, "AAC+") && strcmp(codec, "UNKNOWN")) return false;
    *url = u;
    return true;
}

static void rb_url_enc(const char *in, char *out, int n)
{
    int o = 0;
    for (; *in && o < n - 4; in++) {
        unsigned char c = *in;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '.') out[o++] = c;
        else o += snprintf(out + o, n - o, "%%%02X", c);
    }
    out[o] = 0;
}

static void rb_lookup(const char *q, const char *cc, char *out, int outlen)
{
    char path[200], qe[64];
    char *buf = heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    rb_url_enc(q, qe, sizeof(qe));
    snprintf(path, sizeof(path), "/json/stations/search?name=%s&countrycode=%s&hidebroken=true&order=clickcount&reverse=true&limit=10", qe, cc);
    int len = rb_get(path, buf, 32 * 1024);
    cJSON *j = len > 0 ? cJSON_Parse(buf) : NULL;
    cJSON *it;
    cJSON_ArrayForEach(it, j) {
        const char *u;
        if (rb_playable(it, &u)) { snprintf(out, outlen, "%s", u); break; }
    }
    cJSON_Delete(j);
    free(buf);
    ESP_LOGI(RTAG, "lookup %s/%s -> %s", q, cc, out[0] ? out : "(none, using fallback)");
}

static void find_task(void *arg)
{
    char path[200];
    char *buf = heap_caps_malloc(96 * 1024, MALLOC_CAP_SPIRAM);
    int len = -1;
    net_wifi_use(true);
    if (buf && net_wait_connected(30000)) {
        snprintf(path, sizeof(path), "/json/stations/search?countrycode=%s&hidebroken=true&order=clickcount&reverse=true&limit=60", s_find_cc);
        len = rb_get(path, buf, 96 * 1024);
    }
    net_wifi_use(false);
    int n = 0;
    cJSON *j = len > 0 ? cJSON_Parse(buf) : NULL;
    cJSON *it;
    cJSON_ArrayForEach(it, j) {
        if (n >= FIND_MAX) break;
        const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name")), *u;
        if (!nm || !rb_playable(it, &u)) continue;
        while (*nm == ' ' || *nm == '\t') nm++;
        snprintf(s_found[n].name, sizeof(s_found[n].name), "%s", nm);
        snprintf(s_found[n].url, sizeof(s_found[n].url), "%s", u);
        cJSON *br = cJSON_GetObjectItem(it, "bitrate");
        s_found[n].kbps = cJSON_IsNumber(br) ? br->valueint : 0;
        n++;
    }
    cJSON_Delete(j);
    free(buf);
    s_nfound = n;
    s_find_state = len > 0 ? 2 : 3;
    vTaskDelete(NULL);
}

static void find_pick(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_nfound) return;
    snprintf(g_set.radio_url, sizeof(g_set.radio_url), "%s", s_found[i].url);
    snprintf(g_set.radio_name, sizeof(g_set.radio_name), "%s", s_found[i].name);
    g_set.radio_station = N_BUILTIN;                    /* the custom slot */
    store_save();
    radio_stop();
    radio_start();
    if (s_find_tm) { lv_timer_delete(s_find_tm); s_find_tm = NULL; }
    lv_obj_delete(s_find); s_find = NULL;
    rd_paint(NULL);
}

static void find_paint(lv_timer_t *t)
{
    if (!s_find) return;
    for (int k = 0; k < 2; k++)
        lv_obj_set_style_bg_color(s_find_tab[k], lv_color_hex(!strcmp(s_find_cc, k ? "MY" : "SG") ? 0xEC4899 : C_CARD), 0);
    if (s_find_state == 1) { lv_label_set_text(s_find_st, "搜索中…"); return; }
    if (s_find_state == 3) { lv_label_set_text(s_find_st, "搜不到（WiFi 或电台目录连不上）"); s_find_state = 0; return; }
    if (s_find_state != 2) return;
    s_find_state = 0;
    lv_obj_clean(s_find_list);
    lv_label_set_text_fmt(s_find_st, s_nfound ? "找到 %d 台，点一下播放（会存成自订电台）" : "这个地区没有手表能播的电台", s_nfound);
    for (int i = 0; i < s_nfound; i++) {
        char t[80];
        snprintf(t, sizeof(t), s_found[i].kbps ? "%s · %dk" : "%s", s_found[i].name, s_found[i].kbps);
        lv_obj_t *b = ui_pill_btn(s_find_list, t, C_CARD, C_TEXT, 350, 50);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_width(l, 320);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_add_event_cb(b, find_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void find_start(void)
{
    if (s_find_state == 1) return;
    if (!s_found && !(s_found = heap_caps_calloc(FIND_MAX, sizeof(found_t), MALLOC_CAP_SPIRAM))) return;
    s_find_state = 1;
    if (s_find_list) lv_obj_clean(s_find_list);
    if (xTaskCreateWithCaps(find_task, "rd_find", 8192, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) s_find_state = 3;
    find_paint(NULL);
}
static void find_tab(lv_event_t *e) { snprintf(s_find_cc, sizeof(s_find_cc), "%s", (intptr_t)lv_event_get_user_data(e) ? "MY" : "SG"); find_start(); }
static void find_close(lv_event_t *e)
{
    if (s_find_tm) { lv_timer_delete(s_find_tm); s_find_tm = NULL; }
    if (s_find) { lv_obj_delete(s_find); s_find = NULL; }
    s_find_list = NULL;
}

static void find_open(lv_event_t *e)
{
    if (s_find) return;
    s_find = lv_obj_create(lv_screen_active());
    lv_obj_set_size(s_find, SCR_W, SCR_H);
    lv_obj_set_pos(s_find, 0, 0);
    lv_obj_set_style_bg_color(s_find, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_find, 0, 0);
    lv_obj_set_style_radius(s_find, 0, 0);
    lv_obj_set_style_pad_all(s_find, 0, 0);
    lv_obj_remove_flag(s_find, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *x = ui_pill_btn(s_find, "关闭", C_CARD, C_TEXT, 100, 50);
    lv_obj_set_pos(x, 22, 20);
    lv_obj_add_event_cb(x, find_close, LV_EVENT_CLICKED, NULL);
    for (int k = 0; k < 2; k++) {
        s_find_tab[k] = ui_pill_btn(s_find, k ? "马来西亚" : "新加坡", C_CARD, C_TEXT, 126, 50);
        lv_obj_set_pos(s_find_tab[k], 130 + k * 134, 20);
        lv_obj_add_event_cb(s_find_tab[k], find_tab, LV_EVENT_CLICKED, (void *)(intptr_t)k);
    }
    s_find_st = ui_label(s_find, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_width(s_find_st, 366);
    lv_obj_set_style_text_align(s_find_st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_find_st, LV_ALIGN_TOP_MID, 0, 80);
    s_find_list = lv_obj_create(s_find);
    lv_obj_set_size(s_find_list, 380, 390);
    lv_obj_set_pos(s_find_list, 15, 106);
    lv_obj_set_style_bg_opa(s_find_list, 0, 0);
    lv_obj_set_style_border_width(s_find_list, 0, 0);
    lv_obj_set_style_pad_all(s_find_list, 4, 0);
    lv_obj_set_flex_flow(s_find_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_find_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_find_list, 8, 0);
    lv_obj_set_scroll_dir(s_find_list, LV_DIR_VER);
    s_find_tm = lv_timer_create(find_paint, 300, NULL);
    find_start();
}

void app_radio(lv_obj_t *root)
{
    ui_header(root, "音乐", 0xEC4899);
    lv_obj_t *ic = ui_icon(root, &img_icon_mu, 0xEC4899);
    lv_image_set_scale(ic, 480);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 86);
    s_rd_name = ui_label(root, &font_cn20, C_TEXT, "");
    lv_obj_set_width(s_rd_name, 366);
    lv_obj_set_style_text_align(s_rd_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_rd_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_rd_name, LV_ALIGN_TOP_MID, 0, 186);
    s_rd_st = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_align(s_rd_st, LV_ALIGN_TOP_MID, 0, 220);
    lv_obj_t *prev = ui_pill_btn(root, "上一台", C_CARD, 0xF9A8D4, 110, 60);
    lv_obj_set_pos(prev, 22, 262);
    lv_obj_add_event_cb(prev, rd_next, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    s_rd_btn = ui_pill_btn(root, "播放", 0xEC4899, 0x1A0A12, 126, 60);
    lv_obj_set_pos(s_rd_btn, 142, 262);
    lv_obj_add_event_cb(s_rd_btn, rd_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next = ui_pill_btn(root, "下一台", C_CARD, 0xF9A8D4, 110, 60);
    lv_obj_set_pos(next, 278, 262);
    lv_obj_add_event_cb(next, rd_next, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *vm = ui_pill_btn(root, "−", C_CARD, 0xF9A8D4, 80, 56);
    lv_obj_set_pos(vm, 22, 340);
    lv_obj_add_event_cb(vm, rd_vol, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    s_rd_vol = ui_label(root, &font_cn20, C_TEXT, "");
    lv_obj_align(s_rd_vol, LV_ALIGN_TOP_MID, 0, 354);
    lv_obj_t *vp = ui_pill_btn(root, "+", C_CARD, 0xF9A8D4, 80, 56);
    lv_obj_set_pos(vp, 308, 340);
    lv_obj_add_event_cb(vp, rd_vol, LV_EVENT_CLICKED, (void *)(intptr_t)10);
    lv_obj_t *fs = ui_pill_btn(root, "找电台（新加坡／马来西亚）", C_CARD, 0xF9A8D4, 366, 52);
    lv_obj_set_pos(fs, 22, 414);
    lv_obj_add_event_cb(fs, find_open, LV_EVENT_CLICKED, NULL);
    rd_paint(NULL);
    s_rd_tm = lv_timer_create(rd_paint, 500, NULL);
    g_app_close = rd_close;
}

/* ---------------- mini player: tap the music icon (watch face or the pill on top) ---------------- */
static lv_obj_t *s_mini, *s_mini_name, *s_mini_st, *s_mini_play, *s_mini_vol;
static lv_timer_t *s_mini_tm;

static void mini_paint(lv_timer_t *t)
{
    if (!s_mini) return;
    if (g_set.radio_station >= rd_count()) g_set.radio_station = 0;
    lv_label_set_text(s_mini_name, rd_name(g_set.radio_station));
    static const char *ST[] = {"已停止", "打开 WiFi…", "连接电台…", "缓冲中…", "播放中", ""};
    lv_label_set_text(s_mini_st, s_rd_state == 5 ? s_rd_err : ST[s_rd_state]);
    lv_label_set_text(lv_obj_get_child(s_mini_play, 0), s_rd_play ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text_fmt(s_mini_vol, "音量 %d", g_set.volume);
}
static void mini_close(lv_event_t *e)
{
    if (s_mini_tm) { lv_timer_delete(s_mini_tm); s_mini_tm = NULL; }
    if (s_mini) { lv_obj_delete(s_mini); s_mini = NULL; }
    store_save();                                       /* station + volume */
}
static void mini_toggle(lv_event_t *e) { if (s_rd_play) radio_stop(); else radio_start(); mini_paint(NULL); rd_paint(NULL); }
static void mini_step(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e), n = rd_count();
    g_set.radio_station = (g_set.radio_station + d + n) % n;
    if (s_rd_play) { radio_stop(); radio_start(); }
    mini_paint(NULL); rd_paint(NULL);
}
static void mini_vol(lv_event_t *e)
{
    int v = g_set.volume + (int)(intptr_t)lv_event_get_user_data(e);
    g_set.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    sound_set_volume(g_set.volume);
    mini_paint(NULL); rd_paint(NULL);
}
static lv_obj_t *mini_btn(lv_obj_t *p, const char *sym, uint32_t bg, uint32_t fg, int w, int x, int y, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, w, 56);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, (unsigned char)sym[0] == 0xEF ? &lv_font_montserrat_14 : &font_cn20, 0);   /* LV_SYMBOL_* live in U+F000.. */
    lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return b;
}

void radio_mini_open(void)
{
    if (s_mini) { mini_close(NULL); return; }           /* second tap closes it */
    power_wake();
    /* dimmed backdrop: tapping outside the card closes it */
    s_mini = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_mini, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_mini, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mini, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_mini, 0, 0);
    lv_obj_set_style_radius(s_mini, 0, 0);
    lv_obj_remove_flag(s_mini, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_mini, mini_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *c = lv_obj_create(s_mini);
    lv_obj_set_size(c, 380, 250);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);          /* taps on the card don't reach the backdrop */
    s_mini_name = ui_label(c, &font_cn20, C_TEXT, "");
    lv_obj_set_width(s_mini_name, 340);
    lv_obj_set_style_text_align(s_mini_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_mini_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_mini_name, LV_ALIGN_TOP_MID, 0, 18);
    s_mini_st = ui_label(c, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_width(s_mini_st, 340);
    lv_obj_set_style_text_align(s_mini_st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_mini_st, LV_ALIGN_TOP_MID, 0, 50);
    mini_btn(c, LV_SYMBOL_PREV, 0x3A3A3C, 0xF9A8D4, 100, 20, 86, mini_step, -1);
    s_mini_play = mini_btn(c, LV_SYMBOL_PLAY, 0xEC4899, 0x1A0A12, 120, 130, 86, mini_toggle, 0);
    mini_btn(c, LV_SYMBOL_NEXT, 0x3A3A3C, 0xF9A8D4, 100, 260, 86, mini_step, 1);
    mini_btn(c, "−", 0x3A3A3C, 0xF9A8D4, 80, 20, 168, mini_vol, -10);
    s_mini_vol = ui_label(c, &font_cn20, C_TEXT, "");
    lv_obj_align(s_mini_vol, LV_ALIGN_TOP_MID, 0, 182);
    mini_btn(c, "+", 0x3A3A3C, 0xF9A8D4, 80, 280, 168, mini_vol, 10);
    mini_paint(NULL);
    s_mini_tm = lv_timer_create(mini_paint, 500, NULL);
}
