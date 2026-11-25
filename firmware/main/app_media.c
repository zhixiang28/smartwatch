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
