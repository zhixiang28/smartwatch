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

typedef struct { const char *name; const char *url; } station_t;
static const station_t STATIONS[] = {
    {"Radio Paradise", "http://stream.radioparadise.com/mp3-128"},
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Lush", "http://ice1.somafm.com/lush-128-mp3"},
    {"BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
};
#define N_BUILTIN (int)(sizeof(STATIONS) / sizeof(STATIONS[0]))
#define RD_BUF (64 * 1024)
#define RD_PREBUF (24 * 1024)

static const char *RTAG = "radio";
static StreamBufferHandle_t s_rd_sb;
static TaskHandle_t s_rd_reader, s_rd_decoder;
static volatile bool s_rd_play;          /* user wants it playing */
static volatile int s_rd_gen;            /* bumps on every start/stop: old loops notice and quit */
static volatile int s_rd_state;          /* 0 idle 1 wifi 2 connecting 3 buffering 4 playing 5 error */
static char s_rd_err[64];
static lv_obj_t *s_rd_name, *s_rd_st, *s_rd_vol, *s_rd_btn;
static lv_timer_t *s_rd_tm;

static int rd_count(void) { return N_BUILTIN + (g_set.radio_url[0] ? 1 : 0); }
static const char *rd_url(int i) { return i < N_BUILTIN ? STATIONS[i].url : g_set.radio_url; }
static const char *rd_name(int i) { return i < N_BUILTIN ? STATIONS[i].name : "自订电台"; }
bool radio_playing(void) { return s_rd_play; }

static void rd_fail(const char *m) { snprintf(s_rd_err, sizeof(s_rd_err), "%s", m); s_rd_state = 5; ESP_LOGW(RTAG, "%s", m); }

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
        snprintf(url, sizeof(url), "%s", rd_url(g_set.radio_station));
        esp_http_client_handle_t c = NULL;
        int status = 0;
        for (int hop = 0; hop < 4 && gen == s_rd_gen; hop++) {        /* follow up to 3 redirects */
            esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000, .buffer_size = 2048,
                                             .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true };
            c = esp_http_client_init(&cfg);
            if (!c) break;
            esp_http_client_set_header(c, "Icy-MetaData", "0");
            if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); c = NULL; break; }
            esp_http_client_fetch_headers(c);
            status = esp_http_client_get_status_code(c);
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
            char m[64]; snprintf(m, sizeof(m), "电台连不上（%d）", status); rd_fail(m);
            if (c) { esp_http_client_close(c); esp_http_client_cleanup(c); }
            net_wifi_use(false);
            continue;
        }
        s_rd_state = 3;
        xStreamBufferReset(s_rd_sb);
        xTaskNotifyGive(s_rd_decoder);
        while (s_rd_play && gen == s_rd_gen) {
            int r = esp_http_client_read(c, buf, 4096);
            if (r <= 0) { if (s_rd_play && gen == s_rd_gen) rd_fail("电台断线了"); break; }
            for (int off = 0; off < r && s_rd_play && gen == s_rd_gen;)
                off += xStreamBufferSend(s_rd_sb, buf + off, r - off, pdMS_TO_TICKS(500));
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        net_wifi_use(false);
        if (gen == s_rd_gen && s_rd_state != 5) s_rd_state = 0;
    }
}

static void decoder_task(void *arg)
{
    HMP3Decoder dec = MP3InitDecoder();
    uint8_t *in = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(2304 * 2, MALLOC_CAP_SPIRAM);
    int16_t *out = heap_caps_malloc(4096 * 2, MALLOC_CAP_SPIRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int gen = s_rd_gen;
        if (!dec || !in || !pcm || !out) { rd_fail("内存不足"); continue; }
        while (s_rd_play && gen == s_rd_gen && xStreamBufferBytesAvailable(s_rd_sb) < RD_PREBUF && s_rd_state != 5)
            vTaskDelay(pdMS_TO_TICKS(50));                              /* pre-buffer ~1.5 s */
        int have = 0;
        uint32_t phase = 0;                                             /* resampler position, 16.16 */
        int16_t last = 0;
        while (s_rd_play && gen == s_rd_gen) {
            have += xStreamBufferReceive(s_rd_sb, in + have, 8192 - have, pdMS_TO_TICKS(have < 2048 ? 2000 : 0));
            if (have < 1024) {
                if (s_rd_state == 5 || !s_rd_play || gen != s_rd_gen) break;
                s_rd_state = 3;                                         /* starved: show buffering */
                continue;
            }
            int sync = MP3FindSyncWord(in, have);
            if (sync < 0) { have = 0; continue; }
            const unsigned char *p = in + sync;
            size_t left = have - sync;
            int err = MP3Decode(dec, &p, &left, (short *)pcm, 0);
            int used = p - in;
            memmove(in, p, left);
            have = (int)left;
            if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) continue;
            if (err) { if (used == sync) { if (have) { memmove(in, in + 1, --have); } } continue; }   /* skip a bad frame */
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(dec, &fi);
            if (fi.samprate <= 0 || fi.outputSamps <= 0) continue;
            int n = fi.outputSamps / fi.nChans;
            if (fi.nChans == 2) for (int i = 0; i < n; i++) pcm[i] = (pcm[2 * i] + pcm[2 * i + 1]) / 2;
            /* linear resample to 24 kHz */
            uint32_t step = ((uint32_t)fi.samprate << 16) / 24000;
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
static void rd_close(void)
{
    if (s_rd_tm) { lv_timer_delete(s_rd_tm); s_rd_tm = NULL; }
    s_rd_st = NULL;
    store_save();                       /* station + volume */
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
    lv_obj_t *n = ui_label(root, &font_cn16, 0x6B7280, "离开也会继续播；自订电台在手机设定网页填");
    lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, -26);
    rd_paint(NULL);
    s_rd_tm = lv_timer_create(rd_paint, 500, NULL);
    g_app_close = rd_close;
}
