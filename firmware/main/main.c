#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
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

void app_main(void)
{
    ESP_LOGI(TAG, "FishWatch v0.7 starting");
    store_init();
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
    if (g_safe_mode) ui_toast("安全模式：已关闭声音");
    hw_haptic(40);
    xTaskCreate(stable_task, "stable", 4096, NULL, 2, NULL);
}
