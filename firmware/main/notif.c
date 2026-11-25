/* Phone notifications forwarded by the FishWatch Android app over BLE (characteristic 0009).
 * Kept in RAM (last 10), shown as a popup when they arrive, listed in the 通知 app. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "app.h"

extern void (*g_app_refresh)(void);
extern void (*g_app_close)(void);

#define NMAX 10
typedef struct { char app[32]; char title[96]; char text[300]; time_t ts; } notif_t;
static notif_t *s_n;                    /* ring in PSRAM, s_head = next slot */
static int s_head, s_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *s_pop;
static lv_timer_t *s_pop_tm;

static void pop_close(void)
{
    if (s_pop_tm) { lv_timer_delete(s_pop_tm); s_pop_tm = NULL; }
    if (s_pop) { lv_obj_delete(s_pop); s_pop = NULL; }
}
static void pop_timeout(lv_timer_t *t) { s_pop_tm = NULL; if (s_pop) { lv_obj_delete(s_pop); s_pop = NULL; } }
static void pop_tap(lv_event_t *e) { pop_close(); }

static void show_async(void *arg)
{
    notif_t n;
    taskENTER_CRITICAL(&s_mux);
    n = s_n[(s_head + NMAX - 1) % NMAX];
    taskEXIT_CRITICAL(&s_mux);
    power_wake();
    pop_close();
    s_pop = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_pop, 380, LV_SIZE_CONTENT);
    lv_obj_align(s_pop, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_bg_color(s_pop, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(s_pop, 26, 0);
    lv_obj_set_style_border_width(s_pop, 0, 0);
    lv_obj_set_style_pad_all(s_pop, 16, 0);
    lv_obj_set_flex_flow(s_pop, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_pop, 4, 0);
    lv_obj_remove_flag(s_pop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_pop, pop_tap, LV_EVENT_CLICKED, NULL);
    lv_obj_t *a = ui_label(s_pop, &font_cn16, 0x5EEAD4, n.app);
    (void)a;
    if (n.title[0]) {
        lv_obj_t *t = ui_label(s_pop, &font_cn20, C_TEXT, n.title);
        lv_obj_set_width(t, 348);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    }
    if (n.text[0]) {
        lv_obj_t *x = ui_label(s_pop, &font_cn16, 0xD1D5DB, n.text);
        lv_obj_set_width(x, 348);
        lv_obj_set_style_max_height(x, 90, 0);
        lv_label_set_long_mode(x, LV_LABEL_LONG_DOT);
    }
    s_pop_tm = lv_timer_create(pop_timeout, 6000, NULL);
    lv_timer_set_repeat_count(s_pop_tm, 1);
    hw_haptic(60);
    if (g_app_refresh) g_app_refresh();
}

void notif_push(const char *app, const char *title, const char *text)
{
    if (!s_n) {
        notif_t *p = heap_caps_calloc(NMAX, sizeof(notif_t), MALLOC_CAP_SPIRAM);
        if (!p) return;
        s_n = p;
    }
    taskENTER_CRITICAL(&s_mux);
    notif_t *n = &s_n[s_head];
    snprintf(n->app, sizeof(n->app), "%s", app && app[0] ? app : "通知");
    snprintf(n->title, sizeof(n->title), "%s", title ? title : "");
    snprintf(n->text, sizeof(n->text), "%s", text ? text : "");
    n->ts = time(NULL);
    s_head = (s_head + 1) % NMAX;
    if (s_count < NMAX) s_count++;
    taskEXIT_CRITICAL(&s_mux);
    if (g_set.dnd) return;                              /* do not disturb: keep it, don't pop up */
    if (ui_lock(300)) { lv_async_call(show_async, NULL); ui_unlock(); }
}

/* ---------------- 通知 app ---------------- */
static lv_obj_t *s_list, *s_tog;

static void list_paint(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    int cnt;
    taskENTER_CRITICAL(&s_mux);
    cnt = s_count;
    taskEXIT_CRITICAL(&s_mux);
    if (!cnt) {
        lv_obj_t *e = ui_label(s_list, &font_cn16, 0x8E8E93, g_set.notif_on ? "还没有通知\n手机上要装 FishWatch 通知 App" : "收通知是关的，按上面打开\n手机上要装 FishWatch 通知 App");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(e, 366);
    }
    for (int i = 0; i < cnt; i++) {
        notif_t n;
        taskENTER_CRITICAL(&s_mux);
        n = s_n[(s_head + NMAX - 1 - i) % NMAX];
        taskEXIT_CRITICAL(&s_mux);
        lv_obj_t *c = lv_obj_create(s_list);
        lv_obj_set_size(c, 366, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(c, lv_color_hex(C_CARD), 0);
        lv_obj_set_style_radius(c, 20, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 2, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        char hd[64];
        struct tm tm;
        localtime_r(&n.ts, &tm);
        snprintf(hd, sizeof(hd), "%.24s · %02d:%02d", n.app, tm.tm_hour, tm.tm_min);
        ui_label(c, &font_cn16, 0x5EEAD4, hd);
        if (n.title[0]) { lv_obj_t *t = ui_label(c, &font_cn16, C_TEXT, n.title); lv_obj_set_width(t, 342); lv_label_set_long_mode(t, LV_LABEL_LONG_DOT); }
        if (n.text[0]) { lv_obj_t *x = ui_label(c, &font_cn16, 0xA1A1AA, n.text); lv_obj_set_width(x, 342); lv_obj_set_style_max_height(x, 64, 0); lv_label_set_long_mode(x, LV_LABEL_LONG_DOT); }
    }
}

static void tog_paint(void)
{
    lv_label_set_text(lv_obj_get_child(s_tog, 0), g_set.notif_on ? "收通知：开" : "收通知：关");
    lv_obj_set_style_bg_color(s_tog, lv_color_hex(g_set.notif_on ? 0x115E59 : C_CARD), 0);
}
static void tog_cb(lv_event_t *e)
{
    g_set.notif_on = !g_set.notif_on;
    store_save();
    if (g_set.notif_on && !ble_is_on()) { ble_new_code(); ble_start(); }
    tog_paint();
    list_paint();
}
static void clear_cb(lv_event_t *e)
{
    taskENTER_CRITICAL(&s_mux);
    s_count = 0;
    taskEXIT_CRITICAL(&s_mux);
    list_paint();
}
static void n_refresh(void) { list_paint(); }
static void n_close(void) { s_list = NULL; s_tog = NULL; }

void app_notif(lv_obj_t *root)
{
    ui_header(root, "通知", 0x64748B);
    s_tog = ui_pill_btn(root, "", C_CARD, C_TEAL, 220, 52);
    lv_obj_set_pos(s_tog, 22, 72);
    lv_obj_add_event_cb(s_tog, tog_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_pill_btn(root, "清除", C_CARD, 0xF87171, 136, 52);
    lv_obj_set_pos(cl, 252, 72);
    lv_obj_add_event_cb(cl, clear_cb, LV_EVENT_CLICKED, NULL);
    s_list = lv_obj_create(root);
    lv_obj_set_size(s_list, 390, 360);
    lv_obj_set_pos(s_list, 10, 134);
    lv_obj_set_style_bg_opa(s_list, 0, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 6, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    tog_paint();
    list_paint();
    g_app_refresh = n_refresh;
    g_app_close = n_close;
}
