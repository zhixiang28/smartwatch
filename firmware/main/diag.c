/* Why did the watch restart last time? Reset reason + core-dump summary, shown on the watch
 * and sent to the phone, which can also download the whole dump for decoding. */
#include <stdio.h>
#include <string.h>
#include "esp_system.h"
#include "esp_log.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_app_desc.h"
#include "app.h"

static const char *TAG = "diag";
static esp_reset_reason_t s_rst;
static bool s_dump;
static size_t s_dump_size;
static char s_summary[300];

static const char *rst_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "other";
    }
}

const char *diag_reset_reason(void) { return rst_name(s_rst); }
bool diag_abnormal(void)
{
    return s_rst == ESP_RST_PANIC || s_rst == ESP_RST_INT_WDT || s_rst == ESP_RST_TASK_WDT || s_rst == ESP_RST_WDT ||
           s_rst == ESP_RST_BROWNOUT || s_rst == ESP_RST_PWR_GLITCH || s_rst == ESP_RST_CPU_LOCKUP;
}
bool diag_has_dump(void) { return s_dump; }
const char *diag_summary(void) { return s_summary; }

void diag_init(void)
{
    s_rst = esp_reset_reason();
    s_summary[0] = 0;
    size_t addr = 0;
    s_dump = esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&addr, &s_dump_size) == ESP_OK && s_dump_size > 0;
    if (s_dump) {
        esp_core_dump_summary_t *sm = calloc(1, sizeof(*sm));
        if (sm && esp_core_dump_get_summary(sm) == ESP_OK) {
            int n = snprintf(s_summary, sizeof(s_summary), "task %.15s pc 0x%08lx cause %lu vaddr 0x%08lx bt",
                             sm->exc_task, (unsigned long)sm->exc_pc, (unsigned long)sm->ex_info.exc_cause,
                             (unsigned long)sm->ex_info.exc_vaddr);
            for (uint32_t i = 0; i < sm->exc_bt_info.depth && i < 16 && n < (int)sizeof(s_summary) - 12; i++)
                n += snprintf(s_summary + n, sizeof(s_summary) - n, " 0x%08lx", (unsigned long)sm->exc_bt_info.bt[i]);
            if (sm->exc_bt_info.corrupted && n < (int)sizeof(s_summary) - 4) snprintf(s_summary + n, sizeof(s_summary) - n, " ...");
        }
        free(sm);
    }
    ESP_LOGW(TAG, "reset reason %s, core dump %s (%u bytes) %s", rst_name(s_rst), s_dump ? "present" : "none",
             (unsigned)s_dump_size, s_summary);
}

void diag_clear(void)
{
    if (s_dump) esp_core_dump_image_erase();
    s_dump = false;
    s_dump_size = 0;
    s_summary[0] = 0;
}

/* Streams the dump as JSON lines (base64 chunks) through emit(); read through the cache
 * (mmap), so it is safe from any task. */
bool diag_export(bool (*emit)(const char *line))
{
    char line[600];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(line, sizeof(line), "{\"t\":\"dumpinfo\",\"rst\":\"%s\",\"size\":%u,\"app\":\"%s\",\"elf\":\"%.16s\",\"sum\":\"%s\"}\n",
             rst_name(s_rst), (unsigned)(s_dump ? s_dump_size : 0), app->version, esp_app_get_elf_sha256_str(), s_summary);
    if (!emit(line)) return false;
    if (s_dump) {
        const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
        const uint8_t *map = NULL;
        esp_partition_mmap_handle_t h;
        if (!p || esp_partition_mmap(p, 0, s_dump_size, ESP_PARTITION_MMAP_DATA, (const void **)&map, &h) != ESP_OK) return emit("DUMP_END\n");
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        bool ok = true;
        for (size_t off = 0; off < s_dump_size && ok; off += 300) {
            size_t k = s_dump_size - off > 300 ? 300 : s_dump_size - off;
            int n = snprintf(line, sizeof(line), "{\"t\":\"dump\",\"o\":%u,\"d\":\"", (unsigned)off);
            for (size_t i = 0; i < k; i += 3) {
                uint32_t v = map[off + i] << 16 | (i + 1 < k ? map[off + i + 1] << 8 : 0) | (i + 2 < k ? map[off + i + 2] : 0);
                line[n++] = b64[v >> 18 & 63];
                line[n++] = b64[v >> 12 & 63];
                line[n++] = i + 1 < k ? b64[v >> 6 & 63] : '=';
                line[n++] = i + 2 < k ? b64[v & 63] : '=';
            }
            n += snprintf(line + n, sizeof(line) - n, "\"}\n");
            ok = emit(line);
        }
        esp_partition_munmap(h);
        if (!ok) return false;
    }
    return emit("DUMP_END\n");
}
