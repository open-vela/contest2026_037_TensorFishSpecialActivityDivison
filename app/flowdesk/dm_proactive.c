/*
 * dm_proactive.c — proactive notices with OFFLINE fallback
 *
 * Offline: play prebaked WAV under /resource/audio + face/text cue
 * Online:  try cloud TTS for flexible phrases, else same WAV
 */

#include "deskmate.h"
#include "dm_voice.h"
#include "dm_audio_vol.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>

static int s_med_ann_hour = -1;
static int s_water_ann_min = -1;
static int s_brief_ann_day = -1;
static int s_focus_ann = -1;

struct proactive_audio_job
{
  char path[64];
};

static void *proactive_beep_worker(void *arg)
{
  struct proactive_audio_job *job = arg;
  int rc = job ? dm_voice_play_file_sync(job->path) : -1;
  free(job);
  if (rc != 0)
    {
      int attempt;
      for (attempt = 0; attempt < 120; attempt++)
        {
          rc = dm_vol_test_beep();
          if (rc != -EBUSY) break;
          usleep(250000);
        }
    }
  return NULL;
}

static void proactive_beep_async(const char *path)
{
  pthread_t th;
  pthread_attr_t attr;
  struct proactive_audio_job *job = NULL;
  if (path)
    {
      job = malloc(sizeof(*job));
      if (job) snprintf(job->path, sizeof(job->path), "%s", path);
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  if (pthread_create(&th, &attr, proactive_beep_worker, job) == 0)
    {
      pthread_detach(th);
    }
  else free(job);
  pthread_attr_destroy(&attr);
}

/* play local wav via same path as TTS output */
static void say_offline(const char *wav_rel, const char *text)
{
  char path[64];
  FILE *f;

  snprintf(path, sizeof(path), "/resource/audio/%s", wav_rel);
  f = fopen(path, "rb");
  if (!f)
    {
      snprintf(path, sizeof(path), "/data/audio/%s", wav_rel);
      f = fopen(path, "rb");
    }
  if (f)
    {
      fclose(f);
      proactive_beep_async(path);
      if (text) dm_say(text, text);
      return;
    }

  /* Last resort: do not let audio waits block the LVGL timer callback. */
  proactive_beep_async(NULL);
  if (text)
    {
      dm_say(text, text);
    }
}

static void say(const char *wav, const char *text)
{
  /* prefer offline wav so proactive works without network */
  say_offline(wav, text);
}

static int med_due_count(void)
{
  FILE *f = fopen("/data/deskmate_med.txt", "r");
  char line[160];
  int due = 0;
  time_t now = time(NULL);
  struct tm tmv;
  int hour;

  if (!f)
    {
      return 0;
    }
  localtime_r(&now, &tmv);
  hour = tmv.tm_hour;

  while (fgets(line, sizeof(line), f))
    {
      char name[32];
      int times = 0;
      int h0 = 0, h1 = 0, h2 = 0;
      int taken = 0, day = 0;
      if (sscanf(line, "%31[^|]|%d|%d,%d,%d|%d|%d",
                 name, &times, &h0, &h1, &h2, &taken, &day) == 7)
        {
          int today = (tmv.tm_yday + tmv.tm_year * 1000) & 0xffff;
          int hs[3] = { h0, h1, h2 };
          int k;

          if (day != today)
            {
              taken = 0;
            }
          for (k = 0; k < times && k < 3; k++)
            {
              if (!(taken & (1u << k)) && hour >= hs[k] && hour < hs[k] + 2)
                {
                  due++;
                  break;
                }
            }
        }
    }
  fclose(f);
  return due;
}

static int water_due(void)
{
  FILE *f = fopen("/data/deskmate_water.txt", "r");
  int cups = 0, iv = 30;
  long due = 0;
  int day = 0;
  struct tm current;

  if (!f)
    {
      return 0;
    }
  if (fscanf(f, "%d %d %ld %d", &cups, &iv, &due, &day) != 4)
    {
      fclose(f);
      return 0;
    }
  fclose(f);
  {
    time_t now = time(NULL);
    if (!localtime_r(&now, &current) ||
        day != current.tm_yday + current.tm_year * 1000) return 0;
  }
  return (due != 0 && time(NULL) >= (time_t)due) ? 1 : 0;
}

void dm_proactive_tick(void)
{
  time_t now = time(NULL);
  struct tm tmv;
  int n;

  if (!dm_wifi_clock_valid() || !localtime_r(&now, &tmv)) return;

  if (tmv.tm_hour == 8 && tmv.tm_min >= 30 && tmv.tm_min < 35 &&
      s_brief_ann_day != tmv.tm_yday)
    {
      s_brief_ann_day = tmv.tm_yday;
      say("brief.wav", "早上好。今天也要好好照顾自己");
      return;
    }

  n = med_due_count();
  if (n > 0 && tmv.tm_hour != s_med_ann_hour)
    {
      s_med_ann_hour = tmv.tm_hour;
      say("med.wav", "该吃药了");
      return;
    }

  if (water_due() && tmv.tm_min != s_water_ann_min)
    {
      s_water_ann_min = tmv.tm_min;
      say("water.wav", "喝水时间到");
    }
}

void dm_proactive_focus_done(int32_t minutes)
{
  (void)minutes;
  if (s_focus_ann)
    {
      return;
    }
  s_focus_ann = 1;
  say("focus.wav", "专注完成，休息一下");
  s_focus_ann = 0;
}

#endif /* CONFIG_DESKMATE_APP */
