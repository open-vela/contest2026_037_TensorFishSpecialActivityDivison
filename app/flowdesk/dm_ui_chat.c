/*
 * dm_ui_chat.c — tap-to-talk voice chat
 * Rule: only the LVGL thread may touch widgets.
 * Worker writes results into s_out_* ; dm_chat_tick() applies them.
 */

#include "deskmate.h"
#include "dm_voice.h"
#include "dm_ai_permissions.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#define CHAT_LINE_MAX 224
#define CHAT_FACE_SZ 76

static lv_obj_t *s_ans;
static lv_obj_t *s_sub;
static lv_obj_t *s_ptt_btn;
static volatile bool s_busy;
static volatile bool s_ui_rec;
static volatile int s_job;
static volatile int s_rec_sec;
static uint64_t s_rec_started_ms;
static volatile int s_mic_err;
static int s_out_ready; /* 1=text 2=microphone error */
static bool s_out_local;
static bool s_last_ans_local;
static bool s_out_health;
static bool s_last_ans_health;

static char s_pending[CHAT_LINE_MAX];
static char s_last_ans[CHAT_LINE_MAX];
static char s_out_ans[CHAT_LINE_MAX];
static char s_out_sub[80];

static uint64_t chat_monotonic_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }
  return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

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
  s_rec_started_ms = 0;
  s_busy = false;
  s_job = 0;
  set_ptt("开始说话", "Start talk", C_BTN);
}

/* worker thread — NO LVGL calls here */
static void *voice_worker(void *arg)
{
  char reply[CHAT_LINE_MAX];
  int voice_rc;

  (void)arg;
  s_out_local = false;
  s_out_health = false;

  if (s_job == 1)
    {
      /* nxrecorder_stop() joins its audio worker.  Never call it from the
       * LVGL click callback: a slow/failed PCM read must not freeze the UI. */
      if (dm_voice_rec_stop() != 0)
        {
          snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                   dm_t("录音失败，未采集到有效音频",
                        "Recording failed; no audio captured"));
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("检查麦克风和 /dev/audio/pcm0c",
                        "Check the microphone and /dev/audio/pcm0c"));
          __atomic_store_n(&s_out_ready, 1, __ATOMIC_RELEASE);
          return NULL;
        }

      snprintf(s_out_sub, sizeof(s_out_sub), "%s",
               dm_t("ASR → 模型 → 喇叭播报", "ASR → LLM → speaker"));
      voice_rc = dm_voice_asr_chat_tts("/tmp/dm_voice_in.wav", reply,
                                       sizeof(reply));
      if (voice_rc >= 0)
        {
          s_out_local = voice_rc == 3;
          s_out_health = dm_voice_health_reply() != 0;
          snprintf(s_out_ans, sizeof(s_out_ans), "%s", reply);
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   voice_rc == 3
                     ? dm_t("本地记录仅在设备显示，不上传云端播报",
                            "Local records stay on device; no cloud TTS")
                     : voice_rc == 2
                     ? dm_t("健康结果仅本地显示，未上传云端播报",
                            "Health result kept local; no cloud TTS")
                     : voice_rc == 0
                     ? dm_t("已通过喇叭播放回复", "Reply played on speaker")
                     : dm_t("文字回复已显示，语音播报失败",
                            "Text reply shown; speech playback failed"));
          __atomic_store_n(&s_out_ready, 1, __ATOMIC_RELEASE);
        }
      else
        {
          /* Report the actual blocking condition instead of masking it as a
           * generic recognition failure. */
          switch (dm_voice_last_failure())
            {
              case DM_VOICE_FAILURE_WIFI:
                snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                         dm_t("Wi-Fi 尚未连接", "Wi-Fi is not connected"));
                snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                         dm_t("正在等待网络；连接后再试", "Wait for Wi-Fi, then try again"));
                break;

              case DM_VOICE_FAILURE_TIME:
                snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                         dm_t("设备时间尚未同步", "Device time is not synced"));
                snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                         dm_t("网络时间就绪后再试，TLS证书校验未关闭",
                              "Retry after network time sync; TLS validation remains on"));
                break;

              case DM_VOICE_FAILURE_API_KEY:
                snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                         dm_t("MiMo API密钥未配置", "MiMo API key is not configured"));
                snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                         dm_t("将新密钥保存到 /data/deskmate_mimo.key",
                              "Save a rotated key to /data/deskmate_mimo.key"));
                break;

              case DM_VOICE_FAILURE_CHAT:
                snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                         dm_t("语音对话请求失败", "Voice chat request failed"));
                snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                         dm_t("检查网络/API状态，或点脸使用文字聊天",
                              "Check network/API status or tap the face for text chat"));
                break;

              default:
                snprintf(s_out_ans, sizeof(s_out_ans), "%s",
                         dm_t("语音识别失败，请重试", "Speech recognition failed"));
                snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                         dm_t("检查录音和麦克风，或点脸使用文字聊天",
                              "Check recording/mic or tap the face for text chat"));
                break;
            }
          __atomic_store_n(&s_out_ready, 1, __ATOMIC_RELEASE);
        }
    }
  else if (s_job == 2)
    {
      const char *r = dm_chat_ai_ask(s_pending);
      snprintf(s_out_ans, sizeof(s_out_ans), "%s", r ? r : "……");
      s_out_local = dm_chat_ai_local_reply() != 0;
      s_out_health = dm_chat_ai_health_reply() != 0;
      if (s_out_local)
        {
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("本地记录仅在设备显示，不上传云端播报",
                        "Local records stay on device; no cloud TTS"));
        }
      else if (dm_chat_ai_health_reply() &&
          !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
        {
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("健康结果仅本地显示，未上传云端播报",
                        "Health result kept local; no cloud TTS"));
        }
      else if (r && r[0] &&
          (dm_chat_ai_remote_ok() ||
           (dm_chat_ai_intent_ok() && dm_wifi_connected())))
        {
          voice_rc = dm_chat_ai_health_reply()
                       ? dm_voice_tts_play_private(r) : dm_voice_tts_play(r);
          if (voice_rc == 2)
            {
              snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                       dm_t("健康结果仅本地显示，未上传云端播报",
                            "Health result kept local; no cloud TTS"));
            }
          else if (voice_rc == 0)
            {
              snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                       dm_chat_ai_intent_ok()
                         ? dm_t("已语音回复本地功能结果", "Local action result spoken")
                         : dm_t("已通过喇叭播放回复", "Reply played on speaker"));
            }
          else
            {
              snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                       dm_t("文字回复已显示，语音播报失败",
                            "Text reply shown; speech playback failed"));
            }
        }
      else if (dm_chat_ai_intent_ok())
        {
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("本地功能已处理，离线时不播报", "Local action handled; no offline speech"));
        }
      else if (r && r[0])
        {
          snprintf(s_out_sub, sizeof(s_out_sub), "%s",
                   dm_t("云端连接失败，当前显示离线回复",
                        "Cloud unavailable; showing offline reply"));
        }
      else
        {
          s_out_sub[0] = '\0';
        }
      __atomic_store_n(&s_out_ready, 1, __ATOMIC_RELEASE);
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
  __atomic_store_n(&s_out_ready, 0, __ATOMIC_RELEASE);
  s_out_ans[0] = '\0';
  s_out_sub[0] = '\0';

  dm_face_set(FACE_THINK, EYE_DECOR_NONE, 10);
  set_ans(job == 1 ? dm_t("识别中…", "Recognizing…")
                   : dm_t("思考中…", "Thinking…"));
  set_ptt("处理中…", "Working…", C_BTN_HI);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_VOICE_NET_WORKER_STACK);
  if (pthread_create(&th, &attr, voice_worker, NULL) == 0)
    {
      pthread_detach(th);
    }
  else
    {
      s_busy = false;
      if (job == 1 && dm_voice_rec_active())
        {
          /* Keep the recording controllable if worker creation failed. */
          s_ui_rec = true;
          set_ans(dm_t("正在录音，稍后再点结束",
                       "Recording; tap stop again"));
          set_ptt("结束并发送", "Stop & send", C_HEART);
        }
      else
        {
          set_ptt("开始说话", "Start talk", C_BTN);
        }
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
  int rec_rc;

  (void)e;

  if (s_busy)
    {
      return;
    }

  if (!s_ui_rec)
    {
      if (!dm_wifi_connected())
        {
          set_ans(dm_t("Wi-Fi 尚未就绪，请先连接网络",
                       "Wi-Fi is not ready; connect first"));
          set_sub(dm_t("连接完成后再开始语音聊天",
                       "Start voice chat after connection completes"));
          set_ptt("开始说话", "Start talk", C_BTN);
          return;
        }
      s_mic_err = 0;
      dm_voice_set_speaker_volume(80);
      rec_rc = dm_voice_rec_start(10);
      if (rec_rc != 0)
        {
          if (rec_rc == -EBUSY)
            {
              set_ans(dm_t("音频设备忙，请稍后再试",
                           "Audio device busy; try again shortly"));
              set_sub(dm_t("等待录音或播放结束", "Wait for recording or playback to finish"));
            }
          else
            {
              set_ans(dm_t("麦克风不可用", "Mic unavailable"));
              set_sub(dm_t("检查 /dev/audio/pcm0c",
                           "Check /dev/audio/pcm0c"));
            }
          set_ptt("开始说话", "Start talk", C_BTN);
          return;
        }
      s_ui_rec = true;
      s_rec_sec = 0;
      s_rec_started_ms = chat_monotonic_ms();
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

      if (s_mic_err)
        {
          set_ans(dm_t("麦克风故障，无法录音", "Mic failed"));
          set_sub(dm_t("检查 /dev/audio/pcm0c", "Check mic device"));
          reset_to_idle();
          return;
        }

      /* Stop/join and network work run in voice_worker, not this LVGL event. */
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
  if (s_last_ans_health && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
    {
      set_ans(dm_t("读取健康记录权限已关闭", "Health record access is disabled"));
      set_sub(dm_t("健康结果已从聊天页隐藏", "Previous health result hidden"));
      s_last_ans_health = false;
    }
  if (s_last_ans_local && !dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA))
    {
      set_ans(dm_t("本地生活数据读取权限已关闭", "Local data access is disabled"));
      set_sub(dm_t("已有内容已从聊天页隐藏", "Previous local result hidden"));
      s_last_ans_local = false;
    }
  /* apply worker results on LVGL thread */
  if (__atomic_load_n(&s_out_ready, __ATOMIC_ACQUIRE))
    {
      __atomic_store_n(&s_out_ready, 0, __ATOMIC_RELEASE);
      if (s_out_local && !dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA))
        {
          snprintf(s_out_ans, sizeof(s_out_ans), "%s", "本地生活数据读取权限已关闭，内容不会显示。");
          snprintf(s_out_sub, sizeof(s_out_sub), "%s", "请到权限管理检查读取开关");
        }
      if (s_out_health && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
        {
          snprintf(s_out_ans, sizeof(s_out_ans), "%s", "读取健康记录权限已关闭，已有健康数据不会显示。");
          snprintf(s_out_sub, sizeof(s_out_sub), "%s", "请到权限管理检查读取开关");
        }
      s_last_ans_local = s_out_local;
      s_last_ans_health = s_out_health;
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
      uint64_t now_ms = chat_monotonic_ms();
      if (s_rec_started_ms != 0 && now_ms >= s_rec_started_ms)
        {
          s_rec_sec = (int)((now_ms - s_rec_started_ms) / 1000);
        }
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
          s_ui_rec = false;
          if (s_rec_sec < 2)
            {
              s_mic_err = 1;
              set_ans(dm_t("麦克风故障，无法录音", "Mic failed"));
              set_sub(dm_t("检查 /dev/audio/pcm0c", "Check mic device"));
              reset_to_idle();
            }
          else
            {
              /* Recording auto-stop reached the configured limit.  Send the
               * captured WAV instead of silently discarding it. */
              kick_job(1);
            }
        }
    }
}

#endif /* CONFIG_DESKMATE_APP */
