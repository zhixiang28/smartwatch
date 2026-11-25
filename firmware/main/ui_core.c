#include <string.h>
#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "app.h"

static lv_obj_t *s_tank, *s_apps, *s_ctrl, *s_app, *s_toast;
void (*g_app_refresh)(void);          /* set by an open app that shows live status */
void (*g_app_close)(void);            /* set by an open app that must clean up */

bool ui_lock(int ms) { return bsp_display_lock(ms); }
void ui_unlock(void) { bsp_display_unlock(); }

/* ---------------- small widget helpers ---------------- */
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_image_dsc_t *img, uint32_t color)
{
    lv_obj_t *i = lv_image_create(parent);
    lv_image_set_src(i, img);
    lv_obj_set_style_image_recolor(i, lv_color_hex(color), 0);
    lv_obj_set_style_image_recolor_opa(i, LV_OPA_COVER, 0);
    lv_obj_remove_flag(i, LV_OBJ_FLAG_CLICKABLE);
    return i;
}

static void press_anim_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) { if (g_set.tap_buzz) hw_haptic(12); sound_play(SND_CLICK); }
}

lv_obj_t *ui_pill_btn(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg, int w, int h)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_transform_pivot_x(b, w / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, h / 2, 0);
    lv_obj_add_event_cb(b, press_anim_cb, LV_EVENT_CLICKED, NULL);
    if (txt) {
        lv_obj_t *l = ui_label(b, &font_cn20, fg, txt);
        lv_obj_center(l);
    }
    return b;
}

static void back_clicked(lv_event_t *e) { ui_back(); }

lv_obj_t *ui_header(lv_obj_t *root, const char *title, uint32_t accent)
{
    lv_obj_t *b = lv_button_create(root);
    lv_obj_set_size(b, 60, 60);
    lv_obj_set_pos(b, 18, 14);
    lv_obj_set_style_radius(b, 30, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *ic = ui_icon(b, &img_icon_back, accent);
    lv_image_set_scale(ic, 200);
    lv_obj_center(ic);
    lv_obj_add_event_cb(b, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = ui_label(root, &font_cn20, C_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 28);      /* centred like the subtitles under it; the back button is only the left 78 px */
    return t;
}

static void toast_del(lv_timer_t *t) { if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; } }

void ui_toast(const char *txt)
{
    if (s_toast) lv_obj_delete(s_toast);
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_radius(s_toast, 22, 0);
    lv_obj_set_style_border_width(s_toast, 0, 0);
    lv_obj_set_style_pad_hor(s_toast, 18, 0);
    lv_obj_set_style_pad_ver(s_toast, 10, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ui_label(s_toast, &font_cn16, C_TEXT, txt);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_timer_t *t = lv_timer_create(toast_del, 1800, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* ---------------- navigation ---------------- */
static void del_timer_cb(lv_timer_t *t)
{
    lv_obj_t *o = lv_timer_get_user_data(t);
    if (o && o != lv_screen_active()) lv_obj_delete(o);
}

static void retire(lv_obj_t **pscr)
{
    if (!*pscr) return;
    if (*pscr == s_app) {
        if (g_app_close) g_app_close();
        g_app_close = NULL;
        g_app_refresh = NULL;
    }
    lv_timer_t *t = lv_timer_create(del_timer_cb, 500, *pscr);
    lv_timer_set_repeat_count(t, 1);
    *pscr = NULL;
}

void ui_go_home(void)
{
    if (lv_screen_active() == s_tank) return;
    lv_screen_load_anim(s_tank, LV_SCREEN_LOAD_ANIM_FADE_IN, 160, 0, false);
    retire(&s_app);
    retire(&s_ctrl);
    tank_on_show();
}

void ui_go_apps(void)
{
    if (lv_screen_active() == s_apps) return;
    lv_screen_load_anim(s_apps, LV_SCREEN_LOAD_ANIM_FADE_IN, 160, 0, false);
    retire(&s_app);
    retire(&s_ctrl);
}

void ui_back(void)
{
    lv_obj_t *a = lv_screen_active();
    if (a == s_app) ui_go_apps();
    else ui_go_home();
}


/* ---- edge swipe back (like a phone): drag from the left edge to the right ---- */
static lv_obj_t *s_edge_ind;
static lv_point_t s_edge_start;
static bool s_edge_fired;

static void edge_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_t *ind = lv_event_get_user_data(e);
    if (c == LV_EVENT_PRESSED) {
        s_edge_start = p; s_edge_fired = false;
    } else if (c == LV_EVENT_PRESSING) {
        int dx = p.x - s_edge_start.x;
        if (dx < 0) dx = 0;
        int x = dx > 90 ? 90 : dx;
        lv_obj_set_pos(ind, x - 56, p.y - 28);
        lv_obj_set_style_bg_opa(ind, x * 255 / 90, 0);
        lv_obj_set_style_bg_color(ind, lv_color_hex(dx >= 80 ? C_TEAL : 0x3A3A3C), 0);
        if (dx >= 80 && !s_edge_fired) { s_edge_fired = true; if (g_set.tap_buzz) hw_haptic(15); }
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        lv_obj_set_pos(ind, -60, 0);
        if (s_edge_fired) { sound_play(SND_CLICK); lv_async_call((lv_async_cb_t)ui_back, NULL); }
    }
}

void ui_add_edge_back(lv_obj_t *scr)
{
    lv_obj_t *ind = lv_obj_create(scr);
    lv_obj_set_size(ind, 56, 56);
    lv_obj_set_pos(ind, -60, 0);
    lv_obj_set_style_radius(ind, 28, 0);
    lv_obj_set_style_border_width(ind, 0, 0);
    lv_obj_remove_flag(ind, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ic = ui_icon(ind, &img_icon_back, 0xFFFFFF);
    lv_obj_center(ic);
    lv_obj_t *edge = lv_obj_create(scr);
    lv_obj_set_size(edge, 30, SCR_H);
    lv_obj_set_pos(edge, 0, 0);
    lv_obj_set_style_bg_opa(edge, 0, 0);
    lv_obj_set_style_border_width(edge, 0, 0);
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESSED, ind);
    lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESSING, ind);
    lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_RELEASED, ind);
    lv_obj_add_event_cb(edge, edge_cb, LV_EVENT_PRESS_LOST, ind);
    s_edge_ind = ind;
}

static void gesture_cb(lv_event_t *e)
{
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
    if (d == LV_DIR_RIGHT) ui_back();
}

void ui_open_app(app_build_fn build, lv_color_t tint)
{
    lv_obj_t *old = s_app;
    s_app = NULL;
    if (old) { if (g_app_close) g_app_close(); g_app_close = NULL; g_app_refresh = NULL; }
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, SCR_W, SCR_H);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_transform_pivot_x(root, SCR_W / 2, 0);
    lv_obj_set_style_transform_pivot_y(root, SCR_H / 2, 0);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    build(root);
    s_app = scr;
    lv_screen_load_anim(scr, g_set.power_save ? LV_SCREEN_LOAD_ANIM_NONE : LV_SCREEN_LOAD_ANIM_FADE_IN, 160, 0, false);
    if (old) { lv_timer_t *t = lv_timer_create(del_timer_cb, 500, old); lv_timer_set_repeat_count(t, 1); }
}

static void open_ctrl(void)
{
    if (!s_ctrl) s_ctrl = control_create();
    lv_screen_load_anim(s_ctrl, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, 260, 0, false);
}

void ui_open_control(void) { open_ctrl(); }

void ui_close_control(void)
{
    lv_screen_load_anim(s_tank, LV_SCREEN_LOAD_ANIM_MOVE_TOP, 240, 0, false);
    retire(&s_ctrl);
    tank_on_show();
}

void ui_button_event(int button)
{
    lv_obj_t *a = lv_screen_active();
    if (power_is_off()) { power_wake(); return; }       /* a press on a dark screen only wakes it */
    lv_display_trigger_activity(NULL);
    hw_haptic(15);
    sound_play(SND_CLICK);
    if (button == 0) {                 /* PWR: home <-> apps */
        if (a == s_tank) ui_go_apps();
        else ui_go_home();
    } else {                           /* BOOT: back */
        if (a == s_ctrl) ui_close_control();
        else ui_back();
    }
}

void ui_status_changed(void)
{
    tank_on_show();
    if (g_app_refresh) g_app_refresh();
}

void ui_init(void)
{
    lv_display_t *d = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(d, lv_color_hex(C_TEAL), lv_color_hex(C_ORANGE), true, &font_cn16);
    lv_display_set_theme(d, th);
    s_tank = tank_create();
    power_init();
    s_apps = apps_create();
    lv_screen_load(s_tank);
    tank_on_show();
}
