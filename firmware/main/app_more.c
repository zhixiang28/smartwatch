/* Steps, focus timer, alarm, voice recorder, weather. */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "app.h"

extern void (*g_app_refresh)(void);
extern void (*g_app_close)(void);

/* lv_obj_align_to() is a one-time placement: a label centred while its text is still short
 * (or empty) grows to the right when the text changes. Give it the arc's full width and
 * centre the text inside, so it stays centred whatever the text becomes. */
static void center_on_arc(lv_obj_t *lbl, lv_obj_t *arc, int arc_size, int dy)
{
    lv_obj_set_width(lbl, arc_size);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(lbl, arc, LV_ALIGN_CENTER, 0, dy);
}

static lv_obj_t *arc_create(lv_obj_t *root, int size, int y, uint32_t bg, uint32_t fg)
{
    lv_obj_t *a = lv_arc_create(root);
    lv_obj_set_size(a, size, size);
    lv_obj_align(a, LV_ALIGN_TOP_MID, 0, y);
    lv_arc_set_rotation(a, 270);
    lv_arc_set_bg_angles(a, 0, 360);
    lv_arc_set_range(a, 0, 1000);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(fg), LV_PART_INDICATOR);
    return a;
}

/* ======================= Steps ======================= */
static lv_obj_t *s_st_num, *s_st_arc, *s_st_sub;
static lv_timer_t *s_st_tm;
static void steps_refresh(lv_timer_t *t)
{
    uint32_t s = g_set.steps, goal = g_set.step_goal ? g_set.step_goal : 8000;
    lv_label_set_text_fmt(s_st_num, "%lu", (unsigned long)s);
    lv_arc_set_value(s_st_arc, s >= goal ? 1000 : (int)(s * 1000 / goal));
    lv_label_set_text_fmt(s_st_sub, "目标 %lu · 约 %lu.%lu 公里 · %lu 千卡", (unsigned long)goal,
                          (unsigned long)(s * 7 / 10000), (unsigned long)(s * 7 / 1000 % 10), (unsigned long)(s * 4 / 100));
}
static void steps_close(void) { if (s_st_tm) { lv_timer_delete(s_st_tm); s_st_tm = NULL; } }

void app_steps(lv_obj_t *root)
{
    ui_header(root, "计步", 0x16A34A);
    s_st_arc = arc_create(root, 250, 84, 0x10301C, 0x30D158);
    s_st_num = ui_label(root, &font_num, C_TEXT, "0");
    center_on_arc(s_st_num, s_st_arc, 250, -8);
    lv_obj_t *u = ui_label(root, &font_cn16, 0x86EFAC, "步");
    center_on_arc(u, s_st_arc, 250, 42);
    s_st_sub = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_align(s_st_sub, LV_ALIGN_TOP_MID, 0, 356);
    lv_obj_t *n = ui_label(root, &font_cn16, 0x6B7280, hw_imu_ok() ? "戴在手上走路就会计步" : "没有检测到动作感应器");
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 396);
    steps_refresh(NULL);
    s_st_tm = lv_timer_create(steps_refresh, 1000, NULL);
    g_app_close = steps_close;
}

/* ======================= Focus (pomodoro) ======================= */
static int s_fc_min = 25;
static int64_t s_fc_end, s_fc_left_us;    /* end time when running, remaining when paused */
static bool s_fc_run;
static lv_obj_t *s_fc_lbl, *s_fc_arc, *s_fc_btn, *s_fc_len;
static lv_timer_t *s_fc_tm;

static int64_t fc_left(void) { return s_fc_run ? s_fc_end - esp_timer_get_time() : s_fc_left_us; }

static void fc_paint(lv_timer_t *t)
{
    if (!s_fc_lbl) return;
    int64_t l = fc_left();
    if (l < 0) l = 0;
    int sec = (int)(l / 1000000);
    lv_label_set_text_fmt(s_fc_lbl, "%d:%02d", sec / 60, sec % 60);
    lv_arc_set_value(s_fc_arc, (int)(l / 1000 / (s_fc_min * 60)));
    lv_label_set_text(s_fc_btn, s_fc_run ? "暂停" : "开始");
    lv_label_set_text_fmt(s_fc_len, "%d 分钟", s_fc_min);
}

void focus_check(void)
{
    if (s_fc_run && esp_timer_get_time() >= s_fc_end) {
        s_fc_run = false;
        s_fc_left_us = 0;
        power_wake();
        hw_haptic(600);
        sound_play(SND_PR);
        g_set.hunger -= 5;
        if (g_set.hunger < 0) g_set.hunger = 0;
        ui_toast("专注完成，休息一下");
        fc_paint(NULL);
    }
}

static void fc_toggle(lv_event_t *e)
{
    if (s_fc_run) { s_fc_left_us = s_fc_end - esp_timer_get_time(); s_fc_run = false; }
    else {
        if (s_fc_left_us <= 0) s_fc_left_us = (int64_t)s_fc_min * 60 * 1000000;
        s_fc_end = esp_timer_get_time() + s_fc_left_us;
        s_fc_run = true;
    }
    fc_paint(NULL);
}

static void fc_len_cb(lv_event_t *e)
{
    if (s_fc_run) return;
    s_fc_min = s_fc_min == 15 ? 25 : s_fc_min == 25 ? 45 : 15;
    s_fc_left_us = (int64_t)s_fc_min * 60 * 1000000;
    fc_paint(NULL);
}

static void fc_close(void) { if (s_fc_tm) { lv_timer_delete(s_fc_tm); s_fc_tm = NULL; } s_fc_lbl = NULL; }

void app_focus(lv_obj_t *root)
{
    ui_header(root, "专注", 0xF97316);
    if (!s_fc_run && s_fc_left_us <= 0) s_fc_left_us = (int64_t)s_fc_min * 60 * 1000000;
    s_fc_arc = arc_create(root, 250, 80, 0x2A1A12, 0xFF8A5B);
    s_fc_lbl = ui_label(root, &font_num, C_TEXT, "");
    center_on_arc(s_fc_lbl, s_fc_arc, 250, -6);
    lv_obj_t *t = ui_label(root, &font_cn16, 0xFFB38A, "专心做事，鱼陪你");
    center_on_arc(t, s_fc_arc, 250, 44);
    lv_obj_t *lb = ui_pill_btn(root, NULL, C_CARD, 0xFFB38A, 170, 66);
    lv_obj_set_pos(lb, 22, 400);
    s_fc_len = ui_label(lb, &font_cn20, 0xFFB38A, "");
    lv_obj_center(s_fc_len);
    lv_obj_add_event_cb(lb, fc_len_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = ui_pill_btn(root, NULL, C_ORANGE, 0x1A0A02, 170, 66);
    lv_obj_set_pos(b, 218, 400);
    s_fc_btn = ui_label(b, &font_cn20, 0x1A0A02, "");
    lv_obj_center(s_fc_btn);
    lv_obj_add_event_cb(b, fc_toggle, LV_EVENT_CLICKED, NULL);
    fc_paint(NULL);
    s_fc_tm = lv_timer_create(fc_paint, 500, NULL);
    g_app_close = fc_close;
}

/* ======================= Alarm ======================= */
static lv_obj_t *s_al_time, *s_al_sw;
static int s_fired_min = -1;
static time_t s_snooze;
static lv_timer_t *s_ring_tm;

static void al_paint(void)
{
    lv_label_set_text_fmt(s_al_time, "%02d:%02d", g_set.alarm_h, g_set.alarm_m);
    lv_label_set_text(s_al_sw, g_set.alarm_on ? "闹钟：开" : "闹钟：关");
    lv_obj_set_style_text_color(s_al_sw, lv_color_hex(g_set.alarm_on ? 0x1A0A02 : 0xFDBA74), 0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_al_sw), lv_color_hex(g_set.alarm_on ? 0xFDBA74 : C_CARD), 0);
}
static void al_adj(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == 0) g_set.alarm_h = (g_set.alarm_h + 23) % 24;
    if (k == 1) g_set.alarm_h = (g_set.alarm_h + 1) % 24;
    if (k == 2) g_set.alarm_m = (g_set.alarm_m + 55) % 60;
    if (k == 3) g_set.alarm_m = (g_set.alarm_m + 5) % 60;
    al_paint();
}
static void al_toggle(lv_event_t *e) { g_set.alarm_on = !g_set.alarm_on; al_paint(); store_save(); }
static void al_close(void) { store_save(); }

void app_alarm(lv_obj_t *root)
{
    ui_header(root, "闹钟", 0xFDBA74);
    s_al_time = ui_label(root, &font_time, C_TEXT, "");
    lv_obj_align(s_al_time, LV_ALIGN_TOP_MID, 0, 150);
    static const char *lbl[4] = {"时 −", "时 +", "分 −", "分 +"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_pill_btn(root, lbl[i], C_CARD, 0xFDBA74, 84, 58);
        lv_obj_set_pos(b, 22 + i * 94, 86);
        lv_obj_add_event_cb(b, al_adj, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *n = ui_label(root, &font_cn16, 0xA1A1AA, net_time_valid() ? "响的时候会振动和响铃" : "手表还没对时，闹钟不会响");
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_t *sw = ui_pill_btn(root, NULL, C_CARD, 0xFDBA74, 366, 66);
    lv_obj_set_pos(sw, 22, 400);
    s_al_sw = ui_label(sw, &font_cn20, 0xFDBA74, "");
    lv_obj_center(s_al_sw);
    lv_obj_add_event_cb(sw, al_toggle, LV_EVENT_CLICKED, NULL);
    al_paint();
    g_app_close = al_close;
}

/* ringing screen */
static void ring_tick(lv_timer_t *t) { hw_haptic(500); sound_play(SND_REST_END); }
static void ring_close(void) { if (s_ring_tm) { lv_timer_delete(s_ring_tm); s_ring_tm = NULL; } power_keep_awake(false); }
static void ring_stop(lv_event_t *e) { ui_go_home(); }
static void ring_snooze(lv_event_t *e) { s_snooze = time(NULL) + 5 * 60; ui_go_home(); }

static void app_ring(lv_obj_t *root)
{
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    lv_obj_t *t = ui_label(root, &font_time, 0xFDBA74, "");
    lv_label_set_text_fmt(t, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_t *l = ui_label(root, &font_cn26, C_TEXT, "起床啦");
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 210);
    lv_obj_t *s = ui_pill_btn(root, "再睡 5 分钟", C_CARD, 0xFDBA74, 366, 66);
    lv_obj_set_pos(s, 22, 318);
    lv_obj_add_event_cb(s, ring_snooze, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = ui_pill_btn(root, "停止", 0xFDBA74, 0x1A0A02, 366, 66);
    lv_obj_set_pos(b, 22, 398);
    lv_obj_add_event_cb(b, ring_stop, LV_EVENT_CLICKED, NULL);
    power_keep_awake(true);
    s_ring_tm = lv_timer_create(ring_tick, 1500, NULL);
    ring_tick(NULL);
    g_app_close = ring_close;
}

void alarm_check(void)
{
    if (!net_time_valid()) return;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    bool due = g_set.alarm_on && tm.tm_hour == g_set.alarm_h && tm.tm_min == g_set.alarm_m && m != s_fired_min;
    bool snooze = s_snooze && now >= s_snooze;
    if (due || snooze) {
        s_fired_min = m;
        s_snooze = 0;
        power_wake();
        ui_open_app(app_ring, lv_color_hex(0xFDBA74));
    }
}

/* ======================= Voice recorder ======================= */
#define REC_RATE 24000
#define REC_MAX_S 60
static int16_t *s_rec;
static volatile int s_rec_len;            /* samples */
static volatile bool s_recording, s_playing;
static lv_obj_t *s_rec_lbl, *s_rec_dot, *s_rec_state;
static lv_timer_t *s_rec_tm;

static void rec_task(void *arg)
{
    esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
    if (!mic) { s_recording = false; vTaskDelete(NULL); return; }
    esp_codec_dev_sample_info_t fs = { .sample_rate = REC_RATE, .channel = 4, .bits_per_sample = 16, .channel_mask = 0x0F };
    if (esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) { s_recording = false; vTaskDelete(NULL); return; }
    esp_codec_dev_set_in_gain(mic, 30.0f);
    int16_t frame[256 * 4];
    s_rec_len = 0;
    while (s_recording && s_rec_len < REC_RATE * REC_MAX_S - 256) {
        if (esp_codec_dev_read(mic, frame, sizeof(frame)) != ESP_CODEC_DEV_OK) break;
        for (int i = 0; i < 256; i++) s_rec[s_rec_len++] = frame[i * 4];     /* first microphone */
    }
    esp_codec_dev_close(mic);
    s_recording = false;
    vTaskDelete(NULL);
}

static void play_task(void *arg)
{
    sound_play_pcm(s_rec, s_rec_len, REC_RATE, &s_playing);
    s_playing = false;
    vTaskDelete(NULL);
}

static void save_wav(void)
{
    if (!store_sd_ok() || s_rec_len <= 0) return;
    mkdir(BSP_SD_MOUNT_POINT "/fishwatch/rec", 0775);
    char path[80];
    for (int n = 1; n < 1000; n++) {
        snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/fishwatch/rec/rec%03d.wav", n);
        struct stat st;
        if (stat(path, &st) != 0) break;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t data = s_rec_len * 2, v;
    fwrite("RIFF", 1, 4, f); v = 36 + data; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    uint16_t pcm = 1, ch = 1, bits = 16, align = 2; fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
    v = REC_RATE; fwrite(&v, 4, 1, f); v = REC_RATE * 2; fwrite(&v, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    fwrite(s_rec, 2, s_rec_len, f);
    fclose(f);
}

static void rec_paint(lv_timer_t *t)
{
    int sec = s_rec_len / REC_RATE;
    lv_label_set_text_fmt(s_rec_lbl, "%02d:%02d", sec / 60, sec % 60);
    lv_obj_set_size(s_rec_dot, s_recording ? 34 : 60, s_recording ? 34 : 60);
    lv_obj_set_style_radius(s_rec_dot, s_recording ? 8 : 30, 0);
    lv_obj_center(s_rec_dot);
    lv_label_set_text(s_rec_state, s_recording ? "录音中…" : s_playing ? "播放中…" : s_rec_len ? (store_sd_ok() ? "已存到 SD 卡" : "没有 SD 卡，只能先听") : "点红色按钮开始");
}

static void rec_toggle(lv_event_t *e)
{
    if (s_playing) return;
    if (s_recording) { s_recording = false; vTaskDelay(pdMS_TO_TICKS(120)); save_wav(); }
    else {
        s_recording = true;
        if (xTaskCreatePinnedToCore(rec_task, "rec", 8192, NULL, 4, NULL, 1) != pdPASS) s_recording = false;
    }
    rec_paint(NULL);
}

static void rec_play(lv_event_t *e)
{
    if (s_recording || s_playing || s_rec_len <= 0) return;
    s_playing = true;
    if (xTaskCreatePinnedToCore(play_task, "play", 4096, NULL, 4, NULL, 1) != pdPASS) s_playing = false;
}

static void rec_close(void)
{
    s_recording = false;
    s_playing = false;
    if (s_rec_tm) { lv_timer_delete(s_rec_tm); s_rec_tm = NULL; }
    power_keep_awake(false);
}

void app_recorder(lv_obj_t *root)
{
    ui_header(root, "录音", 0xFCA5A5);
    if (!s_rec) s_rec = heap_caps_malloc(REC_RATE * REC_MAX_S * 2, MALLOC_CAP_SPIRAM);
    if (!s_rec) { ui_label(root, &font_cn20, C_TEXT, "内存不足"); return; }
    s_rec_lbl = ui_label(root, &font_num, C_TEXT, "00:00");
    lv_obj_align(s_rec_lbl, LV_ALIGN_TOP_MID, 0, 100);
    s_rec_state = ui_label(root, &font_cn16, 0xFCA5A5, "");
    lv_obj_align(s_rec_state, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, 104, 104);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 230);
    lv_obj_set_style_radius(b, 52, 0);
    lv_obj_set_style_bg_opa(b, 0, 0);
    lv_obj_set_style_border_width(b, 5, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xFCA5A5), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    s_rec_dot = lv_obj_create(b);
    lv_obj_set_style_bg_color(s_rec_dot, lv_color_hex(0xEF4444), 0);
    lv_obj_set_style_border_width(s_rec_dot, 0, 0);
    lv_obj_remove_flag(s_rec_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, rec_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *p = ui_pill_btn(root, "播放刚才的录音", C_CARD, 0xFCA5A5, 366, 62);
    lv_obj_set_pos(p, 22, 404);
    lv_obj_add_event_cb(p, rec_play, LV_EVENT_CLICKED, NULL);
    rec_paint(NULL);
    s_rec_tm = lv_timer_create(rec_paint, 300, NULL);
    power_keep_awake(true);
    g_app_close = rec_close;
}

/* ======================= Weather ======================= */
static lv_obj_t *s_wx_t, *s_wx_c, *s_wx_r;
static void wx_paint(void)
{
    if (g_weather.ok) {
        lv_label_set_text_fmt(s_wx_t, "%d°", (int)(g_weather.temp + (g_weather.temp >= 0 ? 0.5f : -0.5f)));
        lv_label_set_text(s_wx_c, weather_text(g_weather.code));
        lv_label_set_text_fmt(s_wx_r, "最高 %d° · 最低 %d°\n湿度 %d%% · %s 更新", (int)(g_weather.tmax + 0.5f), (int)(g_weather.tmin + 0.5f), g_weather.hum, g_weather.upd);
    } else {
        lv_label_set_text(s_wx_t, "--°");
        lv_label_set_text(s_wx_c, net_wifi_count() ? "更新中…" : "没有 WiFi");
        lv_label_set_text(s_wx_r, net_wifi_count() ? "WiFi 会自动打开一下再关掉" : "用手机设定网页加入 WiFi\n位置也从手机同步");
    }
}
static void wx_refresh_cb(lv_event_t *e) { net_weather_refresh(); ui_toast(net_wifi_count() ? "正在更新天气" : "还没有设定 WiFi"); }

void app_weather(lv_obj_t *root)
{
    ui_header(root, "天气", 0x7DD3FC);
    s_wx_t = ui_label(root, &font_time, C_TEXT, "");
    lv_obj_align(s_wx_t, LV_ALIGN_TOP_MID, 0, 90);
    s_wx_c = ui_label(root, &font_cn26, 0x7DD3FC, "");
    lv_obj_align(s_wx_c, LV_ALIGN_TOP_MID, 0, 214);
    s_wx_r = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_style_text_align(s_wx_r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wx_r, LV_ALIGN_TOP_MID, 0, 270);
    lv_obj_t *b = ui_pill_btn(root, "更新", C_CARD, 0x7DD3FC, 366, 62);
    lv_obj_set_pos(b, 22, 404);
    lv_obj_add_event_cb(b, wx_refresh_cb, LV_EVENT_CLICKED, NULL);
    wx_paint();
    g_app_refresh = wx_paint;
}
