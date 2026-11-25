/* Office light (Supabase), tilt game, sit / water reminders. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "app.h"

extern void (*g_app_refresh)(void);
extern void (*g_app_close)(void);
static const char *TAG = "extra";

/* ======================= Office light ======================= */
#define SB_URL "https://ogxuwchojexznsxvvdhi.supabase.co"
#define SB_KEY "sb_publishable_pB5HMEXMjVDQiNcWlenV1A_ETc2qaNP"   /* publishable key: safe to ship */

static char s_l_state[2][4];               /* "ON" / "OFF" / "" */
static volatile bool s_l_busy;
static volatile int s_l_err;               /* 0 ok, 1 network, 2 key */
static char s_l_detail[96];
static lv_obj_t *s_l_btn[2], *s_l_info;

static int http_call(const char *path, const char *body, char *out, int outlen)
{
    char url[200];
    snprintf(url, sizeof(url), SB_URL "%s", path);
    /* 2 KB receive buffer: Supabase / Cloudflare reply headers are long, and a header line that
     * does not fit the default 512 bytes fails the whole read. */
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 8000, .crt_bundle_attach = esp_crt_bundle_attach,
                                     .method = body ? HTTP_METHOD_POST : HTTP_METHOD_GET,
                                     .buffer_size = 2048, .buffer_size_tx = 1024 };
    out[0] = 0;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(s_l_detail, sizeof(s_l_detail), "开不了连线（内存不足 %u/%u）",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return -2;
    }
    esp_http_client_set_header(c, "apikey", SB_KEY);
    if (body) esp_http_client_set_header(c, "Content-Type", "application/json");
    int len = 0, status = -1;
    esp_err_t oe = esp_http_client_open(c, body ? strlen(body) : 0);
    if (oe != ESP_OK) { snprintf(s_l_detail, sizeof(s_l_detail), "连不上服务器：%s", esp_err_to_name(oe)); ESP_LOGW(TAG, "http open: %s", esp_err_to_name(oe)); }
    if (oe == ESP_OK) {
        int w = body ? esp_http_client_write(c, body, strlen(body)) : 0;
        int64_t cl = w < 0 ? ESP_FAIL : esp_http_client_fetch_headers(c);
        for (int i = 0; i < 3 && cl == -ESP_ERR_HTTP_EAGAIN; i++) cl = esp_http_client_fetch_headers(c);   /* not ready yet: keep waiting */
        status = esp_http_client_get_status_code(c);
        if (status <= 0) {
            /* -1 from fetch_headers can also just mean "no Content-Length", so judge by the status line */
            snprintf(s_l_detail, sizeof(s_l_detail), "读不到回应（%lld，%d，%d）", (long long)cl, w, esp_http_client_get_errno(c));
            ESP_LOGW(TAG, "http: no status (fetch %lld, write %d, errno %d)", (long long)cl, w, esp_http_client_get_errno(c));
            status = -1;
        } else {
            int r;
            while (len < outlen - 1 && (r = esp_http_client_read(c, out + len, outlen - 1 - len)) > 0) len += r;
        }
    }
    out[len] = 0;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static void light_paint(void);
static void light_async(void *arg) { if (s_l_info) light_paint(); }
static void light_kick(void) { if (ui_lock(300)) { lv_async_call(light_async, NULL); ui_unlock(); } }

/* One worker, created on first use with its stack in PSRAM and then kept. It used to be a
 * fresh 8 KB task per tap with the stack in internal RAM; with WiFi + BLE + the display
 * running that allocation failed, light_run() silently gave up, and the screen just said
 * "点一下开关" with "--" while nothing was ever sent. */
static TaskHandle_t s_l_task;
static volatile int s_l_req;

static void light_job(int ch)
{
    /* ch: 0 = only read, 1/2 = toggle that channel */
    char buf[256];
    s_l_detail[0] = 0;
    if (!net_wait_connected(30000)) {
        snprintf(s_l_detail, sizeof(s_l_detail), "WiFi：%s", net_last_error()[0] ? net_last_error() : "还在找网络");
        s_l_err = 1; s_l_busy = false; light_kick(); return;
    }
    if (ch && g_set.light_person[0] && g_set.light_pin[0]) {
        const char *now = s_l_state[ch - 1];
        char body[200], pin[70], who[80];
        json_esc(pin, sizeof(pin), g_set.light_pin);
        json_esc(who, sizeof(who), g_set.light_person);
        snprintf(body, sizeof(body), "{\"p_person_id\":\"%s\",\"p_pin\":\"%s\",\"ch\":%d,\"new_state\":\"%s\"}",
                 who, pin, ch, strcmp(now, "ON") ? "ON" : "OFF");
        int st = http_call("/rest/v1/rpc/light_set", body, buf, sizeof(buf));
        s_l_err = st == 200 ? 0 : (st >= 400 ? 2 : 1);
        if (st >= 400 && strstr(buf, "not signed in")) {
            /* PIN changed or the account is locked. Forget the PIN at once: retrying a wrong PIN
             * 5 times locks the account (and emails the owner). The phone signs in again. */
            g_set.light_pin[0] = 0;
            store_save();
            ble_notify_status();
            snprintf(s_l_detail, sizeof(s_l_detail), "mr 的 PIN 不对，请在手机重新设定");
        } else if (st >= 400) snprintf(s_l_detail, sizeof(s_l_detail), "服务器回应 %d：%.60s", st, buf);
        ESP_LOGI(TAG, "light set ch%d -> %d %s", ch, st, st == 200 ? buf : "");
    }
    int st = http_call("/rest/v1/smartlight?id=eq.1&select=state,state2", NULL, buf, sizeof(buf));
    if (st == 200) {
        cJSON *j = cJSON_Parse(buf), *o = j ? cJSON_GetArrayItem(j, 0) : NULL;
        if (o) {
            cJSON *a = cJSON_GetObjectItem(o, "state"), *b = cJSON_GetObjectItem(o, "state2");
            snprintf(s_l_state[0], 4, "%s", cJSON_IsString(a) ? a->valuestring : "");
            snprintf(s_l_state[1], 4, "%s", cJSON_IsString(b) ? b->valuestring : "");
            if (!ch) s_l_err = 0;
        } else if (!ch) {
            s_l_err = 1;
            snprintf(s_l_detail, sizeof(s_l_detail), "服务器的回应不对：%.50s", buf);
        }
        cJSON_Delete(j);
    } else if (!ch) { s_l_err = 1; if (!s_l_detail[0]) snprintf(s_l_detail, sizeof(s_l_detail), "读取失败（%d）", st); }
    ESP_LOGI(TAG, "light read -> %d", st);
    s_l_busy = false;
    light_kick();
}

static void light_worker(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        light_job(s_l_req);
    }
}

static void light_run(int ch)
{
    if (s_l_busy || !net_wifi_count()) { light_paint(); return; }
    if (!s_l_task &&
        xTaskCreateWithCaps(light_worker, "light", 8192, NULL, 4, &s_l_task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS &&
        xTaskCreate(light_worker, "light", 8192, NULL, 4, &s_l_task) != pdPASS) {
        s_l_task = NULL;
        s_l_err = 1;
        snprintf(s_l_detail, sizeof(s_l_detail), "内存不足，开不了连线（剩 %u B）",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        ESP_LOGW(TAG, "light worker: no memory");
        light_paint();
        return;
    }
    s_l_busy = true;
    s_l_req = ch;
    xTaskNotifyGive(s_l_task);
    light_paint();
}

static void light_paint(void)
{
    static const char *names[2] = {"天花板灯", "墙壁射灯"};
    for (int i = 0; i < 2; i++) {
        bool on = !strcmp(s_l_state[i], "ON");
        lv_obj_set_style_bg_color(s_l_btn[i], lv_color_hex(on ? 0xFDE68A : C_CARD), 0);
        lv_obj_t *ic = lv_obj_get_child(s_l_btn[i], 0), *nm = lv_obj_get_child(s_l_btn[i], 1), *st = lv_obj_get_child(s_l_btn[i], 2);
        lv_obj_set_style_image_recolor(ic, lv_color_hex(on ? 0x713F12 : 0x8B5CF6), 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(on ? 0x422006 : C_TEXT), 0);
        lv_obj_set_style_text_color(st, lv_color_hex(on ? 0x713F12 : C_DIM), 0);
        lv_label_set_text(nm, names[i]);
        lv_label_set_text(st, s_l_state[i][0] ? (on ? "开着" : "关着") : "--");
    }
    const char *msg = "点一下开关";
    if (!net_wifi_count()) msg = "手表还没记住任何 WiFi";
    else if (!g_set.light_person[0] || !g_set.light_pin[0]) msg = "先在手机网页设定 mr 的 PIN";
    else if (s_l_busy) msg = net_wifi_connected() ? "连线中…" : "正在打开 WiFi…";
    else if (s_l_err == 2) msg = s_l_detail[0] ? s_l_detail : "钥匙不对，手表没有权限";
    else if (s_l_err == 1) msg = s_l_detail[0] ? s_l_detail : "连不上灯光服务器";
    lv_label_set_text(s_l_info, msg);
}

static void light_btn(lv_event_t *e)
{
    if (!g_set.light_person[0] || !g_set.light_pin[0]) { light_paint(); return; }
    hw_haptic(20);
    light_run((int)(intptr_t)lv_event_get_user_data(e));
}
static void light_close(void) { s_l_info = NULL; net_wifi_use(false); }

void app_light(lv_obj_t *root)
{
    ui_header(root, "办公室灯", 0x8B5CF6);
    lv_obj_t *sub = ui_label(root, &font_cn16, C_DIM, "Makerspace");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 66);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(root);
        lv_obj_set_size(b, 175, 220);
        lv_obj_set_pos(b, 22 + i * 191, 104);
        lv_obj_set_style_radius(b, 30, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_t *ic = ui_icon(b, &img_icon_lt, 0x8B5CF6);
        lv_image_set_scale(ic, 420);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 40);
        lv_obj_t *nm = ui_label(b, &font_cn20, C_TEXT, "");
        lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -40);
        lv_obj_t *st = ui_label(b, &font_cn16, C_DIM, "");
        lv_obj_align(st, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_add_event_cb(b, light_btn, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        s_l_btn[i] = b;
    }
    s_l_info = ui_label(root, &font_cn16, 0xA1A1AA, "");
    lv_obj_set_width(s_l_info, 366);
    lv_label_set_long_mode(s_l_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_l_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_l_info, LV_ALIGN_TOP_MID, 0, 344);
    g_app_close = light_close;
    g_app_refresh = light_paint;
    net_wifi_use(true);                     /* WiFi stays on only while this screen is open */
    light_run(0);
}

/* ======================= Tilt game: roll the fish to the food ======================= */
static lv_obj_t *s_g_ball, *s_g_food, *s_g_score, *s_g_time, *s_g_over;
static lv_timer_t *s_g_tm;
static float s_bx, s_by, s_vx, s_vy, s_fx, s_fy, s_tx0, s_ty0;
static int s_score, s_left_ms;
static bool s_g_run;

static void food_move(void)
{
    s_fx = 40 + esp_random() % 330;
    s_fy = 120 + esp_random() % 300;
    lv_obj_set_pos(s_g_food, (int)s_fx - 10, (int)s_fy - 10);
}

static void game_step(lv_timer_t *t)
{
    if (!s_g_run) return;
    float ax, ay;
    hw_tilt(&ax, &ay);
    /* tilt relative to how the watch was held when the round started */
    s_vx += (ay - s_ty0) * 1.6f;
    s_vy += (ax - s_tx0) * 1.6f;
    s_vx *= 0.96f; s_vy *= 0.96f;
    s_bx += s_vx; s_by += s_vy;
    if (s_bx < 24) { s_bx = 24; s_vx = -s_vx * 0.5f; }
    if (s_bx > SCR_W - 24) { s_bx = SCR_W - 24; s_vx = -s_vx * 0.5f; }
    if (s_by < 110) { s_by = 110; s_vy = -s_vy * 0.5f; }
    if (s_by > SCR_H - 30) { s_by = SCR_H - 30; s_vy = -s_vy * 0.5f; }
    lv_obj_set_pos(s_g_ball, (int)s_bx - 57, (int)s_by - 37);
    lv_image_set_src(s_g_ball, s_vx >= 0 ? &img_fish_gold_r0 : &img_fish_gold_l0);
    if (hypotf(s_bx - s_fx, s_by - s_fy) < 30) {
        s_score++;
        lv_label_set_text_fmt(s_g_score, "%d", s_score);
        sound_play(SND_EAT);
        hw_haptic(25);
        food_move();
    }
    s_left_ms -= 33;
    lv_label_set_text_fmt(s_g_time, "%d 秒", s_left_ms > 0 ? (s_left_ms + 999) / 1000 : 0);
    if (s_left_ms <= 0) {
        s_g_run = false;
        bool best = s_score > g_set.game_best;
        if (best) { g_set.game_best = s_score; store_save(); sound_play(SND_PR); hw_haptic(300); }
        else sound_play(SND_REST_END);
        lv_label_set_text_fmt(lv_obj_get_child(s_g_over, 0), "%s%d 分\n最高 %d 分\n点一下再玩", best ? "新纪录！" : "", s_score, g_set.game_best);
        lv_obj_remove_flag(s_g_over, LV_OBJ_FLAG_HIDDEN);
    }
}

static void game_start(lv_event_t *e)
{
    hw_tilt(&s_tx0, &s_ty0);
    s_bx = SCR_W / 2; s_by = 300; s_vx = s_vy = 0;
    s_score = 0;
    s_left_ms = 30000;
    lv_label_set_text(s_g_score, "0");
    food_move();
    lv_obj_add_flag(s_g_over, LV_OBJ_FLAG_HIDDEN);
    s_g_run = true;
}

static void game_close(void) { s_g_run = false; if (s_g_tm) { lv_timer_delete(s_g_tm); s_g_tm = NULL; } power_keep_awake(false); }

void app_game(lv_obj_t *root)
{
    lv_obj_set_style_bg_image_src(root, &img_tank_bg, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    ui_header(root, "滚滚鱼", 0xF9A8D4);
    s_g_score = ui_label(root, &font_num, C_TEXT, "0");
    lv_obj_align(s_g_score, LV_ALIGN_TOP_LEFT, 30, 70);
    s_g_time = ui_label(root, &font_cn20, 0xA8E8DD, "30 秒");
    lv_obj_align(s_g_time, LV_ALIGN_TOP_RIGHT, -30, 84);
    s_g_food = lv_obj_create(root);
    lv_obj_set_size(s_g_food, 20, 20);
    lv_obj_set_style_radius(s_g_food, 10, 0);
    lv_obj_set_style_bg_color(s_g_food, lv_color_hex(0xE8A06A), 0);
    lv_obj_set_style_border_width(s_g_food, 0, 0);
    lv_obj_remove_flag(s_g_food, LV_OBJ_FLAG_CLICKABLE);
    s_g_ball = lv_image_create(root);
    lv_image_set_src(s_g_ball, &img_fish_gold_r0);
    lv_obj_remove_flag(s_g_ball, LV_OBJ_FLAG_CLICKABLE);
    s_g_over = lv_button_create(root);
    lv_obj_set_size(s_g_over, 300, 190);
    lv_obj_align(s_g_over, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_radius(s_g_over, 30, 0);
    lv_obj_set_style_bg_color(s_g_over, lv_color_hex(0x0B1F24), 0);
    lv_obj_set_style_bg_opa(s_g_over, 230, 0);
    lv_obj_set_style_shadow_width(s_g_over, 0, 0);
    lv_obj_t *l = ui_label(s_g_over, &font_cn20, C_TEXT, "把手表放平\n倾斜手腕让鱼去吃饲料\n点一下开始");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(s_g_over, game_start, LV_EVENT_CLICKED, NULL);
    s_g_run = false;
    s_bx = SCR_W / 2; s_by = 300;
    lv_obj_set_pos(s_g_ball, (int)s_bx - 57, (int)s_by - 37);
    food_move();
    s_g_tm = lv_timer_create(game_step, 33, NULL);
    power_keep_awake(true);
    g_app_close = game_close;
}

/* ======================= Reminders ======================= */
void reminders_check(void)
{
    static uint32_t last_steps;
    static int64_t last_move, last_water;
    int64_t now = esp_timer_get_time();
    if (!last_move) { last_move = now; last_water = now; last_steps = g_set.steps; }
    if (g_set.steps > last_steps + 30) { last_steps = g_set.steps; last_move = now; }
    if (g_set.steps < last_steps) last_steps = g_set.steps;          /* new day */
    if (!net_time_valid() || g_set.dnd) return;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    if (tm.tm_hour < 9 || tm.tm_hour >= 21) { last_move = now; last_water = now; return; }
    if (g_set.remind_sit && now - last_move > 60LL * 60 * 1000000) {
        last_move = now;
        tank_attention("坐太久了，起来走走");
    } else if (g_set.remind_water && now - last_water > 120LL * 60 * 1000000) {
        last_water = now;
        tank_attention("喝口水吧");
    }
}
