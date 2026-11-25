#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "app.h"

extern void (*g_app_refresh)(void);
extern void (*g_app_close)(void);

/* =============== Notes (drawing pad) =============== */
#define NOTE_W 366
#define NOTE_H 326
static lv_obj_t *s_canvas;
static uint16_t *s_note_buf;
static lv_color_t s_pen;
static lv_point_t s_prev;
static bool s_has_prev;
static lv_obj_t *s_sw[4];
static const uint32_t PENS[4] = {0xFFF8E6, 0xFCD34D, 0x5EEAD4, 0xF9A8D4};

static void notes_close(void) { if (s_note_buf) { heap_caps_free(s_note_buf); s_note_buf = NULL; } }

static void draw_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) { s_has_prev = false; return; }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(s_canvas, &a);
    p.x -= a.x1; p.y -= a.y1;
    if (!s_has_prev) { s_prev = p; s_has_prev = true; }
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.color = s_pen;
    d.width = 5;
    d.round_start = 1;
    d.round_end = 1;
    d.p1.x = s_prev.x; d.p1.y = s_prev.y;
    d.p2.x = p.x; d.p2.y = p.y;
    lv_draw_line(&layer, &d);
    lv_canvas_finish_layer(s_canvas, &layer);
    s_prev = p;
}

static void pen_paint(void)
{
    for (int i = 0; i < 4; i++)
        lv_obj_set_style_border_color(s_sw[i], lv_color_hex(lv_color_to_u32(s_pen) == lv_color_to_u32(lv_color_hex(PENS[i])) ? 0xFFFFFF : 0x000000), 0);
}

static void pen_cb(lv_event_t *e) { s_pen = lv_color_hex(PENS[(int)(intptr_t)lv_event_get_user_data(e)]); pen_paint(); }
static void clear_cb(lv_event_t *e) { lv_canvas_fill_bg(s_canvas, lv_color_hex(0x141109), LV_OPA_COVER); }

static void save_cb(lv_event_t *e)
{
    char name[32];
    if (store_save_note_bmp(s_note_buf, NOTE_W, NOTE_H, name, sizeof(name))) {
        char msg[64];
        snprintf(msg, sizeof(msg), "已存 %s", name);
        ui_toast(msg);
    } else ui_toast("没有 SD 卡，存不了");
}

void app_notes(lv_obj_t *root)
{
    ui_header(root, "笔记", 0xFCD34D);
    s_note_buf = heap_caps_malloc(NOTE_W * NOTE_H * 2, MALLOC_CAP_SPIRAM);
    if (!s_note_buf) { ui_label(root, &font_cn20, C_TEXT, "内存不足"); return; }
    g_app_close = notes_close;
    s_canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(s_canvas, s_note_buf, NOTE_W, NOTE_H, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(0x141109), LV_OPA_COVER);
    lv_obj_set_pos(s_canvas, 22, 84);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_canvas, draw_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_canvas, draw_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, draw_cb, LV_EVENT_PRESS_LOST, NULL);
    s_pen = lv_color_hex(PENS[0]);
    s_has_prev = false;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, 46, 46);
        lv_obj_set_pos(b, 22 + i * 52, 426);
        lv_obj_set_style_radius(b, 23, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(PENS[i]), 0);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, pen_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_sw[i] = b;
    }
    pen_paint();
    lv_obj_t *clr = ui_pill_btn(root, "清除", C_CARD, 0xFCD34D, 76, 50);
    lv_obj_set_pos(clr, 230, 424);
    lv_obj_add_event_cb(clr, clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sv = ui_pill_btn(root, "保存", 0xFCD34D, 0x2A1F00, 76, 50);
    lv_obj_set_pos(sv, 312, 424);
    lv_obj_add_event_cb(sv, save_cb, LV_EVENT_CLICKED, NULL);
}

/* =============== Phone settings (pairing code + QR) =============== */
static lv_obj_t *s_code, *s_rows[3], *s_qr, *s_bt_lbl, *s_hint_lbl, *s_code_cap;

static void row_set(int i, bool ok, const char *txt)
{
    lv_obj_t *dot = lv_obj_get_child(s_rows[i], 0);
    lv_obj_t *val = lv_obj_get_child(s_rows[i], 2);
    lv_obj_set_style_bg_color(dot, lv_color_hex(ok ? C_GREEN : 0xFF9F0A), 0);
    lv_label_set_text(val, txt);
}

static void phone_refresh(void)
{
    bool on = ble_is_on();
    if (on) {
        char data[140];
        if (g_set.page_url[0]) snprintf(data, sizeof(data), "%s?code=%04u", g_set.page_url, ble_pair_code());
        else snprintf(data, sizeof(data), "FISHWATCH:%04u", ble_pair_code());
        lv_qrcode_update(s_qr, data, strlen(data));
        lv_obj_remove_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(s_code, "%04u", ble_pair_code());
        lv_label_set_text(s_code_cap, g_set.page_url[0] ? "用手机扫描\n或输入配对码" : "第一次请打开\n设定网页输入");
        lv_obj_add_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_code_cap, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_code, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_code_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hint_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_bt_lbl, on ? "关闭蓝牙" : "开启蓝牙配对");
    row_set(0, ble_connected(), !on ? "已关闭" : ble_authed() ? "已配对" : ble_connected() ? "已连接" : "等待手机");
    char w[48];
    if (net_wifi_connected()) snprintf(w, sizeof(w), "%s", g_set.ssid);
    else if (net_wifi_count() && net_last_error()[0]) snprintf(w, sizeof(w), "%.40s", net_last_error());
    else if (net_wifi_count()) snprintf(w, sizeof(w), "记住 %d 个 · 平时关着", net_wifi_count());
    else snprintf(w, sizeof(w), "未设定");
    row_set(1, net_wifi_connected(), w);
    row_set(2, net_time_valid(), net_time_valid() ? "已对时" : "未对时");
}

static void bt_cb(lv_event_t *e) { if (ble_is_on()) ble_stop(); else { ble_new_code(); ble_start(); } phone_refresh(); }

void app_phone(lv_obj_t *root)
{
    ui_header(root, "手机设定", C_BLUE);
    s_qr = lv_qrcode_create(root);
    lv_qrcode_set_size(s_qr, 164);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, 6, 0);
    lv_obj_set_style_radius(s_qr, 8, 0);
    lv_obj_set_pos(s_qr, 22, 86);
    s_code = ui_label(root, &font_num, C_TEXT, "");
    lv_obj_set_pos(s_code, 214, 104);
    s_code_cap = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_pos(s_code_cap, 218, 180);
    s_hint_lbl = ui_label(root, &font_cn20, 0xA1A1AA, "蓝牙平时关着省电\n要设定时再打开");
    lv_obj_set_style_text_align(s_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint_lbl, LV_ALIGN_TOP_MID, 0, 140);
    static const char *names[3] = {"蓝牙", "WiFi", "时间"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *r = lv_obj_create(root);
        lv_obj_set_size(r, 366, 42);
        lv_obj_set_pos(r, 22, 272 + i * 42);
        lv_obj_set_style_bg_opa(r, 0, 0);
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(r, lv_color_hex(0x1C1C1E), 0);
        lv_obj_set_style_border_width(r, i < 2 ? 1 : 0, 0);
        lv_obj_set_style_radius(r, 0, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *dot = lv_obj_create(r);
        lv_obj_set_size(dot, 10, 10);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *n = ui_label(r, &font_cn16, C_TEXT, names[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 20, 0);
        lv_obj_t *v = ui_label(r, &font_cn16, 0xA1A1AA, "");
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
        s_rows[i] = r;
    }
    lv_obj_t *b = ui_pill_btn(root, NULL, C_BLUE, C_TEXT, 366, 62);
    lv_obj_set_pos(b, 22, 412);
    s_bt_lbl = ui_label(b, &font_cn20, C_TEXT, "");
    lv_obj_center(s_bt_lbl);
    lv_obj_add_event_cb(b, bt_cb, LV_EVENT_CLICKED, NULL);
    g_app_refresh = phone_refresh;
    phone_refresh();
}

/* =============== Watch faces =============== */
static lv_obj_t *s_face_btn[2];
static void face_paint(void)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_border_width(s_face_btn[i], g_set.face == i ? 3 : 1, 0);
        lv_obj_set_style_border_color(s_face_btn[i], lv_color_hex(g_set.face == i ? C_TEAL : 0x2A2A2A), 0);
    }
}
static void face_cb(lv_event_t *e)
{
    g_set.face = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    face_paint();
    tank_refresh_face();
    store_save();
    hw_haptic(15);
}

void app_faces(lv_obj_t *root)
{
    ui_header(root, "选择表盘", C_TEAL);
    static const char *names[2] = {"鱼缸", "极简"};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, 172, 250);
        lv_obj_set_pos(b, 22 + i * 194, 90);
        lv_obj_set_style_radius(b, 32, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_clip_corner(b, true, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        if (i == 0) {
            lv_obj_set_style_bg_image_src(b, &img_tank_bg, 0);
            lv_obj_t *f = lv_image_create(b);
            lv_image_set_src(f, &img_fish_gold_r0);
            lv_obj_align(f, LV_ALIGN_CENTER, 0, 40);
        } else {
            lv_obj_set_style_bg_color(b, lv_color_black(), 0);
        }
        lv_obj_t *t = ui_label(b, &font_cn26, i ? C_TEAL : C_TEXT, "12:45");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_t *n = ui_label(root, &font_cn20, C_TEXT, names[i]);
        lv_obj_align_to(n, b, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
        lv_obj_add_event_cb(b, face_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_face_btn[i] = b;
    }
    face_paint();
}

/* =============== Flashlight =============== */
static void fl_close(void) { bsp_display_brightness_set(power_brightness()); power_keep_awake(false); }
static void fl_tap(lv_event_t *e) { ui_back(); }

void app_flashlight(lv_obj_t *root)
{
    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, fl_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(root, &font_cn16, 0x888888, "点一下关闭");
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -30);
    bsp_display_brightness_set(100);
    power_keep_awake(true);
    g_app_close = fl_close;
}

/* =============== Stopwatch =============== */
static lv_obj_t *s_sw_lbl, *s_sw_btn_lbl;
static lv_timer_t *s_sw_tm;
static int64_t s_sw_start, s_sw_acc;
static bool s_sw_run;

static int64_t sw_ms(void) { return (s_sw_acc + (s_sw_run ? esp_timer_get_time() - s_sw_start : 0)) / 1000; }
static void sw_tick(lv_timer_t *t)
{
    int64_t ms = sw_ms();
    lv_label_set_text_fmt(s_sw_lbl, "%02d:%02d.%d", (int)(ms / 60000) % 100, (int)(ms / 1000) % 60, (int)(ms / 100) % 10);
}
static void sw_toggle(lv_event_t *e)
{
    if (s_sw_run) { s_sw_acc += esp_timer_get_time() - s_sw_start; s_sw_run = false; }
    else { s_sw_start = esp_timer_get_time(); s_sw_run = true; }
    lv_label_set_text(s_sw_btn_lbl, s_sw_run ? "暂停" : "开始");
}
static void sw_reset(lv_event_t *e) { s_sw_acc = 0; s_sw_start = esp_timer_get_time(); sw_tick(NULL); }
static void sw_close(void) { if (s_sw_tm) { lv_timer_delete(s_sw_tm); s_sw_tm = NULL; } }

void app_stopwatch(lv_obj_t *root)
{
    ui_header(root, "计时", 0xC4B5FD);
    s_sw_lbl = ui_label(root, &font_num, C_TEXT, "00:00.0");
    lv_obj_align(s_sw_lbl, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_t *r = ui_pill_btn(root, "重置", C_CARD, 0xC4B5FD, 170, 64);
    lv_obj_set_pos(r, 22, 380);
    lv_obj_add_event_cb(r, sw_reset, LV_EVENT_CLICKED, NULL);
    lv_obj_t *s = ui_pill_btn(root, NULL, 0x7C3AED, C_TEXT, 170, 64);
    lv_obj_set_pos(s, 218, 380);
    s_sw_btn_lbl = ui_label(s, &font_cn20, C_TEXT, s_sw_run ? "暂停" : "开始");
    lv_obj_center(s_sw_btn_lbl);
    lv_obj_add_event_cb(s, sw_toggle, LV_EVENT_CLICKED, NULL);
    s_sw_tm = lv_timer_create(sw_tick, 100, NULL);
    sw_tick(NULL);
    g_app_close = sw_close;
}

/* =============== Find phone =============== */
static void fp_close(void) { ble_find_phone(false); }
static void fp_stop(lv_event_t *e) { ui_back(); }

void app_findphone(lv_obj_t *root)
{
    ui_header(root, "找手机", 0xEAB308);
    lv_obj_t *ic = ui_icon(root, &img_icon_fp, 0xEAB308);
    lv_image_set_scale(ic, 700);
    lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 150);
    if (!ble_is_on()) ble_start();
    lv_obj_t *t = ui_label(root, &font_cn20, C_TEXT, ble_connected() ? "正在让手机响…" : "手机没有连上\n打开设定网页连接手表");
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_t *b = ui_pill_btn(root, "停止", C_CARD, 0xEAB308, 366, 60);
    lv_obj_set_pos(b, 22, 400);
    lv_obj_add_event_cb(b, fp_stop, LV_EVENT_CLICKED, NULL);
    ble_find_phone(true);
    sound_play(SND_FIND);
    g_app_close = fp_close;
}

/* =============== Settings / about =============== */
static lv_obj_t *s_to_lbl, *s_rem_lbl[2];
static void remind_paint(void)
{
    lv_label_set_text(s_rem_lbl[0], g_set.remind_sit ? "久坐提醒：开" : "久坐提醒：关");
    lv_label_set_text(s_rem_lbl[1], g_set.remind_water ? "喝水提醒：开" : "喝水提醒：关");
    lv_obj_set_style_text_color(s_rem_lbl[0], lv_color_hex(g_set.remind_sit ? C_TEAL : C_DIM), 0);
    lv_obj_set_style_text_color(s_rem_lbl[1], lv_color_hex(g_set.remind_water ? C_TEAL : C_DIM), 0);
}
static void remind_cb(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e) == 0) g_set.remind_sit = !g_set.remind_sit;
    else g_set.remind_water = !g_set.remind_water;
    remind_paint();
    store_save();
}
static void timeout_paint(void)
{
    if (g_set.screen_timeout) lv_label_set_text_fmt(s_to_lbl, "自动熄屏：%d 秒", g_set.screen_timeout);
    else lv_label_set_text(s_to_lbl, "自动熄屏：永不");
}
static void timeout_cb(lv_event_t *e)
{
    static const uint8_t opts[] = {5, 10, 15, 30, 60, 0};
    int i = 0;
    while (i < 6 && opts[i] != g_set.screen_timeout) i++;
    g_set.screen_timeout = opts[(i + 1) % 6];
    timeout_paint();
    store_save();
}

static lv_obj_t *set_row(lv_obj_t *root, int y, const char *k, const char *v)
{
    lv_obj_t *r = lv_obj_create(root);
    lv_obj_set_size(r, 366, 46);
    lv_obj_set_pos(r, 22, y);
    lv_obj_set_style_bg_opa(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(r, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *kl = ui_label(r, &font_cn16, 0x8E8E93, k);
    lv_obj_align(kl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *vl = ui_label(r, &font_cn16, C_TEXT, v);
    lv_obj_align(vl, LV_ALIGN_RIGHT_MID, 0, 0);
    return vl;
}

void app_settings(lv_obj_t *root)
{
    ui_header(root, "设置", 0xCBD5E1);
    char b1[16], b2[40];
    int b = hw_battery_percent();
    if (b >= 0) snprintf(b1, sizeof(b1), "%d%%%s", b, hw_charging() ? " 充电中" : ""); else snprintf(b1, sizeof(b1), "--");
    snprintf(b2, sizeof(b2), "%u 次 · %lu kg", g_set.workouts, (unsigned long)g_set.total_kg);
    set_row(root, 84, "版本", "鱼缸手表 v0.7.1");
    set_row(root, 130, "电量", b1);
    set_row(root, 176, "动作感应", hw_imu_ok() ? "正常" : "未检测到");
    set_row(root, 222, "SD 卡", store_sd_ok() ? "已插入" : "没有");
    set_row(root, 268, "训练", b2);
    s_rem_lbl[0] = NULL;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *rb = ui_pill_btn(root, NULL, C_CARD, C_TEAL, 178, 56);
        lv_obj_set_pos(rb, 22 + i * 188, 340);
        s_rem_lbl[i] = ui_label(rb, &font_cn16, C_TEAL, "");
        lv_obj_center(s_rem_lbl[i]);
        lv_obj_add_event_cb(rb, remind_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    remind_paint();
    lv_obj_t *tb = ui_pill_btn(root, NULL, C_CARD, C_TEAL, 366, 62);
    lv_obj_set_pos(tb, 22, 412);
    s_to_lbl = ui_label(tb, &font_cn20, C_TEAL, "");
    lv_obj_center(s_to_lbl);
    lv_obj_add_event_cb(tb, timeout_cb, LV_EVENT_CLICKED, NULL);
    timeout_paint();
}

/* =============== Coming soon =============== */
static const char *s_soon_name, *s_soon_desc;
void app_soon_set(const char *name, const char *desc) { s_soon_name = name; s_soon_desc = desc; }

void app_soon(lv_obj_t *root)
{
    ui_header(root, s_soon_name ? s_soon_name : "", C_TEAL);
    lv_obj_t *t = ui_label(root, &font_cn26, C_TEXT, "下一版加入");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_t *d = ui_label(root, &font_cn16, 0xA1A1AA, s_soon_desc ? s_soon_desc : "");
    lv_obj_set_width(d, 330);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 226);
}
