/* Barbell training log: pick a lift -> count reps (IMU or tap) -> rest -> summary. */
#include <stdio.h>
#include "app.h"

extern void (*g_app_close)(void);

static const char *LIFTS[5] = {"深蹲", "卧推", "硬拉", "推举", "划船"};
static const float DEF_KG[5] = {60, 40, 80, 30, 40};

static lv_obj_t *s_root, *s_kg, *s_reps, *s_clock, *s_arc, *s_mode, *s_kg_unit;
static lv_timer_t *s_tm;
static int s_lift, s_set, s_total_reps, s_rest_left, s_rest_total;
static float s_weight, s_volume, s_best_kg;
static int s_base_reps;   /* IMU count at start of set */
static int s_manual;      /* taps added manually */

static void build_list(void);
static void build_lift(void);
static void build_rest(void);
static void build_summary(void);

static void stop_timer(void) { if (s_tm) { lv_timer_delete(s_tm); s_tm = NULL; } hw_rep_enable(false); }
static void (*s_next)(void);
static void on_close(void) { stop_timer(); s_next = NULL; power_keep_awake(false); }

static void stage_async(void *arg)
{
    if (!s_next || !lv_obj_is_valid(s_root)) return;
    lv_obj_clean(s_root);
    void (*fn)(void) = s_next;
    s_next = NULL;
    fn();
}

static void stage(void (*fn)(void))
{
    stop_timer();
    s_next = fn;
    lv_async_call(stage_async, NULL);   /* never delete the clicked button inside its own event */
}

/* LVGL's printf may not support floats: format kg as integer tenths */
static const char *kg_str(float kg)
{
    static char b[4][16];
    static int k;
    k = (k + 1) & 3;
    int t = (int)(kg * 10 + 0.5f);
    if (t % 10) snprintf(b[k], sizeof(b[k]), "%d.%d", t / 10, t % 10);
    else snprintf(b[k], sizeof(b[k]), "%d", t / 10);
    return b[k];
}

static int reps_now(void) { int r = hw_rep_count() - s_base_reps + s_manual; return r < 0 ? 0 : r; }

/* ---------- list ---------- */
static void pick_cb(lv_event_t *e)
{
    s_lift = (int)(intptr_t)lv_event_get_user_data(e);
    s_weight = g_set.pr_kg10[s_lift] ? g_set.pr_kg10[s_lift] / 10.0f * 0.8f : DEF_KG[s_lift];
    s_weight = ((int)(s_weight / 2.5f)) * 2.5f;
    if (s_weight < 20) s_weight = 20;
    s_set = 1; s_total_reps = 0; s_volume = 0; s_best_kg = 0;
    stage(build_lift);
}

static void build_list(void)
{
    ui_header(s_root, "今天练什么", C_ORANGE);
    lv_obj_t *list = lv_obj_create(s_root);
    lv_obj_set_size(list, SCR_W, SCR_H - 86);
    lv_obj_set_pos(list, 0, 86);
    lv_obj_set_style_bg_opa(list, 0, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_hor(list, 22, 0);
    lv_obj_set_style_pad_bottom(list, 30, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = ui_pill_btn(list, NULL, C_CARD, C_TEXT, 366, 86);
        lv_obj_set_style_radius(b, 24, 0);
        lv_obj_t *n = ui_label(b, &font_cn20, C_TEXT, LIFTS[i]);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 8, -14);
        lv_obj_t *pr = ui_label(b, &font_cn16, 0xFFB38A, "");
        if (g_set.pr_kg10[i]) lv_label_set_text_fmt(pr, "最高纪录 %s kg", kg_str(g_set.pr_kg10[i] / 10.0f));
        else lv_label_set_text(pr, "还没有纪录");
        lv_obj_align(pr, LV_ALIGN_LEFT_MID, 8, 18);
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

/* ---------- lifting ---------- */
static void refresh_lift(lv_timer_t *t)
{
    lv_label_set_text_fmt(s_reps, "%d", reps_now());
}

static void kg_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    s_weight += d * 2.5f;
    if (s_weight < 0) s_weight = 0;
    lv_label_set_text(s_kg, kg_str(s_weight));
    /* the number re-centres itself (lv_obj_align); move "kg" to follow its new right edge */
    if (s_kg_unit) { lv_obj_update_layout(s_kg); lv_obj_align_to(s_kg_unit, s_kg, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8); }
}

static void tap_rep(lv_event_t *e) { s_manual++; hw_haptic(15); sound_play(SND_REP); refresh_lift(NULL); }
static void minus_rep(lv_event_t *e) { if (reps_now() > 0) s_manual--; refresh_lift(NULL); }

static void done_set(lv_event_t *e)
{
    int r = reps_now();
    if (r > 0) {
        s_total_reps += r;
        s_volume += s_weight * r;
        if (s_weight > s_best_kg) s_best_kg = s_weight;
        store_log_set(LIFTS[s_lift], s_weight, r);
        health_add_set(s_lift, s_weight, r);
    }
    stage(build_rest);
}

static void build_lift(void)
{
    char t[32];
    snprintf(t, sizeof(t), "%s · 第 %d 组", LIFTS[s_lift], s_set);
    ui_header(s_root, t, C_ORANGE);

    lv_obj_t *row = lv_obj_create(s_root);
    lv_obj_set_size(row, 366, 84);
    lv_obj_set_pos(row, 22, 86);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x111111), 0);
    lv_obj_set_style_radius(row, 22, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *m = ui_pill_btn(row, "−", 0x2A1A12, C_ORANGE, 66, 66);
    lv_obj_align(m, LV_ALIGN_LEFT_MID, -6, 0);
    lv_obj_add_event_cb(m, kg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *p = ui_pill_btn(row, "+", 0x2A1A12, C_ORANGE, 66, 66);
    lv_obj_align(p, LV_ALIGN_RIGHT_MID, 6, 0);
    lv_obj_add_event_cb(p, kg_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    s_kg = ui_label(row, &font_num, C_TEXT, "");
    lv_label_set_text(s_kg, kg_str(s_weight));
    lv_obj_align(s_kg, LV_ALIGN_CENTER, -14, 0);
    s_kg_unit = ui_label(row, &font_cn16, C_DIM, "kg");
    lv_obj_align_to(s_kg_unit, s_kg, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8);

    lv_obj_t *big = lv_button_create(s_root);
    lv_obj_set_size(big, 366, 190);
    lv_obj_set_pos(big, 22, 180);
    lv_obj_set_style_radius(big, 30, 0);
    lv_obj_set_style_bg_color(big, lv_color_hex(0x0F2B2A), 0);
    lv_obj_set_style_shadow_width(big, 0, 0);
    lv_obj_add_event_cb(big, tap_rep, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(big, minus_rep, LV_EVENT_LONG_PRESSED, NULL);
    s_reps = ui_label(big, &font_time, C_TEXT, "0");
    lv_obj_align(s_reps, LV_ALIGN_CENTER, 0, -12);
    s_mode = ui_label(big, &font_cn16, 0x8FD8CC, hw_imu_ok() ? "自动计次 · 点 +1 · 长按 -1" : "点 +1 · 长按 -1");
    lv_obj_align(s_mode, LV_ALIGN_BOTTOM_MID, 0, -4);

    lv_obj_t *d = ui_pill_btn(s_root, "完成这组", C_ORANGE, 0x1A0A02, 366, 70);
    lv_obj_set_pos(d, 22, 384);
    lv_obj_add_event_cb(d, done_set, LV_EVENT_CLICKED, NULL);

    s_manual = 0;
    hw_rep_reset();
    s_base_reps = 0;
    hw_rep_enable(true);
    s_tm = lv_timer_create(refresh_lift, 200, NULL);
}

/* ---------- rest ---------- */
static void tick_rest(lv_timer_t *t)
{
    if (s_rest_left > 0) s_rest_left--;
    lv_label_set_text_fmt(s_clock, "%d:%02d", s_rest_left / 60, s_rest_left % 60);
    lv_arc_set_value(s_arc, s_rest_total ? s_rest_left * 1000 / s_rest_total : 0);
    if (s_rest_left == 0) { hw_haptic(500); sound_play(SND_REST_END); lv_timer_pause(t); }
    else if (s_rest_left <= 3) { hw_haptic(40); sound_play(SND_CLICK); }
}

static void more_cb(lv_event_t *e) { s_rest_left += 30; if (s_rest_left > s_rest_total) s_rest_total = s_rest_left; lv_timer_resume(s_tm); tick_rest(s_tm); }
static void next_cb(lv_event_t *e) { s_set++; stage(build_lift); }
static void end_cb(lv_event_t *e) { stage(build_summary); }

static void build_rest(void)
{
    ui_header(s_root, "组间休息", C_TEAL);
    s_arc = lv_arc_create(s_root);
    lv_obj_set_size(s_arc, 230, 230);
    lv_obj_align(s_arc, LV_ALIGN_TOP_MID, 0, 78);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_arc_set_value(s_arc, 1000);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x12302F), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(C_TEAL), LV_PART_INDICATOR);
    s_clock = ui_label(s_root, &font_num, C_TEXT, "1:30");
    lv_obj_set_width(s_clock, 230);                 /* fixed width, centred text: stays put as it counts down */
    lv_obj_set_style_text_align(s_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_clock, s_arc, LV_ALIGN_CENTER, 0, -10);
    lv_obj_t *nx = ui_label(s_root, &font_cn16, 0x8FD8CC, "");
    lv_label_set_text_fmt(nx, "下一组 %s kg", kg_str(s_weight));
    lv_obj_align_to(nx, s_arc, LV_ALIGN_CENTER, 0, 34);
    lv_obj_t *sm = ui_label(s_root, &font_cn16, 0xFFB38A, "");
    lv_label_set_text_fmt(sm, "已完成 %d 组 · 共 %d 次", s_set, s_total_reps);
    lv_obj_align(sm, LV_ALIGN_TOP_MID, 0, 318);

    lv_obj_t *b1 = ui_pill_btn(s_root, "+30 秒", 0x12302F, C_TEAL, 112, 60);
    lv_obj_set_pos(b1, 22, 372);
    lv_obj_add_event_cb(b1, more_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = ui_pill_btn(s_root, "下一组", C_TEAL, 0x00201B, 112, 60);
    lv_obj_set_pos(b2, 149, 372);
    lv_obj_add_event_cb(b2, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b3 = ui_pill_btn(s_root, "结束", 0x2A1A12, 0xFFB38A, 112, 60);
    lv_obj_set_pos(b3, 276, 372);
    lv_obj_add_event_cb(b3, end_cb, LV_EVENT_CLICKED, NULL);

    s_rest_total = s_rest_left = 90;
    s_tm = lv_timer_create(tick_rest, 1000, NULL);
}

/* ---------- summary ---------- */
static void home_cb(lv_event_t *e) { ui_go_home(); }

static void build_summary(void)
{
    ui_header(s_root, "训练完成", C_ORANGE);
    lv_obj_t *v = ui_label(s_root, &font_num, C_TEXT, "");
    lv_label_set_text_fmt(v, "%d", (int)(s_volume + 0.5f));
    lv_obj_align(v, LV_ALIGN_TOP_MID, -16, 92);
    lv_obj_t *u = ui_label(s_root, &font_cn20, C_DIM, "kg");
    lv_obj_align_to(u, v, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -8);
    lv_obj_t *d = ui_label(s_root, &font_cn16, 0xBFEEE6, "");
    lv_label_set_text_fmt(d, "%s · %d 组 · %d 次", LIFTS[s_lift], s_set, s_total_reps);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 160);

    bool pr = s_best_kg * 10 > g_set.pr_kg10[s_lift] && s_total_reps > 0;
    if (s_total_reps > 0) {
        g_set.total_kg += (uint32_t)s_volume;
        g_set.workouts++;
        if (pr) g_set.pr_kg10[s_lift] = (uint16_t)(s_best_kg * 10);
        g_set.hunger -= 10;           /* a good session cheers the fish up */
        if (g_set.hunger < 0) g_set.hunger = 0;
        store_save();
    }
    lv_obj_t *card = lv_obj_create(s_root);
    lv_obj_set_size(card, 366, 100);
    lv_obj_set_pos(card, 22, 200);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, pr ? 1 : 0, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x5A3A26), 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(pr ? 0x1A1410 : 0x111111), 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t1 = ui_label(card, &font_cn20, pr ? 0xFFD2B8 : C_TEXT, "");
    if (pr) lv_label_set_text_fmt(t1, "新纪录 %s %s kg", LIFTS[s_lift], kg_str(s_best_kg));
    else lv_label_set_text(t1, s_total_reps ? "练得不错" : "这次没有记录到动作");
    lv_obj_align(t1, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_t *t2 = ui_label(card, &font_cn16, 0xFFB38A, "");
    lv_label_set_text_fmt(t2, "累计 %lu kg · 第 %u 次训练", (unsigned long)g_set.total_kg, g_set.workouts);
    lv_obj_align(t2, LV_ALIGN_BOTTOM_LEFT, 4, -4);
    if (pr) { hw_haptic(300); sound_play(SND_PR); }

    lv_obj_t *s = ui_label(s_root, &font_cn16, 0x7FB6AD, store_sd_ok() ? "每组都存到 SD 卡 workout.csv" : "没有 SD 卡，只保存总数");
    lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 318);
    lv_obj_t *b = ui_pill_btn(s_root, "回鱼缸", C_ORANGE, 0x1A0A02, 366, 64);
    lv_obj_set_pos(b, 22, 378);
    lv_obj_add_event_cb(b, home_cb, LV_EVENT_CLICKED, NULL);
}

void app_workout(lv_obj_t *root)
{
    s_root = root;
    g_app_close = on_close;
    power_keep_awake(true);           /* stay on during a workout */
    build_list();
}
