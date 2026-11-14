// 우리 집 정보 타일 — Muse 휴대용 1.75인치 둥근 AMOLED (466x466)
//   Muse 얼굴에서 위로 밀면: 1 주식 → 2 날씨 → 3 교통(73번) → 4 일정
//   데이터: Worker /api/gadget?k=<열쇠>. 열쇠는 펌웨어에 넣지 않고 시리얼 ">home_key=…" 로 기기에 저장 (NVS)
//   화면은 LVGL 타이머(=Muse UI 작업, 잠금 안)에서만 고치고, 받기는 별도 작업이 함
#include "home_tiles.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "cJSON.h"

static const char *TAG = "home";
LV_FONT_DECLARE(home_kr_16);
LV_FONT_DECLARE(home_kr_20);
LV_FONT_DECLARE(home_num_26);
LV_FONT_DECLARE(home_num_80);
LV_FONT_DECLARE(home_num_88l);
#define F16 (&home_kr_16)
#define F20 (&home_kr_20)
#define N26 (&home_num_26)
#define N80 (&home_num_80)
#define N88 (&home_num_88l)
#define W 466
#define NT 4                                   /* 우리 타일 수 */
#define UP 0xFF5C76
#define DOWN 0x5AA2FF
#define MUTED 0x8A8F9C

/* ── 받은 데이터 ── */
typedef struct { char t[12], icon[16]; int temp; } hour_t;
typedef struct { char title[64], time[48], who[16]; uint32_t color; } ev_t;
typedef struct { int in; char at[8]; bool live; } bus_t;
typedef struct {
    bool ok, stock, wx, bus, cal;
    /* 주식 */ char name[32], status[16], cd[48]; long price, chg, hi, lo, prev; double pct; int np; int px[64]; long py[64];
               bool flow; long fi, ff, fo;
    /* 날씨 */ int temp, feels, hi_t, lo_t, rain; char desc[32], icon[16], sunset[24]; int nh; hour_t h[4];
    /* 교통 */ char route[8]; int walk, nb; bus_t b[3]; int64_t at_us;
    /* 일정 */ char dow[16]; int day, mon, ne, more; ev_t e[3]; bool tm; char tm_title[64], tm_time[24];
    char err[64];
} home_data_t;
static home_data_t *s_d;                         /* 받기 작업이 채움 (PSRAM) */
static home_data_t *s_v;                         /* 화면이 쓰는 사본 */
static SemaphoreHandle_t s_mtx;
static volatile uint32_t s_ver, s_shown;
static volatile bool s_viewing;                  /* 우리 타일을 보고 있음 */
static volatile int64_t s_last_ok_us;
static TaskHandle_t s_task;
static char s_key[96], s_url[160];

/* ── 화면 ── */
static lv_obj_t *s_tv, *s_face, *s_tile[NT], *s_msg[NT];
/* 주식 */ static lv_obj_t *st_name, *st_pill, *st_pill_l, *st_cd, *st_price, *st_won, *st_chg, *st_chart, *st_base, *st_fv[3], *st_hl;
static lv_point_precise_t st_pts[64], st_bpts[2];
/* 날씨 */ static lv_obj_t *wx_icon, *wx_temp, *wx_desc, *wx_hl, *wx_hc[4], *wx_ht[4], *wx_hi[4], *wx_hv[4], *wx_sun;
/* 교통 */ static lv_obj_t *bs_ring, *bs_route, *bs_state, *bs_cnt, *bs_sub, *bs_row[2], *bs_rt[2], *bs_rm[2], *bs_foot;
/* 일정 */ static lv_obj_t *cl_dow, *cl_day, *cl_sub, *cl_row[3], *cl_bar[3], *cl_t[3], *cl_s[3], *cl_tm;

static lv_obj_t *lbl(lv_obj_t *p, const lv_font_t *f, uint32_t c, const char *t, int x, int y, int w, lv_text_align_t a)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    if (w > 0) { lv_obj_set_width(l, w); lv_obj_set_style_text_align(l, a, 0); }
    return l;
}
static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, uint32_t c, int r)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y); lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0); lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
static void set_txt(lv_obj_t *l, const char *t) { if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }
static void set_col(lv_obj_t *l, uint32_t c) { lv_obj_set_style_text_color(l, lv_color_hex(c), 0); }
static void tint(lv_obj_t *tile, uint32_t top)       /* 위쪽만 은은한 색, 아래는 검정 (AMOLED) */
{
    lv_obj_set_style_bg_color(tile, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(tile, 0, 0);
    lv_obj_set_style_bg_grad_stop(tile, 170, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
}
static void comma(long v, char *b, size_t n)
{
    char t[24]; snprintf(t, sizeof t, "%ld", v < 0 ? -v : v);
    int len = strlen(t), o = 0;
    if (v < 0 && o < (int)n - 1) b[o++] = '-';
    for (int i = 0; i < len && o < (int)n - 1; i++) { if (i && (len - i) % 3 == 0 && o < (int)n - 1) b[o++] = ','; b[o++] = t[i]; }
    b[o] = 0;
}
static void dots(lv_obj_t *tile, int idx)            /* 오른쪽 가장자리: 0 = Muse, 1~4 = 우리 */
{
    int y0 = W / 2 - (5 * 6 + 4 * 7 + 12) / 2;
    for (int i = 0, y = y0; i <= NT; i++) {
        bool on = (i == idx);
        box(tile, W - 26, y, 6, on ? 18 : 6, on ? 0xFFFFFF : 0x48484C, 3);
        y += (on ? 18 : 6) + 7;
    }
}

/* ── 날씨 아이콘 (원과 둥근 네모로 그림) ── */
static void icon_clear(lv_obj_t *c) { lv_obj_clean(c); }
static void cloud(lv_obj_t *c, int x, int y, int s, uint32_t col)
{
    box(c, x, y + s * 40 / 100, s, s * 45 / 100, col, s * 22 / 100);
    box(c, x + s * 30 / 100, y + s * 12 / 100, s * 52 / 100, s * 52 / 100, col, s);
    box(c, x + s * 12 / 100, y + s * 28 / 100, s * 36 / 100, s * 36 / 100, col, s);
}
static void draw_icon(lv_obj_t *c, const char *k, int s, uint32_t bg)
{
    icon_clear(c);
    bool night = strstr(k, "moon") || strstr(k, "night");
    bool cl = strstr(k, "cloud") || strstr(k, "partly") || strstr(k, "rain") || strstr(k, "snow") || strstr(k, "storm") ||
              strstr(k, "thunder") || strstr(k, "drizzle") || strstr(k, "shower") || strstr(k, "overcast");
    bool full = cl && !strstr(k, "partly") && !strstr(k, "mooncloud");
    if (strstr(k, "fog") || strstr(k, "mist")) {
        for (int i = 0; i < 3; i++) box(c, s * (8 + i * 6) / 100, s * (30 + i * 18) / 100, s * 80 / 100, s * 9 / 100, 0xB8C2D0, s);
        return;
    }
    if (!full) {                                           /* 해 또는 달 */
        int d = cl ? s * 52 / 100 : s * 70 / 100, x = cl ? 0 : s * 15 / 100, y = cl ? 0 : s * 5 / 100;
        if (night) { box(c, x, y, d, d, 0xF4E3A1, d); box(c, x + d * 35 / 100, y - d * 10 / 100, d, d, bg, d); }
        else { lv_obj_t *sun = box(c, x, y, d, d, 0xFFC23D, d);
               lv_obj_set_style_shadow_color(sun, lv_color_hex(0xFFB020), 0); lv_obj_set_style_shadow_width(sun, s / 4, 0); lv_obj_set_style_shadow_opa(sun, 140, 0); }
    }
    if (cl) {
        uint32_t cc = (strstr(k, "rain") || strstr(k, "storm") || strstr(k, "thunder") || strstr(k, "drizzle") || strstr(k, "shower")) ? 0xB9C4D4 : 0xF2F5F9;
        cloud(c, s * 18 / 100, s * 18 / 100, s * 80 / 100, cc);
        if (strstr(k, "rain") || strstr(k, "drizzle") || strstr(k, "shower"))
            for (int i = 0; i < 3; i++) box(c, s * (34 + i * 18) / 100, s * 86 / 100, s * 5 / 100, s * 14 / 100, 0x6FB4FF, 2);
        if (strstr(k, "snow"))
            for (int i = 0; i < 3; i++) box(c, s * (32 + i * 18) / 100, s * 88 / 100, s * 9 / 100, s * 9 / 100, 0xFFFFFF, s);
        if (strstr(k, "storm") || strstr(k, "thunder")) box(c, s * 48 / 100, s * 82 / 100, s * 8 / 100, s * 18 / 100, 0xFFD23F, 1);
    }
}

/* ── 타일 만들기 ── */
static lv_obj_t *new_tile(int row)
{
    lv_obj_t *t = lv_tileview_add_tile(s_tv, 0, row, row < NT ? (LV_DIR_TOP | LV_DIR_BOTTOM) : LV_DIR_TOP);
    lv_obj_set_scrollbar_mode(t, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(t, lv_color_black(), 0); lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    return t;
}
static lv_obj_t *centered_row(lv_obj_t *p, int y)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return r;
}
static void build_stock(lv_obj_t *t)
{
    lv_obj_t *r = centered_row(t, 54);
    st_name = lbl(r, F20, 0xFFFFFF, "주식", 0, 0, 0, 0);
    st_pill = lv_obj_create(r); lv_obj_remove_style_all(st_pill); lv_obj_set_size(st_pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(st_pill, lv_color_hex(0xFF4D6A), 0); lv_obj_set_style_bg_opa(st_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st_pill, 12, 0); lv_obj_set_style_pad_hor(st_pill, 9, 0); lv_obj_set_style_pad_ver(st_pill, 2, 0);
    st_pill_l = lbl(st_pill, F16, 0xFFFFFF, "", 0, 0, 0, 0);
    st_cd = lbl(t, F16, MUTED, "", 0, 86, W, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *pr = centered_row(t, 106);
    lv_obj_set_style_pad_column(pr, 4, 0);
    st_price = lbl(pr, N80, 0xFFFFFF, "-", 0, 0, 0, 0);
    st_won = lbl(pr, F20, 0xFFFFFF, "원", 0, 0, 0, 0); lv_obj_set_style_pad_bottom(st_won, 12, 0);
    st_chg = lbl(t, N26, UP, "", 0, 196, W, LV_TEXT_ALIGN_CENTER);
    st_base = lv_line_create(t); lv_obj_set_pos(st_base, 58, 236);
    lv_obj_set_style_line_color(st_base, lv_color_hex(0x5A5D66), 0); lv_obj_set_style_line_width(st_base, 1, 0);
    lv_obj_set_style_line_dash_width(st_base, 4, 0); lv_obj_set_style_line_dash_gap(st_base, 5, 0);
    st_chart = lv_line_create(t); lv_obj_set_pos(st_chart, 58, 236);
    lv_obj_set_style_line_width(st_chart, 3, 0); lv_obj_set_style_line_rounded(st_chart, true, 0);
    const char *who[3] = { "개인", "외국인", "기관" };
    for (int i = 0; i < 3; i++) {
        int x = 78 + i * 104;
        lbl(t, F16, MUTED, who[i], x, 346, 104, LV_TEXT_ALIGN_CENTER);
        st_fv[i] = lbl(t, F16, 0xFFFFFF, "-", x, 368, 104, LV_TEXT_ALIGN_CENTER);
    }
    st_hl = lbl(t, F16, 0x6A6E78, "", 0, 404, W, LV_TEXT_ALIGN_CENTER);
}
static void build_weather(lv_obj_t *t)
{
    lbl(t, F16, 0xB8C2D6, "Belmont", 0, 56, W, LV_TEXT_ALIGN_CENTER);
    wx_icon = lv_obj_create(t); lv_obj_remove_style_all(wx_icon); lv_obj_set_size(wx_icon, 96, 80); lv_obj_align(wx_icon, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_remove_flag(wx_icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    wx_temp = lbl(t, N88, 0xFFFFFF, "-", 0, 166, W, LV_TEXT_ALIGN_CENTER);
    wx_desc = lbl(t, F20, 0xFFFFFF, "", 0, 266, W, LV_TEXT_ALIGN_CENTER);
    wx_hl = lbl(t, F16, 0xB8C2D6, "", 0, 296, W, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 4; i++) {
        int x = 92 + i * 72;
        wx_hc[i] = lv_obj_create(t); lv_obj_remove_style_all(wx_hc[i]); lv_obj_set_pos(wx_hc[i], x, 326); lv_obj_set_size(wx_hc[i], 72, 76);
        lv_obj_remove_flag(wx_hc[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        wx_ht[i] = lbl(wx_hc[i], F16, MUTED, "", 0, 0, 72, LV_TEXT_ALIGN_CENTER);
        wx_hi[i] = lv_obj_create(wx_hc[i]); lv_obj_remove_style_all(wx_hi[i]); lv_obj_set_pos(wx_hi[i], 22, 22); lv_obj_set_size(wx_hi[i], 28, 28);
        wx_hv[i] = lbl(wx_hc[i], F16, 0xFFFFFF, "", 0, 54, 72, LV_TEXT_ALIGN_CENTER);
    }
    wx_sun = lbl(t, F16, 0x6A6E78, "", 0, 410, W, LV_TEXT_ALIGN_CENTER);
}
static void build_bus(lv_obj_t *t)
{
    bs_ring = lv_arc_create(t);
    lv_obj_set_size(bs_ring, W - 18, W - 18); lv_obj_center(bs_ring);
    lv_arc_set_rotation(bs_ring, 270); lv_arc_set_bg_angles(bs_ring, 0, 360); lv_arc_set_range(bs_ring, 0, 1200); lv_arc_set_value(bs_ring, 0);
    lv_obj_remove_style(bs_ring, NULL, LV_PART_KNOB); lv_obj_remove_flag(bs_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(bs_ring, 12, LV_PART_MAIN); lv_obj_set_style_arc_width(bs_ring, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(bs_ring, lv_color_hex(0x1A1C20), LV_PART_MAIN);
    lv_obj_set_style_arc_color(bs_ring, lv_color_hex(0x2FD49A), LV_PART_INDICATOR); lv_obj_set_style_arc_rounded(bs_ring, true, LV_PART_INDICATOR);
    lv_obj_t *r = centered_row(t, 66);
    lv_obj_t *pill = lv_obj_create(r); lv_obj_remove_style_all(pill); lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0xFFD23F), 0); lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pill, 7, 0); lv_obj_set_style_pad_hor(pill, 10, 0); lv_obj_set_style_pad_ver(pill, 2, 0);
    bs_route = lbl(pill, F20, 0x1A1A1A, "73", 0, 0, 0, 0);
    lbl(r, F16, 0xC8CCD4, "Cushing Sq → Harvard", 0, 0, 0, 0);
    bs_state = lbl(t, F20, 0x2FD49A, "", 0, 106, W, LV_TEXT_ALIGN_CENTER);
    bs_cnt = lbl(t, N80, 0xFFFFFF, "-:--", 0, 136, W, LV_TEXT_ALIGN_CENTER);
    bs_sub = lbl(t, F16, MUTED, "", 0, 240, W, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 2; i++) {
        bs_row[i] = box(t, 96, 278 + i * 50, W - 192, 42, i ? 0x000000 : 0x0E3326, 12);
        if (i) lv_obj_set_style_bg_opa(bs_row[i], LV_OPA_TRANSP, 0);
        bs_rt[i] = lbl(bs_row[i], F20, i ? 0xC8CCD4 : 0xFFFFFF, "", 14, 9, 0, 0);
        bs_rm[i] = lbl(bs_row[i], F20, i ? MUTED : 0x2FD49A, "", 0, 9, W - 192 - 14, LV_TEXT_ALIGN_RIGHT);
    }
    bs_foot = lbl(t, F16, 0x6A6E78, "", 0, 400, W, LV_TEXT_ALIGN_CENTER);
}
static void build_cal(lv_obj_t *t)
{
    cl_dow = lbl(t, F16, 0xFF6B6B, "", 0, 46, W, LV_TEXT_ALIGN_CENTER);
    cl_day = lbl(t, N88, 0xFFFFFF, "", 0, 62, W, LV_TEXT_ALIGN_CENTER);
    cl_sub = lbl(t, F16, MUTED, "", 0, 158, W, LV_TEXT_ALIGN_CENTER);
    for (int i = 0; i < 3; i++) {
        cl_row[i] = box(t, 74, 188 + i * 60, W - 148, 52, 0x6C8CFF, 12); lv_obj_set_style_bg_opa(cl_row[i], 36, 0);
        cl_bar[i] = box(cl_row[i], 12, 9, 5, 34, 0x6C8CFF, 3);
        cl_t[i] = lbl(cl_row[i], F16, 0xFFFFFF, "", 28, 6, W - 148 - 40, LV_TEXT_ALIGN_LEFT); lv_label_set_long_mode(cl_t[i], LV_LABEL_LONG_DOT); lv_obj_set_height(cl_t[i], 20);
        cl_s[i] = lbl(cl_row[i], F16, 0x9AA0AC, "", 28, 27, W - 148 - 40, LV_TEXT_ALIGN_LEFT); lv_label_set_long_mode(cl_s[i], LV_LABEL_LONG_DOT); lv_obj_set_height(cl_s[i], 20);
        lv_obj_add_flag(cl_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    cl_tm = lbl(t, F16, 0xB0B4BE, "", 60, 376, W - 120, LV_TEXT_ALIGN_CENTER); lv_label_set_long_mode(cl_tm, LV_LABEL_LONG_DOT); lv_obj_set_height(cl_tm, 20);
}

/* ── 화면 채우기 ── */
static void render_stock(const home_data_t *d)
{
    if (!d->stock) return;
    bool up = d->chg > 0, down = d->chg < 0; uint32_t c = up ? UP : down ? DOWN : 0xFFFFFF;
    tint(s_tile[0], up ? 0x2A0A12 : down ? 0x0A1830 : 0x15161A);
    set_txt(st_name, d->name);
    set_txt(st_pill_l, d->status);
    lv_obj_set_style_bg_color(st_pill, lv_color_hex(!strcmp(d->status, "본장") ? 0xFF4D6A : 0x3A3D46), 0);
    set_txt(st_cd, d->cd);
    char b[48], b2[24]; comma(d->price, b, sizeof b); set_txt(st_price, b);
    comma(labs(d->chg), b2, sizeof b2);
    snprintf(b, sizeof b, "%s %.2f%%   %s%s", up ? "▲" : down ? "▼" : "", d->pct < 0 ? -d->pct : d->pct, up ? "+" : down ? "-" : "", b2);
    set_txt(st_chg, b); set_col(st_chg, c);
    /* 장중 그래프: 9:00~15:30 를 350px 에, 위아래는 값 범위 + 전일 종가 */
    if (d->np > 1) {
        long lo = d->prev, hi = d->prev;
        for (int i = 0; i < d->np; i++) { if (d->py[i] < lo) lo = d->py[i]; if (d->py[i] > hi) hi = d->py[i]; }
        if (hi == lo) hi = lo + 1;
        for (int i = 0; i < d->np; i++) {
            int x = (d->px[i] - 540) * 350 / 390; if (x < 0) x = 0; if (x > 350) x = 350;
            st_pts[i].x = x; st_pts[i].y = 8 + (int)((hi - d->py[i]) * 80 / (hi - lo));
        }
        lv_line_set_points(st_chart, st_pts, d->np);
        int by = 8 + (int)((hi - d->prev) * 80 / (hi - lo));
        st_bpts[0].x = 0; st_bpts[0].y = by; st_bpts[1].x = 350; st_bpts[1].y = by; lv_line_set_points(st_base, st_bpts, 2);
        lv_obj_set_style_line_color(st_chart, lv_color_hex(c == 0xFFFFFF ? 0xC8CCD4 : c), 0);
        lv_obj_remove_flag(st_chart, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(st_chart, LV_OBJ_FLAG_HIDDEN);
    long fv[3] = { d->fi, d->ff, d->fo };
    for (int i = 0; i < 3; i++) {
        if (!d->flow) { set_txt(st_fv[i], "-"); continue; }
        long a = labs(fv[i]);
        if (a >= 10000) snprintf(b, sizeof b, "%s%.1f만", fv[i] > 0 ? "▲" : fv[i] < 0 ? "▼" : "", a / 10000.0);
        else { comma(a, b2, sizeof b2); snprintf(b, sizeof b, "%s%s", fv[i] > 0 ? "▲" : fv[i] < 0 ? "▼" : "", b2); }
        set_txt(st_fv[i], b); set_col(st_fv[i], fv[i] > 0 ? UP : fv[i] < 0 ? DOWN : 0xFFFFFF);
    }
    comma(d->hi, b2, sizeof b2); snprintf(b, sizeof b, "고 %s · 저 ", b2); size_t n = strlen(b); comma(d->lo, b + n, sizeof b - n); set_txt(st_hl, b);
}
static void render_weather(const home_data_t *d)
{
    if (!d->wx) return;
    bool night = strstr(d->icon, "moon") || strstr(d->icon, "night");
    uint32_t bg = night ? 0x0A1426 : 0x1E3A66;
    tint(s_tile[1], bg);
    draw_icon(wx_icon, d->icon, 80, bg);
    char b[64]; snprintf(b, sizeof b, "%d°", d->temp); set_txt(wx_temp, b);
    snprintf(b, sizeof b, "%s · 체감 %d°", d->desc, d->feels); set_txt(wx_desc, b);
    snprintf(b, sizeof b, "↑%d°  ↓%d°  · 비 %d%%", d->hi_t, d->lo_t, d->rain); set_txt(wx_hl, b);
    for (int i = 0; i < 4; i++) {
        if (i >= d->nh) { lv_obj_add_flag(wx_hc[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(wx_hc[i], LV_OBJ_FLAG_HIDDEN);
        set_txt(wx_ht[i], d->h[i].t); draw_icon(wx_hi[i], d->h[i].icon, 28, bg);
        snprintf(b, sizeof b, "%d°", d->h[i].temp); set_txt(wx_hv[i], b);
    }
    snprintf(b, sizeof b, "해 지는 시간 %s", d->sunset); set_txt(wx_sun, b);
}
static void hm12(const char *at, char *o, size_t n)      /* "14:39" → "2:39" */
{
    int h = atoi(at), m = atoi(strchr(at, ':') ? strchr(at, ':') + 1 : "0");
    snprintf(o, n, "%d:%02d", h % 12 ? h % 12 : 12, m);
}
static void render_bus(const home_data_t *d)
{
    tint(s_tile[2], 0x062A1F);
    if (!d->bus || d->nb == 0) { set_txt(bs_state, "버스 정보 없음"); set_txt(bs_cnt, "-:--"); return; }
    int el = (int)((esp_timer_get_time() - d->at_us) / 1000000), walk = d->walk * 60, k = -1;
    for (int i = 0; i < d->nb; i++) if (d->b[i].in - el - walk >= -30) { k = i; break; }
    if (k < 0) { set_txt(bs_state, "다음 버스를 기다리는 중"); set_txt(bs_cnt, "-:--"); return; }
    int leave = d->b[k].in - el - walk; if (leave < 0) leave = 0;
    uint32_t c = leave > 300 ? 0x2FD49A : leave > 0 ? 0xFFA94D : 0xFF5C5C;
    set_txt(bs_state, leave > 600 ? "여유 있어요" : leave > 300 ? "곧 준비하세요" : leave > 60 ? "지금 준비하세요" : "지금 나가세요!");
    set_col(bs_state, c);
    char b[48]; snprintf(b, sizeof b, "%d:%02d", leave / 60, leave % 60); set_txt(bs_cnt, b);
    lv_arc_set_value(bs_ring, leave > 1200 ? 1200 : leave);
    lv_obj_set_style_arc_color(bs_ring, lv_color_hex(c), LV_PART_INDICATOR);
    snprintf(b, sizeof b, "나가야 할 때까지 (정류장까지 %d분)", d->walk); set_txt(bs_sub, b);
    for (int i = 0; i < 2; i++) {
        int j = k + i;
        if (j >= d->nb) { lv_obj_add_flag(bs_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(bs_row[i], LV_OBJ_FLAG_HIDDEN);
        char t[12]; hm12(d->b[j].at, t, sizeof t); set_txt(bs_rt[i], t);
        int mins = (d->b[j].in - el) / 60; if (mins < 0) mins = 0;
        snprintf(b, sizeof b, mins ? "%d분 뒤" : "곧 도착", mins); set_txt(bs_rm[i], b);
    }
    set_txt(bs_foot, d->b[k].live ? "● 실시간 위치 기준" : "○ 시간표 기준");
}
static void render_cal(const home_data_t *d)
{
    tint(s_tile[3], 0x22122A);
    if (!d->cal) return;
    set_txt(cl_dow, d->dow);
    char b[192]; snprintf(b, sizeof b, "%d", d->day); set_txt(cl_day, b);
    if (d->ne) snprintf(b, sizeof b, "%d월 · 오늘 남은 일정 %d개%s", d->mon, d->ne + d->more, d->more ? "" : "");
    else snprintf(b, sizeof b, "%d월 · 오늘 남은 일정이 없어요", d->mon);
    set_txt(cl_sub, b);
    for (int i = 0; i < 3; i++) {
        if (i >= d->ne) { lv_obj_add_flag(cl_row[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_remove_flag(cl_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(cl_row[i], lv_color_hex(d->e[i].color), 0);
        lv_obj_set_style_bg_color(cl_bar[i], lv_color_hex(d->e[i].color), 0);
        set_txt(cl_t[i], d->e[i].title);
        snprintf(b, sizeof b, "%s · %s", d->e[i].time, d->e[i].who); set_txt(cl_s[i], b);
    }
    if (d->tm) snprintf(b, sizeof b, "내일 · %s %s", d->tm_title, d->tm_time); else b[0] = 0;
    lv_obj_set_y(cl_tm, 188 + (d->ne ? d->ne : 1) * 60 + 8);
    set_txt(cl_tm, b);
}
static void render_all(void)
{
    for (int i = 0; i < NT; i++) {
        const char *m = !s_key[0] ? "열쇠를 넣어 주세요\n(굽기 페이지의 home_key)" : !s_v->ok ? (s_v->err[0] ? s_v->err : "불러오는 중…") : "";
        set_txt(s_msg[i], m);
        if (m[0]) lv_obj_remove_flag(s_msg[i], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_msg[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_v->ok) return;
    render_stock(s_v); render_weather(s_v); render_bus(s_v); render_cal(s_v);
}

/* ── 받기 ── */
static void cp(char *dst, size_t n, const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItem(o, k); snprintf(dst, n, "%s", cJSON_IsString(v) ? v->valuestring : ""); }
static long num(const cJSON *o, const char *k) { const cJSON *v = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(v) ? (long)v->valuedouble : 0; }
static uint32_t hexcol(const char *s) { return (uint32_t)strtoul(s[0] == '#' ? s + 1 : s, NULL, 16); }
static void parse(const char *body, home_data_t *d)
{
    memset(d, 0, sizeof *d);
    cJSON *j = cJSON_Parse(body);
    if (!j) { snprintf(d->err, sizeof d->err, "데이터를 읽지 못했어요"); return; }
    const cJSON *s = cJSON_GetObjectItem(j, "stock");
    if (cJSON_IsObject(s)) {
        d->stock = true; cp(d->name, sizeof d->name, s, "name"); cp(d->status, sizeof d->status, s, "status"); cp(d->cd, sizeof d->cd, s, "cd");
        d->price = num(s, "price"); d->chg = num(s, "chg"); d->hi = num(s, "hi"); d->lo = num(s, "lo"); d->prev = num(s, "prev");
        const cJSON *p = cJSON_GetObjectItem(s, "pct"); d->pct = cJSON_IsNumber(p) ? p->valuedouble : 0;
        const cJSON *pts = cJSON_GetObjectItem(s, "pts"), *it;
        cJSON_ArrayForEach(it, pts) {
            if (d->np >= 64 || cJSON_GetArraySize(it) < 2) break;
            d->px[d->np] = cJSON_GetArrayItem(it, 0)->valueint; d->py[d->np] = (long)cJSON_GetArrayItem(it, 1)->valuedouble; d->np++; }
        const cJSON *f = cJSON_GetObjectItem(s, "flow");
        if (cJSON_IsObject(f)) { d->flow = true; d->fi = num(f, "i"); d->ff = num(f, "f"); d->fo = num(f, "o"); }
    }
    const cJSON *w = cJSON_GetObjectItem(j, "wx");
    if (cJSON_IsObject(w)) {
        d->wx = true; d->temp = num(w, "temp"); d->feels = num(w, "feels"); d->hi_t = num(w, "hi"); d->lo_t = num(w, "lo"); d->rain = num(w, "rain");
        cp(d->desc, sizeof d->desc, w, "desc"); cp(d->icon, sizeof d->icon, w, "icon"); cp(d->sunset, sizeof d->sunset, w, "sunset");
        const cJSON *it; cJSON_ArrayForEach(it, cJSON_GetObjectItem(w, "hourly")) {
            if (d->nh >= 4) break;
            hour_t *h = &d->h[d->nh++]; cp(h->t, sizeof h->t, it, "t"); cp(h->icon, sizeof h->icon, it, "icon"); h->temp = num(it, "temp");
        }
    }
    const cJSON *b = cJSON_GetObjectItem(j, "bus");
    if (cJSON_IsObject(b)) {
        d->bus = true; cp(d->route, sizeof d->route, b, "route"); d->walk = num(b, "walk"); d->at_us = esp_timer_get_time();
        const cJSON *it; cJSON_ArrayForEach(it, cJSON_GetObjectItem(b, "next")) {
            if (d->nb >= 3) break;
            bus_t *x = &d->b[d->nb++]; x->in = num(it, "in"); cp(x->at, sizeof x->at, it, "at"); x->live = cJSON_IsTrue(cJSON_GetObjectItem(it, "live"));
        }
    }
    const cJSON *c = cJSON_GetObjectItem(j, "cal");
    if (cJSON_IsObject(c)) {
        d->cal = true; cp(d->dow, sizeof d->dow, c, "dow"); d->day = num(c, "day"); d->mon = num(c, "mon"); d->more = num(c, "more");
        const cJSON *it; cJSON_ArrayForEach(it, cJSON_GetObjectItem(c, "today")) {
            if (d->ne >= 3) break;
            ev_t *e = &d->e[d->ne++]; cp(e->title, sizeof e->title, it, "title"); cp(e->time, sizeof e->time, it, "time"); cp(e->who, sizeof e->who, it, "who");
            char col[12]; cp(col, sizeof col, it, "color"); e->color = col[0] ? hexcol(col) : 0x6C8CFF;
        }
        const cJSON *tm = cJSON_GetObjectItem(c, "tomorrow");
        if (cJSON_IsObject(tm)) { d->tm = true; cp(d->tm_title, sizeof d->tm_title, tm, "title"); cp(d->tm_time, sizeof d->tm_time, tm, "time"); }
    }
    d->ok = d->stock || d->wx || d->bus || d->cal;
    cJSON_Delete(j);
}
static void fetch_once(void)
{
    if (!s_key[0]) return;
    char url[320]; snprintf(url, sizeof url, "%s/api/gadget?k=%s", s_url, s_key);
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 15000, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return;
    const int cap = 12 * 1024; char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM); int len = 0, status = 0;
    home_data_t *tmp = heap_caps_calloc(1, sizeof(home_data_t), MALLOC_CAP_SPIRAM);
    if (buf && tmp && esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h); status = esp_http_client_get_status_code(h);
        int r; while (len < cap - 1 && (r = esp_http_client_read(h, buf + len, cap - 1 - len)) > 0) len += r;
        buf[len] = 0;
        if (status == 200) parse(buf, tmp);
        else snprintf(tmp->err, sizeof tmp->err, status == 403 ? "열쇠가 맞지 않아요" : "서버 응답 %d", status);
    } else if (tmp) snprintf(tmp->err, sizeof tmp->err, "인터넷 연결을 기다리는 중");
    esp_http_client_close(h); esp_http_client_cleanup(h);
    if (tmp) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        if (tmp->ok || !s_d->ok) memcpy(s_d, tmp, sizeof *tmp);   /* 실패해도 이전 값은 유지 */
        else snprintf(s_d->err, sizeof s_d->err, "%s", tmp->err);
        s_ver++;
        xSemaphoreGive(s_mtx);
        if (tmp->ok) s_last_ok_us = esp_timer_get_time();
        ESP_LOGI(TAG, "gadget %d, %d bytes, %s", status, len, tmp->ok ? "ok" : tmp->err);
    }
    free(buf); free(tmp);
}
static void task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
        if (s_viewing) fetch_once();                        /* 보고 있을 때만 (배터리) */
    }
}

/* ── 설정 저장 (NVS "home") ── */
static void load_cfg(void)
{
    snprintf(s_url, sizeof s_url, "%s", CONFIG_HOME_TILES_URL);
    nvs_handle_t n;
    if (nvs_open("home", NVS_READONLY, &n) == ESP_OK) {
        size_t l = sizeof s_key; if (nvs_get_str(n, "key", s_key, &l) != ESP_OK) s_key[0] = 0;
        l = sizeof s_url; if (nvs_get_str(n, "url", s_url, &l) != ESP_OK) snprintf(s_url, sizeof s_url, "%s", CONFIG_HOME_TILES_URL);
        nvs_close(n);
    }
}
static bool save_str(const char *k, const char *v)
{
    nvs_handle_t n; if (nvs_open("home", NVS_READWRITE, &n) != ESP_OK) return false;
    bool ok = nvs_set_str(n, k, v) == ESP_OK && nvs_commit(n) == ESP_OK; nvs_close(n); return ok;
}
bool home_tiles_console(const char *line)
{
    if (!strncmp(line, "home_key=", 9)) {
        const char *v = line + 9; size_t l = strlen(v);
        if (l < 16 || l >= sizeof s_key) { printf("@home_key error length\n"); fflush(stdout); return true; }
        if (!save_str("key", v)) { printf("@home_key error storage\n"); fflush(stdout); return true; }
        snprintf(s_key, sizeof s_key, "%s", v);
        printf("@home_key ok\n"); fflush(stdout);
        if (s_task) xTaskNotifyGive(s_task);
        return true;
    }
    if (!strncmp(line, "home_url=", 9)) {
        const char *v = line + 9;
        if (strncmp(v, "https://", 8) || strlen(v) >= sizeof s_url || !save_str("url", v)) { printf("@home_url error\n"); fflush(stdout); return true; }
        snprintf(s_url, sizeof s_url, "%s", v); printf("@home_url ok\n"); fflush(stdout); return true;
    }
    if (!strcmp(line, "home")) {
        printf("@home {\"key\":%s,\"url\":\"%s\",\"ok\":%s}\n", s_key[0] ? "true" : "false", s_url, s_last_ok_us ? "true" : "false"); fflush(stdout);
        if (s_task) { s_viewing = true; xTaskNotifyGive(s_task); }
        return true;
    }
    return false;
}

/* ── 타일 바뀜·자동 복귀·주기 갱신 ── */
static void tile_cb(lv_event_t *e)
{
    lv_obj_t *a = lv_tileview_get_tile_active(s_tv);
    bool ours = false; for (int i = 0; i < NT; i++) if (a == s_tile[i]) ours = true;
    s_viewing = ours;
    if (ours && s_task && (esp_timer_get_time() - s_last_ok_us > 50LL * 1000000 || !s_last_ok_us)) xTaskNotifyGive(s_task);
}
static void ui_timer(lv_timer_t *t)
{
    if (s_ver != s_shown) {
        xSemaphoreTake(s_mtx, portMAX_DELAY); memcpy(s_v, s_d, sizeof *s_v); s_shown = s_ver; xSemaphoreGive(s_mtx);
        render_all();
    } else if (s_viewing && s_v->ok) render_bus(s_v);       /* 1초마다 버스 타이머 */
    if (s_viewing && lv_display_get_inactive_time(NULL) > CONFIG_HOME_TILES_IDLE_S * 1000)
        lv_tileview_set_tile(s_tv, s_face, LV_ANIM_ON);
}

void home_tiles_create(lv_obj_t *tv, lv_obj_t *face)
{
    s_tv = tv; s_face = face;
    s_mtx = xSemaphoreCreateMutex();
    s_d = heap_caps_calloc(1, sizeof(home_data_t), MALLOC_CAP_SPIRAM);
    s_v = heap_caps_calloc(1, sizeof(home_data_t), MALLOC_CAP_SPIRAM);
    load_cfg();
    for (int i = 0; i < NT; i++) {
        s_tile[i] = new_tile(i + 1);
        switch (i) { case 0: build_stock(s_tile[i]); break; case 1: build_weather(s_tile[i]); break; case 2: build_bus(s_tile[i]); break; default: build_cal(s_tile[i]); }
        dots(s_tile[i], i + 1);
        s_msg[i] = lbl(s_tile[i], F20, 0xFFFFFF, "", 40, 200, W - 80, LV_TEXT_ALIGN_CENTER);
        lv_obj_set_style_bg_color(s_msg[i], lv_color_black(), 0); lv_obj_set_style_bg_opa(s_msg[i], 200, 0);
        lv_obj_set_style_pad_ver(s_msg[i], 14, 0); lv_obj_set_style_radius(s_msg[i], 16, 0);
    }
    render_all();
    lv_obj_add_event_cb(tv, tile_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_timer_create(ui_timer, 1000, NULL);
    xTaskCreatePinnedToCoreWithCaps(task, "home", 10 * 1024, NULL, 3, &s_task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "tiles ready, key %s", s_key[0] ? "set" : "missing");
}
