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
#define VOICE_KEY    "tp-s4ivq5cxzz7uke2mh9ms0bqzzci2oartptn4wm3b7608ukon"
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
static int s_rec_fd = -1;
static FILE *s_rec_out;
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

static void wav_write_header(FILE *f, uint32_t data_bytes)
{
  uint32_t byte_rate = REC_RATE * REC_CH * (REC_BITS / 8);
  uint16_t block_align = REC_CH * (REC_BITS / 8);
  uint8_t hdr[44];

  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr, "RIFF", 4);
  {
    uint32_t riff = 36 + data_bytes;
    memcpy(hdr + 4, &riff, 4);
  }
  memcpy(hdr + 8, "WAVEfmt ", 8);
  {
    uint32_t fmt_sz = 16;
    uint16_t fmt = 1;
    uint16_t ch = REC_CH;
    uint32_t rate = REC_RATE;
    uint16_t bits = REC_BITS;
    memcpy(hdr + 16, &fmt_sz, 4);
    memcpy(hdr + 20, &fmt, 2);
    memcpy(hdr + 22, &ch, 2);
    memcpy(hdr + 24, &rate, 4);
    memcpy(hdr + 28, &byte_rate, 4);
    memcpy(hdr + 32, &block_align, 2);
    memcpy(hdr + 34, &bits, 2);
  }
  memcpy(hdr + 36, "data", 4);
  memcpy(hdr + 40, &data_bytes, 4);
  fwrite(hdr, 1, 44, f);
}

static void *rec_thread(void *arg)
{
  int max_sec = (int)(intptr_t)arg;
  char buf[1024];
  size_t got = 0;
  size_t want;

  if (max_sec <= 0)
    {
      max_sec = REC_MAX_SEC;
    }
  if (max_sec > REC_MAX_SEC)
    {
      max_sec = REC_MAX_SEC;
    }
  want = (size_t)max_sec * REC_RATE * REC_CH * (REC_BITS / 8);

  s_rec_fd = open("/dev/audio/pcm0c", O_RDONLY);
  if (s_rec_fd < 0)
    {
      s_rec_active = 0;
      s_st = DM_VOICE_ERR;
      return NULL;
    }

#ifdef AUDIOIOC_CONFIGURE
  {
    struct audio_caps_desc_s cap;
    memset(&cap, 0, sizeof(cap));
    cap.caps.ac_len = sizeof(cap.caps);
    cap.caps.ac_type = AUDIO_TYPE_INPUT;
    cap.caps.ac_channels = REC_CH;
    cap.caps.ac_controls.hw[0] = (uint16_t)REC_RATE;
    cap.caps.ac_controls.b[2] = (uint8_t)REC_BITS;
    (void)ioctl(s_rec_fd, AUDIOIOC_CONFIGURE, &cap);
  }
#endif

  s_rec_out = fopen(WAV_REC, "wb");
  if (!s_rec_out)
    {
      close(s_rec_fd);
      s_rec_fd = -1;
      s_rec_active = 0;
      s_st = DM_VOICE_ERR;
      return NULL;
    }
  wav_write_header(s_rec_out, 0);

  while (!s_rec_stop && got < want)
    {
      ssize_t n = read(s_rec_fd, buf, sizeof(buf));
      if (n <= 0)
        {
          /* fd closed by dm_voice_rec_stop() or device error */
          break;
        }
      fwrite(buf, 1, (size_t)n, s_rec_out);
      got += (size_t)n;
    }

  /* patch sizes */
  if (s_rec_out)
    {
      uint32_t data_bytes = (uint32_t)got;
      uint32_t riff = 36 + data_bytes;
      fseek(s_rec_out, 4, SEEK_SET);
      fwrite(&riff, 4, 1, s_rec_out);
      fseek(s_rec_out, 40, SEEK_SET);
      fwrite(&data_bytes, 4, 1, s_rec_out);
      fclose(s_rec_out);
      s_rec_out = NULL;
    }
  if (s_rec_fd >= 0)
    {
      close(s_rec_fd);
      s_rec_fd = -1;
    }

  s_rec_active = 0;
  s_st = DM_VOICE_IDLE;
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

  /* ask thread to stop, then close device to unblock read() */
  s_rec_stop = 1;
  if (s_rec_fd >= 0)
    {
      int fd = s_rec_fd;
      s_rec_fd = -1;
      close(fd);
    }

  /* short wait only — do not freeze LVGL thread */
  for (i = 0; i < 15 && s_rec_active; i++)
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

/* ---- HTTP helpers ---- */

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
           "curl -sS -m 25 -X POST \"%s%s\" "
           "-H \"Authorization: Bearer %s\" "
           "-H \"Content-Type: application/json\" "
           "-d @%s -o %s",
           VOICE_BASE, path, VOICE_KEY, JSON_REQ, out_path);
  return system(cmd) == 0 ? 0 : -1;
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
           "curl -sS -m 25 -X POST \"%s/audio/transcriptions\" "
           "-H \"Authorization: Bearer %s\" "
           "-F \"file=@%s\" -F \"model=%s\" -o %s",
           VOICE_BASE, VOICE_KEY, wav, ASR_MODEL, TXT_ASR);
  if (system(cmd) != 0)
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
           "curl -sS -m 25 -X POST \"%s/audio/speech\" "
           "-H \"Authorization: Bearer %s\" "
           "-H \"Content-Type: application/json\" "
           "-d @%s -o %s",
           VOICE_BASE, VOICE_KEY, JSON_REQ, wav_path);
  return system(cmd) == 0 ? 0 : -1;
}

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

static int play_wav(const char *path)
{
  char cmd[192];
  int fd;
  int ch = dm_out_get();

  if (ch == 2 || (ch == 0 && dm_out_headset_present()))
    {
      snprintf(cmd, sizeof(cmd),
               "bttool a2dp play %s 2>/dev/null", path);
      if (system(cmd) == 0)
        {
          return 0;
        }
      if (ch == 2)
        {
          return -1;
        }
    }

  snprintf(cmd, sizeof(cmd),
           "amixer set PCM %d%% 2>/dev/null",
           s_speaker_vol);
  (void)system(cmd);

  snprintf(cmd, sizeof(cmd),
           "nxplayer -p %s 2>/dev/null || play %s 2>/dev/null",
           path, path);
  if (system(cmd) == 0)
    {
      return 0;
    }

  {
    FILE *f = fopen(path, "rb");
    char buf[1024];
    size_t n;
    if (!f)
      {
        return -1;
      }
    fd = open("/dev/audio/pcm0p", O_WRONLY);
    if (fd < 0)
      {
        fclose(f);
        return -1;
      }
    fseek(f, 44, SEEK_SET);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
      {
        if (write(fd, buf, n) < 0)
          {
            break;
          }
      }
    close(fd);
    fclose(f);
    return 0;
  }
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
