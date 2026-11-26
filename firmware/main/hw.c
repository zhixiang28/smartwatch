#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include "app.h"

static const char *TAG = "hw";

#define PIN_MOTOR   GPIO_NUM_18
#define PIN_PWR     GPIO_NUM_10   /* high while PWR is held */
#define PIN_BOOT    GPIO_NUM_0    /* low while BOOT is held */

#define ADDR_PMU    0x34
#define ADDR_RTC    0x51

static i2c_master_dev_handle_t s_pmu, s_rtc, s_imu;
static bool s_imu_ok;
static volatile int s_batt = -1;
static volatile bool s_chg;
static volatile bool s_rep_on;
static volatile bool s_raise_on;
static volatile int s_reps;
static esp_timer_handle_t s_motor_timer;

/* ---------------- i2c helpers ---------------- */
static i2c_master_dev_handle_t add_dev(uint8_t addr)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return NULL;
    if (i2c_master_probe(bus, addr, 50) != ESP_OK) return NULL;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    i2c_master_dev_handle_t h = NULL;
    if (i2c_master_bus_add_device(bus, &cfg, &h) != ESP_OK) return NULL;
    return h;
}

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    if (!d) return ESP_FAIL;
    return i2c_master_transmit_receive(d, &reg, 1, buf, n, 50);
}

static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    if (!d) return ESP_FAIL;
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(d, b, 2, 50);
}

/* ---------------- PMU (AXP2101) ---------------- */
static void pmu_init(void)
{
    s_pmu = add_dev(ADDR_PMU);
    if (!s_pmu) { ESP_LOGW(TAG, "AXP2101 not found"); return; }
    uint8_t v;
    if (rd(s_pmu, 0x18, &v, 1) == ESP_OK) wr(s_pmu, 0x18, v | 0x08);   /* enable fuel gauge */
}

static void pmu_poll(void)
{
    uint8_t st1, st2, pct;
    if (rd(s_pmu, 0x00, &st1, 1) != ESP_OK) return;
    if (!(st1 & 0x08)) { s_batt = -1; return; }          /* battery not connected */
    if (rd(s_pmu, 0xA4, &pct, 1) == ESP_OK && pct <= 100) s_batt = pct;
    if (rd(s_pmu, 0x01, &st2, 1) == ESP_OK) s_chg = ((st2 >> 5) & 0x03) == 0x01;
}

int hw_battery_percent(void) { return s_batt; }
bool hw_charging(void) { return s_chg; }

/* ---------------- RTC (PCF85063) ---------------- */
static uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool hw_rtc_read(time_t *out)
{
    uint8_t r[7];
    if (rd(s_rtc, 0x04, r, 7) != ESP_OK) return false;
    if (r[0] & 0x80) return false;                       /* oscillator stopped: time invalid */
    struct tm tm = {0};
    tm.tm_sec = bcd2bin(r[0] & 0x7F);
    tm.tm_min = bcd2bin(r[1] & 0x7F);
    tm.tm_hour = bcd2bin(r[2] & 0x3F);
    tm.tm_mday = bcd2bin(r[3] & 0x3F);
    tm.tm_mon = bcd2bin(r[5] & 0x1F) - 1;
    tm.tm_year = bcd2bin(r[6]) + 100;
    if (tm.tm_year < 124) return false;                  /* before 2024: not set */
    /* RTC keeps UTC */
    setenv("TZ", "UTC0", 1); tzset();
    *out = mktime(&tm);
    net_apply_tz();
    return true;
}

void hw_rtc_write(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    uint8_t b[8] = {0x04,
        bin2bcd(tm.tm_sec), bin2bcd(tm.tm_min), bin2bcd(tm.tm_hour), bin2bcd(tm.tm_mday),
        (uint8_t)tm.tm_wday, bin2bcd(tm.tm_mon + 1), bin2bcd(tm.tm_year - 100)};
    if (s_rtc) i2c_master_transmit(s_rtc, b, sizeof(b), 50);
}

/* ---------------- IMU (QMI8658) ---------------- */
static void imu_init(void)
{
    const uint8_t addrs[2] = {0x6B, 0x6A};
    for (int i = 0; i < 2 && !s_imu_ok; i++) {
        i2c_master_dev_handle_t d = add_dev(addrs[i]);
        if (!d) continue;
        uint8_t who = 0;
        if (rd(d, 0x00, &who, 1) == ESP_OK && who == 0x05) {
            s_imu = d;
            s_imu_ok = true;
            wr(d, 0x02, 0x40);      /* CTRL1: address auto-increment, little endian */
            wr(d, 0x03, 0x26);      /* CTRL2: accel +-8 g, ODR ~117 Hz */
            wr(d, 0x08, 0x01);      /* CTRL7: accel enable */
            ESP_LOGI(TAG, "QMI8658 at 0x%02x", addrs[i]);
        } else {
            i2c_master_bus_rm_device(d);
        }
    }
    if (!s_imu_ok) ESP_LOGW(TAG, "QMI8658 not found");
}

bool hw_imu_ok(void) { return s_imu_ok; }

static bool imu_read_xyz(float *ax, float *ay, float *az)
{
    uint8_t b[6];
    if (rd(s_imu, 0x35, b, 6) != ESP_OK) return false;
    const float lsb = 8.0f / 32768.0f;
    *ax = (int16_t)(b[0] | b[1] << 8) * lsb;
    *ay = (int16_t)(b[2] | b[3] << 8) * lsb;
    *az = (int16_t)(b[4] | b[5] << 8) * lsb;
    return true;
}

static bool imu_read_g(float *mag)
{
    uint8_t b[6];
    if (rd(s_imu, 0x35, b, 6) != ESP_OK) return false;
    int16_t x = (int16_t)(b[0] | b[1] << 8), y = (int16_t)(b[2] | b[3] << 8), z = (int16_t)(b[4] | b[5] << 8);
    const float lsb = 8.0f / 32768.0f;
    *mag = sqrtf((float)x * x + (float)y * y + (float)z * z) * lsb;
    return true;
}

/* Rep counter: the wrist goes up and down with the bar, so the acceleration
 * magnitude rises above 1 g when the bar is driven up and dips below when it
 * slows at the top. One full above-then-below cycle = one rep. */
static void rep_step(float g)
{
    static float lp = 1.0f;
    static int phase;
    static int64_t last_us;
    lp += (g - lp) * 0.25f;
    int64_t now = esp_timer_get_time();
    if (phase == 0 && lp > 1.22f) phase = 1;
    else if (phase == 1 && lp < 0.88f) {
        phase = 0;
        if (now - last_us > 700000) { s_reps++; last_us = now; hw_haptic(25); sound_play(SND_REP); }
    }
}

/* Step counter: walking makes the acceleration magnitude swing around 1 g about
 * twice a second. Count each swing above 1.12 g followed by a dip under 0.92 g,
 * 0.28-2 s apart. The first steps of a walk are only counted once 6 steps in a row. */
static volatile uint32_t s_steps_new, s_motion;
static volatile float s_ax, s_ay;
uint32_t hw_motion(void) { uint32_t m = s_motion; s_motion = 0; return m; }
void hw_tilt(float *x, float *y) { *x = s_ax; *y = s_ay; }
uint32_t hw_steps(void) { uint32_t n = s_steps_new; s_steps_new = 0; return n; }
static void step_step(float g)
{
    static float lp = 1.0f;
    static int phase, streak;
    static int64_t last;
    lp += (g - lp) * 0.35f;
    int64_t now = esp_timer_get_time();
    if (phase == 0 && lp > 1.12f) phase = 1;
    else if (phase == 1 && lp < 0.92f) {
        phase = 0;
        int64_t gap = now - last;
        last = now;
        if (gap > 280000 && gap < 2000000) {
            streak++;
            if (streak == 6) s_steps_new += 6;
            else if (streak > 6) s_steps_new++;
        } else streak = 1;
    }
}

void hw_rep_enable(bool on) { s_rep_on = on; }
void hw_raise_enable(bool on) { s_raise_on = on; }

/* Wrist raise: the arm hangs or swings (screen roughly vertical, little gravity on the
 * screen's Z axis), then within a second the screen turns to face up and holds still. */
static void raise_async(void *arg) { ui_raise_wake(); }
static void raise_step(float x, float y, float z)
{
    static int64_t last_vertical, cooldown;
    static int stable;
    int64_t now = esp_timer_get_time();
    float az = fabsf(z);
    if (az < 0.45f) last_vertical = now;
    if (az > 0.8f && fabsf(x) < 0.6f && fabsf(y) < 0.6f) stable++; else stable = 0;
    if (stable >= 4 && now - last_vertical < 1200000 && now > cooldown) {
        cooldown = now + 2000000;
        stable = 0;
        if (ui_lock(100)) { lv_async_call(raise_async, NULL); ui_unlock(); }
    }
}
int hw_rep_count(void) { return s_reps; }
void hw_rep_reset(void) { s_reps = 0; }

/* ---------------- vibration motor ---------------- */
static void motor_off(void *arg) { gpio_set_level(PIN_MOTOR, 0); }

void hw_haptic(int ms)
{
    if (g_set.mute || g_set.dnd) return;
    gpio_set_level(PIN_MOTOR, 1);
    esp_timer_stop(s_motor_timer);
    esp_timer_start_once(s_motor_timer, ms * 1000);
}

/* ---------------- buttons + polling task ---------------- */
static void btn_async(void *arg) { ui_button_event((int)(intptr_t)arg); }

static void hw_task(void *arg)
{
    int pwr_cnt = 0, boot_cnt = 0, tick = 0;
    for (;;) {
        bool pwr = gpio_get_level(PIN_PWR) == 1;
        bool boot = gpio_get_level(PIN_BOOT) == 0;
        if (pwr) pwr_cnt++; else { if (pwr_cnt >= 2 && pwr_cnt < 50) { if (ui_lock(100)) { lv_async_call(btn_async, (void *)0); ui_unlock(); } } pwr_cnt = 0; }
        if (boot) boot_cnt++; else { if (boot_cnt >= 2 && boot_cnt < 50) { if (ui_lock(100)) { lv_async_call(btn_async, (void *)1); ui_unlock(); } } boot_cnt = 0; }

        if (s_imu_ok && s_rep_on) {
            float g;
            if (imu_read_g(&g)) rep_step(g);
        } else if (s_imu_ok && (tick & 1)) {          /* 25 Hz: steps + wrist raise */
            float x, y, z;
            if (imu_read_xyz(&x, &y, &z)) {
                float g = sqrtf(x * x + y * y + z * z);
                s_ax = x; s_ay = y;
                if (fabsf(g - 1.0f) > 0.06f) s_motion++;
                step_step(g);
                if (s_raise_on) raise_step(x, y, z);
            }
        }
        if (s_pmu && (tick % 500) == 0) pmu_poll();     /* every 10 s */
        tick++;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void hw_init(void)
{
    gpio_config_t out = { .pin_bit_mask = 1ULL << PIN_MOTOR, .mode = GPIO_MODE_OUTPUT };
    gpio_set_level(PIN_MOTOR, 0);
    gpio_config(&out);
    gpio_config_t in = { .pin_bit_mask = (1ULL << PIN_PWR) | (1ULL << PIN_BOOT), .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_DISABLE };
    gpio_config(&in);
    gpio_set_pull_mode(PIN_BOOT, GPIO_PULLUP_ONLY);
    esp_timer_create_args_t ta = { .callback = motor_off, .name = "motor" };
    esp_timer_create(&ta, &s_motor_timer);

    bsp_i2c_init();
    pmu_init();
    s_rtc = add_dev(ADDR_RTC);
    if (!s_rtc) ESP_LOGW(TAG, "PCF85063 not found");
    imu_init();
    if (s_pmu) pmu_poll();
    xTaskCreatePinnedToCore(hw_task, "hw", 4096, NULL, 4, NULL, 0);
}
