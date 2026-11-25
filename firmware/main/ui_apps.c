/* Honeycomb app launcher: drag to pan, icons shrink toward the edges,
 * the nearest icon snaps to the centre and its name shows at the bottom. */
#include <math.h>
#include <stdlib.h>
#include "app.h"

typedef struct {
    const char *name;
    const char *cat;
    uint32_t color;
    const lv_image_dsc_t *icon;
    app_build_fn build;          /* NULL = "coming in the next version" */
    const char *soon;
} app_def_t;

#define CAT_AI   "AI 助手", 0x14B8A6
#define CAT_FIT  "健身", 0xF97316
#define CAT_MED  "媒体", 0xEC4899
#define CAT_LIFE "生活", 0xEAB308
#define CAT_HOME "家居", 0x8B5CF6
#define CAT_SYS  "系统", 0x64748B

static const app_def_t APPS[] = {
    {"小智", CAT_AI, &img_icon_ai, NULL, "语音 AI 助手。下一版接上小智服务器，用手机设定网页填激活码。"},
    {"翻译", CAT_AI, &img_icon_tr, NULL, "按住说话，中英互译并朗读。需要联网，下一版加入。"},
    {"笔记", CAT_AI, &img_icon_nt, app_notes, NULL},
    {"训练", CAT_FIT, &img_icon_wk, app_workout, NULL},
    {"计步", CAT_FIT, &img_icon_st, app_steps, NULL},
    {"计时", CAT_FIT, &img_icon_tm, app_stopwatch, NULL},
    {"专注", CAT_FIT, &img_icon_fo, app_focus, NULL},
    {"音乐", CAT_MED, &img_icon_mu, NULL, "网络电台，用手表喇叭播放。下一版加入。"},
    {"录音", CAT_MED, &img_icon_rc, app_recorder, NULL},
    {"遥控拍照", CAT_MED, &img_icon_cm, NULL, "按一下让手机拍照。下一版加入。"},
    {"闹钟", CAT_LIFE, &img_icon_al, app_alarm, NULL},
    {"天气", CAT_LIFE, &img_icon_we, app_weather, NULL},
    {"手电筒", CAT_LIFE, &img_icon_fl, app_flashlight, NULL},
    {"找手机", CAT_LIFE, &img_icon_fp, app_findphone, NULL},
    {"办公室灯", CAT_HOME, &img_icon_lt, app_light, NULL},
    {"表盘", CAT_SYS, &img_icon_fc, app_faces, NULL},
    {"手机设定", CAT_SYS, &img_icon_ph, app_phone, NULL},
    {"设置", CAT_SYS, &img_icon_se, app_settings, NULL},
    {"通知", CAT_SYS, &img_icon_no, NULL, "显示手机通知，下一版加入。"},
    {"滚滚鱼", CAT_MED, &img_icon_fish, app_game, NULL},
};
#define NAPPS (sizeof(APPS) / sizeof(APPS[0]))

static lv_obj_t *s_scr, *s_bub[NAPPS], *s_name, *s_cat, *s_dot;
static float s_px[NAPPS], s_py[NAPPS];
static int s_sz[NAPPS];
static lv_point_t s_press;
static float s_press_ox, s_press_oy;
static float s_ox, s_oy, s_from_x, s_from_y, s_to_x, s_to_y;
static bool s_moved;
static int s_best;

#define CX 205
#define CY 236
#define BUB 78

static void layout(void)
{
    float bd = 1e9;
    for (int i = 0; i < (int)NAPPS; i++) {
        float x = CX + s_px[i] + s_ox, y = CY + s_py[i] + s_oy;
        float d = hypotf(x - CX, y - CY);
        if (d < bd) { bd = d; s_best = i; }
        float s = 1.0f - (d - 95) / 175.0f;
        if (s > 1) s = 1;
        if (s < 0.28f) s = 0.28f;
        /* resize instead of transform: transforms force a slow off-screen layer per icon */
        int sz = (int)(BUB * s);
        if (sz != s_sz[i]) {
            s_sz[i] = sz;
            lv_obj_set_size(s_bub[i], sz, sz);
            lv_obj_t *ic = lv_obj_get_child(s_bub[i], 0);
            if (sz < 34) lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
            else { lv_obj_remove_flag(ic, LV_OBJ_FLAG_HIDDEN); lv_image_set_scale(ic, (uint32_t)(256 * s)); }
        }
        lv_obj_set_pos(s_bub[i], (int)x - sz / 2, (int)y - sz / 2);
    }
    lv_label_set_text(s_name, APPS[s_best].name);
    lv_label_set_text(s_cat, APPS[s_best].cat);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(APPS[s_best].color), 0);
}

static void snap_exec(void *var, int32_t v)
{
    float t = v / 1024.0f;
    s_ox = s_from_x + (s_to_x - s_from_x) * t;
    s_oy = s_from_y + (s_to_y - s_from_y) * t;
    layout();
}

static void snap(void)
{
    sound_play(SND_CLICK);
    s_from_x = s_ox; s_from_y = s_oy;
    s_to_x = -s_px[s_best]; s_to_y = -s_py[s_best];
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_scr);
    lv_anim_set_values(&a, 0, 1024);
    lv_anim_set_duration(&a, 360);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_set_exec_cb(&a, snap_exec);
    lv_anim_start(&a);
}

static void open_app(int i)
{
    const app_def_t *a = &APPS[i];
    hw_haptic(15);
    sound_play(SND_CLICK);
    if (a->build) ui_open_app(a->build, lv_color_hex(a->color));
    else { app_soon_set(a->name, a->soon); ui_open_app(app_soon, lv_color_hex(a->color)); }
}

static void drag_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) {
        s_moved = false;
        lv_anim_delete(s_scr, snap_exec);
        s_press = p; s_press_ox = s_ox; s_press_oy = s_oy;
    } else if (c == LV_EVENT_PRESSING) {
        /* absolute tracking: works even if frames are dropped */
        int dx = p.x - s_press.x, dy = p.y - s_press.y;
        if (abs(dx) + abs(dy) > 8) s_moved = true;
        if (!s_moved) return;
        s_ox = s_press_ox + dx; s_oy = s_press_oy + dy;
        if (s_ox > 220) s_ox = 220;
        if (s_ox < -220) s_ox = -220;
        if (s_oy > 200) s_oy = 200;
        if (s_oy < -200) s_oy = -200;
        layout();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (s_moved) snap();
    }
}

static void bub_click(lv_event_t *e)
{
    if (s_moved) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    open_app(i);
}

lv_obj_t *apps_create(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* spiral of hex cells so each category forms a cluster */
    static const int D[6][2] = {{1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}};
    int ax[64][2] = {{0, 0}}, n = 1;
    for (int k = 1; n < (int)NAPPS; k++) {
        int q = D[4][0] * k, r = D[4][1] * k;
        for (int i = 0; i < 6; i++) for (int j = 0; j < k; j++) { if (n < 64) { ax[n][0] = q; ax[n][1] = r; n++; } q += D[i][0]; r += D[i][1]; }
    }
    for (int i = 0; i < (int)NAPPS; i++) {
        s_px[i] = 92.0f * (ax[i][0] + ax[i][1] / 2.0f);
        s_py[i] = 80.0f * ax[i][1];
        lv_obj_t *b = lv_obj_create(s_scr);
        lv_obj_set_size(b, BUB, BUB);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(APPS[i].color), 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_mix(lv_color_hex(APPS[i].color), lv_color_white(), 200), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_stop(b, 0, 0);
        lv_obj_set_style_bg_grad_stop(b, 255, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_t *ic = ui_icon(b, APPS[i].icon, 0xFFFFFF);
        lv_obj_center(ic);
        lv_obj_add_event_cb(b, bub_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        s_bub[i] = b;
    }

    /* bottom fade + centre name */
    lv_obj_t *fade = lv_obj_create(s_scr);
    lv_obj_set_size(fade, SCR_W, 96);
    lv_obj_align(fade, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_width(fade, 0, 0);
    lv_obj_set_style_radius(fade, 0, 0);
    lv_obj_set_style_bg_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(fade, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(fade, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_opa(fade, 0, 0);
    lv_obj_set_style_bg_grad_opa(fade, 255, 0);
    lv_obj_set_style_bg_main_stop(fade, 0, 0);
    lv_obj_set_style_bg_grad_stop(fade, 150, 0);
    lv_obj_remove_flag(fade, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_name = ui_label(s_scr, &font_cn26, C_TEXT, "");
    lv_obj_align(s_name, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_t *row = lv_obj_create(s_scr);
    lv_obj_set_size(row, 200, 26);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_opa(row, 0, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_dot = lv_obj_create(row);
    lv_obj_set_size(s_dot, 10, 10);
    lv_obj_set_style_radius(s_dot, 5, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    s_cat = ui_label(row, &font_cn16, 0xA1A1AA, "");

    lv_obj_add_event_cb(s_scr, drag_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, drag_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_scr, drag_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scr, drag_cb, LV_EVENT_PRESS_LOST, NULL);
    layout();
    return s_scr;
}
