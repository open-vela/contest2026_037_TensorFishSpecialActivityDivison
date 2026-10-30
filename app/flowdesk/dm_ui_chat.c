/*
 * dm_ui_chat.c — tap-to-talk voice chat
 * Rule: only the LVGL thread may touch widgets.
 * Worker writes results into s_out_* ; dm_chat_tick() applies them.
 */

#include "deskmate.h"
#include "dm_voice.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define CHAT_LINE_MAX 96
#define CHAT_FACE_SZ 76

static lv_obj_t *s_ans;
static lv_obj_t *s_sub;
static lv_obj_t *s_ptt_btn;
static volatile bool s_busy;
static volatile bool s_ui_rec;
static volatile int s_job;
static volatile int s_rec_sec;
static volatile int s_mic_err;
static volatile int s_out_ready; /* 1=text 2=microphone error */

static char s_pending[CHAT_LINE_MAX];
static char s_last_ans[CHAT_LINE_MAX];
static char s_out_ans[CHAT_LINE_MAX];
static char s_out_sub[80];

static void set_ans(const char *text)
{
  if (!text)
    {
      text = "";
    }
  strncpy(s_last_ans, text, sizeof(s_last_ans) - 1);
  s_last_ans[sizeof(s_last_ans) - 1] = '\0';
  if (s_ans)
    {
      lv_label_set_text(s_ans, s_last_ans[0] ? s_last_ans
                                             : dm_t("点按钮开始说话",
                                                    "Tap button to talk"));
    }
}

static void set_sub(const char *text)
{
  if (s_sub)
    {
      lv_label_set_text(s_sub, text ? text : "");
    }
}

static void set_ptt(const char *zh, const char *en, uint32_t bg)
{
  if (!s_ptt_btn)
    {
      return;
    }
  lv_obj_set_style_bg_color(s_ptt_btn, lv_color_hex(bg), LV_PART_MAIN);
  {
    lv_obj_t *lab = lv_obj_get_child(s_ptt_btn, 0);
    if (lab)
      {
        lv_label_set_text(lab, dm_t(zh, en));
      }
  }
}

static void reset_to_idle(void)
{
  s_ui_rec = false;
  s_rec_sec = 0;
  s_busy = false;
  s_job = 0;
  set_ptt("开始说话", "Start talk", C_BTN);
}

/* worker thread — NO LVGL calls here */
static void *voice_worker(void *arg)
{
  char reply[CHAT_LINE_MAX];

  (void)arg;

  if (s_job == 1)
    {
      snprintf(s_out_sub, sizeof(s_out_sub), "%s",
               dm_t("ASR → 模型 → 喇叭播报", "ASR → LLM → speaker"));
      if (dm_voice_asr_chat_tts("/tmp/dm_voice_in.wav", reply,
                                sizeof(reply)) == 0)
        {
          snprintf(s_out_ans, sizeof(s_out_ans), "%s", reply);
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("已通过喇叭播放回复", "Reply played on speaker"));
          s_out_ready = 1;
        }
      else
        {
          snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                   dm_t("没听清或网络忙，再说一次？",
                        "Didn't catch that, try again"));
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("检查麦克风 / 网络", "Check mic / network"));
          s_out_ready = 1;
        }
    }
  else if (s_job == 2)
    {
      const char *r = dm_chat_ai_ask(s_pending);
      snprintf(s_out_ans, sizeof(s_out_ans), "%s", r ? r : "……");
      if (r && r[0])
        {
          (void)dm_voice_tts_play(r);
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("已通过喇叭播放回复", "Reply played on speaker"));
        }
      else
        {
          s_out_sub[0] = '\0';
        }
      s_out_ready = 1;
    }
  return NULL;
}

static void kick_job(int job)
{
  pthread_t th;
  pthread_attr_t attr;

  if (s_busy)
    {
      return;
    }
  s_busy = true;
  s_job = job;
  s_ui_rec = false;
  s_rec_sec = 0;
  s_out_ready = 0;
  s_out_ans[0] = '\0';
  s_out_sub[0] = '\0';

  dm_face_set(FACE_THINK, EYE_DECOR_NONE, 10);
  set_ans(job == 1 ? dm_t("识别中…", "Recognizing…")
                   : dm_t("思考中…", "Thinking…"));
  set_ptt("处理中…", "Working…", C_BTN_HI);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 24576);
  if (pthread_create(&th, &attr, voice_worker, NULL) == 0)
    {
      pthread_detach(th);
    }
  else
    {
      s_busy = false;
      set_ptt("开始说话", "Start talk", C_BTN);
    }
  pthread_attr_destroy(&attr);
}

static void face_click(lv_event_t *e)
{
  (void)e;
  if (s_busy || s_ui_rec)
    {
      return;
    }
  dm_face_set(FACE_HAPPY, EYE_DECOR_HEART, 8);
  strncpy(s_pending, dm_t("陪我聊聊天", "Chat with me"), sizeof(s_pending) - 1);
  s_pending[sizeof(s_pending) - 1] = '\0';
  kick_job(2);
}

static void ptt_click(lv_event_t *e)
{
  (void)e;

  if (s_busy)
    {
      return;
    }

  if (!s_ui_rec)
    {
      s_mic_err = 0;
      dm_voice_set_speaker_volume(80);
      if (dm_voice_rec_start(12) != 0)
        {
          set_ans(dm_t("麦克风不可用", "Mic unavailable"));
          set_sub(dm_t("检查 /dev/audio/pcm0c", "Check /dev/audio/pcm0c"));
          set_ptt("开始说话", "Start talk", C_BTN);
          return;
        }
      s_ui_rec = true;
      s_rec_sec = 0;
      dm_face_set(FACE_THINK, EYE_DECOR_NONE, 10);
      set_ans(dm_t("● 录音中… 再点一下结束并发送",
                   "● Recording… tap again to send"));
      set_sub(dm_t("再点一下结束", "Tap again to stop"));
      set_ptt("结束并发送", "Stop & send", C_HEART);
    }
  else
    {
      s_ui_rec = false;
      s_rec_sec = 0;
      set_ptt("发送中…", "Sending…", C_BTN_HI);
      set_ans(dm_t("正在结束录音…", "Stopping mic…"));

      (void)dm_voice_rec_stop();

      if (s_mic_err)
        {
          set_ans(dm_t("麦克风故障，无法录音", "Mic failed"));
          set_sub(dm_t("检查 /dev/audio/pcm0c", "Check mic device"));
          reset_to_idle();
          return;
        }

      kick_job(1);
    }
}

void dm_create_chat(void)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_t *title;

  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  g_dm_pages[PAGE_CHAT] = page;

  title = dm_lbl(page, "聊天", "Chat", g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 12, 4);

  dm_face_build(page, (DM_SCR_W - CHAT_FACE_SZ) / 2, 28, CHAT_FACE_SZ);
  if (g_dm_face)
    {
      lv_obj_set_size(g_dm_face, CHAT_FACE_SZ, CHAT_FACE_SZ);
      lv_obj_add_event_cb(g_dm_face, face_click, LV_EVENT_CLICKED, NULL);
    }

  s_ans = dm_lbl(page, "点按钮开始说话", "Tap button to talk",
                 g_dm_font_s, C_INK);
  lv_label_set_long_mode(s_ans, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_ans, 288);
  lv_obj_set_style_text_align(s_ans, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(s_ans, 16, 112);

  s_sub = dm_lbl(page, "点一下录 · 再点结束并发送 · 喇叭播回复",
                 "Tap rec · tap send · speaker replies", g_dm_font_s, C_DIM);
  lv_obj_set_width(s_sub, 288);
  lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(s_sub, 16, 140);

  s_ptt_btn = dm_btn(page, "开始说话", "Start talk", 296, 30, C_BTN,
                     C_ACCENT, ptt_click, NULL);
  lv_obj_set_pos(s_ptt_btn, 12, 162);

  {
    lv_obj_t *hint = dm_lbl(page, "喇叭 4Ω3W · 回复自动播报",
                            "Speaker 4ohm 3W · auto speak", g_dm_font_s,
                            C_MUTED);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(hint, 288);
    lv_obj_set_pos(hint, 16, 200);
  }

  s_busy = false;
  s_ui_rec = false;
  s_rec_sec = 0;
  s_last_ans[0] = '\0';
  dm_voice_set_speaker_volume(80);
}

void dm_chat_tick(void)
{
  /* apply worker results on LVGL thread */
  if (s_out_ready)
    {
      s_out_ready = 0;
      if (s_out_ans[0])
        {
          set_ans(s_out_ans);
        }
      if (s_out_sub[0])
        {
          set_sub(s_out_sub);
        }
      dm_face_set(FACE_HAPPY, EYE_DECOR_NONE, 8);
      reset_to_idle();
    }

  if (s_ui_rec && !s_busy)
    {
      char buf[80];
      s_rec_sec++;
      if (dm_voice_rec_active())
        {
          snprintf(buf, sizeof(buf),
                   dm_t("● 录音中 %d 秒 · 再点一下结束并发送",
                        "● Recording %ds · tap again to send"),
                   s_rec_sec);
          set_ans(buf);
          set_sub((s_rec_sec & 1) ? dm_t("正在听…", "Listening…")
                                  : dm_t("对准麦克风说话", "Speak to the mic"));
        }
      else
        {
          if (s_rec_sec < 2)
            {
              s_mic_err = 1;
              set_ans(dm_t("麦克风故障，无法录音", "Mic failed"));
              set_sub(dm_t("检查 /dev/audio/pcm0c", "Check mic device"));
            }
          s_ui_rec = false;
          reset_to_idle();
        }
    }
}

#endif /* CONFIG_DESKMATE_APP */
