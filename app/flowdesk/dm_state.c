/****************************************************************************
 * dm_state.c — fonts, navigation, shared helpers
 ****************************************************************************/

#include "deskmate.h"
#include <string.h>
#include <stdio.h>

#ifdef CONFIG_DESKMATE_APP

dm_ctx_t g_dm = {
  .page = PAGE_FOCUS_HOME,
  .face = FACE_IDLE,
  .decor = EYE_DECOR_NONE,
  .focus_left = 25 * 60,
  .focus_total = 25 * 60,
  .focus_sel_min = 25,
  .focus_custom_min = 1,
  .focus_run = false,
  .focus_finished = false,
  .zh = true,
};

const lv_font_t *g_dm_font_s;
const lv_font_t *g_dm_font_m;
const lv_font_t *g_dm_font_l;
const lv_font_t *g_dm_font_xl;

lv_obj_t *g_dm_root;
lv_obj_t *g_dm_pages[PAGE_COUNT];
lv_obj_t *g_dm_dock;
lv_obj_t *g_dm_face;
lv_obj_t *g_dm_bubble;
lv_obj_t *g_dm_eye_l;
lv_obj_t *g_dm_eye_r;
lv_obj_t *g_dm_mouth;
lv_obj_t *g_dm_decor_l;
lv_obj_t *g_dm_decor_r;
lv_obj_t *g_dm_blush_l;
lv_obj_t *g_dm_blush_r;

const char *dm_t(const char *zh, const char *en)
{
  return g_dm.zh ? zh : en;
}

void dm_style(lv_obj_t *o, const lv_font_t *f, uint32_t c)
{
  lv_obj_set_style_text_font(o, f, LV_PART_MAIN);
  lv_obj_set_style_text_color(o, lv_color_hex(c), LV_PART_MAIN);
}

lv_obj_t *dm_lbl(lv_obj_t *p, const char *zh, const char *en,
                 const lv_font_t *f, uint32_t c)
{
  lv_obj_t *o = lv_label_create(p);
  lv_label_set_text(o, dm_t(zh, en));
  dm_style(o, f, c);
  return o;
}

lv_obj_t *dm_btn(lv_obj_t *p, const char *zh, const char *en, int w, int h,
                 uint32_t bg, uint32_t fg, lv_event_cb_t cb, void *ud)
{
  lv_obj_t *b = lv_button_create(p);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, h / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  if (cb)
    {
      lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }

  lv_obj_t *t = dm_lbl(b, zh, en, g_dm_font_s, fg);
  lv_obj_center(t);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
  return b;
}

void dm_say(const char *zh, const char *en)
{
  if (g_dm_bubble)
    {
      lv_label_set_text(g_dm_bubble, dm_t(zh, en));
    }
}

static const lv_font_t *try_ft(const char *path, uint16_t sz)
{
#ifdef LV_USE_FREETYPE
  return lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                 sz, LV_FREETYPE_FONT_STYLE_NORMAL);
#else
  (void)path;
  (void)sz;
  return NULL;
#endif
}

void dm_init_fonts(void)
{
  const char *paths[] = {
    "/data/font/MiSans-Regular.ttf",
    "/resource/fonts/MiSans-Normal.ttf",
    "/resource/fonts/MiSans-Regular.ttf",
  };
  int i;

  g_dm_font_s = NULL;
  g_dm_font_m = NULL;
  g_dm_font_l = NULL;
  g_dm_font_xl = NULL;

  for (i = 0; i < 3 && !g_dm_font_s; i++)
    {
      g_dm_font_s = try_ft(paths[i], 12);
      if (g_dm_font_s)
        {
          g_dm_font_m = try_ft(paths[i], 16);
          g_dm_font_l = try_ft(paths[i], 20);
          g_dm_font_xl = try_ft(paths[i], 42);
        }
    }

  if (!g_dm_font_s)
    {
      g_dm_font_s = &lv_font_montserrat_12;
      g_dm_font_m = &lv_font_montserrat_16;
      g_dm_font_l = &lv_font_montserrat_20;
      g_dm_font_xl = &lv_font_montserrat_48;
    }
  if (!g_dm_font_m)
    {
      g_dm_font_m = &lv_font_montserrat_16;
    }
  if (!g_dm_font_l)
    {
      g_dm_font_l = &lv_font_montserrat_20;
    }
  if (!g_dm_font_xl)
    {
      g_dm_font_xl = &lv_font_montserrat_48;
    }
}

static void set_page_opa(void *obj, int32_t v)
{
  lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

static void fade_in(lv_obj_t *page)
{
  lv_anim_t a;
  lv_obj_clear_flag(page, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_anim_init(&a);
  lv_anim_set_var(&a, page);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_time(&a, 320);
  lv_anim_set_exec_cb(&a, set_page_opa);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

void dm_show(dm_page_t p)
{
  int i;

  if (p >= PAGE_COUNT)
    {
      return;
    }

  g_dm.page = p;
  for (i = 0; i < PAGE_COUNT; i++)
    {
      if (!g_dm_pages[i])
        {
          continue;
        }

      if (i == (int)p)
        {
          fade_in(g_dm_pages[i]);
          /* keep active page on top (later-created pages e.g. 2048) */
          lv_obj_move_foreground(g_dm_pages[i]);
        }
      else
        {
          lv_obj_add_flag(g_dm_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (g_dm_dock)
    {
      /* Fix: Hide bottom dock on PAGE_WIFI as well so it does not cover
       * the AP list and the "连接所选" (Join) button at y=202..230. */
      if (p == PAGE_FOCUS_RUN || p == PAGE_FOCUS_DONE || p == PAGE_MOOD_LOG ||
          p == PAGE_SUPERVISE ||
          p == PAGE_NOTE || p == PAGE_WIFI || p == PAGE_WIFI_PW ||
          p == PAGE_WIFI_KB || p == PAGE_WIFI_CONN ||
          p == PAGE_WIFI_DONE ||
          p == PAGE_WORD_STUDY || p == PAGE_WORD_RES ||
          p == PAGE_WORD_QUIZ || p == PAGE_WORD_WRONG ||
          p == PAGE_WORD_LIST || p == PAGE_WORD_WSET || p == PAGE_CALC ||
          p == PAGE_SENSORS || p == PAGE_GUESS || p == PAGE_CONVERT ||
          p == PAGE_WATER || p == PAGE_COUNTDOWN || p == PAGE_EAT ||
          p == PAGE_24 || p == PAGE_DRAW || p == PAGE_BMI || p == PAGE_MED ||
          p == PAGE_MED_ADD || p == PAGE_MED_KB || p == PAGE_2048 ||
          p == PAGE_MBTI || p == PAGE_MBTI_Q || p == PAGE_MBTI_RESULT ||
          p == PAGE_NOTE_ADD || p == PAGE_NOTE_KB ||
          p == PAGE_DICT || p == PAGE_DICT_SEARCH || p == PAGE_DICT_DETAIL ||
          p == PAGE_VOLUME ||
          p == PAGE_VOICE_SEL ||
          p == PAGE_ABOUT)
        {
          lv_obj_add_flag(g_dm_dock, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_clear_flag(g_dm_dock, LV_OBJ_FLAG_HIDDEN);
          /* dock must stay above content pages */
          lv_obj_move_foreground(g_dm_dock);
        }
      dm_dock_highlight(p);
    }

  if (p == PAGE_HEALTH)
    {
      dm_health_refresh();
    }

  if (p == PAGE_CHAT && g_dm_pages[PAGE_CHAT])
    {
      dm_face_attach(g_dm_pages[PAGE_CHAT], (DM_SCR_W - 76) / 2, 32);
      if (g_dm_face)
        {
          lv_obj_set_size(g_dm_face, 76, 76);
        }
    }
  else if (p == PAGE_FOCUS_HOME && g_dm_pages[PAGE_FOCUS_HOME])
    {
      dm_face_attach(g_dm_pages[PAGE_FOCUS_HOME], (DM_SCR_W - 68) / 2, 24);
      if (g_dm_face)
        {
          lv_obj_set_size(g_dm_face, 68, 68);
        }
    }
  else if (p == PAGE_FOCUS_RUN && g_dm_pages[PAGE_FOCUS_RUN])
    {
      dm_face_attach(g_dm_pages[PAGE_FOCUS_RUN], (DM_SCR_W - 84) / 2, 24);
      if (g_dm_face)
        {
          lv_obj_set_size(g_dm_face, 84, 84);
        }
    }
  else if (p == PAGE_FOCUS_DONE && g_dm_pages[PAGE_FOCUS_DONE])
    {
      dm_face_attach(g_dm_pages[PAGE_FOCUS_DONE], (DM_SCR_W - 64) / 2, 22);
      if (g_dm_face)
        {
          lv_obj_set_size(g_dm_face, 64, 64);
        }
    }

  if (p != PAGE_FOCUS_RUN && p != PAGE_FOCUS_DONE && g_dm.focus_run)
    {
      dm_face_set(FACE_IDLE, EYE_DECOR_NONE, 0);
    }
}

void dm_tick(void)
{
  if (g_dm.page == PAGE_2048)
    {
      dm_health_add_game_ms(DM_TICK_MS);
    }
  dm_face_tick();
  dm_focus_run_tick();
  dm_wifi_tick();
  dm_proactive_tick();
  dm_btnet_tick();
  dm_appconn_tick();
  dm_tools_tick();
  dm_life_tick();
  dm_fun_tick();
  dm_chat_tick();
  dm_med_tick();
}

#endif /* CONFIG_DESKMATE_APP */
