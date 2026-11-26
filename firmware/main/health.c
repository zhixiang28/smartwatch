/* Health history kept on the watch (no SD needed) and sent to the phone only
 * while it is connected over Bluetooth. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "nvs.h"
#include "esp_log.h"
#include "app.h"

#define NDAYS 30
#define NSETS 150
typedef struct { uint16_t day; uint32_t steps; } day_t;                 /* day = days since 1970 (local) */
typedef struct { uint32_t ts; uint8_t lift; uint8_t reps; uint16_t kg10; } setrec_t;
typedef struct {
    day_t days[NDAYS];
    uint16_t hour[24];
    uint16_t today;
    setrec_t sets[NSETS];
    uint16_t set_head, set_count;
    uint16_t sleep_day;          /* the morning this night belongs to */
    uint8_t sleep[60];           /* movement per 10 min, 22:00 -> 08:00 */
} health_t;

static health_t H;
static bool s_dirty;
static const char *LIFT_NAMES[5] = {"深蹲", "卧推", "硬拉", "推举", "划船"};

static uint16_t local_day(time_t t)
{
    return (uint16_t)((t + (time_t)g_set.tz_min * 60) / 86400);
}

void health_init(void)
{
    nvs_handle_t h;
    if (nvs_open("health", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(H);
        memset(&H, 0, sizeof(H));
        if (nvs_get_blob(h, "h", &H, &len) != ESP_OK || len > sizeof(H)) memset(&H, 0, sizeof(H));
        nvs_close(h);
    }
}

void health_save(void)
{
    if (!s_dirty) return;
    nvs_handle_t h;
    if (nvs_open("health", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "h", &H, sizeof(H));
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
}

/* called every 0.5 s from the power tick with the new steps */
void health_tick(uint32_t new_steps)
{
    if (!net_time_valid()) return;
    time_t now = time(NULL);
    uint16_t d = local_day(now);
    if (H.today != d) {
        if (H.today) {                                   /* close yesterday */
            memmove(&H.days[0], &H.days[1], sizeof(day_t) * (NDAYS - 1));
            uint32_t sum = 0;
            for (int i = 0; i < 24; i++) sum += H.hour[i];
            H.days[NDAYS - 1].day = H.today;
            H.days[NDAYS - 1].steps = sum;
        }
        memset(H.hour, 0, sizeof(H.hour));
        H.today = d;
        s_dirty = true;
    }
    if (new_steps) {
        struct tm tm;
        localtime_r(&now, &tm);
        H.hour[tm.tm_hour] += new_steps;
        s_dirty = true;
    }
}

/* Sleep: count how often the wrist moves in each 10-minute slot between 22:00 and 08:00.
 * Long quiet stretches = deep sleep, a little movement = light sleep, lots = awake. */
void health_sleep_tick(void)
{
    uint32_t m = hw_motion();
    if (!net_time_valid()) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int h = tm.tm_hour;
    if (!(h >= 22 || h < 8)) return;
    uint16_t night = local_day(now) + (h >= 22 ? 1 : 0);
    if (H.sleep_day != night) { H.sleep_day = night; memset(H.sleep, 0, sizeof(H.sleep)); s_dirty = true; }
    int mins = (h >= 22 ? (h - 22) * 60 : (h + 2) * 60) + tm.tm_min;
    int slot = mins / 10;
    if (slot < 0 || slot >= 60) return;
    uint32_t v = H.sleep[slot] + m;
    H.sleep[slot] = v > 254 ? 254 : v;
    if (m) s_dirty = true;
}

void health_add_set(int lift, float kg, int reps)
{
    setrec_t *r = &H.sets[H.set_head];
    r->ts = net_time_valid() ? (uint32_t)time(NULL) : 0;
    r->lift = lift;
    r->reps = reps;
    r->kg10 = (uint16_t)(kg * 10 + 0.5f);
    H.set_head = (H.set_head + 1) % NSETS;
    if (H.set_count < NSETS) H.set_count++;
    s_dirty = true;
    health_save();
}

/* Writes the export as JSON lines through emit(); returns false if emit fails. */
bool health_export(bool (*emit)(const char *line))
{
    char b[200];
    snprintf(b, sizeof(b), "{\"t\":\"info\",\"ver\":\"0.6\",\"today\":%u,\"tz\":%d,\"goal\":%lu,\"hunger\":%d,\"dirt\":%d}\n",
             H.today, g_set.tz_min, (unsigned long)g_set.step_goal, (int)g_set.hunger, (int)g_set.dirt);
    if (!emit(b)) return false;
    int n = snprintf(b, sizeof(b), "{\"t\":\"hour\",\"day\":%u,\"h\":[", H.today);
    for (int i = 0; i < 24; i++) n += snprintf(b + n, sizeof(b) - n, "%s%u", i ? "," : "", H.hour[i]);
    snprintf(b + n, sizeof(b) - n, "]}\n");
    if (!emit(b)) return false;
    for (int i = 0; i < NDAYS; i++) {
        if (!H.days[i].day) continue;
        snprintf(b, sizeof(b), "{\"t\":\"day\",\"day\":%u,\"steps\":%lu}\n", H.days[i].day, (unsigned long)H.days[i].steps);
        if (!emit(b)) return false;
    }
    for (int k = 0; k < H.set_count; k++) {
        int i = (H.set_head - H.set_count + k + NSETS) % NSETS;
        setrec_t *r = &H.sets[i];
        snprintf(b, sizeof(b), "{\"t\":\"set\",\"ts\":%lu,\"lift\":\"%s\",\"kg\":%u.%u,\"reps\":%u}\n",
                 (unsigned long)r->ts, LIFT_NAMES[r->lift % 5], r->kg10 / 10, r->kg10 % 10, r->reps);
        if (!emit(b)) return false;
    }
    if (H.sleep_day) {
        int n2 = snprintf(b, sizeof(b), "{\"t\":\"sleep\",\"day\":%u,\"m\":[", H.sleep_day);
        for (int i = 0; i < 60 && n2 < (int)sizeof(b) - 8; i++) {
            n2 += snprintf(b + n2, sizeof(b) - n2, "%s%u", i ? "," : "", H.sleep[i] == 255 ? 0 : H.sleep[i]);
            if (n2 > 150 && i < 59) {                     /* long line: split across emits */
                if (!emit(b)) return false;
                n2 = 0; b[0] = 0;
            }
        }
        snprintf(b + n2, sizeof(b) - n2, "]}\n");
        if (!emit(b)) return false;
    }
    for (int i = 0; i < 5; i++) {
        snprintf(b, sizeof(b), "{\"t\":\"pr\",\"lift\":\"%s\",\"kg\":%u.%u}\n", LIFT_NAMES[i], g_set.pr_kg10[i] / 10, g_set.pr_kg10[i] % 10);
        if (!emit(b)) return false;
    }
    return emit("END\n");
}
