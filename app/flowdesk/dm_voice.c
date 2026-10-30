/*
 * dm_voice.c — tap-to-talk voice pipeline + speaker playback
 *
 * Flow: tap1 record -> tap2 stop -> ASR -> LLM -> TTS -> speaker
 * Audio nodes: /dev/audio/pcm0c (mic), /dev/audio/pcm0p (4R 3W speaker)
 * Do not modify audio drivers.
 */

#include "deskmate.h"
#include "dm_voice.h"
#include "dm_tts_voice.h"
#include "media_recorder.h"
#include "media_player.h"
#include "media_defs.h"
#include <uv.h>
#include "dm_audio_route.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>

#define VOICE_BASE   "https://token-plan-sgp.xiaomimimo.com/v1"
#define VOICE_KEY    "tp-s8f46wpkkkdpqika5xchqalgg7m632kp0n9wdd9omjlnn64c"
#define ASR_MODEL    "mimo-v2.5-asr"
#define TTS_MODEL    "mimo-v2.5-tts"
#define CHAT_MODEL   "mimo-v2.6-flash"

#define WAV_REC      "/tmp/dm_voice_in.wav"
#define WAV_TTS      "/tmp/dm_voice_out.wav"
#define TXT_ASR      "/tmp/dm_voice_asr.txt"
#define TXT_CHAT     "/tmp/dm_voice_chat.txt"
#define JSON_REQ     "/tmp/dm_voice_req.json"

#define REC_RATE     16000
#define REC_BITS     16
#define REC_CH       1
#define REC_MAX_SEC  15

static volatile dm_voice_state_t s_st = DM_VOICE_IDLE;
static volatile int s_rec_active;
static volatile int s_rec_stop;
static pthread_t s_rec_th;
static int s_speaker_vol = 80; /* percent */

dm_voice_state_t dm_voice_state(void)
{
  return s_st;
}

void dm_voice_reset(void)
{
  s_st = DM_VOICE_IDLE;
  s_rec_active = 0;
  s_rec_stop = 1;
}

int dm_voice_mic_available(void)
{
  int fd = open("/dev/audio/pcm0c", O_RDONLY);
  if (fd < 0)
    {
      return 0;
    }
  close(fd);
  return 1;
}

int dm_voice_rec_active(void)
{
  return s_rec_active;
}

/* ---------- media async (uv loop + proper callbacks) ---------- */

static uv_loop_t s_uv;
static uv_thread_t s_uv_th;
static volatile int s_uv_ready;

static void *s_rec_h;
static void *s_play_h;
static volatile int s_rec_active;
static volatile int s_rec_stop;
static volatile int s_rec_started;
static volatile int s_play_done;
static volatile int s_play_started;
static uv_timer_t s_rec_timer;

static void uv_loop_thread(void *arg)
{
  (void)arg;
  uv_loop_init(&s_uv);
  s_uv_ready = 1;
  uv_run(&s_uv, UV_RUN_DEFAULT);
  uv_loop_close(&s_uv);
  s_uv_ready = 0;
}

static void uv_ensure(void)
{
  int i;
  if (s_uv_ready)
    {
      return;
    }
  if (uv_thread_create(&s_uv_th, uv_loop_thread, NULL) != 0)
    {
      return;
    }
  for (i = 0; i < 50 && !s_uv_ready; i++)
    {
      usleep(10 * 1000);
    }
}

/* ---- recorder (async) ---- */

static void rec_on_start(void *cookie, int ret)
{
  (void)cookie;
  s_rec_started = (ret == 0) ? 1 : -1;
}

static void rec_on_prepare(void *cookie, int ret)
{
  (void)cookie;
  if (ret != 0)
    {
      s_rec_started = -1;
      s_rec_active = 0;
      return;
    }
  media_uv_recorder_start_auto(s_rec_h, MEDIA_SCENARIO_RECORD,
                               rec_on_start, NULL);
}

static void rec_on_open(void *cookie, int ret)
{
  char opts[128];

  (void)cookie;
  if (ret != 0 || !s_rec_h)
    {
      s_rec_started = -1;
      s_rec_active = 0;
      return;
    }
  snprintf(opts, sizeof(opts),
           "format=wav:sample_rate=%d:ch_layout=mono", REC_RATE);
  unlink(WAV_REC);
  media_uv_recorder_prepare(s_rec_h, WAV_REC, opts,
                            NULL, rec_on_prepare, NULL);
}

static void rec_on_stop(void *cookie, int ret)
{
  (void)cookie;
  (void)ret;
  if (s_rec_h)
    {
      media_uv_recorder_close(s_rec_h, NULL);
      s_rec_h = NULL;
    }
  s_rec_active = 0;
  s_st = DM_VOICE_IDLE;
}

static void rec_poll_cb(uv_timer_t *timer)
{
  if (!s_rec_stop && s_rec_started >= 0)
    {
      return;
    }
  uv_timer_stop(timer);
  if (s_rec_h && s_rec_started > 0)
    {
      media_uv_recorder_stop(s_rec_h, rec_on_stop, NULL);
    }
  else
    {
      if (s_rec_h)
        {
          media_uv_recorder_close(s_rec_h, NULL);
          s_rec_h = NULL;
        }
      s_rec_active = 0;
      s_st = DM_VOICE_IDLE;
    }
}

static void *rec_thread(void *arg)
{
  int max_sec = (int)(intptr_t)arg;
  int waited;

  if (max_sec <= 0)
    {
      max_sec = REC_MAX_SEC;
    }

  s_st = DM_VOICE_REC;
  s_rec_started = 0;
  s_rec_stop = 0;
  uv_ensure();

  s_rec_h = media_uv_recorder_open(&s_uv, MEDIA_SOURCE_MIC, rec_on_open, NULL);

  uv_timer_init(&s_uv, &s_rec_timer);
  uv_timer_start(&s_rec_timer, rec_poll_cb, 100, 100);

  waited = 0;
  while (!s_rec_stop && waited < max_sec * 10)
    {
      usleep(100 * 1000);
      waited++;
      if (s_rec_started < 0)
        {
          break;
        }
    }
  s_rec_stop = 1;
  return NULL;
}

int dm_voice_rec_start(int max_seconds)
{
  if (s_rec_active)
    {
      return 0;
    }
  s_rec_stop = 0;
  s_rec_active = 1;
  s_st = DM_VOICE_REC;
  if (pthread_create(&s_rec_th, NULL, rec_thread,
                     (void *)(intptr_t)max_seconds) != 0)
    {
      s_rec_active = 0;
      s_st = DM_VOICE_ERR;
      return -1;
    }
  pthread_detach(s_rec_th);
  return 0;
}

int dm_voice_rec_stop(void)
{
  int i;

  if (!s_rec_active)
    {
      FILE *f = fopen(WAV_REC, "rb");
      if (f)
        {
          fclose(f);
          return 0;
        }
      return -1;
    }

  s_rec_stop = 1;
  for (i = 0; i < 50 && s_rec_active; i++)
    {
      usleep(10 * 1000);
    }

  {
    FILE *f = fopen(WAV_REC, "rb");
    if (!f)
      {
        return -1;
      }
    fclose(f);
    return 0;
  }
}

/* ---- player (async) ---- */

static void play_event_cb(void *cookie, int event, int result, const char *data)
{
  (void)cookie;
  (void)result;
  (void)data;
  if (event == MEDIA_EVENT_COMPLETED || event == MEDIA_EVENT_STOPPED ||
      event == MEDIA_EVENT_NOP)
    {
      s_play_done = 1;
    }
}

static void play_on_start(void *cookie, int ret)
{
  (void)cookie;
  s_play_started = (ret == 0) ? 1 : -1;
  if (s_play_started < 0)
    {
      s_play_done = 1;
    }
}

static void play_on_prepare(void *cookie, int ret)
{
  (void)cookie;
  if (ret != 0)
    {
      s_play_started = -1;
      s_play_done = 1;
      return;
    }
  media_uv_player_start_auto(s_play_h, MEDIA_SCENARIO_TTS, play_on_start, NULL);
}

static void play_on_open(void *cookie, int ret)
{
  (void)cookie;
  if (ret != 0 || !s_play_h)
    {
      s_play_started = -1;
      s_play_done = 1;
      return;
    }
  media_uv_player_listen(s_play_h, play_event_cb);
  media_uv_player_prepare(s_play_h, WAV_TTS, NULL,
                          NULL, play_on_prepare, NULL);
}

static int play_wav(const char *path)
{
  int ch = dm_out_get();
  int i;

  if (ch == 2 && !dm_out_headset_present())
    {
      return -1;
    }

  {
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "amixer set PCM %d%% 2>/dev/null",
             s_speaker_vol);
    (void)system(cmd);
  }

  if (path && strcmp(path, WAV_TTS) != 0)
    {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "cp %s %s 2>/dev/null", path, WAV_TTS);
      (void)system(cmd);
    }

  uv_ensure();
  s_play_done = 0;
  s_play_started = 0;
  s_play_h = media_uv_player_open(&s_uv, MEDIA_STREAM_MUSIC,
                                  play_on_open, NULL);

  for (i = 0; i < 150 && !s_play_done; i++)
    {
      usleep(100 * 1000);
    }

  if (s_play_h)
    {
      media_uv_player_stop(s_play_h, NULL, NULL);
      media_uv_player_close(s_play_h, 0, NULL);
      s_play_h = NULL;
    }
  return 0;
}


/* ---- HTTP / ASR / LLM / TTS (sync, run in worker thread) ---- */

void dm_voice_set_speaker_volume(int percent)
{
  if (percent < 0)
    {
      percent = 0;
    }
  if (percent > 100)
    {
      percent = 100;
    }
  s_speaker_vol = percent;
}

static void json_escape(const char *in, char *out, size_t cap)
{
  size_t o = 0;
  size_t i;
  if (!in || !out || cap < 2)
    {
      if (out && cap)
        {
          out[0] = '\0';
        }
      return;
    }
  for (i = 0; in[i] && o + 8 < cap; i++)
    {
      unsigned char ch = (unsigned char)in[i];
      if (ch == '"' || ch == '\\')
        {
          out[o++] = '\\';
          out[o++] = (char)ch;
        }
      else if (ch == '\n')
        {
          out[o++] = '\\';
          out[o++] = 'n';
        }
      else if (ch >= 0x20)
        {
          out[o++] = (char)ch;
        }
    }
  out[o] = '\0';
}

/* nsh system() strips quotes — run curl from a script file */
static int run_curl_script(const char *line)
{
  FILE *f = fopen("/tmp/dm_curl.sh", "w");
  int rc;
  if (!f)
    {
      return -1;
    }
  fputs("#!/bin/sh\n", f);
  fputs(line, f);
  fputs("\n", f);
  fclose(f);
  rc = system("sh /tmp/dm_curl.sh");
  return rc == 0 ? 0 : -1;
}

static int http_post_json(const char *path, const char *json_body,
                          const char *out_path)
{
  char cmd[1400];
  FILE *f = fopen(JSON_REQ, "w");

  if (!f)
    {
      return -1;
    }
  fwrite(json_body, 1, strlen(json_body), f);
  fclose(f);
  unlink(out_path);
  snprintf(cmd, sizeof(cmd),
           "curl -sS --cacert /resource/etc/ssl/curl/ca-certificates.crt "
           "-m 25 -X POST %s%s "
           "-H 'Authorization: Bearer %s' "
           "-H 'Content-Type: application/json' "
           "-d @%s -o %s",
           VOICE_BASE, path, VOICE_KEY, JSON_REQ, out_path);
  return run_curl_script(cmd);
}

static int asr_wav(const char *wav, char *text, int cap)
{
  char cmd[1200];
  FILE *f;
  long sz;
  char *buf;
  char *p;
  char *q;

  text[0] = '\0';
  unlink(TXT_ASR);
  snprintf(cmd, sizeof(cmd),
           "curl -sS --cacert /resource/etc/ssl/curl/ca-certificates.crt "
           "-m 25 -X POST %s/audio/transcriptions "
           "-H 'Authorization: Bearer %s' "
           "-F file=@%s -F model=%s -o %s",
           VOICE_BASE, VOICE_KEY, wav, ASR_MODEL, TXT_ASR);
  if (run_curl_script(cmd) != 0)
    {
      return -1;
    }

  f = fopen(TXT_ASR, "rb");
  if (!f)
    {
      return -1;
    }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 32 * 1024)
    {
      fclose(f);
      return -1;
    }
  buf = malloc((size_t)sz + 1);
  if (!buf)
    {
      fclose(f);
      return -1;
    }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
      free(buf);
      fclose(f);
      return -1;
    }
  buf[sz] = '\0';
  fclose(f);

  p = strstr(buf, "\"text\"");
  if (p)
    {
      p = strchr(p + 6, '"');
      if (p)
        {
          q = strchr(p + 1, '"');
          if (q)
            {
              size_t n = (size_t)(q - p - 1);
              if (n >= (size_t)cap)
                {
                  n = (size_t)cap - 1;
                }
              memcpy(text, p + 1, n);
              text[n] = '\0';
            }
        }
    }
  if (!text[0])
    {
      strncpy(text, buf, (size_t)cap - 1);
      text[cap - 1] = '\0';
    }
  free(buf);
  return text[0] ? 0 : -1;
}

static int chat_reply(const char *user, char *out, int cap)
{
  char esc[400];
  char body[900];
  char *rsp;
  FILE *f;
  long sz;
  int n;
  char *p;
  char *q;

  out[0] = '\0';
  json_escape(user, esc, sizeof(esc));

  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"system\",\"content\":\"你是openvela语音小助手，口语化简短回答，不超过60字。\"},"
               "{\"role\":\"user\",\"content\":\"%s\"}],"
               "\"temperature\":0.7,\"max_tokens\":120}",
               CHAT_MODEL, esc);
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return -1;
    }
  if (http_post_json("/chat/completions", body, TXT_CHAT) != 0)
    {
      return -1;
    }

  f = fopen(TXT_CHAT, "rb");
  if (!f)
    {
      return -1;
    }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024)
    {
      fclose(f);
      return -1;
    }
  rsp = malloc((size_t)sz + 1);
  if (!rsp)
    {
      fclose(f);
      return -1;
    }
  if (fread(rsp, 1, (size_t)sz, f) != (size_t)sz)
    {
      free(rsp);
      fclose(f);
      return -1;
    }
  rsp[sz] = '\0';
  fclose(f);

  p = strstr(rsp, "\"content\"");
  if (p)
    {
      p = strchr(p + 9, '"');
      if (p)
        {
          q = strchr(p + 1, '"');
          if (q)
            {
              size_t ln = (size_t)(q - p - 1);
              if (ln >= (size_t)cap)
                {
                  ln = (size_t)cap - 1;
                }
              memcpy(out, p + 1, ln);
              out[ln] = '\0';
            }
        }
    }
  free(rsp);
  return out[0] ? 0 : -1;
}

static int tts_synth(const char *text, const char *wav_path)
{
  char esc[400];
  char body[700];
  char cmd[1200];
  FILE *f;
  int n;

  json_escape(text, esc, sizeof(esc));
  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"input\":\"%s\",\"voice\":\"%s\"}",
               TTS_MODEL, esc, dm_voice_profile_api_id());
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return -1;
    }
  f = fopen(JSON_REQ, "w");
  if (!f)
    {
      return -1;
    }
  fwrite(body, 1, (size_t)n, f);
  fclose(f);

  unlink(wav_path);
  snprintf(cmd, sizeof(cmd),
           "curl -sS --cacert /resource/etc/ssl/curl/ca-certificates.crt "
           "-m 25 -X POST %s/audio/speech "
           "-H 'Authorization: Bearer %s' "
           "-H 'Content-Type: application/json' "
           "-d @%s -o %s",
           VOICE_BASE, VOICE_KEY, JSON_REQ, wav_path);
  return run_curl_script(cmd);
}

int dm_voice_asr_chat_tts(const char *wav_path, char *reply, int cap)
{
  char text[256];

  if (!wav_path || !reply || cap < 8)
    {
      return -1;
    }
  reply[0] = '\0';

  s_st = DM_VOICE_ASR;
  if (asr_wav(wav_path, text, sizeof(text)) != 0)
    {
      s_st = DM_VOICE_ERR;
      return -1;
    }

  s_st = DM_VOICE_CHAT;
  if (chat_reply(text, reply, cap) != 0)
    {
      strncpy(reply, "（网络忙，没听清）", (size_t)cap - 1);
      reply[cap - 1] = '\0';
      s_st = DM_VOICE_ERR;
      return -1;
    }

  /* show text first, then speak on 4R 3W speaker */
  s_st = DM_VOICE_TTS;
  if (tts_synth(reply, WAV_TTS) == 0)
    {
      s_st = DM_VOICE_PLAY;
      play_wav(WAV_TTS);
    }
  s_st = DM_VOICE_IDLE;
  return 0;
}

int dm_voice_tts_play(const char *text)
{
  if (!text || !text[0])
    {
      return -1;
    }
  s_st = DM_VOICE_TTS;
  if (tts_synth(text, WAV_TTS) != 0)
    {
      s_st = DM_VOICE_ERR;
      return -1;
    }
  s_st = DM_VOICE_PLAY;
  play_wav(WAV_TTS);
  s_st = DM_VOICE_IDLE;
  return 0;
}


/* play a local wav file (offline alerts) */
int dm_voice_play_file(const char *wav_path)
{
  if (!wav_path || !wav_path[0])
    {
      return -1;
    }
  return play_wav(wav_path);
}

#endif /* CONFIG_DESKMATE_APP */
