/* The watch face: one fish tank behind the time.
 * Double tap = drop food where you tapped, fish swim over and eat it.
 * Rub back and forth = clean the water.
 * Hungry fish sink to the bottom and go pale; dirty water turns murky green. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "esp_random.h"
#include "esp_timer.h"
#include "app.h"

void ui_open_control(void);

#define NFISH   3
#define NPEL    16
#define FRAMES  4
#define TICK_MS 40

typedef struct {
    const lv_image_dsc_t *img[2][FRAMES];    /* [0]=facing right, [1]=facing left */
    lv_obj_t *obj;
    float x, y, tx, ty;
    int w, h, dir, frame;
    float ft;
    bool chase, hungry;
} fish_t;

typedef struct { lv_obj_t *obj; float x, y, floor; bool on; } pel_t;

static lv_obj_t *s_scr, *s_time, *s_date, *s_hint, *s_batt, *s_bt, *s_wifi, *s_music, *s_murk, *s_algae[2], *s_night, *s_handle;
static fish_t s_fish[NFISH];
static pel_t s_pel[NPEL];
static lv_timer_t *s_timer;
static int s_tick;
static int64_t s_last_us;
static bool s_alerted;
static int64_t s_swish_us;

/* touch tracking */
static lv_point_t s_start, s_last;
static int s_rub_dir, s_rub_travel;
static bool s_swiped;

static float frand(float a, float b) { return a + (b - a) * (esp_random() % 10000) / 10000.0f; }

static bool is_night(void)
{
    if (!net_time_valid()) return false;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_hour >= 23 || tm.tm_hour < 7;
}

static void set_hint(void)
{
    const char *t = "双击喂食 · 来回擦清理";
    uint32_t c = 0xA8E8DD;
    bool floor_food = false;
    for (int i = 0; i < NPEL; i++) if (s_pel[i].on && s_pel[i].floor > 0) floor_food = true;
    if (floor_food) { t = "吃剩的饲料会把水弄脏"; c = 0xD8E89A; }
    if (g_set.hunger > 70) { t = "鱼饿到沉底了 · 双击喂食"; c = 0xFFB38A; }
    else if (g_set.dirt > 60) { t = "水变浑了 · 来回擦一擦"; c = 0xD8E89A; }
    if (is_night()) { t = "鱼在睡觉 · 晚安"; c = 0x7FB6AD; }
    lv_label_set_text(s_hint, t);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(c), 0);
}

static void update_overlays(void)
{
    float d = g_set.dirt;
    int murk = d > 20 ? (int)((d - 20) / 80.0f * 200) : 0;
    lv_obj_set_style_bg_opa(s_murk, murk, 0);
    if (murk) lv_obj_remove_flag(s_murk, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_murk, LV_OBJ_FLAG_HIDDEN);
    int al = d > 50 ? (int)((d - 50) / 50.0f * 150) : 0;
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_opa(s_algae[i], al, 0);
        lv_obj_set_style_shadow_opa(s_algae[i], al, 0);
        if (al) lv_obj_remove_flag(s_algae[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_algae[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (is_night()) lv_obj_remove_flag(s_night, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_night, LV_OBJ_FLAG_HIDDEN);
}

static void update_clock(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (net_time_valid()) {
        lv_label_set_text_fmt(s_time, "%02d:%02d", tm.tm_hour, tm.tm_min);
        static const char *wd[] = {"日", "一", "二", "三", "四", "五", "六"};
        if (g_weather.ok) lv_label_set_text_fmt(s_date, "%d月%d日 周%s · %d°", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday], (int)(g_weather.temp + 0.5f));
        else lv_label_set_text_fmt(s_date, "%d月%d日 周%s", tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday]);
    } else {
        lv_label_set_text(s_time, "--:--");
        lv_label_set_text(s_date, "连接手机对时");
    }
    int b = hw_battery_percent();
    if (b >= 0) lv_label_set_text_fmt(s_batt, "%s%d%%", hw_charging() ? "+" : "", b);
    else lv_label_set_text(s_batt, "");
    lv_obj_set_style_image_recolor(s_bt, lv_color_hex(ble_connected() ? C_BLUE : 0x5A6B68), 0);
    if (ble_is_on()) lv_obj_remove_flag(s_bt, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_bt, LV_OBJ_FLAG_HIDDEN);
    if (net_wifi_busy()) lv_obj_remove_flag(s_wifi, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    if (radio_playing()) lv_obj_remove_flag(s_music, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_music, LV_OBJ_FLAG_HIDDEN);
}

static void music_clicked(lv_event_t *e) { radio_mini_open(); }

static void drop_food(int cx)
{
    if (is_night()) return;
    for (int k = 0; k < 7; k++) {
        for (int i = 0; i < NPEL; i++) {
            if (s_pel[i].on) continue;
            s_pel[i].on = true;
            s_pel[i].floor = 0;
            s_pel[i].x = cx - 40 + frand(0, 80);
            s_pel[i].y = 70 + frand(0, 12);
            lv_obj_remove_flag(s_pel[i].obj, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_pel[i].obj, lv_color_hex(0xE8A06A), 0);
            break;
        }
    }
    hw_haptic(30);
    sound_play(SND_FEED);
}

static void clean(float amount)
{
    float before = g_set.dirt;
    g_set.dirt -= amount;
    if (g_set.dirt < 0) g_set.dirt = 0;
    if (before > 0 && g_set.dirt == 0) { hw_haptic(80); sound_play(SND_CLEAN); store_save_fish(); }
    else if (before > 0) {
        int64_t n = esp_timer_get_time();
        if (n - s_swish_us > 180000) { s_swish_us = n; sound_play(SND_SWISH); hw_haptic(8); }
    }
    update_overlays();
}

static void step(lv_timer_t *t)
{
    int64_t now_us = esp_timer_get_time();
    float dt = (now_us - s_last_us) / 1e6f;
    s_last_us = now_us;
    if (lv_screen_active() != s_scr || power_is_off()) return;
    if (g_set.face == 1 || g_set.power_save) {           /* minimal face: no fish to animate */
        if (++s_tick % 33 == 0) update_clock();
        return;
    }
    if (dt > 0.2f) dt = 0.2f;              /* movement is time based: smooth even if frames drop */
    if (dt <= 0) return;
    bool night = is_night();
    s_tick++;

    /* slow world clock: hunger fills in ~12 h, dirt in ~3 days */
    if (!night) g_set.hunger += dt * (100.0f / (12 * 3600));
    g_set.dirt += dt * (100.0f / (72 * 3600));
    int rotting = 0;

    for (int i = 0; i < NPEL; i++) {
        pel_t *p = &s_pel[i];
        if (!p->on) continue;
        if (p->y < 418) p->y += 24 * dt; else { p->floor += dt; rotting++; }
        if (p->floor > 10) lv_obj_set_style_bg_color(p->obj, lv_color_hex(0x7A6A3A), 0);
        if (p->floor > 120) { p->on = false; lv_obj_add_flag(p->obj, LV_OBJ_FLAG_HIDDEN); g_set.dirt += 2; }
        lv_obj_set_pos(p->obj, (int)p->x - 3, (int)p->y - 3);
    }
    g_set.dirt += rotting * dt * 0.05f;
    if (g_set.hunger > 100) g_set.hunger = 100;
    if (g_set.dirt > 100) g_set.dirt = 100;
    bool hungry = g_set.hunger > 70;
    if (hungry && !s_alerted) { s_alerted = true; hw_haptic(200); sound_play(SND_ALERT); }
    if (g_set.hunger < 50) s_alerted = false;

    for (int n = 0; n < NFISH; n++) {
        fish_t *f = &s_fish[n];
        /* Direction is decided from the body centre with hysteresis. (Deciding it from the
         * mouth made the fish flip every frame: flipping moves the mouth to the other side.) */
        float cx = f->x + f->w * 0.5f, cy = f->y + f->h * 0.5f;
        pel_t *target = NULL;
        float best = 1e9;
        for (int i = 0; i < NPEL; i++) {
            if (!s_pel[i].on) continue;
            float d = hypotf(s_pel[i].x - cx, s_pel[i].y - cy);
            if (d < best) { best = d; target = &s_pel[i]; }
        }
        float lowY = night ? 330 : 400;
        float ymin = 150 + f->h * 0.5f, ymax = 440 - f->h * 0.5f - 2;
        float tx, ty, spd;
        if (target) {
            tx = target->x; ty = target->y; spd = 85;
        } else {
            if (hypotf(f->tx - cx, f->ty - cy) < 14) {
                f->tx = frand(40, 370);
                f->ty = (hungry || night) ? frand(lowY, lowY + 20) : frand(180, 370);
            }
            if ((hungry || night) && f->ty < lowY - 5) f->ty = frand(lowY, lowY + 20);
            if (f->ty < ymin) f->ty = ymin;
            if (f->ty > ymax) f->ty = ymax;
            tx = f->tx; ty = f->ty;
            spd = night ? 8 : hungry ? (cy < lowY - 30 ? 30 : 10) : 34;
        }
        float hx = tx - cx;
        if (hx > 10) f->dir = 1; else if (hx < -10) f->dir = -1;
        /* when chasing, steer the mouth onto the pellet; otherwise steer the body centre */
        float px = target ? (f->dir > 0 ? f->x + f->w * 0.9f : f->x + f->w * 0.1f) : cx;
        float dx = tx - px, dy = ty - cy, d = hypotf(dx, dy);
        if (d < 0.01f) d = 0.01f;
        float st = spd * dt;
        if (st > d) st = d;
        f->x += dx / d * st;
        f->y += dy / d * st;
        if (f->x < -10) f->x = -10;
        if (f->x > SCR_W - f->w + 10) f->x = SCR_W - f->w + 10;
        if (f->y < 150) f->y = 150;
        if (f->y > 440 - f->h) f->y = 440 - f->h;
        if (target && d < 14) {
            target->on = false;
            lv_obj_add_flag(target->obj, LV_OBJ_FLAG_HIDDEN);
            g_set.hunger -= 6;
            if (g_set.hunger < 0) g_set.hunger = 0;
            sound_play(SND_EAT);
        }
        /* tail beat follows mood */
        float period = target ? 0.06f : (hungry || night) ? 0.24f : 0.11f;
        f->ft += dt;
        if (f->ft >= period) { f->ft = 0; f->frame = (f->frame + 1) % FRAMES; }
        lv_image_set_src(f->obj, f->img[f->dir > 0 ? 0 : 1][f->frame]);
        lv_obj_set_pos(f->obj, (int)f->x, (int)f->y);
        bool pale = hungry && !target;
        if (pale != f->hungry) {
            f->hungry = pale;
            lv_obj_set_style_image_recolor_opa(f->obj, pale ? 150 : 0, 0);
        }
        f->chase = target != NULL;
    }

    if (s_tick % 30 == 0) { update_clock(); update_overlays(); set_hint(); }
    static int64_t last_save;
    if (now_us - last_save > 5LL * 60 * 1000000) { last_save = now_us; store_save_fish(); }
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) {
        s_start = s_last = p;
        s_rub_dir = 0; s_rub_travel = 0; s_swiped = false;
    } else if (c == LV_EVENT_PRESSING) {
        int dx = p.x - s_last.x;
        s_last = p;
        if (s_swiped) return;
        /* edge swipes */
        if (s_start.y > SCR_H - 80 && s_start.y - p.y > 70 && abs(p.x - s_start.x) < 60) { s_swiped = true; ui_go_apps(); return; }
        if (s_start.y < 70 && p.y - s_start.y > 70 && abs(p.x - s_start.x) < 60) { s_swiped = true; ui_open_control(); return; }
        /* rubbing: count direction reversals after some travel */
        int sgn = dx > 0 ? 1 : dx < 0 ? -1 : 0;
        if (sgn) {
            if (sgn == s_rub_dir) s_rub_travel += abs(dx);
            else {
                if (s_rub_travel > 18 && s_rub_dir != 0) clean(5);
                s_rub_dir = sgn; s_rub_travel = abs(dx);
            }
        }
    } else if (c == LV_EVENT_DOUBLE_CLICKED) {
        int cx = p.x < 60 ? 60 : p.x > 350 ? 350 : p.x;
        drop_food(cx);
    }
}

static void handle_clicked(lv_event_t *e) { ui_go_apps(); }
static void status_clicked(lv_event_t *e) { ui_open_control(); }

static lv_obj_t *blob(lv_obj_t *parent, int x, int y, int r, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_set_pos(o, x - r, y - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *overlay(lv_obj_t *parent, uint32_t col)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, SCR_W, SCR_H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(col), 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

void tank_refresh_face(void)
{
    bool minimal = g_set.face == 1 || g_set.power_save;
    lv_obj_set_style_bg_image_src(s_scr, minimal ? NULL : &img_tank_bg, 0);
    for (int i = 0; i < NPEL; i++) if (minimal) lv_obj_add_flag(s_pel[i].obj, LV_OBJ_FLAG_HIDDEN);
    if (minimal) { lv_obj_add_flag(s_murk, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_algae[0], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_algae[1], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_night, LV_OBJ_FLAG_HIDDEN); }
    for (int i = 0; i < NFISH; i++) {
        if (minimal) lv_obj_add_flag(s_fish[i].obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(s_fish[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_color(s_time, lv_color_hex(minimal ? C_TEAL : 0xF2FFFC), 0);
    if (minimal) lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
}

void tank_on_show(void)
{
    if (!s_scr) return;
    update_clock();
    if (!(g_set.face == 1 || g_set.power_save)) update_overlays();
    set_hint();
    tank_refresh_face();
}

lv_obj_t *tank_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_image_src(s_scr, &img_tank_bg, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int i = 0; i < NPEL; i++) {
        s_pel[i].obj = blob(s_scr, 0, 0, 3, 0xE8A06A);
        lv_obj_add_flag(s_pel[i].obj, LV_OBJ_FLAG_HIDDEN);
    }

    static const lv_image_dsc_t *imgs[NFISH][2][FRAMES] = {
        {{&img_fish_gold_r0, &img_fish_gold_r1, &img_fish_gold_r2, &img_fish_gold_r3},
         {&img_fish_gold_l0, &img_fish_gold_l1, &img_fish_gold_l2, &img_fish_gold_l3}},
        {{&img_fish_koi_r0, &img_fish_koi_r1, &img_fish_koi_r2, &img_fish_koi_r3},
         {&img_fish_koi_l0, &img_fish_koi_l1, &img_fish_koi_l2, &img_fish_koi_l3}},
        {{&img_fish_black_r0, &img_fish_black_r1, &img_fish_black_r2, &img_fish_black_r3},
         {&img_fish_black_l0, &img_fish_black_l1, &img_fish_black_l2, &img_fish_black_l3}},
    };
    for (int n = 0; n < NFISH; n++) {
        fish_t *f = &s_fish[n];
        memcpy(f->img, imgs[n], sizeof(f->img));
        f->w = imgs[n][0][0]->header.w;
        f->h = imgs[n][0][0]->header.h;
        f->x = 60 + n * 90; f->y = 230 + n * 50;
        f->tx = 60 + n * 130; f->ty = 200 + n * 70;
        f->dir = (n % 2) ? -1 : 1;
        f->obj = lv_image_create(s_scr);
        lv_image_set_src(f->obj, f->img[0][0]);
        lv_obj_set_style_image_recolor(f->obj, lv_color_hex(0x55605E), 0);
        lv_obj_remove_flag(f->obj, LV_OBJ_FLAG_CLICKABLE);
    }

    s_murk = overlay(s_scr, 0x4E5A2A);
    s_algae[0] = blob(s_scr, 10, 120, 30, 0x50782A);
    s_algae[1] = blob(s_scr, 400, 360, 26, 0x50782A);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_shadow_color(s_algae[i], lv_color_hex(0x5A8A2A), 0);
        lv_obj_set_style_shadow_width(s_algae[i], 120, 0);
        lv_obj_set_style_shadow_spread(s_algae[i], 30, 0);
    }
    s_night = overlay(s_scr, 0x000814);
    lv_obj_set_style_bg_opa(s_night, 120, 0);

    s_time = ui_label(s_scr, &font_time, 0xF2FFFC, "--:--");
    lv_obj_align(s_time, LV_ALIGN_TOP_MID, 0, 86);
    s_date = ui_label(s_scr, &font_cn20, 0xD3F5EF, "");
    lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, 202);

    lv_obj_t *st = lv_obj_create(s_scr);
    lv_obj_set_size(st, 220, 40);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_set_style_bg_opa(st, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(st, 0, 0);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st, 8, 0);
    lv_obj_remove_flag(st, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(st, status_clicked, LV_EVENT_CLICKED, NULL);
    s_music = ui_label(st, &lv_font_montserrat_14, 0xF472B6, LV_SYMBOL_AUDIO);   /* radio playing */
    s_wifi = ui_label(st, &lv_font_montserrat_14, 0x5EEAD4, LV_SYMBOL_WIFI);     /* network in use */
    lv_obj_add_flag(s_music, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_music, 18);
    lv_obj_add_event_cb(s_music, music_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_wifi, LV_OBJ_FLAG_HIDDEN);
    s_bt = ui_icon(st, &img_icon_bt, 0x5A6B68);
    lv_image_set_scale(s_bt, 110);
    s_batt = ui_label(st, &font_cn16, 0xA8E8DD, "");

    s_hint = ui_label(s_scr, &font_cn16, 0xA8E8DD, "");
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -64);

    s_handle = lv_obj_create(s_scr);
    lv_obj_set_size(s_handle, 120, 44);
    lv_obj_align(s_handle, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_bg_opa(s_handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_handle, 0, 0);
    lv_obj_remove_flag(s_handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_handle, handle_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bar = lv_obj_create(s_handle);
    lv_obj_set_size(bar, 64, 5);
    lv_obj_center(bar);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xE6FFFA), 0);
    lv_obj_set_style_bg_opa(bar, 140, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr, touch_cb, LV_EVENT_DOUBLE_CLICKED, NULL);

    /* catch up on time that passed while the watch was off */
    if (net_time_valid() && g_set.saved_at > 0) {
        int64_t gone = (int64_t)time(NULL) - g_set.saved_at;
        if (gone > 0 && gone < 30LL * 24 * 3600) {
            g_set.hunger += gone * (100.0f / (12 * 3600));
            g_set.dirt += gone * (100.0f / (72 * 3600));
            if (g_set.hunger > 100) g_set.hunger = 100;
            if (g_set.dirt > 100) g_set.dirt = 100;
        }
    }
    s_last_us = esp_timer_get_time();
    s_timer = lv_timer_create(step, 30, NULL);
    return s_scr;
}

/* Reminders: the fish swim up to the glass in front of the time to get your attention. */
void tank_attention(const char *msg)
{
    power_wake();
    ui_go_home();
    for (int n = 0; n < NFISH; n++) {
        s_fish[n].tx = 120 + n * 85;
        s_fish[n].ty = 250 + (n % 2) * 30;
    }
    hw_haptic(250);
    sound_play(SND_ALERT);
    ui_toast(msg);
}
