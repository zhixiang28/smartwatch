/* Screen timeout, wake on touch / button / wrist raise, and power-save mode. */
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "app.h"

static const char *TAG = "power";
static bool s_off;
static int s_keep;
static lv_obj_t *s_catcher;
static int64_t s_off_since;
static lv_timer_t *s_tm;

bool power_is_off(void) { return s_off; }
void power_keep_awake(bool on) { s_keep += on ? 1 : -1; if (s_keep < 0) s_keep = 0; }

int power_brightness(void)
{
    int b = g_set.brightness;
    if (g_set.power_save && b > 40) b = 40;
    return b;
}

static void catcher_cb(lv_event_t *e)
{
    /* the touch that wakes the screen is swallowed so it doesn't press a button */
    if (lv_event_get_code(e) == LV_EVENT_RELEASED || lv_event_get_code(e) == LV_EVENT_PRESS_LOST) {
        if (s_catcher) { lv_obj_delete_async(s_catcher); s_catcher = NULL; }
    } else if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        power_wake();
    }
}

void power_sleep(void)
{
    if (s_off) return;
    s_off = true;
    s_off_since = esp_timer_get_time();
    bsp_display_brightness_set(0);
    if (!s_catcher) {
        s_catcher = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_catcher, SCR_W, SCR_H);
        lv_obj_set_style_bg_opa(s_catcher, 0, 0);
        lv_obj_set_style_border_width(s_catcher, 0, 0);
        lv_obj_remove_flag(s_catcher, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_catcher, catcher_cb, LV_EVENT_ALL, NULL);
    }
    hw_raise_enable(g_set.aod);
    store_save_fish();
    health_save();
    ESP_LOGI(TAG, "screen off");
}

void power_wake(void)
{
    lv_display_trigger_activity(NULL);
    if (!s_off) return;
    s_off = false;
    hw_raise_enable(false);
    /* after a long sleep go back to the watch face, like a real watch */
    if (esp_timer_get_time() - s_off_since > 60LL * 1000000) ui_go_home();
    tank_on_show();
    lv_refr_now(NULL);
    bsp_display_brightness_set(power_brightness());
    if (s_catcher && !lv_indev_active()) { lv_obj_delete(s_catcher); s_catcher = NULL; }
}

void ui_raise_wake(void) { power_wake(); }

static void tick(lv_timer_t *t)
{
    /* The UI task is watched too: if it stops running timers for 10 s (stuck on a lock,
     * spinning) the task watchdog reboots the watch and leaves a core dump. */
    static bool wdt_added;
    if (!wdt_added) wdt_added = esp_task_wdt_add(NULL) == ESP_OK;
    else esp_task_wdt_reset();
    /* background jobs that run even with the screen off */
    uint32_t n = hw_steps();
    if (net_time_valid()) {
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        if (g_set.step_day != tm.tm_yday) { g_set.step_day = tm.tm_yday; g_set.steps = 0; }
    }
    g_set.steps += n;
    health_tick(n);
    health_sleep_tick();
    reminders_check();
    static int64_t last_hsave;
    if (esp_timer_get_time() - last_hsave > 10LL * 60 * 1000000) { last_hsave = esp_timer_get_time(); health_save(); }
    alarm_check();
    focus_check();
    ble_poll();
    static int64_t last_wx, last_ntp;
    int64_t us = esp_timer_get_time();
    if (!last_ntp) last_ntp = us;
    if (!s_off && (us - last_wx > 60LL * 60 * 1000000 || !last_wx)) { last_wx = us; net_weather_refresh(); }
    if (us - last_ntp > 6LL * 3600 * 1000000) { last_ntp = us; net_time_sync(); }
    if (s_off) return;
    int timeout = g_set.power_save ? 5 : g_set.screen_timeout;
    if (timeout == 0 || s_keep) return;
    if (lv_display_get_inactive_time(NULL) > (uint32_t)timeout * 1000) power_sleep();
}

void power_apply_mode(void)
{
    net_wifi_enable(!g_set.power_save);
    bsp_display_brightness_set(power_brightness());
    tank_refresh_face();
}

void power_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false };
    if (esp_pm_configure(&pm) == ESP_OK) ESP_LOGI(TAG, "DFS 80-240 MHz");
#endif
    s_tm = lv_timer_create(tick, 500, NULL);
    if (g_set.power_save) power_apply_mode();
}
