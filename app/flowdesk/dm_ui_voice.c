/*
 * dm_ui_voice.c — Settings → 播报声音 (independent page)
 * Pick from multiple TTS voices + preview.
 */

#include "deskmate.h"
#include "dm_tts_voice.h"
#include "dm_voice.h"
#include "dm_audio_vol.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>

#define VOICE_ROWS 9

static lv_obj_t *s_page;
static lv_obj_t *s_rows[VOICE_ROWS];
static lv_obj_t *s_row_sub[VOICE_ROWS];
static lv_obj_t *s_cur_l;
static lv_obj_t *s_tip_l;
static volatile int s_preview_busy;
static int s_preview_done;
static volatile int s_preview_ok;

struct voice_preview_job_s
{
  int profile_idx;
};

static void *voice_preview_worker(void *arg)
{
  struct voice_preview_job_s *job = arg;
  int profile_idx = job->profile_idx;

  free(job);
  s_preview_ok = (dm_voice_profile_preview(profile_idx) == 0);
  __atomic_store_n(&s_preview_done, 1, __ATOMIC_RELEASE);
  return NULL;
}

static void request_preview(int profile_idx)
{
  struct voice_preview_job_s *job;
  pthread_t th;
  pthread_attr_t attr;
  int ret;

  if (s_preview_busy)
    {
      return;
    }

  job = malloc(sizeof(*job));
  if (!job)
    {
      if (s_tip_l)
        {
          lv_label_set_text(s_tip_l, dm_t("内存不足，无法试听",
                                          "Not enough memory to preview"));
        }
      return;
    }
  job->profile_idx = profile_idx;
  s_preview_busy = 1;
  __atomic_store_n(&s_preview_done, 0, __ATOMIC_RELEASE);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_VOICE_NET_WORKER_STACK);
  ret = pthread_create(&th, &attr, voice_preview_worker, job);
  pthread_attr_destroy(&attr);
  if (ret == 0)
    {
      pthread_detach(th);
    }
  else
    {
      free(job);
      s_preview_busy = 0;
      if (s_tip_l)
        {
          lv_label_set_text(s_tip_l, dm_t("试听启动失败",
                                          "Could not start preview"));
        }
    }
}

void dm_voice_sel_tick(void)
{
  if (!__atomic_load_n(&s_preview_done, __ATOMIC_ACQUIRE))
    {
      return;
    }

  __atomic_store_n(&s_preview_done, 0, __ATOMIC_RELEASE);
  s_preview_busy = 0;
  if (s_tip_l)
    {
      lv_label_set_text(s_tip_l,
                        s_preview_ok
          ? dm_t("试听完成（内置音色/缓存）",
                 "Preview played (bundled/cache)")
          : dm_t("试听失败：检查音频输出、资源或网络",
                 "Preview failed: check audio, resources or network"));
    }
}

static void refresh_list(void)
{
  int i;
  int sel = dm_voice_profile_sel();

  for (i = 0; i < VOICE_ROWS && i < dm_voice_profile_count(); i++)
    {
      const dm_voice_profile_t *p = dm_voice_profile_get(i);
      if (!s_rows[i])
        {
          continue;
        }
      lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
      {
        lv_obj_t *lab = lv_obj_get_child(s_rows[i], 0);
        if (lab)
          {
            lv_label_set_text(lab, p->name);
          }
      }
      if (s_row_sub[i])
        {
          char sub[48];
          snprintf(sub, sizeof(sub), "%s · %s",
                   p->offline ? "离线音色" : "云端音色",
                   p->api_id);
          lv_label_set_text(s_row_sub[i], sub);
        }
      lv_obj_set_style_bg_color(
          s_rows[i],
          lv_color_hex(i == sel ? 0x0d3a4a : 0x1a1a1a), LV_PART_MAIN);
    }
  if (s_cur_l)
    {
      lv_label_set_text(s_cur_l, dm_voice_profile_name());
    }
}

static void preview(lv_event_t *e)
{
  (void)e;
  if (s_tip_l)
    {
      lv_label_set_text(s_tip_l, dm_t("试听中…", "Playing…"));
    }
  /* Bundled samples play offline; remaining voices generate/cache on first use. */
  request_preview(dm_voice_profile_sel());
}

static void pick(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= dm_voice_profile_count())
    {
      return;
    }
  dm_voice_profile_set(idx);
  refresh_list();
  if (s_tip_l)
    {
      char msg[48];
      snprintf(msg, sizeof(msg), dm_t("已切换为「%s」", "Switched to %s"),
               dm_voice_profile_name());
      lv_label_set_text(s_tip_l, msg);
    }
  request_preview(idx);
}

static void back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_SETTINGS);
}

void dm_create_voice_sel(void)
{
  lv_obj_t *title, *b, *card, *lab;
  int i;

  s_page = lv_obj_create(g_dm_root);
  lv_obj_set_size(s_page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(s_page, 0, 0);
  lv_obj_set_style_bg_color(s_page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
  g_dm_pages[PAGE_VOICE_SEL] = s_page;

  b = dm_btn(s_page, "←", "<", 34, 24, MD_SURFACE_HIGH, MD_ON_SURFACE,
             back, NULL);
  lv_obj_set_pos(b, 8, 8);
  title = dm_lbl(s_page, "播报声音", "Voice", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(title, 48, 8);

  /* current + preview */
  card = lv_obj_create(s_page);
  lv_obj_set_size(card, 304, 56);
  lv_obj_set_pos(card, 8, 40);
  lv_obj_set_style_bg_color(card, lv_color_hex(MD_SURFACE_CONTAINER),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lab = dm_lbl(card, "当前", "Current", g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(lab, 0, 0);
  s_cur_l = dm_lbl(card, "冰糖", "Bingtang", g_dm_font_m, MD_PRIMARY);
  lv_obj_set_pos(s_cur_l, 0, 20);

  b = dm_btn(card, "试听", "Preview", 88, 28, MD_PRIMARY, MD_ON_PRIMARY,
             preview, NULL);
  lv_obj_align(b, LV_ALIGN_RIGHT_MID, -2, 0);

  /* scrollable voice list */
  {
    lv_obj_t *sc = lv_obj_create(s_page);
    lv_obj_set_size(sc, 304, 120);
    lv_obj_set_pos(sc, 8, 104);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sc, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(sc, 4, LV_PART_MAIN);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);

    for (i = 0; i < VOICE_ROWS; i++)
      {
        lv_obj_t *row = lv_obj_create(sc);
        lv_obj_set_size(row, 292, 40);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(row, 6, LV_PART_MAIN);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        {
          lv_obj_t *w = dm_lbl(row, "", "", g_dm_font_s, C_INK);
          lv_obj_set_pos(w, 4, 2);
          lv_obj_t *d = dm_lbl(row, "", "", g_dm_font_s, C_MUTED);
          lv_obj_set_pos(d, 4, 20);
          s_row_sub[i] = d;
        }
        s_rows[i] = row;
      }
  }

  s_tip_l = dm_lbl(s_page, "冰糖/茉莉/苏打内置离线试听",
                   "3 bundled offline previews",
                   g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_tip_l, 12, 232);

  dm_voice_profile_init();
  refresh_list();
}

#endif /* CONFIG_DESKMATE_APP */
