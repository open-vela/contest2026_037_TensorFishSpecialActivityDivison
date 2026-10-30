/****************************************************************************
 * dm_ui_2048.c — 2048 game (logic + layout fixed)
 *
 * Slide/merge uses the standard "consume pairs" algorithm (i += 2),
 * unit-tested for 2222→44, 2220→42, 8844→16+8, direction transforms.
 ****************************************************************************/

#include "deskmate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef CONFIG_DESKMATE_APP

#define G2048_N 4
#define TILE_PX 32
#define TILE_GAP 4
#define BOARD_PAD 6
#define BOARD_IN (G2048_N * TILE_PX + (G2048_N - 1) * TILE_GAP)
#define BOARD_W (BOARD_IN + BOARD_PAD * 2)
#define BOARD_X ((DM_SCR_W - BOARD_W) / 2)
#define BOARD_Y 42
#define BTN_Y 206
#define BEST_PATH "/data/deskmate_2048_best.txt"

static uint16_t s_bd[G2048_N][G2048_N];
static int s_score;
static int s_best;
static bool s_over;
static bool s_won;
static lv_obj_t *s_tiles[G2048_N][G2048_N];
static lv_obj_t *s_score_l;
static lv_obj_t *s_best_l;
static lv_obj_t *s_status_l;

static void g2048_back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_FEATURES);
}

static uint32_t tile_color(int v)
{
  switch (v)
    {
      case 0: return 0x1a1c20;
      case 2: return 0x2c3038;
      case 4: return 0x3d4450;
      case 8: return 0xf59e0b;
      case 16: return 0xf97316;
      case 32: return 0xef4444;
      case 64: return 0xe11d48;
      case 128: return 0xfbbf24;
      case 256: return 0xf59e0b;
      case 512: return 0xd97706;
      case 1024: return 0xa78bfa;
      case 2048: return C_STAR;
      default: return C_ACCENT;
    }
}

static uint32_t tile_fg(int v)
{
  return (v >= 8) ? 0x111111 : C_INK;
}

static const lv_font_t *tile_font(int v)
{
  return (v >= 1024) ? g_dm_font_s : g_dm_font_m;
}

static void load_best(void)
{
  FILE *f = fopen(BEST_PATH, "r");
  if (f)
    {
      if (fscanf(f, "%d", &s_best) != 1)
        {
          s_best = 0;
        }
      fclose(f);
    }
}

static void save_best(void)
{
  FILE *f = fopen(BEST_PATH, "w");
  if (f)
    {
      fprintf(f, "%d\n", s_best);
      fclose(f);
    }
}

static void paint_cell(int r, int c)
{
  char buf[8];
  int v = s_bd[r][c];
  lv_obj_t *t = s_tiles[r][c];

  if (!t)
    {
      return;
    }

  lv_obj_set_style_bg_color(t, lv_color_hex(tile_color(v)), LV_PART_MAIN);
  if (v)
    {
      snprintf(buf, sizeof(buf), "%d", v);
    }
  else
    {
      buf[0] = '\0';
    }
  lv_label_set_text(t, buf);
  lv_obj_set_style_text_color(t, lv_color_hex(tile_fg(v)), LV_PART_MAIN);
  lv_obj_set_style_text_font(t, tile_font(v), LV_PART_MAIN);
}

static void paint_all(void)
{
  int r;
  int c;

  for (r = 0; r < G2048_N; r++)
    {
      for (c = 0; c < G2048_N; c++)
        {
          paint_cell(r, c);
        }
    }

  if (s_score_l)
    {
      char b[32];
      snprintf(b, sizeof(b), "%d", s_score);
      lv_label_set_text(s_score_l, b);
    }
  if (s_best_l)
    {
      char b[32];
      snprintf(b, sizeof(b), "%d", s_best);
      lv_label_set_text(s_best_l, b);
    }
  if (s_status_l)
    {
      if (s_over)
        {
          lv_label_set_text(s_status_l,
                            dm_t("游戏结束 · 点「新局」", "Over · New"));
          lv_obj_set_style_text_color(s_status_l, lv_color_hex(C_HEART),
                                      LV_PART_MAIN);
        }
      else if (s_won)
        {
          lv_label_set_text(s_status_l,
                            dm_t("达成 2048，可继续！", "2048! Keep going"));
          lv_obj_set_style_text_color(s_status_l, lv_color_hex(C_STAR),
                                      LV_PART_MAIN);
        }
      else
        {
          lv_label_set_text(s_status_l,
                            dm_t("滑动合并，或用方向键", "Swipe or arrow keys"));
          lv_obj_set_style_text_color(s_status_l, lv_color_hex(C_DIM),
                                      LV_PART_MAIN);
        }
    }
}

static void spawn(void)
{
  int empty[G2048_N * G2048_N];
  int n = 0;
  int r;
  int c;
  int pick;

  for (r = 0; r < G2048_N; r++)
    {
      for (c = 0; c < G2048_N; c++)
        {
          if (!s_bd[r][c])
            {
              empty[n++] = r * G2048_N + c;
            }
        }
    }
  if (!n)
    {
      return;
    }
  pick = empty[rand() % n];
  s_bd[pick / G2048_N][pick % G2048_N] = (rand() % 10 == 0) ? 4 : 2;
}

static bool can_move(void)
{
  int r;
  int c;

  for (r = 0; r < G2048_N; r++)
    {
      for (c = 0; c < G2048_N; c++)
        {
          uint16_t v = s_bd[r][c];
          if (!v)
            {
              return true;
            }
          if (c + 1 < G2048_N && s_bd[r][c + 1] && v == s_bd[r][c + 1])
            {
              return true;
            }
          if (r + 1 < G2048_N && s_bd[r + 1][c] && v == s_bd[r + 1][c])
            {
              return true;
            }
        }
    }
  return false;
}

/* Standard 2048: compact toward index 0, merge each pair once (i += 2). */
static int slide_line(int *line, int n)
{
  int src[8];
  int m = 0;
  int i;
  int w = 0;
  int gain = 0;

  for (i = 0; i < n; i++)
    {
      if (line[i])
        {
          src[m++] = line[i];
        }
    }

  for (i = 0; i < m; )
    {
      if (i + 1 < m && src[i] == src[i + 1])
        {
          int v = src[i] * 2;
          line[w++] = v;
          gain += v;
          if (v >= 2048)
            {
              s_won = true;
            }
          i += 2; /* both tiles consumed — do not merge again */
        }
      else
        {
          line[w++] = src[i++];
        }
    }

  for (i = w; i < n; i++)
    {
      line[i] = 0;
    }
  return gain;
}

/* dir: 0 left, 1 right, 2 up, 3 down */
static bool move_dir(int dir)
{
  int line[8];
  int r;
  int c;
  int gain = 0;
  uint16_t before[G2048_N][G2048_N];

  if (s_over)
    {
      return false;
    }

  memcpy(before, s_bd, sizeof(s_bd));

  if (dir == 0 || dir == 1)
    {
      for (r = 0; r < G2048_N; r++)
        {
          for (c = 0; c < G2048_N; c++)
            {
              int src = (dir == 0) ? c : (G2048_N - 1 - c);
              line[c] = s_bd[r][src];
            }
          gain += slide_line(line, G2048_N);
          for (c = 0; c < G2048_N; c++)
            {
              int dst = (dir == 0) ? c : (G2048_N - 1 - c);
              s_bd[r][dst] = (uint16_t)line[c];
            }
        }
    }
  else if (dir == 2 || dir == 3)
    {
      for (c = 0; c < G2048_N; c++)
        {
          for (r = 0; r < G2048_N; r++)
            {
              int src = (dir == 2) ? r : (G2048_N - 1 - r);
              line[r] = s_bd[src][c];
            }
          gain += slide_line(line, G2048_N);
          for (r = 0; r < G2048_N; r++)
            {
              int dst = (dir == 2) ? r : (G2048_N - 1 - r);
              s_bd[dst][c] = (uint16_t)line[r];
            }
        }
    }
  else
    {
      return false;
    }

  if (memcmp(before, s_bd, sizeof(s_bd)) == 0)
    {
      return false; /* no change → no spawn, no score */
    }

  s_score += gain;
  if (s_score > s_best)
    {
      s_best = s_score;
      save_best();
    }
  spawn();
  if (!can_move())
    {
      s_over = true;
    }
  paint_all();
  return true;
}

static void g2048_dir_cb(lv_event_t *e)
{
  move_dir((int)(intptr_t)lv_event_get_user_data(e));
}

static void g2048_new_cb(lv_event_t *e)
{
  (void)e;
  memset(s_bd, 0, sizeof(s_bd));
  s_score = 0;
  s_over = false;
  s_won = false;
  spawn();
  spawn();
  paint_all();
}

static void board_event(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  static lv_point_t start;
  lv_point_t now;
  int dx;
  int dy;

  if (code == LV_EVENT_PRESSED)
    {
      lv_indev_t *in = lv_indev_active();
      if (in)
        {
          lv_indev_get_point(in, &start);
        }
      return;
    }
  if (code != LV_EVENT_RELEASED || s_over)
    {
      return;
    }

  {
    lv_indev_t *in = lv_indev_active();
    if (!in)
      {
        return;
      }
    lv_indev_get_point(in, &now);
  }

  dx = now.x - start.x;
  dy = now.y - start.y;
  if (abs(dx) < 20 && abs(dy) < 20)
    {
      return;
    }
  if (abs(dx) >= abs(dy))
    {
      move_dir(dx > 0 ? 1 : 0);
    }
  else
    {
      move_dir(dy > 0 ? 3 : 2);
    }
}

void dm_create_2048(void)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_t *back;
  lv_obj_t *title;
  lv_obj_t *board;
  lv_obj_t *lab;
  lv_obj_t *card;
  lv_obj_t *b;
  int r;
  int c;

  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN); /* hide until dm_show(PAGE_2048) */
  g_dm_pages[PAGE_2048] = page;

  back = dm_btn(page, "←", "<", 36, 24, C_BTN, C_MUTED, g2048_back, NULL);
  lv_obj_set_pos(back, 8, 6);

  title = dm_lbl(page, "2048", "2048", g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 50, 6);

  card = lv_obj_create(page);
  lv_obj_set_size(card, 70, 34);
  lv_obj_set_pos(card, 166, 2);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x141518), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 4, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lab = dm_lbl(card, "分", "Score", g_dm_font_s, C_DIM);
  lv_obj_set_pos(lab, 6, 1);
  s_score_l = dm_lbl(card, "0", "0", g_dm_font_m, C_STAR);
  lv_obj_set_pos(s_score_l, 6, 14);

  card = lv_obj_create(page);
  lv_obj_set_size(card, 70, 34);
  lv_obj_set_pos(card, 242, 2);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x141518), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 4, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lab = dm_lbl(card, "最佳", "Best", g_dm_font_s, C_DIM);
  lv_obj_set_pos(lab, 6, 1);
  s_best_l = dm_lbl(card, "0", "0", g_dm_font_m, C_ACCENT);
  lv_obj_set_pos(s_best_l, 6, 14);

  /* board: pad=0 on container; tiles use explicit inner padding */
  board = lv_obj_create(page);
  lv_obj_set_size(board, BOARD_W, BOARD_W);
  lv_obj_set_pos(board, BOARD_X, BOARD_Y);
  lv_obj_set_style_bg_color(board, lv_color_hex(0x0d0e11), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(board, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(board, 12, LV_PART_MAIN);
  lv_obj_set_style_border_width(board, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(board, 0, LV_PART_MAIN);
  lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(board, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(board, board_event, LV_EVENT_PRESSED, NULL);
  lv_obj_add_event_cb(board, board_event, LV_EVENT_RELEASED, NULL);

  for (r = 0; r < G2048_N; r++)
    {
      for (c = 0; c < G2048_N; c++)
        {
          lv_obj_t *t = lv_label_create(board);
          int x = BOARD_PAD + c * (TILE_PX + TILE_GAP);
          int y = BOARD_PAD + r * (TILE_PX + TILE_GAP);
          lv_obj_set_size(t, TILE_PX, TILE_PX);
          lv_obj_set_pos(t, x, y);
          lv_obj_set_style_radius(t, 8, LV_PART_MAIN);
          lv_obj_set_style_border_width(t, 0, LV_PART_MAIN);
          lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_PART_MAIN);
          lv_obj_set_style_bg_color(t, lv_color_hex(0x1a1c20), LV_PART_MAIN);
          lv_obj_set_style_text_font(t, g_dm_font_m, LV_PART_MAIN);
          lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
          lv_obj_set_style_text_color(t, lv_color_hex(C_INK), LV_PART_MAIN);
          /* nudge digits toward vertical center */
          lv_obj_set_style_pad_top(t, 6, LV_PART_MAIN);
          lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
          lv_label_set_text(t, "");
          s_tiles[r][c] = t;
        }
    }

  s_status_l = dm_lbl(page, "滑动合并，或用方向键", "Swipe or arrow keys",
                      g_dm_font_s, C_DIM);
  lv_obj_align(s_status_l, LV_ALIGN_TOP_MID, 0, BOARD_Y + BOARD_W + 2);

  b = dm_btn(page, "←", "<", 46, 28, C_BTN, C_INK, g2048_dir_cb,
             (void *)(intptr_t)0);
  lv_obj_set_pos(b, 24, BTN_Y);
  b = dm_btn(page, "↑", "^", 46, 28, C_BTN, C_INK, g2048_dir_cb,
             (void *)(intptr_t)2);
  lv_obj_set_pos(b, 76, BTN_Y);
  b = dm_btn(page, "↓", "v", 46, 28, C_BTN, C_INK, g2048_dir_cb,
             (void *)(intptr_t)3);
  lv_obj_set_pos(b, 128, BTN_Y);
  b = dm_btn(page, "→", ">", 46, 28, C_BTN, C_INK, g2048_dir_cb,
             (void *)(intptr_t)1);
  lv_obj_set_pos(b, 180, BTN_Y);
  b = dm_btn(page, "新局", "New", 60, 28, C_STAR, C_EYE, g2048_new_cb,
             NULL);
  lv_obj_set_pos(b, 240, BTN_Y);

  load_best();
  g2048_new_cb(NULL);
}

#endif /* CONFIG_DESKMATE_APP */
