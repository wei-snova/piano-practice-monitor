#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "net.h"
#include "store.h"

/* Values are chosen to stay neutral after the panel's RGB565 quantisation. */
#define COL_BG      lv_color_hex(0xF5F9F5)
#define COL_CARD    lv_color_hex(0xFFFFFF)
#define COL_PANEL   lv_color_hex(0xE9F1EA)
#define COL_LINE    lv_color_hex(0xDDE6DE)
#define COL_TEXT    lv_color_hex(0x1F2F3A)
#define COL_DIM     lv_color_hex(0x5F6F75)
#define COL_CELLO   lv_color_hex(0xA6D6AE)
#define COL_PIANO   lv_color_hex(0x96D5CE)
#define COL_CELLO_STRONG lv_color_hex(0x2F8A4A)
#define COL_PIANO_STRONG lv_color_hex(0x15857C)
#define COL_BAR_TRACK lv_color_hex(0xE4EBE5)
#define COL_CELLO_TODAY_TRACK lv_color_hex(0xD9EEDC)
#define COL_PIANO_TODAY_TRACK lv_color_hex(0xD2EDE9)
#define COL_ICON_BG lv_color_hex(0x263B4B)
#define COL_ICON_FG lv_color_hex(0xF7F5E9)
#define COL_GREEN   lv_color_hex(0x26945A)
#define COL_AMBER   lv_color_hex(0xB0741F)

/* The built-in Montserrat fonts have no U+00B7, so separators use this glyph. */
#define DOT " " LV_SYMBOL_BULLET " "

#define PAUSE_SHOW_MS   10000
#define IDENTIFY_NOTES  8
#define DIM_AFTER_MS    120000
#define DISPLAY_OFF_AFTER_MS 300000
#define SWIPE_MIN_DISTANCE 48

typedef enum {
    PAGE_PIANO = 0,
    PAGE_CELLO,
    PAGE_DIAGNOSTICS,
    PAGE_GOALS,
    PAGE_COUNT,
} page_t;

typedef struct {
    lv_obj_t *status_dot;
    lv_obj_t *status;
    lv_obj_t *confidence;
    lv_obj_t *total;
    lv_obj_t *goal_bar;
    lv_obj_t *goal_text;
    lv_obj_t *day_bar[7];
    lv_obj_t *day_label[7];
} practice_widgets_t;

static const char *const s_instr_name[INSTR_COUNT] = { "Cello", "Piano" };

static lv_obj_t *s_page[PAGE_COUNT];
static practice_widgets_t s_practice[INSTR_COUNT];
static lv_obj_t *s_diag_classifier, *s_diag_time, *s_diag_mic;
static lv_obj_t *s_diag_mic_bar, *s_diag_sens_slider, *s_diag_activity, *s_diag_model, *s_diag_wifi;
static lv_obj_t *s_diag_hint;
static lv_obj_t *s_wifi_dialog, *s_wifi_ssid, *s_wifi_password, *s_wifi_keyboard, *s_wifi_status;
static bool s_wifi_connect_pending;
static lv_obj_t *s_goal_value[INSTR_COUNT];
static uint16_t s_goal_min[INSTR_COUNT] = {
    CONFIG_PL_DAILY_GOAL_MIN, CONFIG_PL_DAILY_GOAL_MIN,
};
static uint32_t s_loaded_version = UINT32_MAX;
static uint32_t s_loaded_date = UINT32_MAX;
static day_rec_t s_today;
static day_rec_t s_week[7];
static int s_today_index = -1;
static page_t s_current_page = PAGE_PIANO;
static instrument_t s_last_auto_instr = INSTR_COUNT;
static bool s_was_practicing;
static int s_brightness = -1;
static bool s_touch_start_valid;
static page_t s_touch_start_page;
static lv_point_t s_touch_start;

typedef struct {
    instrument_t instr;
    int delta;
} goal_adjust_t;

static goal_adjust_t s_goal_adjust[INSTR_COUNT * 2] = {
    { INSTR_CELLO, -5 }, { INSTR_CELLO, 5 },
    { INSTR_PIANO, -5 }, { INSTR_PIANO, 5 },
};

static void show_page(page_t page);

static void allow_gesture_bubble(lv_obj_t *obj)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
}

static lv_color_t accent(instrument_t instr)
{
    return instr == INSTR_PIANO ? COL_PIANO_STRONG : COL_CELLO_STRONG;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    lv_label_set_text(obj, text);
    allow_gesture_bubble(obj);
    return obj;
}

static lv_obj_t *button(lv_obj_t *parent, int x, int y, int w, int h, const char *text,
                        lv_color_t bg, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    allow_gesture_bubble(btn);
    lv_obj_t *txt = label(btn, &lv_font_montserrat_14, lv_color_white(), text);
    lv_obj_center(txt);
    return btn;
}

static lv_obj_t *icon_shape(lv_obj_t *parent, int x, int y, int w, int h,
                            lv_color_t color, int radius)
{
    lv_obj_t *shape = lv_obj_create(parent);
    lv_obj_set_size(shape, w, h);
    lv_obj_set_pos(shape, x, y);
    lv_obj_set_style_bg_color(shape, color, 0);
    lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(shape, 0, 0);
    lv_obj_set_style_pad_all(shape, 0, 0);
    lv_obj_set_style_radius(shape, radius, 0);
    lv_obj_set_scrollable(shape, false);
    allow_gesture_bubble(shape);
    return shape;
}

static void create_piano_icon(lv_obj_t *parent)
{
    /* A compact keyboard glyph, drawn from native LVGL shapes. */
    (void)icon_shape(parent, 13, 32, 94, 80, COL_ICON_FG, 9);
    for (int i = 1; i < 5; i++) icon_shape(parent, 13 + i * 94 / 5 - 1, 38, 2, 67, COL_ICON_BG, 0);
    icon_shape(parent, 31, 38, 10, 39, COL_ICON_BG, 0);
    icon_shape(parent, 50, 38, 10, 39, COL_ICON_BG, 0);
    icon_shape(parent, 88, 38, 10, 39, COL_ICON_BG, 0);
}

static void create_cello_icon(lv_obj_t *parent)
{
    /* The overlapping rounded forms read as the cello's shoulders and body. */
    (void)icon_shape(parent, 39, 46, 42, 48, COL_ICON_FG, 20);
    (void)icon_shape(parent, 26, 68, 68, 56, COL_ICON_FG, 24);
    (void)icon_shape(parent, 48, 11, 24, 28, COL_ICON_FG, 10);
    (void)icon_shape(parent, 55, 27, 10, 68, COL_ICON_BG, 4);
    (void)icon_shape(parent, 59, 26, 2, 89, COL_ICON_FG, 1);
    (void)icon_shape(parent, 39, 88, 42, 4, COL_ICON_BG, 2);
    (void)icon_shape(parent, 40, 76, 4, 16, COL_ICON_BG, 2);
    (void)icon_shape(parent, 76, 76, 4, 16, COL_ICON_BG, 2);
    (void)icon_shape(parent, 58, 121, 4, 13, COL_ICON_FG, 1);
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_bg_color(obj, COL_CARD, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, COL_LINE, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 18, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_scrollable(obj, false);
    allow_gesture_bubble(obj);
    return obj;
}

static lv_obj_t *line_label(lv_obj_t *parent, int x, int y, int w, lv_color_t color, const char *text)
{
    lv_obj_t *obj = label(parent, &lv_font_montserrat_14, color, text);
    lv_obj_set_width(obj, w);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

static lv_obj_t *create_page(page_t page)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root, 480, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_scrollable(root, false);
    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    s_page[page] = root;
    return root;
}

static void fmt_duration(char *buf, size_t n, int64_t ms)
{
    int seconds = (int)(ms / 1000);
    if (seconds < 0) seconds = 0;
    if (seconds >= 3600) {
        snprintf(buf, n, "%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    } else {
        snprintf(buf, n, "%d:%02d", seconds / 60, seconds % 60);
    }
}

static uint32_t live_seconds_for(const live_state_t *live, instrument_t instr)
{
    if (live->state != SS_PRACTICING || live->instrument != instr) return 0;
    int64_t end = live->now_ms;
    int64_t gap = live->now_ms - live->last_active_ms;
    if (gap >= PAUSE_SHOW_MS) end = live->last_active_ms;
    if (end <= live->start_ms) return 0;
    return (uint32_t)((end - live->start_ms) / 1000);
}

static void set_brightness(int pct)
{
    if (pct != s_brightness) {
        bsp_display_brightness_set(pct);
        s_brightness = pct;
    }
}

static void show_page(page_t page)
{
    if (page < PAGE_PIANO || page >= PAGE_COUNT || !s_page[page]) return;
    s_current_page = page;
    if (s_wifi_dialog && page != PAGE_DIAGNOSTICS) {
        lv_obj_add_flag(s_wifi_dialog, LV_OBJ_FLAG_HIDDEN);
    }
    for (int p = 0; p < PAGE_COUNT; p++) {
        if (s_page[p]) lv_obj_add_flag(s_page[p], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_page[page], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_page[page]);
}

/* Track touch endpoints directly so page swipes do not depend on LVGL gesture heuristics. */
static void on_page_touch(lv_event_t *event)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    page_t page = (page_t)(intptr_t)lv_event_get_user_data(event);
    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        if (s_wifi_dialog && !lv_obj_has_flag(s_wifi_dialog, LV_OBJ_FLAG_HIDDEN)) return;
        if (lv_event_get_target(event) == s_diag_sens_slider) return;   /* a drag is not a swipe */
        lv_indev_get_point(indev, &s_touch_start);
        s_touch_start_page = page;
        s_touch_start_valid = true;
        return;
    }

    if (lv_event_get_code(event) != LV_EVENT_RELEASED || !s_touch_start_valid) return;
    s_touch_start_valid = false;
    if (s_wifi_dialog && !lv_obj_has_flag(s_wifi_dialog, LV_OBJ_FLAG_HIDDEN)) return;
    if (page != s_touch_start_page || page != s_current_page) return;

    lv_point_t end;
    lv_indev_get_point(indev, &end);
    int dx = end.x - s_touch_start.x;
    int dy = end.y - s_touch_start.y;
    int abs_dx = dx < 0 ? -dx : dx;
    int abs_dy = dy < 0 ? -dy : dy;
    if (abs_dx < SWIPE_MIN_DISTANCE || abs_dx <= abs_dy) return;

    if (dx < 0 && s_current_page < PAGE_GOALS) {
        show_page((page_t)(s_current_page + 1));
    } else if (dx > 0 && s_current_page > PAGE_PIANO) {
        show_page((page_t)(s_current_page - 1));
    }
}

static void add_page_footer(lv_obj_t *parent, page_t page)
{
    static const char *const hint[PAGE_COUNT][4] = {
        { "Swipe left", "Cello", "Diagnostics", NULL },
        { "Swipe right", "Piano", "Swipe left", "Diagnostics" },
        { "Swipe right", "Cello", "Swipe left", "Goals" },
        { "Swipe right", "Diagnostics", NULL, NULL },
    };

    /* Montserrat 14 in this build lacks U+00B7. Draw the separators so they
     * stay round instead of relying on a missing-glyph square or baseline dot. */
    lv_obj_t *footer = lv_obj_create(parent);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, 448, 22);
    lv_obj_set_layout(footer, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(footer, 8, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -4);
    for (int i = 0; i < 4 && hint[page][i]; i++) {
        if (i > 0) {
            lv_obj_t *dot = lv_obj_create(footer);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 4, 4);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(dot, COL_DIM, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        }
        lv_obj_t *item = label(footer, &lv_font_montserrat_14, COL_DIM, hint[page][i]);
        lv_obj_set_style_pad_all(item, 0, 0);
    }
    void *page_data = (void *)(intptr_t)page;
    lv_obj_add_event_cb(parent, on_page_touch, LV_EVENT_PRESSED, page_data);
    lv_obj_add_event_cb(parent, on_page_touch, LV_EVENT_RELEASED, page_data);
}

static void on_diag_label(lv_event_t *event)
{
    instrument_t instr = (instrument_t)(intptr_t)lv_event_get_user_data(event);
    if (instr < INSTR_COUNT) app_send_cmd(CMD_SET_INSTRUMENT, (int)instr);
}

static void on_sensitivity(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    app_send_cmd(CMD_SET_SENSITIVITY, 36 - lv_slider_get_value(slider));
}

static void on_preview(lv_event_t *event)
{
    (void)event;
    live_state_t live;
    app_get_live(&live);
    if (live.state != SS_IDLE) {
        show_page(live.instrument == INSTR_PIANO ? PAGE_PIANO : PAGE_CELLO);
    } else if (live.last_valid) {
        show_page(live.last_instrument == INSTR_PIANO ? PAGE_PIANO : PAGE_CELLO);
    }
}

static void on_wifi_open(lv_event_t *event)
{
    (void)event;
    char password[65] = { 0 };
    if (store_get_string_setting("wifi_pass", password, sizeof(password)) != ESP_OK) {
        strlcpy(password, CONFIG_PL_WIFI_PASSWORD, sizeof(password));
    }
    lv_textarea_set_text(s_wifi_ssid, net_ssid()[0] ? net_ssid() : CONFIG_PL_WIFI_SSID);
    lv_textarea_set_text(s_wifi_password, password);
    if (net_connected()) lv_label_set_text_fmt(s_wifi_status, "Connected" DOT "%s", net_ip());
    else lv_label_set_text(s_wifi_status, net_ssid()[0] ? "Saved network" DOT "not connected" : "Enter your network details");
    s_wifi_connect_pending = false;
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_ssid);
    lv_obj_remove_flag(s_wifi_dialog, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_wifi_dialog);
}

static void on_wifi_close(lv_event_t *event)
{
    (void)event;
    lv_obj_add_flag(s_wifi_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void on_wifi_field(lv_event_t *event)
{
    lv_keyboard_set_textarea(s_wifi_keyboard, lv_event_get_target(event));
}

static void on_wifi_connect(lv_event_t *event)
{
    (void)event;
    const char *ssid = lv_textarea_get_text(s_wifi_ssid);
    const char *password = lv_textarea_get_text(s_wifi_password);
    esp_err_t err = net_set_wifi_credentials(ssid, password);
    if (err == ESP_OK) {
        lv_label_set_text(s_wifi_status, "Saved on this board; connecting...");
        s_wifi_connect_pending = true;
    } else if (!ssid[0]) {
        lv_label_set_text(s_wifi_status, "Enter a network name first");
    } else {
        lv_label_set_text(s_wifi_status, "Could not save/connect; check the network details");
    }
}

static void on_adjust_goal(lv_event_t *event)
{
    goal_adjust_t *adjust = lv_event_get_user_data(event);
    if (!adjust || adjust->instr >= INSTR_COUNT) return;
    int next = (int)s_goal_min[adjust->instr] + adjust->delta;
    if (next < 5 || next > 240) return;
    s_goal_min[adjust->instr] = (uint16_t)next;
    lv_label_set_text_fmt(s_goal_value[adjust->instr], "%u min", (unsigned)next);
    app_send_cmd(CMD_SET_GOAL, (int)adjust->instr * 1000 + next);
    s_loaded_version = UINT32_MAX;
}

static void reload_history(uint32_t today)
{
    static const char *const weekdays[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    int today_index = 6;
    memset(s_week, 0, sizeof(s_week));
    memset(&s_today, 0, sizeof(s_today));
    if (today) {
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        today_index = (local.tm_wday + 6) % 7;
    }
    s_today_index = today ? today_index : -1;
    for (int i = 0; i < 7; i++) {
        uint32_t date = 0;
        day_rec_t record = { 0 };
        if (today && i <= today_index) {
            date = app_date_minus(today, today_index - i);
            store_get_day(date, &record);
        } else if (!today && i == 6) {
            /* Preserve the undated total while leaving the weekday chart unassigned. */
            store_get_day(0, &s_today);
        }
        if (today && i == today_index) s_today = record;
        s_week[i] = record;

        for (int k = 0; k < INSTR_COUNT; k++) {
            lv_label_set_text(s_practice[k].day_label[i], weekdays[i]);
            lv_obj_set_style_text_color(s_practice[k].day_label[i],
                                        today && i == today_index ? accent((instrument_t)k) : COL_DIM, 0);
        }
    }
}

static void update_week_bars(const live_state_t *live, uint32_t today)
{
    for (int instr = 0; instr < INSTR_COUNT; instr++) {
        uint32_t goal = s_goal_min[instr] ? s_goal_min[instr] : 1;
        for (int day = 0; day < 7; day++) {
            uint64_t seconds = s_week[day].secs[instr];
            bool is_today = today && day == s_today_index;
            if (is_today) seconds += live_seconds_for(live, (instrument_t)instr);
            uint64_t pct = seconds * 100u / ((uint64_t)goal * 60u);
            if (pct > 100) pct = 100;
            lv_bar_set_value(s_practice[instr].day_bar[day], (int32_t)pct, LV_ANIM_OFF);
            lv_color_t track = !is_today ? COL_BAR_TRACK :
                               instr == INSTR_PIANO ? COL_PIANO_TODAY_TRACK : COL_CELLO_TODAY_TRACK;
            lv_color_t fill = is_today ? accent((instrument_t)instr) :
                              instr == INSTR_PIANO ? COL_PIANO : COL_CELLO;
            lv_obj_set_style_bg_color(s_practice[instr].day_bar[day], track, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_practice[instr].day_bar[day], fill, LV_PART_INDICATOR);
        }
    }
}

static void create_nav_header(lv_obj_t *page, const lv_font_t *font, const char *title, const char *counter)
{
    lv_obj_t *heading = label(page, font, COL_TEXT, title);
    lv_obj_set_pos(heading, 20, 14);
    lv_obj_t *page_no = label(page, &lv_font_montserrat_14, COL_DIM, counter);
    lv_obj_align(page_no, LV_ALIGN_TOP_RIGHT, -22, 22);
}

static void create_practice_page(page_t page, instrument_t instr)
{
    lv_obj_t *root = create_page(page);
    practice_widgets_t *widgets = &s_practice[instr];
    lv_color_t color = accent(instr);
    create_nav_header(root, &lv_font_montserrat_28, instr == INSTR_PIANO ? "Piano practice" : "Cello practice",
                      instr == INSTR_PIANO ? "1 / 4" : "2 / 4");

    widgets->status_dot = icon_shape(root, 22, 59, 10, 10, COL_LINE, 5);
    widgets->status = label(root, &lv_font_montserrat_14, COL_DIM, "Listening");
    lv_obj_set_pos(widgets->status, 39, 56);

    widgets->confidence = label(root, &lv_font_montserrat_14, COL_DIM, "--%");
    lv_obj_set_style_bg_color(widgets->confidence, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(widgets->confidence, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(widgets->confidence, 13, 0);
    lv_obj_set_style_pad_hor(widgets->confidence, 12, 0);
    lv_obj_set_style_pad_ver(widgets->confidence, 5, 0);
    lv_obj_align(widgets->confidence, LV_ALIGN_TOP_RIGHT, -20, 50);

    lv_obj_t *hero = card(root, 20, 88, 440, 176);
    lv_obj_t *icon = icon_shape(hero, 16, 16, 120, 144, COL_ICON_BG, 16);
    if (instr == INSTR_PIANO) create_piano_icon(icon);
    else create_cello_icon(icon);

    lv_obj_t *today_label = label(hero, &lv_font_montserrat_14, COL_DIM, "TODAY");
    lv_obj_set_pos(today_label, 160, 22);
    widgets->total = label(hero, &lv_font_montserrat_48, COL_TEXT, "0:00");
    lv_obj_set_pos(widgets->total, 158, 42);
    widgets->goal_bar = lv_bar_create(hero);
    lv_obj_set_size(widgets->goal_bar, 256, 10);
    lv_obj_set_pos(widgets->goal_bar, 160, 112);
    lv_bar_set_range(widgets->goal_bar, 0, 100);
    lv_obj_set_style_bg_color(widgets->goal_bar, COL_BAR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widgets->goal_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(widgets->goal_bar, color, LV_PART_INDICATOR);
    allow_gesture_bubble(widgets->goal_bar);
    widgets->goal_text = label(hero, &lv_font_montserrat_14, COL_DIM, "0 of 30 min");
    lv_obj_set_pos(widgets->goal_text, 160, 132);

    for (int i = 0; i < 7; i++) {
        widgets->day_bar[i] = lv_bar_create(root);
        lv_obj_set_size(widgets->day_bar[i], 34, 112);
        lv_obj_set_pos(widgets->day_bar[i], 34 + i * 440 / 7, 300);
        lv_bar_set_orientation(widgets->day_bar[i], LV_BAR_ORIENTATION_VERTICAL);
        lv_bar_set_range(widgets->day_bar[i], 0, 100);
        lv_bar_set_value(widgets->day_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(widgets->day_bar[i], COL_BAR_TRACK, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(widgets->day_bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(widgets->day_bar[i], color, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(widgets->day_bar[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_border_width(widgets->day_bar[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(widgets->day_bar[i], 8, LV_PART_MAIN);
        lv_obj_set_style_radius(widgets->day_bar[i], 8, LV_PART_INDICATOR);
        allow_gesture_bubble(widgets->day_bar[i]);

        widgets->day_label[i] = label(root, &lv_font_montserrat_14, COL_DIM, "");
        lv_obj_set_width(widgets->day_label[i], 440 / 7);
        lv_obj_set_style_text_align(widgets->day_label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(widgets->day_label[i], LV_ALIGN_TOP_LEFT, 20 + i * 440 / 7, 418);
    }
    add_page_footer(root, page);
}

static void style_text_field(lv_obj_t *field)
{
    lv_obj_set_style_radius(field, 10, 0);
    lv_obj_set_style_border_color(field, COL_LINE, 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_border_color(field, COL_PIANO_STRONG, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(field, on_wifi_field, LV_EVENT_CLICKED, NULL);
    allow_gesture_bubble(field);
}

static void create_diagnostics_page(float sensitivity_db)
{
    lv_obj_t *root = create_page(PAGE_DIAGNOSTICS);
    create_nav_header(root, &lv_font_montserrat_24, "Microphone diagnostics", "3 / 4");

    lv_obj_t *recognition = card(root, 20, 54, 440, 72);
    s_diag_classifier = label(recognition, &lv_font_montserrat_18, COL_AMBER, "Waiting for notes");
    lv_obj_set_width(s_diag_classifier, 408);
    lv_label_set_long_mode(s_diag_classifier, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(s_diag_classifier, 16, 12);
    s_diag_time = line_label(recognition, 16, 42, 408, COL_DIM, "Clock not synced");

    lv_obj_t *mic = card(root, 20, 134, 440, 124);
    s_diag_mic = line_label(mic, 16, 12, 408, COL_TEXT, "Mic -- dBFS");
    s_diag_mic_bar = lv_bar_create(mic);
    lv_obj_set_size(s_diag_mic_bar, 408, 10);
    lv_obj_set_pos(s_diag_mic_bar, 16, 36);
    lv_bar_set_range(s_diag_mic_bar, 0, 40);
    lv_obj_set_style_bg_color(s_diag_mic_bar, COL_BAR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_diag_mic_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_diag_mic_bar, COL_PIANO_STRONG, LV_PART_INDICATOR);
    allow_gesture_bubble(s_diag_mic_bar);

    s_diag_activity = line_label(mic, 16, 56, 408, COL_DIM, "LISTENING");
    lv_obj_t *sensitivity = label(mic, &lv_font_montserrat_14, COL_DIM, "Sensitivity");
    lv_obj_set_pos(sensitivity, 16, 90);
    s_diag_sens_slider = lv_slider_create(mic);
    lv_obj_set_size(s_diag_sens_slider, 296, 10);
    lv_slider_set_range(s_diag_sens_slider, 6, 30);
    int slider_value = 36 - (int)(sensitivity_db + 0.5f);
    if (slider_value < 6) slider_value = 6;
    if (slider_value > 30) slider_value = 30;
    lv_slider_set_value(s_diag_sens_slider, slider_value, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s_diag_sens_slider, 14);
    lv_obj_set_pos(s_diag_sens_slider, 118, 94);
    lv_obj_set_style_bg_color(s_diag_sens_slider, COL_BAR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_diag_sens_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_diag_sens_slider, COL_PIANO_STRONG, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_diag_sens_slider, COL_PIANO_STRONG, LV_PART_KNOB);
    lv_obj_add_event_cb(s_diag_sens_slider, on_sensitivity, LV_EVENT_RELEASED, NULL);
    allow_gesture_bubble(s_diag_sens_slider);

    lv_obj_t *system = card(root, 20, 266, 440, 62);
    s_diag_wifi = line_label(system, 16, 10, 408, COL_TEXT, "Wi-Fi offline");
    s_diag_model = line_label(system, 16, 33, 408, COL_DIM, "Adaptive profiles are stored on the board");

    s_diag_hint = line_label(root, 22, 338, 436, COL_DIM, "Tap Piano or Cello to label this session");
    button(root, 20, 364, 102, 48, "Piano", COL_PIANO_STRONG, on_diag_label,
           (void *)(intptr_t)INSTR_PIANO);
    button(root, 132, 364, 102, 48, "Cello", COL_CELLO_STRONG, on_diag_label,
           (void *)(intptr_t)INSTR_CELLO);
    button(root, 244, 364, 102, 48, "Preview", COL_ICON_BG, on_preview, NULL);
    button(root, 356, 364, 104, 48, "Wi-Fi", COL_ICON_BG, on_wifi_open, NULL);

    add_page_footer(root, PAGE_DIAGNOSTICS);

    s_wifi_dialog = lv_obj_create(root);
    lv_obj_set_size(s_wifi_dialog, 480, 480);
    lv_obj_set_pos(s_wifi_dialog, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_dialog, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_wifi_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_dialog, 0, 0);
    lv_obj_set_style_radius(s_wifi_dialog, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_dialog, 0, 0);
    lv_obj_set_scrollable(s_wifi_dialog, false);
    allow_gesture_bubble(s_wifi_dialog);
    lv_obj_t *wifi_title = label(s_wifi_dialog, &lv_font_montserrat_24, COL_TEXT, "Wi-Fi setup");
    lv_obj_set_pos(wifi_title, 20, 12);
    s_wifi_status = line_label(s_wifi_dialog, 20, 44, 440, COL_DIM, "Enter your network details");

    lv_obj_t *ssid_label = label(s_wifi_dialog, &lv_font_montserrat_14, COL_TEXT, "Network");
    lv_obj_set_pos(ssid_label, 20, 80);
    s_wifi_ssid = lv_textarea_create(s_wifi_dialog);
    lv_obj_set_size(s_wifi_ssid, 362, 40);
    lv_obj_set_pos(s_wifi_ssid, 98, 70);
    lv_textarea_set_one_line(s_wifi_ssid, true);
    lv_textarea_set_max_length(s_wifi_ssid, 32);
    lv_textarea_set_placeholder_text(s_wifi_ssid, "SSID");
    style_text_field(s_wifi_ssid);

    lv_obj_t *password_label = label(s_wifi_dialog, &lv_font_montserrat_14, COL_TEXT, "Password");
    lv_obj_set_pos(password_label, 20, 126);
    s_wifi_password = lv_textarea_create(s_wifi_dialog);
    lv_obj_set_size(s_wifi_password, 362, 40);
    lv_obj_set_pos(s_wifi_password, 98, 116);
    lv_textarea_set_one_line(s_wifi_password, true);
    lv_textarea_set_max_length(s_wifi_password, 64);
    lv_textarea_set_password_mode(s_wifi_password, true);
    lv_textarea_set_placeholder_text(s_wifi_password, "Password (blank for open Wi-Fi)");
    style_text_field(s_wifi_password);

    button(s_wifi_dialog, 20, 164, 150, 38, "Save & connect", COL_PIANO_STRONG, on_wifi_connect, NULL);
    lv_obj_t *cancel = button(s_wifi_dialog, 350, 164, 110, 38, "Cancel", COL_PANEL, on_wifi_close, NULL);
    lv_obj_set_style_text_color(lv_obj_get_child(cancel, 0), COL_TEXT, 0);

    /* lv_keyboard defaults to bottom alignment, so set_pos alone would push it off screen. */
    s_wifi_keyboard = lv_keyboard_create(s_wifi_dialog);
    lv_obj_set_size(s_wifi_keyboard, 464, 262);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_TOP_LEFT, 8, 210);
    lv_keyboard_set_mode(s_wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_wifi_keyboard, s_wifi_ssid);
    lv_obj_set_style_bg_color(s_wifi_keyboard, COL_PANEL, 0);
    lv_obj_set_style_radius(s_wifi_keyboard, 14, 0);
    lv_obj_set_style_radius(s_wifi_keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_wifi_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_wifi_keyboard, &lv_font_montserrat_14, LV_PART_ITEMS);
    allow_gesture_bubble(s_wifi_keyboard);
    lv_obj_add_flag(s_wifi_dialog, LV_OBJ_FLAG_HIDDEN);
}

static void create_goals_page(void)
{
    lv_obj_t *root = create_page(PAGE_GOALS);
    create_nav_header(root, &lv_font_montserrat_28, "Practice goals", "4 / 4");
    const instrument_t instruments[] = { INSTR_PIANO, INSTR_CELLO };
    const char *const names[] = { "Piano", "Cello" };
    for (int row = 0; row < INSTR_COUNT; row++) {
        instrument_t instr = instruments[row];
        lv_obj_t *goal = card(root, 20, 70 + row * 164, 440, 150);
        (void)icon_shape(goal, 20, 25, 12, 12, accent(instr), 6);
        lv_obj_t *name = label(goal, &lv_font_montserrat_20, COL_TEXT, names[row]);
        lv_obj_set_pos(name, 42, 19);
        lv_obj_t *daily = label(goal, &lv_font_montserrat_14, COL_DIM, "per day");
        lv_obj_align(daily, LV_ALIGN_TOP_RIGHT, -20, 23);

        lv_obj_t *minus = button(goal, 20, 66, 88, 64, LV_SYMBOL_MINUS, accent(instr), on_adjust_goal,
                                 &s_goal_adjust[instr * 2]);
        lv_obj_t *plus = button(goal, 332, 66, 88, 64, LV_SYMBOL_PLUS, accent(instr), on_adjust_goal,
                                &s_goal_adjust[instr * 2 + 1]);
        lv_obj_set_style_text_font(lv_obj_get_child(minus, 0), &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), &lv_font_montserrat_24, 0);
        s_goal_value[instr] = label(goal, &lv_font_montserrat_28, COL_TEXT, "30 min");
        lv_obj_set_width(s_goal_value[instr], 200);
        lv_obj_set_style_text_align(s_goal_value[instr], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_goal_value[instr], 120, 82);
        lv_label_set_text_fmt(s_goal_value[instr], "%u min", (unsigned)s_goal_min[instr]);
    }
    lv_obj_t *hint = label(root, &lv_font_montserrat_14, COL_DIM,
                           "Adjust in 5-minute steps (5 to 240 min)");
    lv_obj_set_width(hint, 440);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(hint, 20, 402);
    add_page_footer(root, PAGE_GOALS);
}

static void update_instrument_page(const live_state_t *live, instrument_t instr)
{
    practice_widgets_t *widgets = &s_practice[instr];
    uint32_t seconds = s_today.secs[instr] + live_seconds_for(live, instr);
    char text[96];
    fmt_duration(text, sizeof(text), (int64_t)seconds * 1000);
    lv_label_set_text(widgets->total, text);

    lv_label_set_text_fmt(widgets->goal_text, "%lu of %u min", (unsigned long)(seconds / 60),
                          (unsigned)s_goal_min[instr]);

    uint64_t goal_seconds = (uint64_t)(s_goal_min[instr] ? s_goal_min[instr] : 1) * 60u;
    uint64_t goal_pct = (uint64_t)seconds * 100u / goal_seconds;
    lv_bar_set_value(widgets->goal_bar, goal_pct > 100 ? 100 : (int32_t)goal_pct, LV_ANIM_OFF);

    bool this_session = live->state == SS_PRACTICING && live->instrument == instr;
    bool paused = this_session && live->now_ms - live->last_active_ms >= PAUSE_SHOW_MS;
    lv_label_set_text(widgets->status, paused ? "Paused" : this_session ? "Practicing" : "Listening");
    lv_obj_set_style_bg_color(widgets->status_dot, paused ? COL_AMBER :
                              this_session ? accent(instr) : COL_LINE, 0);
    if (this_session && live->set_by_user) {
        lv_label_set_text(widgets->confidence, "100%");
        lv_obj_set_style_text_color(widgets->confidence, COL_GREEN, 0);
    } else if (this_session && live->n_notes >= IDENTIFY_NOTES) {
        lv_label_set_text_fmt(widgets->confidence, "%d%%",
                              (int)(live->confidence * 100.0f + 0.5f));
        lv_obj_set_style_text_color(widgets->confidence,
                                    live->confidence >= 0.8f ? COL_GREEN : COL_AMBER, 0);
    } else {
        lv_label_set_text(widgets->confidence, "--%");
        lv_obj_set_style_text_color(widgets->confidence, COL_DIM, 0);
    }
}

static void update_diagnostics(const live_state_t *live, time_t now)
{
    char text[192];
    if (live->state == SS_PRACTICING && live->set_by_user) {
        snprintf(text, sizeof(text), "%s" DOT "selected by you", s_instr_name[live->instrument]);
    } else if (live->state == SS_PRACTICING && live->n_notes >= IDENTIFY_NOTES) {
        snprintf(text, sizeof(text), "%s %d%%" DOT "%d notes",
                 s_instr_name[live->instrument], (int)(live->confidence * 100.0f + 0.5f), live->n_notes);
    } else if (live->state == SS_PRACTICING) {
        snprintf(text, sizeof(text), "Waiting for notes" DOT "%d so far", live->n_notes);
    } else {
        snprintf(text, sizeof(text), "Waiting for a session");
    }
    lv_label_set_text(s_diag_classifier, text);

    if (live->state == SS_PRACTICING) {
        lv_label_set_text(s_diag_hint, "Tap Piano or Cello to label this session");
    } else if (live->last_valid) {
        lv_label_set_text(s_diag_hint, "Tap Piano or Cello to correct the last session");
    } else {
        lv_label_set_text(s_diag_hint, "Labels apply to a current or recently ended session");
    }

    if (app_clock_valid()) {
        struct tm tm;
        localtime_r(&now, &tm);
        snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d" DOT "clock synced",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(text, sizeof(text), "Clock not synced" DOT "time zone %s", CONFIG_PL_TIMEZONE);
    }
    lv_label_set_text(s_diag_time, text);

    snprintf(text, sizeof(text), "Mic %.1f dBFS" DOT "floor %.1f" DOT "sens. %.1f dB",
             live->level_db, live->floor_db, live->sens_db);
    lv_label_set_text(s_diag_mic, text);
    int slider_value = 36 - (int)(live->sens_db + 0.5f);
    if (slider_value < 6) slider_value = 6;
    if (slider_value > 30) slider_value = 30;
    if (!lv_obj_has_state(s_diag_sens_slider, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_diag_sens_slider, slider_value, LV_ANIM_OFF);
    }
    float above = live->level_db - live->floor_db;
    if (above < 0) above = 0;
    if (above > 40) above = 40;
    lv_bar_set_value(s_diag_mic_bar, (int32_t)above, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_diag_mic_bar, live->active ? COL_GREEN : COL_PIANO_STRONG,
                              LV_PART_INDICATOR);

    const char *state = live->state == SS_PRACTICING ? "PRACTICING" :
                        live->state == SS_CANDIDATE ? "CONFIRMING" : "LISTENING";
    snprintf(text, sizeof(text), "%s" DOT "activity %.2f" DOT "%d notes",
             state, live->activity, live->n_notes);
    lv_label_set_text(s_diag_activity, text);
    lv_label_set_text(s_diag_model, "Adaptive profiles are stored in local board flash");

    if (net_connected()) {
        snprintf(text, sizeof(text), "Wi-Fi connected" DOT "%s", net_ip());
    } else if (net_ssid()[0]) {
        snprintf(text, sizeof(text), "Wi-Fi configured" DOT "connecting");
    } else {
        snprintf(text, sizeof(text), "Wi-Fi offline" DOT "tap Wi-Fi to configure");
    }
    lv_label_set_text(s_diag_wifi, text);
    if (s_wifi_connect_pending && net_connected()) {
        lv_label_set_text_fmt(s_wifi_status, "Connected" DOT "%s", net_ip());
        s_wifi_connect_pending = false;
    }
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    live_state_t live;
    app_get_live(&live);
    time_t now = time(NULL);
    uint32_t today = app_date_of(now);

    if (store_version() != s_loaded_version || today != s_loaded_date) {
        s_loaded_version = store_version();
        s_loaded_date = today;
        reload_history(today);
    }
    for (int i = 0; i < INSTR_COUNT; i++) update_instrument_page(&live, (instrument_t)i);
    update_week_bars(&live, today);
    update_diagnostics(&live, now);

    /* Follow a newly detected/current instrument, but keep Diagnostics under user control. */
    if (live.state == SS_PRACTICING) {
        if (!s_was_practicing || live.instrument != s_last_auto_instr) {
            if (s_current_page != PAGE_DIAGNOSTICS) {
                show_page(live.instrument == INSTR_PIANO ? PAGE_PIANO : PAGE_CELLO);
            }
            s_last_auto_instr = live.instrument;
        }
        s_was_practicing = true;
    } else {
        s_was_practicing = false;
        s_last_auto_instr = INSTR_COUNT;
    }

    uint32_t inactive_ms = lv_display_get_inactive_time(NULL);
    bool idle = live.state == SS_IDLE;
    int brightness = !idle || inactive_ms <= DIM_AFTER_MS ? 80
                   : inactive_ms >= DISPLAY_OFF_AFTER_MS ? 0 : 10;
    set_brightness(brightness);
}

void ui_init(float sens_db)
{
    s_goal_min[INSTR_CELLO] = (uint16_t)store_get_setting("goal_cello", CONFIG_PL_DAILY_GOAL_MIN);
    s_goal_min[INSTR_PIANO] = (uint16_t)store_get_setting("goal_piano", CONFIG_PL_DAILY_GOAL_MIN);
    for (int i = 0; i < INSTR_COUNT; i++) {
        if (s_goal_min[i] < 5 || s_goal_min[i] > 240) s_goal_min[i] = CONFIG_PL_DAILY_GOAL_MIN;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);

    create_practice_page(PAGE_PIANO, INSTR_PIANO);
    create_practice_page(PAGE_CELLO, INSTR_CELLO);
    create_diagnostics_page(sens_db);
    create_goals_page();
    show_page(PAGE_PIANO);
    lv_timer_create(ui_tick, 200, NULL);
}
