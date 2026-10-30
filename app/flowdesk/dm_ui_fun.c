/****************************************************************************
 * dm_ui_fun.c — What-to-eat / 24 / Draw / BMI
 ****************************************************************************/

#include "deskmate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#ifdef CONFIG_DESKMATE_APP

#define DRAW_W 240
#define DRAW_H 168

static void fun_back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_FEATURES);
}

static lv_obj_t *mk_fun_page(dm_page_t id, const char *zh, const char *en)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_t *title;
  lv_obj_t *back;

  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  back = dm_btn(page, "←", "<", 36, 24, C_BTN, C_MUTED, fun_back, NULL);
  lv_obj_set_pos(back, 8, 6);

  title = dm_lbl(page, zh, en, g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 50, 8);

  g_dm_pages[id] = page;
  return page;
}

/* ---------- 吃什么 ---------- */

static const char *const s_food[4][8] = {
  { "火锅", "烧烤", "麻辣烫", "黄焖鸡", "兰州拉面", "沙县小吃", "炸鸡",
    "汉堡" },
  { "寿司", "披萨", "牛排", "咖喱", "拌饭", "轻食沙拉", "意面",
    "日式拉面" },
  { "盖浇饭", "米线", "螺蛳粉", "酸菜鱼", "小龙虾", "烤鱼", "冒菜",
    "干锅" },
  { "饺子", "包子", "煎饼果子", "肉夹馍", "凉皮", "炒饭", "炒面",
    "煲仔饭" },
};

static lv_obj_t *s_eat_out;
static lv_obj_t *s_eat_sub;
static lv_timer_t *s_eat_timer;
static int s_eat_set;
static int s_eat_idx;
static bool s_eat_spin;

static void eat_paint_sub(const char *zh)
{
  if (s_eat_sub)
    {
      lv_label_set_text(s_eat_sub, zh);
    }
}

static void eat_stop(void)
{
  s_eat_spin = false;
  if (s_eat_timer)
    {
      lv_timer_pause(s_eat_timer);
    }
}

static void eat_tick_cb(lv_timer_t *t)
{
  (void)t;
  if (!s_eat_out)
    {
      return;
    }
  s_eat_idx = (s_eat_idx + 1) % 8;
  lv_label_set_text(s_eat_out, s_food[s_eat_set][s_eat_idx]);
}

static void eat_go_cb(lv_event_t *e)
{
  (void)e;
  if (s_eat_spin)
    {
      eat_stop();
      eat_paint_sub("就吃这个！");
      return;
    }
  s_eat_spin = true;
  eat_paint_sub("转转转…再点一次停");
  if (s_eat_timer)
    {
      lv_timer_resume(s_eat_timer);
    }
}

static void eat_swap_cb(lv_event_t *e)
{
  (void)e;
  eat_stop();
  s_eat_set = (s_eat_set + 1) % 4;
  if (s_eat_out)
    {
      lv_label_set_text(s_eat_out, "？");
    }
  eat_paint_sub("已换一组，点开始");
}

void dm_create_eat(void)
{
  lv_obj_t *page = mk_fun_page(PAGE_EAT, "吃什么", "Eat");
  lv_obj_t *b;

  s_eat_out = dm_lbl(page, "？", "?", g_dm_font_xl, C_STAR);
  lv_obj_align(s_eat_out, LV_ALIGN_TOP_MID, 0, 48);

  s_eat_sub = dm_lbl(page, "点「开始」让我帮你选一顿", "Tap start",
                     g_dm_font_s, C_MUTED);
  lv_obj_align(s_eat_sub, LV_ALIGN_TOP_MID, 0, 108);

  b = dm_btn(page, "开始", "Start", 140, 36, C_STAR, C_EYE, eat_go_cb,
             NULL);
  lv_obj_set_pos(b, 16, 150);
  b = dm_btn(page, "换一组", "Next set", 140, 36, C_BTN, C_INK, eat_swap_cb,
             NULL);
  lv_obj_set_pos(b, 164, 150);

  s_eat_timer = lv_timer_create(eat_tick_cb, 80, NULL);
  lv_timer_pause(s_eat_timer);
  s_eat_set = 0;
  s_eat_idx = 0;
  s_eat_spin = false;
}

/* ---------- 24 点 ---------- */

typedef struct
{
  double v;
  char s[48];
} n24_t;

static int s_n4[4];
static lv_obj_t *s_n4_lbl[4];
static lv_obj_t *s_n4_hint;

static void n24_fmt(double v, char *out, size_t n)
{
  if (fabs(v - floor(v + 0.5)) < 1e-9 && fabs(v) < 1e9)
    {
      snprintf(out, n, "%d", (int)floor(v + 0.5));
    }
  else
    {
      snprintf(out, n, "%.4g", v);
    }
}

static int n24_try(const n24_t *a, int n, char *out, size_t outn)
{
  int i;
  int j;
  int k;
  n24_t next[4];
  static const char ops[4] = { '+', '-', '*', '/' };

  if (n == 1)
    {
      if (fabs(a[0].v - 24.0) < 1e-6)
        {
          snprintf(out, outn, "%s", a[0].s);
          return 1;
        }
      return 0;
    }

  for (i = 0; i < n; i++)
    {
      for (j = 0; j < n; j++)
        {
          if (i == j)
            {
              continue;
            }
          for (k = 0; k < 4; k++)
            {
              double r;
              int m = 0;
              int p;
              char tmp[48];

              if (ops[k] == '+')
                {
                  r = a[i].v + a[j].v;
                }
              else if (ops[k] == '-')
                {
                  r = a[i].v - a[j].v;
                }
              else if (ops[k] == '*')
                {
                  r = a[i].v * a[j].v;
                }
              else
                {
                  if (fabs(a[j].v) < 1e-9)
                    {
                      continue;
                    }
                  r = a[i].v / a[j].v;
                }
              snprintf(tmp, sizeof(tmp), "(%s%c%s)", a[i].s, ops[k],
                       a[j].s);
              for (p = 0; p < n; p++)
                {
                  if (p != i && p != j)
                    {
                      next[m++] = a[p];
                    }
                }
              n24_fmt(r, next[m].s, sizeof(next[m].s));
              /* keep expression not just number for leaf display */
              snprintf(next[m].s, sizeof(next[m].s), "%s", tmp);
              next[m].v = r;
              if (n24_try(next, m + 1, out, outn))
                {
                  return 1;
                }
            }
        }
    }
  return 0;
}

static void n24_deal(void)
{
  char buf[8];
  int i;
  for (i = 0; i < 4; i++)
    {
      s_n4[i] = 1 + (int)(rand() % 13);
      snprintf(buf, sizeof(buf), "%d", s_n4[i]);
      if (s_n4_lbl[i])
        {
          lv_label_set_text(s_n4_lbl[i], buf);
        }
    }
  if (s_n4_hint)
    {
      lv_label_set_text(s_n4_hint, "用 + − × ÷ 把四个数凑成 24");
    }
}

static void n24_next_cb(lv_event_t *e)
{
  (void)e;
  n24_deal();
}

static void n24_hint_cb(lv_event_t *e)
{
  n24_t a[4];
  char out[96];
  int i;
  (void)e;

  for (i = 0; i < 4; i++)
    {
      a[i].v = (double)s_n4[i];
      snprintf(a[i].s, sizeof(a[i].s), "%d", s_n4[i]);
    }
  out[0] = 0;
  if (n24_try(a, 4, out, sizeof(out)))
    {
      if (s_n4_hint)
        {
          lv_label_set_text(s_n4_hint, out);
        }
    }
  else if (s_n4_hint)
    {
      lv_label_set_text(s_n4_hint, "这组无解，下一局吧");
    }
}

void dm_create_24(void)
{
  lv_obj_t *page = mk_fun_page(PAGE_24, "24 点", "24");
  lv_obj_t *card;
  lv_obj_t *b;
  int i;

  for (i = 0; i < 4; i++)
    {
      card = lv_obj_create(page);
      lv_obj_set_size(card, 64, 64);
      lv_obj_set_pos(card, 16 + i * 72, 48);
      lv_obj_set_style_bg_color(card, lv_color_hex(C_BTN), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(card, 12, LV_PART_MAIN);
      lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
      lv_obj_set_style_border_color(card, lv_color_hex(0x1c1c1c),
                                    LV_PART_MAIN);
      lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
      s_n4_lbl[i] = dm_lbl(card, "?", "?", g_dm_font_l, C_ACCENT);
      lv_obj_center(s_n4_lbl[i]);
    }

  s_n4_hint = dm_lbl(page, "用 + − × ÷ 把四个数凑成 24", "Make 24",
                     g_dm_font_s, C_STAR);
  lv_label_set_long_mode(s_n4_hint, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_n4_hint, 296);
  lv_obj_set_style_text_align(s_n4_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(s_n4_hint, 12, 128);

  b = dm_btn(page, "提示", "Hint", 140, 32, 0x0d3a4a, C_ACCENT, n24_hint_cb,
             NULL);
  lv_obj_set_pos(b, 16, 180);
  b = dm_btn(page, "下一局", "Next", 140, 32, C_OK, C_EYE, n24_next_cb,
             NULL);
  lv_obj_set_pos(b, 164, 180);

  n24_deal();
}

/* ---------- 画板 ---------- */

static lv_obj_t *s_draw_cv;
static lv_color_t *s_draw_buf;
static int s_draw_w = 2;
static uint32_t s_draw_col = 0x7DD3FC;
static bool s_draw_down;
static lv_point_t s_draw_last;

static void draw_clear_buf(void)
{
  if (!s_draw_buf)
    {
      return;
    }
  lv_canvas_fill_bg(s_draw_cv, lv_color_hex(0x0a0a0a), LV_OPA_COVER);
}

static void draw_dot(int x, int y)
{
  int dx;
  int dy;
  int r = s_draw_w;
  if (!s_draw_cv || !s_draw_buf)
    {
      return;
    }
  if (x < 0 || y < 0 || x >= DRAW_W || y >= DRAW_H)
    {
      return;
    }
  for (dy = -r; dy <= r; dy++)
    {
      for (dx = -r; dx <= r; dx++)
        {
          int px = x + dx;
          int py = y + dy;
          if (px < 0 || py < 0 || px >= DRAW_W || py >= DRAW_H)
            {
              continue;
            }
          if (dx * dx + dy * dy <= r * r + 1)
            {
              lv_canvas_set_px(s_draw_cv, px, py, lv_color_hex(s_draw_col),
                               LV_OPA_COVER);
            }
        }
    }
}

static void draw_line(int x0, int y0, int x1, int y1)
{
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  for (;;)
    {
      draw_dot(x0, y0);
      if (x0 == x1 && y0 == y1)
        {
          break;
        }
      {
        int e2 = 2 * err;
        if (e2 > -dy)
          {
            err -= dy;
            x0 += sx;
          }
        if (e2 < dx)
          {
            err += dx;
            y0 += sy;
          }
        }
    }
}

static void draw_get_local(lv_event_t *e, int *ox, int *oy)
{
  lv_indev_t *indev = lv_indev_active();
  lv_point_t p;
  lv_area_t a;
  lv_obj_get_coords(s_draw_cv, &a);
  lv_indev_get_point(indev, &p);
  *ox = p.x - a.x1;
  *oy = p.y - a.y1;
}

static void draw_press_cb(lv_event_t *e)
{
  int x;
  int y;
  s_draw_down = true;
  draw_get_local(e, &x, &y);
  s_draw_last.x = (lv_coord_t)x;
  s_draw_last.y = (lv_coord_t)y;
  draw_dot(x, y);
}

static void draw_move_cb(lv_event_t *e)
{
  int x;
  int y;
  if (!s_draw_down)
    {
      return;
    }
  draw_get_local(e, &x, &y);
  draw_line(s_draw_last.x, s_draw_last.y, x, y);
  s_draw_last.x = (lv_coord_t)x;
  s_draw_last.y = (lv_coord_t)y;
}

static void draw_rel_cb(lv_event_t *e)
{
  (void)e;
  s_draw_down = false;
}

static void draw_clear_cb(lv_event_t *e)
{
  (void)e;
  draw_clear_buf();
  lv_obj_invalidate(s_draw_cv);
}

static void draw_w_cb(lv_event_t *e)
{
  s_draw_w = (int)(intptr_t)lv_event_get_user_data(e);
}

static void draw_col_cb(lv_event_t *e)
{
  s_draw_col = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
}

void dm_create_draw(void)
{
  lv_obj_t *page = mk_fun_page(PAGE_DRAW, "画板涂鸦", "Draw");
  lv_obj_t *b;
  size_t sz = DRAW_W * DRAW_H * sizeof(lv_color_t);
  static const uint32_t cols[8] = {
    0xFFFFFF, 0xFF6B8A, 0xFFD54F, 0x6BCB77,
    0x7DD3FC, 0xB388FF, 0xFF8A3D, 0x4DD0E1,
  };
  int i;

  s_draw_buf = (lv_color_t *)lv_malloc(sz);
  s_draw_cv = lv_canvas_create(page);
  lv_obj_set_pos(s_draw_cv, 6, 34);
  lv_canvas_set_buffer(s_draw_cv, s_draw_buf, DRAW_W, DRAW_H,
                       LV_COLOR_FORMAT_RGB565);
  draw_clear_buf();
  lv_obj_add_flag(s_draw_cv, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_draw_cv, draw_press_cb, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(s_draw_cv, draw_move_cb, LV_EVENT_PRESSING, NULL);
  lv_obj_add_event_cb(s_draw_cv, draw_rel_cb, LV_EVENT_RELEASED, NULL);

  /* tools on the RIGHT so they don't cover the back button */
  b = dm_btn(page, "清屏", "Clr", 60, 22, C_BTN, C_INK, draw_clear_cb,
             NULL);
  lv_obj_set_pos(b, 252, 36);
  b = dm_btn(page, "细", "S", 60, 22, 0x0d3a4a, C_ACCENT, draw_w_cb,
             (void *)(intptr_t)1);
  lv_obj_set_pos(b, 252, 62);
  b = dm_btn(page, "中", "M", 60, 22, 0x0d3a4a, C_ACCENT, draw_w_cb,
             (void *)(intptr_t)2);
  lv_obj_set_pos(b, 252, 88);
  b = dm_btn(page, "粗", "L", 60, 22, 0x0d3a4a, C_ACCENT, draw_w_cb,
             (void *)(intptr_t)3);
  lv_obj_set_pos(b, 252, 114);

  /* palette bottom */
  for (i = 0; i < 8; i++)
    {
      b = dm_btn(page, " ", " ", 32, 22, cols[i], cols[i], draw_col_cb,
                 (void *)(uintptr_t)cols[i]);
      lv_obj_set_pos(b, 8 + i * 38, 206);
      lv_obj_set_style_radius(b, 11, LV_PART_MAIN);
    }
}

/* ---------- BMI ---------- */

static int s_bmi_h = 170;
static int s_bmi_w = 60;
static lv_obj_t *s_bmi_val;
static lv_obj_t *s_bmi_tag;
static lv_obj_t *s_bmi_h_l;
static lv_obj_t *s_bmi_w_l;

static void bmi_paint(void)
{
  char buf[16];
  double m = s_bmi_h / 100.0;
  double bmi = s_bmi_w / (m * m);
  const char *tag = "正常";
  uint32_t c = C_OK;

  if (bmi < 18.5)
    {
      tag = "偏瘦";
      c = C_ACCENT;
    }
  else if (bmi < 24.0)
    {
      tag = "正常";
      c = C_OK;
    }
  else if (bmi < 28.0)
    {
      tag = "偏胖";
      c = C_STAR;
    }
  else
    {
      tag = "肥胖";
      c = 0xFF6B8A;
    }

  snprintf(buf, sizeof(buf), "%.1f", bmi);
  if (s_bmi_val)
    {
      lv_label_set_text(s_bmi_val, buf);
    }
  if (s_bmi_tag)
    {
      lv_label_set_text(s_bmi_tag, tag);
      lv_obj_set_style_text_color(s_bmi_tag, lv_color_hex(c), LV_PART_MAIN);
    }
  snprintf(buf, sizeof(buf), "%d cm", s_bmi_h);
  if (s_bmi_h_l)
    {
      lv_label_set_text(s_bmi_h_l, buf);
    }
  snprintf(buf, sizeof(buf), "%d kg", s_bmi_w);
  if (s_bmi_w_l)
    {
      lv_label_set_text(s_bmi_w_l, buf);
    }
}

static void bmi_adj_cb(lv_event_t *e)
{
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  if (d > 0)
    {
      s_bmi_h = s_bmi_h < 220 ? s_bmi_h + 1 : 220;
    }
  else
    {
      s_bmi_h = s_bmi_h > 100 ? s_bmi_h - 1 : 100;
    }
  bmi_paint();
}

static void bmi_wadj_cb(lv_event_t *e)
{
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  if (d > 0)
    {
      s_bmi_w = s_bmi_w < 200 ? s_bmi_w + 1 : 200;
    }
  else
    {
      s_bmi_w = s_bmi_w > 20 ? s_bmi_w - 1 : 20;
    }
  bmi_paint();
}

void dm_create_bmi(void)
{
  lv_obj_t *page = mk_fun_page(PAGE_BMI, "BMI 计算", "BMI");
  lv_obj_t *b;
  lv_obj_t *row;
  lv_obj_t *lab;

  s_bmi_val = dm_lbl(page, "22.0", "22.0", g_dm_font_xl, C_ACCENT);
  lv_obj_align(s_bmi_val, LV_ALIGN_TOP_MID, 0, 40);

  s_bmi_tag = dm_lbl(page, "正常", "OK", g_dm_font_m, C_OK);
  lv_obj_align(s_bmi_tag, LV_ALIGN_TOP_MID, 0, 92);

  row = lv_obj_create(page);
  lv_obj_set_size(row, 304, 44);
  lv_obj_set_pos(row, 8, 124);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lab = dm_lbl(row, "身高", "Height", g_dm_font_s, C_DIM);
  lv_obj_set_pos(lab, 10, 14);
  s_bmi_h_l = dm_lbl(row, "170 cm", "170 cm", g_dm_font_s, C_INK);
  lv_obj_set_pos(s_bmi_h_l, 70, 14);
  b = dm_btn(row, "−", "-", 28, 24, 0x222222, C_INK, bmi_adj_cb,
             (void *)(intptr_t)-1);
  lv_obj_set_pos(b, 190, 10);
  b = dm_btn(row, "+", "+", 28, 24, 0x222222, C_INK, bmi_adj_cb,
             (void *)(intptr_t)1);
  lv_obj_set_pos(b, 226, 10);

  row = lv_obj_create(page);
  lv_obj_set_size(row, 304, 44);
  lv_obj_set_pos(row, 8, 176);
  lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lab = dm_lbl(row, "体重", "Weight", g_dm_font_s, C_DIM);
  lv_obj_set_pos(lab, 10, 14);
  s_bmi_w_l = dm_lbl(row, "60 kg", "60 kg", g_dm_font_s, C_INK);
  lv_obj_set_pos(s_bmi_w_l, 70, 14);
  b = dm_btn(row, "−", "-", 28, 24, 0x222222, C_INK, bmi_wadj_cb,
             (void *)(intptr_t)-1);
  lv_obj_set_pos(b, 190, 10);
  b = dm_btn(row, "+", "+", 28, 24, 0x222222, C_INK, bmi_wadj_cb,
             (void *)(intptr_t)1);
  lv_obj_set_pos(b, 226, 10);

  bmi_paint();
}

/* ---------- entry ---------- */

void dm_create_fun(void)
{
  dm_create_eat();
  dm_create_24();
  dm_create_draw();
  dm_create_bmi();
  /* dm_create_2048() is called once in deskmate_main.c */
}

void dm_fun_tick(void)
{
}

#endif /* CONFIG_DESKMATE_APP */
