#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_app_desc.h"
#include "app.h"

static const char *TAG = "fishwatch";
bool g_safe_mode;

static void stable_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(8000));
    store_boot_ok();
    ESP_LOGI(TAG, "stable, internal free %u, psram free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!g_safe_mode) {
        sound_init();                    /* started late so a codec problem can't stop the boot */
        ESP_LOGI(TAG, "sound started");
    }
    vTaskDelete(NULL);
}


/* After a crash: show where it happened, so a photo of the watch is enough to find the bug. */
static lv_obj_t *s_crash;
static void crash_close(lv_event_t *e) { if (lv_event_get_user_data(e)) diag_clear(); lv_obj_delete(s_crash); s_crash = NULL; }
static void crash_screen(void)
{
    s_crash = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_crash, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(s_crash, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_crash, 0, 0);
    lv_obj_set_style_radius(s_crash, 0, 0);
    lv_obj_remove_flag(s_crash, LV_OBJ_FLAG_SCROLLABLE);
    char t[400];
    snprintf(t, sizeof(t), "手表上次当机了（%s）\n拍照传给我就能找到原因：\n\n%s", diag_reset_reason(), diag_summary());
    lv_obj_t *l = ui_label(s_crash, &font_cn16, 0xFCD34D, t);
    lv_obj_set_width(l, 360);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_t *a = ui_pill_btn(s_crash, "关闭", C_CARD, C_TEXT, 170, 56);
    lv_obj_set_pos(a, 28, 410);
    lv_obj_add_event_cb(a, crash_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b = ui_pill_btn(s_crash, "清除记录", C_CARD, 0xF87171, 170, 56);
    lv_obj_set_pos(b, 212, 410);
    lv_obj_add_event_cb(b, crash_close, LV_EVENT_CLICKED, (void *)1);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FishWatch v%s starting", esp_app_get_description()->version);
    store_init();
    diag_init();
    int crashes = store_boot_begin();
    g_safe_mode = crashes >= 2;
    ESP_LOGI(TAG, "previous failed boots: %d%s", crashes, g_safe_mode ? " -> SAFE MODE" : "");
    ESP_LOGI(TAG, "internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (g_safe_mode) {
        bsp_display_start();             /* the board's default, known to work */
    } else {
        bsp_display_cfg_t dcfg = {
            .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
            .buffer_size = SCR_W * 40,       /* 40 lines, single buffer in DMA RAM */
            .double_buffer = false,
            .flags = { .buff_dma = true, .buff_spiram = false },
        };
        dcfg.lvgl_port_cfg.task_stack = 10240;
        if (!bsp_display_start_with_config(&dcfg)) ESP_LOGE(TAG, "display start failed");
    }
    bsp_display_brightness_set(g_set.power_save && g_set.brightness > 40 ? 40 : g_set.brightness);
    ESP_LOGI(TAG, "display ok, internal free %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    hw_init();
    ESP_LOGI(TAG, "hw ok");
    net_init();
    ESP_LOGI(TAG, "net ok");
    ble_init();
    ESP_LOGI(TAG, "ble ok");
    if (ui_lock(1000)) {
        ui_init();
        ui_unlock();
    }
    ESP_LOGI(TAG, "ui ok");
    if (g_safe_mode) { if (ui_lock(1000)) { ui_toast("安全模式：已关闭声音"); ui_unlock(); } }   /* LVGL runs on its own task: lock */
    else if (diag_abnormal()) {
        char t[80];
        snprintf(t, sizeof(t), "上次重启不正常（%s）%s", diag_reset_reason(), diag_has_dump() ? "，连手机看原因" : "");
        if (ui_lock(1000)) { ui_toast(t); ui_unlock(); }
    }
    if (!g_safe_mode && diag_has_dump() && diag_summary()[0] && ui_lock(1000)) { crash_screen(); ui_unlock(); }
    if (g_set.notif_on) ble_start();     /* the notification app reconnects by itself */
    hw_haptic(40);
    xTaskCreate(stable_task, "stable", 4096, NULL, 2, NULL);
}
