#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "app.h"

static const char *TAG = "store";
settings_t g_set;
static bool s_sd_ok;

static void defaults(void)
{
    memset(&g_set, 0, sizeof(g_set));
    g_set.hunger = 35;
    g_set.dirt = 10;
    g_set.brightness = 80;
    g_set.volume = 60;
    g_set.aod = true;
    g_set.tz_min = 480;          /* Singapore */
    g_set.screen_timeout = 10;
    g_set.lat = 1.29f; g_set.lon = 103.85f;
    g_set.step_goal = 8000;
    g_set.alarm_h = 7;
    g_set.remind_sit = 1;
    g_set.remind_water = 1;
}

void store_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    defaults();
    nvs_handle_t h;
    if (nvs_open("fw", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(g_set);
        settings_t tmp = g_set;            /* older, shorter blobs keep the new fields' defaults */
        if (nvs_get_blob(h, "set", &tmp, &len) == ESP_OK && len <= sizeof(g_set)) {
            g_set = tmp;
            if (len < sizeof(g_set)) { if (len < offsetof(settings_t, lat)) { g_set.screen_timeout = 10; g_set.lat = 1.29f; g_set.lon = 103.85f; g_set.step_goal = 8000; g_set.alarm_h = 7; } if (len < offsetof(settings_t, page_url)) { g_set.page_url[0] = 0; g_set.light_key[0] = 0; g_set.remind_sit = 1; g_set.remind_water = 1; g_set.game_best = 0; } }
        }
        nvs_close(h);
    }
    if (g_set.brightness < 10) g_set.brightness = 80;
    health_init();

    if (bsp_sdcard_mount() == ESP_OK) {
        s_sd_ok = true;
        mkdir(BSP_SD_MOUNT_POINT "/fishwatch", 0775);
        mkdir(BSP_SD_MOUNT_POINT "/fishwatch/notes", 0775);
        ESP_LOGI(TAG, "SD card mounted");
    } else {
        ESP_LOGW(TAG, "no SD card");
    }
}

void store_save(void)
{
    nvs_handle_t h;
    if (nvs_open("fw", NVS_READWRITE, &h) != ESP_OK) return;
    time_t now = time(NULL);
    if (net_time_valid()) g_set.saved_at = now;
    nvs_set_blob(h, "set", &g_set, sizeof(g_set));
    nvs_commit(h);
    nvs_close(h);
}

void store_save_fish(void) { store_save(); }

bool store_sd_ok(void) { return s_sd_ok; }

void store_log_set(const char *lift, float kg, int reps)
{
    if (!s_sd_ok) return;
    const char *path = BSP_SD_MOUNT_POINT "/fishwatch/workout.csv";
    struct stat st;
    bool fresh = stat(path, &st) != 0;
    FILE *f = fopen(path, "a");
    if (!f) return;
    if (fresh) fprintf(f, "time,lift,kg,reps,volume\n");
    char ts[32] = "unknown";
    time_t now = time(NULL);
    if (net_time_valid()) {
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    }
    fprintf(f, "%s,%s,%.1f,%d,%.0f\n", ts, lift, kg, reps, kg * reps);
    fclose(f);
}

bool store_save_note_bmp(const uint16_t *px, int w, int h, char *out_name, int out_len)
{
    if (!s_sd_ok) return false;
    char path[96];
    int n = 1;
    for (; n < 1000; n++) {
        snprintf(path, sizeof(path), BSP_SD_MOUNT_POINT "/fishwatch/notes/note%03d.bmp", n);
        struct stat st;
        if (stat(path, &st) != 0) break;
    }
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int row = (w * 3 + 3) & ~3;
    uint32_t size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    #define P32(o, v) do { hdr[o] = (v) & 0xFF; hdr[o+1] = ((v) >> 8) & 0xFF; hdr[o+2] = ((v) >> 16) & 0xFF; hdr[o+3] = ((v) >> 24) & 0xFF; } while (0)
    P32(2, size); P32(10, 54); P32(14, 40); P32(18, w); P32(22, h);
    hdr[26] = 1; hdr[28] = 24; P32(34, row * h);
    fwrite(hdr, 1, 54, f);
    uint8_t *line = malloc(row);
    if (!line) { fclose(f); return false; }
    memset(line, 0, row);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            uint16_t c = px[y * w + x];
            line[x * 3 + 0] = (c & 0x1F) << 3;
            line[x * 3 + 1] = ((c >> 5) & 0x3F) << 2;
            line[x * 3 + 2] = ((c >> 11) & 0x1F) << 3;
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
    snprintf(out_name, out_len, "note%03d.bmp", n);
    return true;
}

/* crash guard: count boots that never reached "stable" */
int store_boot_begin(void)
{
    nvs_handle_t h;
    uint8_t c = 0;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) return 0;
    nvs_get_u8(h, "crash", &c);
    if (c < 250) nvs_set_u8(h, "crash", c + 1);
    nvs_commit(h);
    nvs_close(h);
    return c;
}

void store_boot_ok(void)
{
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "crash", 0);
    nvs_commit(h);
    nvs_close(h);
}
