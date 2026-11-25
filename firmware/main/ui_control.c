#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "app.h"

void ui_close_control(void);

typedef struct { const char *name; const lv_image_dsc_t *icon; uint32_t color; } tile_t;
static const tile_t TILES[6] = {
    {"允许WiFi", &img_icon_wifi, 0x0A84FF},
    {"勿扰", &img_icon_moon, 0x5E5CE6},
    {"手电筒", &img_icon_fl, 0xFFD60A},
    {"静音", &img_icon_mute, 0xFF453A},
    {"省电", &img_icon_batt, 0x30D158},
    {"抬手亮屏", &img_icon_eye, 0x5EEAD4},
};
static lv_obj_t *s_tile[6], *s_info;
static bool s_wifi_on = true;

static bool get_state(int i)
{
    switch (i) {
    case 0: return s_wifi_on;
    case 1: return g_set.dnd;
    case 3: return g_set.mute;
    case 4: return g_set.power_save;
    case 5: return g_set.aod;
    default: return false;
    }
}

static void paint(int i)
{
    if (i < 0 || i > 5) return;
    bool on = get_state(i);
    lv_obj_set_style_bg_color(s_tile[i], lv_color_hex(on ? TILES[i].color : C_CARD), 0);
    lv_obj_t *ic = lv_obj_get_child(s_tile[i], 0);
    uint32_t fg = on && (i == 2 || i == 5) ? 0x0B1F1C : 0xFFFFFF;
    lv_obj_set_style_image_recolor(ic, lv_color_hex(fg), 0);
    lv_obj_t *lb = lv_obj_get_child(s_tile[i], 1);
    if (lb) lv_obj_set_style_text_color(lb, lv_color_hex(fg), 0);
}

static void tile_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 5) return;
    switch (i) {
    case 0: s_wifi_on = !s_wifi_on; net_wifi_enable(s_wifi_on); break;
    case 1: g_set.dnd = !g_set.dnd; break;
    case 2: ui_open_app(app_flashlight, lv_color_white()); return;
    case 3: g_set.mute = !g_set.mute; break;
    case 4: g_set.power_save = !g_set.power_save; power_apply_mode(); break;
    case 5: g_set.aod = !g_set.aod; break;
    }
    hw_haptic(15);
    sound_play(SND_CLICK);
    paint(i);
    if (i == 4) lv_label_set_text(s_info, g_set.power_save ? "省电：极简表盘、关 WiFi、5 秒熄屏" : "省电：关");
    else lv_label_set_text_fmt(s_info, "%s：%s", TILES[i].name, get_state(i) ? "开" : "关");
    store_save();
}

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    g_set.brightness = lv_slider_get_value(s);
    bsp_display_brightness_set(power_brightness());
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) store_save();
}

static void vol_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target_obj(e);
    g_set.volume = lv_slider_get_value(s);
    sound_set_volume(g_set.volume);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) sound_play(SND_CLICK);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) store_save();
}

static void close_cb(lv_event_t *e) { ui_close_control(); }

static void gesture_cb(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) ui_close_control();
}

static lv_obj_t *slider_row(lv_obj_t *p, const char *name, int val, lv_event_cb_t cb, int y)
{
    lv_obj_t *l = ui_label(p, &font_cn16, 0xA1A1AA, name);
    lv_obj_set_pos(l, 28, y + 4);
    lv_obj_t *s = lv_slider_create(p);
    lv_obj_set_size(s, 290, 30);
    lv_obj_set_pos(s, 92, y);
    lv_slider_set_range(s, name[0] == 0 ? 0 : 0, 100);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, 15, LV_PART_MAIN);
    lv_obj_set_style_radius(s, 15, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return s;
}

lv_obj_t *control_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x050505), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *close = lv_button_create(scr);
    lv_obj_set_size(close, 48, 48);
    lv_obj_set_pos(close, 22, 18);
    lv_obj_set_style_radius(close, 24, 0);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_t *ci = ui_icon(close, &img_icon_back, 0xE5E5EA);
    lv_image_set_rotation(ci, 2700);
    lv_image_set_scale(ci, 160);
    lv_obj_center(ci);
    lv_obj_add_event_cb(close, close_cb, LV_EVENT_CLICKED, NULL);

    int b = hw_battery_percent();
    lv_obj_t *bl = ui_label(scr, &font_cn20, C_GREEN, "");
    if (b >= 0) lv_label_set_text_fmt(bl, "%s电量 %d%%", hw_charging() ? "充电中 · " : "", b);
    else lv_label_set_text(bl, "电量 --");
    lv_obj_align(bl, LV_ALIGN_TOP_RIGHT, -28, 30);

    for (int i = 0; i < 6; i++) {
        lv_obj_t *t = lv_button_create(scr);
        lv_obj_set_size(t, 108, 76);
        lv_obj_set_pos(t, 26 + (i % 3) * 122, 86 + (i / 3) * 90);
        lv_obj_set_style_radius(t, 38, 0);
        lv_obj_set_style_shadow_width(t, 0, 0);
        lv_obj_t *ic = ui_icon(t, TILES[i].icon, 0xFFFFFF);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -8);
        lv_image_set_scale(ic, 190);
        lv_obj_t *lb = ui_label(t, &font_cn16, 0xFFFFFF, TILES[i].name);
        lv_obj_align(lb, LV_ALIGN_BOTTOM_MID, 0, 4);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_tile[i] = t;
        paint(i);
    }
    s_info = ui_label(scr, &font_cn16, 0xA1A1AA, "上滑或按 BOOT 键关闭");
    lv_obj_align(s_info, LV_ALIGN_TOP_MID, 0, 274);
    slider_row(scr, "亮度", g_set.brightness, bright_cb, 330);
    slider_row(scr, "音量", g_set.volume, vol_cb, 384);
    return scr;
}
