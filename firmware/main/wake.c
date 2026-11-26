/* "你好小智" wake word: esp-sr WakeNet9 listens on the microphone in the background (when
 * switched on in 设置) and opens 小智 hands-free. Other microphone users (小智 itself, 翻译,
 * 录音) call mic_claim() / mic_release(): the wake listener closes the mic while they record. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#include "app.h"

static const char *TAG = "wake";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile int s_users;            /* others recording right now */
static volatile bool s_open;            /* the wake listener has the mic open */
static TaskHandle_t s_task;
static volatile bool s_heard;
/* Mapping the model partition stops the cache for a moment, so it must run on a task whose stack
 * is in internal RAM (app_main / the UI task), never on the PSRAM-stack wake task (0.9.3 crashed). */
static srmodel_list_t *s_models;

void mic_claim(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_users++;
    taskEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < 60 && s_open; i++) vTaskDelay(pdMS_TO_TICKS(10));   /* it lets go within one read */
}

void mic_release(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_users > 0) s_users--;
    taskEXIT_CRITICAL(&s_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

bool wake_listening(void) { return s_open; }

static void fire_async(void *arg)
{
    power_wake();
    sound_play(SND_CLICK);
    xz_from_wake();
    ui_open_app(app_xiaozhi, lv_color_hex(0x14B8A6));
}

static void wake_task(void *arg)
{
    char *name = s_models ? esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL) : NULL;
    const esp_wn_iface_t *wn = name ? esp_wn_handle_from_name(name) : NULL;
    model_iface_data_t *md = wn ? wn->create(name, DET_MODE_90) : NULL;
    if (!md) { ESP_LOGE(TAG, "wakenet model not found"); s_task = NULL; vTaskDelete(NULL); return; }
    int chunk = wn->get_samp_chunksize(md);                 /* samples at 16 kHz */
    ESP_LOGI(TAG, "%s ready, chunk %d", name, chunk);
    int16_t *frame = heap_caps_malloc(256 * 4 * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    int16_t *buf = heap_caps_malloc(chunk * 2, MALLOC_CAP_SPIRAM);
    esp_pm_lock_handle_t pm = NULL;
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "wake", &pm);
    if (!frame || !buf) { ESP_LOGE(TAG, "no memory"); s_task = NULL; vTaskDelete(NULL); return; }
    for (;;) {
        if (!g_set.wake_on || s_users > 0 || s_heard) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000)); continue; }
        esp_codec_dev_handle_t mic = bsp_audio_codec_microphone_init();
        /* the speaker shares the I2S clock: 24 kHz like everything else, resampled to 16 kHz here */
        esp_codec_dev_sample_info_t fs = { .sample_rate = 24000, .channel = 4, .bits_per_sample = 16, .channel_mask = 0x0F };
        if (!mic || esp_codec_dev_open(mic, &fs) != ESP_CODEC_DEV_OK) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000)); continue; }
        esp_codec_dev_set_in_gain(mic, 30.0f);
        s_open = true;
        if (pm) esp_pm_lock_acquire(pm);
        int n = 0;
        uint32_t phase = 0;
        int16_t prev = 0;
        wn->clean(md);
        while (g_set.wake_on && s_users == 0) {
            if (esp_codec_dev_read(mic, frame, 256 * 4 * 2) != ESP_CODEC_DEV_OK) break;
            for (;;) {
                uint32_t idx = phase >> 16;
                if (idx >= 256) break;
                int16_t a = idx ? frame[(idx - 1) * 4] : prev, b = frame[idx * 4];
                buf[n++] = (int16_t)(a + (((int32_t)(b - a) * (int32_t)(phase & 0xFFFF)) >> 16));
                phase += 0x18000;                            /* 24k -> 16k */
                if (n == chunk) {
                    n = 0;
                    if (wn->detect(md, buf) == WAKENET_DETECTED) {
                        ESP_LOGI(TAG, "你好小智!");
                        s_heard = true;
                        break;
                    }
                }
            }
            phase -= 256u << 16;
            prev = frame[255 * 4];
            if (s_heard) break;
        }
        esp_codec_dev_close(mic);
        if (pm) esp_pm_lock_release(pm);
        s_open = false;
        if (s_heard) {
            if (ui_lock(1000)) { lv_async_call(fire_async, NULL); ui_unlock(); }
            vTaskDelay(pdMS_TO_TICKS(1500));                 /* 小智 takes the mic from here */
            s_heard = false;
        }
    }
}

void wake_apply(void)
{
    if (g_safe_mode) return;
    if (g_set.wake_on && !s_models) {
        s_models = esp_srmodel_init("model");             /* called from app_main or the UI task: internal stack */
        if (!s_models) { ESP_LOGE(TAG, "no model partition"); return; }
    }
    if (g_set.wake_on && !s_task)
        xTaskCreateWithCaps(wake_task, "wake", 16384, NULL, 4, &s_task, MALLOC_CAP_SPIRAM);
    if (s_task) xTaskNotifyGive(s_task);
}
