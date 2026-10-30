/*
 * dm_ui_vol.c — independent volume settings page (Settings → 音量)
 * PAGE_VOLUME: slider + mute + test beep + preset steps
 */

#include "deskmate.h"
#include "dm_audio_vol.h"
#include "dm_audio_route.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>

static lv_obj_t *s_vol_page;
static lv_obj_t *s_slider;
static lv_obj_t *s_pct_l;
static lv_obj_t *s_state_l;
static lv_obj_t *s_mute_btn;
static lv_obj_t *s_vol_sub;
static bool s_syncing;

static void refresh_labels(void)
{
  char buf[32];
  int v = dm_vol_get();
  int mute = dm_vol_is_mute();

  snprintf(buf, sizeof(buf), "%d%%", v);
  if (s_pct_l)
    {
      lv_label_set_text(s_pct_l, buf);
    }
  if (s_state_l)
    {
      if (mute)
        {
          lv_label_set_text(s_state_l, dm_t("已静音", "Muted"));
          lv_obj_set_style_text_color(s_state_l, lv_color_hex(C_HEART),
                                      LV_PART_MAIN);
        }
      else
        {
          lv_label_set_text(s_state_l, dm_t("播放中音量", "Output volume"));
          lv_obj_set_style_text_color(s_state_l, lv_color_hex(C_OK),
                                      LV_PART_MAIN);
        }
    }
  if (s_mute_btn)
    {
      lv_obj_t *lab = lv_obj_get_child(s_mute_btn, 0);
      if (lab)
        {
          lv_label_set_text(lab, mute ? dm_t("取消静音", "Unmute")
                                      : dm_t("静音", "Mute"));
        }
    }
  if (s_vol_sub)
    {
      snprintf(buf, sizeof(buf), dm_t("设备音量 %d/1000", "HW %d/1000"),
               mute ? 0 : v * 10);
      lv_label_set_text(s_vol_sub, buf);
    }
  if (s_slider)
    {
      s_syncing = true;
      lv_slider_set_value(s_slider, v, LV_ANIM_OFF);
      s_syncing = false;
    }
}

static void slider_cb(lv_event_t *e)
{
  int v;
  (void)e;
  if (s_syncing || !s_slider)
    {
      return;
    }
  v = (int)lv_slider_get_value(s_slider);
  (void)dm_vol_set(v);
  refresh_labels();
}

static void vol_plus(lv_event_t *e)
{
  (void)e;
  (void)dm_vol_set(dm_vol_get() + 5);
  refresh_labels();
}

static void vol_minus(lv_event_t *e)
{
  (void)e;
  (void)dm_vol_set(dm_vol_get() - 5);
  refresh_labels();
}

static void vol_mute(lv_event_t *e)
{
  (void)e;
  (void)dm_vol_set_mute(!dm_vol_is_mute());
  refresh_labels();
}

static void vol_test(lv_event_t *e)
{
  (void)e;
  if (dm_vol_is_mute())
    {
      if (s_state_l)
        {
          lv_label_set_text(s_state_l, dm_t("已静音，先取消静音",
                                            "Muted — unmute first"));
        }
      return;
    }
  if (s_state_l)
    {
      lv_label_set_text(s_state_l, dm_t("试听中…", "Playing test tone…"));
    }
  (void)dm_vol_test_beep();
  refresh_labels();
}

static void vol_preset(lv_event_t *e)
{
  int v = (int)(intptr_t)lv_event_get_user_data(e);
  (void)dm_vol_set(v);
  refresh_labels();
}

static void vol_back(lv_event_t *e)
{
  (void)e;
  dm_vol_apply();
  dm_show(PAGE_SETTINGS);
}


static lv_obj_t *s_out_l;

static void out_pick(lv_event_t *e)
{
  int ch = (int)(intptr_t)lv_event_get_user_data(e);
  dm_out_set(ch);
  if (s_out_l)
    {
      lv_label_set_text(s_out_l, dm_out_name(dm_out_get()));
    }
  if (s_state_l)
    {
      lv_label_set_text(s_state_l,
                        dm_t("输出：", "Output: "));
    }
}

void dm_create_volume(void)
{
  lv_obj_t *title, *back, *card, *lab, *btn;
  int i;

  s_vol_page = lv_obj_create(g_dm_root);
  lv_obj_set_size(s_vol_page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(s_vol_page, 0, 0);
  lv_obj_set_style_bg_color(s_vol_page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_vol_page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_vol_page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_vol_page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_vol_page, LV_OBJ_FLAG_SCROLLABLE);
  g_dm_pages[PAGE_VOLUME] = s_vol_page;

  back = dm_btn(s_vol_page, "←", "<", 34, 24, MD_SURFACE_HIGH, MD_ON_SURFACE,
                vol_back, NULL);
  lv_obj_set_pos(back, 8, 8);
  title = dm_lbl(s_vol_page, "音量", "Volume", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(title, 48, 8);

  /* main card */
  card = lv_obj_create(s_vol_page);
  lv_obj_set_size(card, 304, 96);
  lv_obj_set_pos(card, 8, 40);
  lv_obj_set_style_bg_color(card, lv_color_hex(MD_SURFACE_CONTAINER),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lab = dm_lbl(card, "喇叭音量 (4Ω 3W)", "Speaker volume", g_dm_font_s,
               MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(lab, 0, 0);

  s_pct_l = dm_lbl(card, "80%", "80%", g_dm_font_l, MD_PRIMARY);
  lv_obj_align(s_pct_l, LV_ALIGN_TOP_RIGHT, 0, -4);

  s_state_l = dm_lbl(card, "播放中音量", "Output volume", g_dm_font_s, C_OK);
  lv_obj_set_pos(s_state_l, 0, 26);

  s_slider = lv_slider_create(card);
  lv_obj_set_size(s_slider, 260, 12);
  lv_obj_set_pos(s_slider, 0, 52);
  lv_slider_set_range(s_slider, 0, 100);
  lv_obj_set_style_bg_color(s_slider, lv_color_hex(MD_SURFACE_HIGHEST),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_slider, lv_color_hex(MD_PRIMARY),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_slider, lv_color_hex(MD_ON_PRIMARY),
                            LV_PART_KNOB);
  lv_obj_set_style_pad_all(s_slider, 6, LV_PART_KNOB);
  lv_obj_add_event_cb(s_slider, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

  /* +/- */
  btn = dm_btn(card, "−", "-", 40, 22, MD_SURFACE_HIGH, MD_ON_SURFACE,
               vol_minus, NULL);
  lv_obj_set_pos(btn, 0, 70);
  btn = dm_btn(card, "＋", "+", 40, 22, MD_SURFACE_HIGH, MD_ON_SURFACE,
               vol_plus, NULL);
  lv_obj_set_pos(btn, 220, 70);

  s_vol_sub = dm_lbl(card, "设备音量 800/1000", "HW 800/1000", g_dm_font_s,
                     MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_vol_sub, 52, 74);

  /* presets */
  {
    static const int presets[] = { 20, 50, 80, 100 };
    int x = 8;
    for (i = 0; i < 4; i++)
      {
        char t[8];
        snprintf(t, sizeof(t), "%d", presets[i]);
        btn = dm_btn(s_vol_page, t, t, 70, 24, MD_SECONDARY_CONTAINER,
                     MD_ON_SECONDARY_CONT, vol_preset,
                     (void *)(intptr_t)presets[i]);
        lv_obj_set_pos(btn, x, 188);
        x += 76;
      }
  }

  /* mute / test */
  s_mute_btn = dm_btn(s_vol_page, "静音", "Mute", 140, 26, MD_SURFACE_HIGH,
                      MD_ON_SURFACE, vol_mute, NULL);
  lv_obj_set_pos(s_mute_btn, 20, 210);
  btn = dm_btn(s_vol_page, "试听", "Test", 140, 26, MD_PRIMARY, MD_ON_PRIMARY,
               vol_test, NULL);
  lv_obj_set_pos(btn, 160, 210);

  /* audio output channel */
  {
    lv_obj_t *ol = dm_lbl(s_vol_page, "音频输出", "Output", g_dm_font_s,
                          MD_ON_SURFACE_VARIANT);
    lv_obj_set_pos(ol, 12, 142);
    s_out_l = dm_lbl(s_vol_page, "自动", "Auto", g_dm_font_s, MD_PRIMARY);
    lv_obj_set_pos(s_out_l, 240, 142);
    {
      lv_obj_t *b1 = dm_btn(s_vol_page, "自动", "Auto", 88, 24,
                            MD_SECONDARY_CONTAINER, MD_ON_SECONDARY_CONT,
                            out_pick, (void *)(intptr_t)DM_OUT_AUTO);
      lv_obj_set_pos(b1, 12, 142);
      /* keep label visible: shift buttons below label */
      lv_obj_set_pos(b1, 12, 160);
      lv_obj_t *b2 = dm_btn(s_vol_page, "喇叭", "Spk", 88, 24,
                            MD_SECONDARY_CONTAINER, MD_ON_SECONDARY_CONT,
                            out_pick, (void *)(intptr_t)DM_OUT_SPEAKER);
      lv_obj_set_pos(b2, 108, 160);
      lv_obj_t *b3 = dm_btn(s_vol_page, "耳机", "BT", 88, 24,
                            MD_SECONDARY_CONTAINER, MD_ON_SECONDARY_CONT,
                            out_pick, (void *)(intptr_t)DM_OUT_HEADSET);
      lv_obj_set_pos(b3, 204, 160);
    }
  }

  {
    lv_obj_t *hint = dm_lbl(s_vol_page,
                            "音量保存在 /data/deskmate_vol.txt",
                            "Saved to /data/deskmate_vol.txt",
                            g_dm_font_s, MD_ON_SURFACE_VARIANT);
    lv_obj_set_pos(hint, 12, 182);
  }

  dm_vol_init();
  dm_out_init();
  if (s_out_l) lv_label_set_text(s_out_l, dm_out_name(dm_out_get()));
  refresh_labels();
}

#endif /* CONFIG_DESKMATE_APP */
