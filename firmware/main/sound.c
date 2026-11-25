/* Tiny synthesizer for UI sounds through the ES8311 speaker codec. */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "app.h"

#define RATE 24000
static const char *TAG = "sound";
static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_q;
static int16_t *s_buf, *s_st;
static SemaphoreHandle_t s_mx;
#define BUF_SAMPLES (RATE / 2)

typedef struct { float f0, f1; int ms; float vol; int wave; } note_t;   /* wave 0 sine, 1 triangle, 2 noise */

static int synth(const note_t *n, int16_t *out, int max)
{
    int len = RATE * n->ms / 1000;
    if (len > max) len = max;
    float ph = 0;
    uint32_t rnd = 0x1234567;
    for (int i = 0; i < len; i++) {
        float t = (float)i / len;
        float f = n->f0 + (n->f1 - n->f0) * t;
        ph += f / RATE;
        if (ph > 1) ph -= 1;
        float v;
        if (n->wave == 0) v = sinf(ph * 2 * (float)M_PI);
        else if (n->wave == 1) v = ph < 0.5f ? ph * 4 - 1 : 3 - ph * 4;
        else { rnd = rnd * 1664525u + 1013904223u; v = ((rnd >> 16) & 0xFFFF) / 32768.0f - 1; }
        float env = t < 0.08f ? t / 0.08f : (1 - t) * (1 - t) / (0.92f * 0.92f) * 1.0f;
        if (env > 1) env = 1;
        out[i] = (int16_t)(v * env * n->vol * 12000);
    }
    return len;
}

/* the speaker runs as 16-bit stereo: duplicate mono samples */
static void write_mono(const int16_t *m, int n)
{
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int off = 0; off < n; off += 1024) {
        int k = n - off > 1024 ? 1024 : n - off;
        for (int i = 0; i < k; i++) { s_st[i * 2] = m[off + i]; s_st[i * 2 + 1] = m[off + i]; }
        esp_codec_dev_write(s_spk, s_st, k * 4);
    }
    xSemaphoreGive(s_mx);
}

void sound_play_pcm(const int16_t *pcm, int samples, int rate, volatile bool *keep)
{
    if (!s_spk || rate != RATE) return;
    for (int off = 0; off < samples && (!keep || *keep); off += 4096) {
        int k = samples - off > 4096 ? 4096 : samples - off;
        write_mono(pcm + off, k);
    }
}

static void play_seq(const note_t *seq, int count, int gap_ms)
{
    int pos = 0;
    for (int k = 0; k < count && pos < BUF_SAMPLES; k++) {
        pos += synth(&seq[k], s_buf + pos, BUF_SAMPLES - pos);
        int gap = RATE * gap_ms / 1000;
        for (int g = 0; g < gap && pos < BUF_SAMPLES; g++) s_buf[pos++] = 0;
    }
    write_mono(s_buf, pos);
}

static void sound_task(void *arg)
{
    snd_t s;
    for (;;) {
        if (xQueueReceive(s_q, &s, portMAX_DELAY) != pdTRUE) continue;
        if (g_set.mute || g_set.dnd || !s_spk) continue;
        switch (s) {
        case SND_CLICK: { note_t n[] = {{2200, 1800, 14, 0.35f, 0}}; play_seq(n, 1, 0); break; }
        case SND_FEED: { note_t n[] = {{900, 1400, 50, 0.5f, 0}, {1100, 1700, 50, 0.45f, 0}, {1300, 2000, 60, 0.4f, 0}}; play_seq(n, 3, 25); break; }
        case SND_EAT: { note_t n[] = {{600, 1500, 35, 0.45f, 0}}; play_seq(n, 1, 0); break; }
        case SND_SWISH: { note_t n[] = {{0, 0, 70, 0.18f, 2}}; play_seq(n, 1, 0); break; }
        case SND_CLEAN: { note_t n[] = {{1568, 1568, 70, 0.4f, 0}, {2093, 2093, 70, 0.4f, 0}, {2637, 2637, 140, 0.4f, 0}}; play_seq(n, 3, 15); break; }
        case SND_REP: { note_t n[] = {{1400, 1400, 30, 0.45f, 1}}; play_seq(n, 1, 0); break; }
        case SND_REST_END: { note_t n[] = {{1760, 1760, 120, 0.6f, 1}, {1760, 1760, 120, 0.6f, 1}, {2349, 2349, 260, 0.6f, 1}}; play_seq(n, 3, 90); break; }
        case SND_PR: { note_t n[] = {{1047, 1047, 110, 0.5f, 1}, {1319, 1319, 110, 0.5f, 1}, {1568, 1568, 110, 0.5f, 1}, {2093, 2093, 320, 0.5f, 1}}; play_seq(n, 4, 20); break; }
        case SND_ALERT: { note_t n[] = {{700, 500, 120, 0.5f, 0}, {700, 500, 120, 0.5f, 0}}; play_seq(n, 2, 80); break; }
        case SND_FIND: { note_t n[] = {{1320, 1320, 150, 0.6f, 1}}; play_seq(n, 1, 0); break; }
        }
    }
}

void sound_set_volume(int vol)
{
    if (s_spk) esp_codec_dev_set_out_vol(s_spk, vol);
}

void sound_play(snd_t s)
{
    if (s_q) xQueueSend(s_q, &s, 0);
}

void sound_init(void)
{
    /* same I2S setup the factory voice firmware uses: 24 kHz, stereo out, TDM mic in */
    if (bsp_audio_init_voice_24k() != ESP_OK) ESP_LOGW(TAG, "voice audio mode failed, using default");
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) { ESP_LOGW(TAG, "speaker codec not available"); return; }
    esp_codec_dev_sample_info_t fs = { .sample_rate = RATE, .channel = 2, .bits_per_sample = 16 };
    if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) { ESP_LOGW(TAG, "open failed"); s_spk = NULL; return; }
    sound_set_volume(g_set.volume);
    s_buf = heap_caps_malloc(BUF_SAMPLES * 2, MALLOC_CAP_SPIRAM);
    s_st = heap_caps_malloc(1024 * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_q = xQueueCreate(6, sizeof(snd_t));
    s_mx = xSemaphoreCreateMutex();
    if (!s_buf || !s_q || !s_st || !s_mx) { s_spk = NULL; return; }
    xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 3, NULL, 0);
}
