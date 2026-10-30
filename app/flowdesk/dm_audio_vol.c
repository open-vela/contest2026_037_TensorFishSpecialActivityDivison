/*
 * dm_audio_vol.c — volume control for 4R 3W speaker
 * Uses NuttX audio FEATURE/VOLUME ioctl (see nxplayer_setvolume),
 * value range 0..1000 on device, 0..100% in UI.
 * Persist to /data/deskmate_vol.txt
 */

#include "deskmate.h"
#include "dm_audio_vol.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <nuttx/audio/audio.h>
#include "media_player.h"
#include "media_defs.h"

#define VOL_PATH "/data/deskmate_vol.txt"
#define VOL_DEFAULT 80

static int s_vol = VOL_DEFAULT;
static int s_mute;

static void vol_save(void)
{
  FILE *f = fopen(VOL_PATH, "w");
  if (!f)
    {
      return;
    }
  fprintf(f, "%d %d\n", s_vol, s_mute);
  fclose(f);
}

void dm_vol_init(void)
{
  FILE *f = fopen(VOL_PATH, "r");
  int v = VOL_DEFAULT;
  int m = 0;

  if (f)
    {
      if (fscanf(f, "%d %d", &v, &m) >= 1)
        {
          if (v < 0)
            {
              v = 0;
            }
          if (v > 100)
            {
              v = 100;
            }
          s_vol = v;
          s_mute = m ? 1 : 0;
        }
      fclose(f);
    }
  dm_vol_apply();
}

int dm_vol_get(void)
{
  return s_vol;
}

int dm_vol_is_mute(void)
{
  return s_mute;
}

int dm_vol_set_mute(int on)
{
  s_mute = on ? 1 : 0;
  dm_vol_apply();
  vol_save();
  return s_mute;
}

void dm_vol_apply(void)
{
  struct audio_caps_desc_s cap;
  int fd;
  int dev_vol;

  if (s_mute)
    {
      dev_vol = 0;
    }
  else
    {
      dev_vol = s_vol * 10; /* 0..100 -> 0..1000 */
      if (dev_vol > 1000)
        {
          dev_vol = 1000;
        }
    }

  fd = open("/dev/audio/pcm0p", O_WRONLY);
  if (fd < 0)
    {
      /* device may be busy; try playback node used by nxplayer */
      fd = open("/dev/audio/pcm0p", O_RDWR);
    }
  if (fd < 0)
    {
      return;
    }

  memset(&cap, 0, sizeof(cap));
  cap.caps.ac_len = sizeof(struct audio_caps_s);
  cap.caps.ac_type = AUDIO_TYPE_FEATURE;
  cap.caps.ac_format.hw = AUDIO_FU_VOLUME;
  cap.caps.ac_controls.hw[0] = (uint16_t)dev_vol;
  (void)ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap);
  close(fd);

  /* also keep TTS helper in sync (percent, 0 when mute) */
  {
    extern void dm_voice_set_speaker_volume(int percent);
    dm_voice_set_speaker_volume(s_mute ? 0 : s_vol);
  }
}

int dm_vol_set(int percent)
{
  if (percent < 0)
    {
      percent = 0;
    }
  if (percent > 100)
    {
      percent = 100;
    }
  s_vol = percent;
  if (s_vol > 0)
    {
      s_mute = 0;
    }
  dm_vol_apply();
  vol_save();
  return s_vol;
}

/* short 440Hz beep ~0.25s at 16kHz mono */
int dm_vol_test_beep(void)
{
  void *mp;

  if (s_mute)
    {
      return -1;
    }

  /* play prebaked tone via media framework (speaker 4ohm 3W) */
  mp = media_player_open(MEDIA_STREAM_MUSIC);
  if (!mp)
    {
      return -1;
    }
  if (media_player_prepare(mp, "/resource/audio/med.wav", NULL) != 0)
    {
      media_player_close(mp, 0);
      return -1;
    }
  (void)media_player_start(mp);
  usleep(400 * 1000);
  (void)media_player_stop(mp);
  (void)media_player_close(mp, 0);
  return 0;
}

#endif /* CONFIG_DESKMATE_APP */
