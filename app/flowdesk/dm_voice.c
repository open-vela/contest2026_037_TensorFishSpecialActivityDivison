/*
 * dm_voice.c — tap-to-talk voice pipeline + speaker playback
 *
 * Flow: tap1 record -> tap2 stop -> ASR -> LLM -> TTS -> speaker
 * Audio nodes: /dev/audio/pcm0c (mic), /dev/audio/pcm0p (4R 3W speaker)
 * Do not modify audio drivers.
 */

#include "deskmate.h"
#include "dm_ai_permissions.h"
#include "dm_voice.h"
#include "dm_tts_voice.h"
#include <system/nxplayer.h>
#include <system/nxrecorder.h>
#include "dm_audio_route.h"
#include "dm_audio_vol.h"

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
#include <sys/stat.h>
#include <syslog.h>
#include <nuttx/audio/audio.h>
#include <netutils/cJSON.h>

#define VOICE_BASE   "https://token-plan-sgp.xiaomimimo.com/v1"
#define MIMO_KEY_PATH "/data/deskmate_mimo.key"
#define MIMO_CURL_CONFIG "/tmp/dm_voice_mimo.conf"
#define ASR_MODEL    "mimo-v2.5-asr"
#define TTS_MODEL    "mimo-v2.5-tts"
#define CHAT_MODEL   "mimo-v2.6-flash"

#define WAV_REC      "/tmp/dm_voice_in.wav"
#define WAV_ASR      "/tmp/dm_voice_asr_input.wav"
#define WAV_TTS      "/tmp/dm_voice_out.wav"
#define WAV_VOL_TEST_FMT "/data/deskmate_volume_test_%d.wav"
#define WAV_PREVIEW_BINGTANG "/resource/audio/voice_bingtang.wav"
#define WAV_PREVIEW_JASMINE  "/resource/audio/voice_jasmine.wav"
#define WAV_PREVIEW_SODA     "/resource/audio/voice_soda.wav"
#define PCM_RAW      "/tmp/dm_play.raw"
#define TXT_ASR      "/tmp/dm_voice_asr.txt"
#define TXT_CHAT     "/tmp/dm_voice_chat.txt"
#define JSON_REQ     "/tmp/dm_voice_req.json"

#define REC_RATE     8000
#define REC_BITS     16
#define REC_CH       1
#define REC_MAX_SEC  600
#define VOICE_MAX_RECORD_SEC 600
#define VOICE_MAX_RECORD_BYTES \
  ((uint32_t)REC_RATE * REC_CH * REC_BITS / 8 * VOICE_MAX_RECORD_SEC)
#define VOICE_WIFI_WAIT_SEC 20
#define MEETING_ASR_SEGMENT_SEC 20
#define MEETING_ASR_SEGMENT_BYTES \
  ((uint32_t)REC_RATE * REC_CH * REC_BITS / 8 * MEETING_ASR_SEGMENT_SEC)
#define MEETING_CHUNK_WAV "/tmp/dm_meeting_asr_chunk.wav"
#define MEETING_CHAT_RSP "/tmp/dm_meeting_chat_rsp.json"
#define MEETING_SUMMARY_MODEL CHAT_MODEL

static volatile dm_voice_state_t s_st = DM_VOICE_IDLE;
static volatile int s_rec_active;
static pthread_mutex_t s_rec_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_audio_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_voice_api_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_meeting_progress_done;
static int s_meeting_progress_total;
static pthread_mutex_t s_meeting_progress_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_speaker_vol = 80; /* percent */
static volatile dm_voice_failure_t s_last_failure = DM_VOICE_FAILURE_NONE;
static int s_health_reply;

int dm_voice_health_reply(void)
{
  return s_health_reply;
}

dm_voice_failure_t dm_voice_last_failure(void)
{
  return s_last_failure;
}

dm_voice_state_t dm_voice_state(void)
{
  return s_st;
}

void dm_voice_reset(void)
{
  (void)dm_voice_rec_stop();
  s_st = DM_VOICE_IDLE;
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
  int active;

  /* This is polled by the LVGL tick; do not wait behind recorder stop/join. */
  if (pthread_mutex_trylock(&s_rec_lock) != 0)
    {
      return 1;
    }
  active = s_rec_active;
  pthread_mutex_unlock(&s_rec_lock);
  return active;
}

/* ---------- recorder: nxrecorder (buffer-queue API) ----------
 * plain read() on /dev/audio/pcm0c is a no-op on this driver (no .read
 * op), only the nxrecorder buffer-queue path actually captures audio.
 */

static struct nxrecorder_s *s_rec;
static uint32_t s_rec_generation;

struct rec_auto_stop_arg_s
{
  int max_sec;
  uint32_t generation;
};

static int dm_voice_rec_stop_generation(uint32_t generation, int check_generation);

static void *rec_auto_stop(void *arg)
{
  struct rec_auto_stop_arg_s *timer = arg;
  int max_sec = timer->max_sec;
  uint32_t generation = timer->generation;

  free(timer);
  sleep(max_sec > 0 ? max_sec : REC_MAX_SEC);
  (void)dm_voice_rec_stop_generation(generation, 1);
  return NULL;
}

int dm_voice_rec_start(int max_seconds)
{
  struct rec_auto_stop_arg_s *timer;
  pthread_t th;
  pthread_attr_t attr;
  uint32_t generation;
  int ret;

  if (max_seconds <= 0)
    {
      max_seconds = REC_MAX_SEC;
    }
  if (max_seconds > VOICE_MAX_RECORD_SEC)
    {
      max_seconds = VOICE_MAX_RECORD_SEC;
    }

  pthread_mutex_lock(&s_rec_lock);
  if (s_rec_active)
    {
      pthread_mutex_unlock(&s_rec_lock);
      return -EBUSY;
    }

  /* Do not contend for the audio DMA while playback is active.  Use
   * trylock so a UI click never waits behind a long speaker operation. */
  if (pthread_mutex_trylock(&s_audio_lock) != 0)
    {
      pthread_mutex_unlock(&s_rec_lock);
      return -EBUSY;
    }

  unlink(WAV_REC);
  s_rec = nxrecorder_create();
  if (!s_rec)
    {
      pthread_mutex_unlock(&s_audio_lock);
      pthread_mutex_unlock(&s_rec_lock);
      s_st = DM_VOICE_ERR;
      return -1;
    }
  ret = nxrecorder_setdevice(s_rec, "/dev/audio/pcm0c");
  if (ret != OK)
    {
      syslog(LOG_ERR, "[dm-voice] set microphone device failed: %d\n", ret);
      nxrecorder_release(s_rec);
      s_rec = NULL;
      pthread_mutex_unlock(&s_audio_lock);
      pthread_mutex_unlock(&s_rec_lock);
      s_st = DM_VOICE_ERR;
      return ret;
    }

  ret = nxrecorder_recordinternal(s_rec, WAV_REC, AUDIO_FMT_PCM,
                                  REC_CH, REC_BITS, REC_RATE, 1);
  if (ret != OK)
    {
      syslog(LOG_ERR,
             "[dm-voice] recorder start failed: %d (rate=%d ch=%d bits=%d)\n",
             ret, REC_RATE, REC_CH, REC_BITS);
      nxrecorder_release(s_rec);
      s_rec = NULL;
      pthread_mutex_unlock(&s_audio_lock);
      pthread_mutex_unlock(&s_rec_lock);
      s_st = DM_VOICE_ERR;
      return ret;
    }

  s_rec_generation++;
  if (s_rec_generation == 0)
    {
      s_rec_generation++;
    }
  generation = s_rec_generation;
  s_rec_active = 1;
  s_st = DM_VOICE_REC;
  pthread_mutex_unlock(&s_audio_lock);
  pthread_mutex_unlock(&s_rec_lock);

  timer = malloc(sizeof(*timer));
  if (!timer)
    {
      (void)dm_voice_rec_stop_generation(generation, 1);
      return -ENOMEM;
    }
  timer->max_sec = max_seconds;
  timer->generation = generation;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  ret = pthread_create(&th, &attr, rec_auto_stop, timer);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      free(timer);
      (void)dm_voice_rec_stop_generation(generation, 1);
      return -ret;
    }
  pthread_detach(th);
  return 0;
}

static int dm_voice_rec_stop_generation(uint32_t generation,
                                        int check_generation)
{
  FILE *f;
  long size;

  pthread_mutex_lock(&s_rec_lock);
  if (check_generation &&
      (!s_rec_active || generation != s_rec_generation))
    {
      pthread_mutex_unlock(&s_rec_lock);
      return 0;
    }

  if (s_rec_active)
    {
      /* Stop/join can wait for the NuttX audio worker.  Keep this off the
       * LVGL event thread and hold the audio gate until the recorder releases
       * its DMA/session resources. */
      pthread_mutex_lock(&s_audio_lock);
      if (s_rec)
        {
          (void)nxrecorder_stop(s_rec);
          nxrecorder_release(s_rec);
          s_rec = NULL;
        }
      s_rec_active = 0;
      s_st = DM_VOICE_IDLE;
      pthread_mutex_unlock(&s_audio_lock);
    }

  /* Keep the recorder lock until the completed file has been checked. This
   * prevents a new recording from truncating WAV_REC during validation. */
  f = fopen(WAV_REC, "rb");
  if (!f)
    {
      pthread_mutex_unlock(&s_rec_lock);
      syslog(LOG_ERR, "[dm-voice] recording file missing\n");
      s_st = DM_VOICE_ERR;
      return -1;
    }

  if (fseek(f, 0, SEEK_END) != 0)
    {
      fclose(f);
      pthread_mutex_unlock(&s_rec_lock);
      s_st = DM_VOICE_ERR;
      return -1;
    }

  size = ftell(f);
  fclose(f);
  if (size <= 44)
    {
      pthread_mutex_unlock(&s_rec_lock);
      syslog(LOG_ERR, "[dm-voice] recording has no audio payload (bytes=%ld)\n",
             size);
      s_st = DM_VOICE_ERR;
      return -1;
    }

  /* nxrecorder writes raw PCM for AUDIO_FMT_PCM; report only sizes/format
   * metadata so we can distinguish a capture failure from an API failure. */
  syslog(LOG_INFO, "[dm-voice] captured PCM bytes=%ld rate=%d ch=%d bits=%d\n",
         size, REC_RATE, REC_CH, REC_BITS);
  pthread_mutex_unlock(&s_rec_lock);
  return 0;
}

int dm_voice_rec_stop(void)
{
  return dm_voice_rec_stop_generation(0, 0);
}



/* ---- player: nxplayer (buffer-queue API) ---- */

/* The audio driver owns a small number of DMA channels.  Serialize all
 * playback and recording setup/teardown so the app never starts a new stream
 * while an earlier stream is still releasing its buffers. */

/* Parse a PCM WAV and normalize it to the board's reliable playback format.
 * The R528 speaker path is known to work at 16 kHz, 16-bit stereo. In
 * particular, do not send 8 kHz mono recorder data or 24 kHz mono preview
 * files directly to the speaker endpoint. */
#define SPEAKER_RATE 16000
#define WAV_INPUT_MAX (12 * 1024 * 1024)

static uint16_t voice_get_le16(const uint8_t *p)
{
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t voice_get_le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int voice_wav_find_data(FILE *f, uint16_t *channels, uint32_t *rate,
                               uint16_t *bits, long *data_offset,
                               uint32_t *data_size)
{
  uint8_t riff[12];
  uint8_t chunk[8];
  uint8_t fmt[16];
  long file_size;
  long offset = 12;
  int have_fmt = 0;
  int have_data = 0;

  if (fread(riff, 1, sizeof(riff), f) != sizeof(riff) ||
      memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0 ||
      fseek(f, 0, SEEK_END) != 0 || (file_size = ftell(f)) < 44)
    {
      return -1;
    }

  while (offset <= file_size - 8)
    {
      uint32_t chunk_size;
      long payload_offset;

      if (fseek(f, offset, SEEK_SET) != 0 ||
          fread(chunk, 1, sizeof(chunk), f) != sizeof(chunk))
        {
          return -1;
        }
      chunk_size = voice_get_le32(chunk + 4);
      payload_offset = offset + 8;
      if (payload_offset > file_size ||
          chunk_size > (uint32_t)(file_size - payload_offset))
        {
          return -1;
        }

      if (memcmp(chunk, "fmt ", 4) == 0)
        {
          if (chunk_size < sizeof(fmt) ||
              fseek(f, payload_offset, SEEK_SET) != 0 ||
              fread(fmt, 1, sizeof(fmt), f) != sizeof(fmt) ||
              voice_get_le16(fmt) != 1)
            {
              return -1;
            }
          *channels = voice_get_le16(fmt + 2);
          *rate = voice_get_le32(fmt + 4);
          *bits = voice_get_le16(fmt + 14);
          have_fmt = 1;
        }
      else if (memcmp(chunk, "data", 4) == 0)
        {
          *data_offset = payload_offset;
          *data_size = chunk_size;
          have_data = 1;
        }

      if (have_fmt && have_data)
        {
          return 0;
        }

      offset = payload_offset + (long)chunk_size + (chunk_size & 1u);
    }

  return -1;
}

static int play_wav(const char *path)
{
  const char *src = (path && path[0]) ? path : WAV_TTS;
  struct nxplayer_s *p = NULL;
  FILE *f = NULL;
  FILE *o = NULL;
  uint8_t in_frame[4];
  uint8_t outbuf[4096];
  size_t out_used = 0;
  uint16_t ch = 0;
  uint16_t bits = 0;
  uint32_t rate = 0;
  uint32_t data_size = 0;
  uint64_t phase = 0;
  uint64_t out_frames = 0;
  uint64_t source_frames;
  long data_offset = 0;
  uint32_t i;
  int rc = -1;
  const char *failed_at = "lock";

  if (pthread_mutex_trylock(&s_audio_lock) != 0)
    {
      return -EBUSY;
    }
  if (s_rec_active)
    {
      pthread_mutex_unlock(&s_audio_lock);
      return -EBUSY;
    }

  failed_at = "open WAV";
  f = fopen(src, "rb");
  if (!f)
    {
      syslog(LOG_ERR, "[dm-voice] playback cannot open WAV errno=%d\n", errno);
      goto out;
    }
  if (voice_wav_find_data(f, &ch, &rate, &bits, &data_offset,
                          &data_size) != 0 ||
      (ch != 1 && ch != 2) || (bits != 8 && bits != 16) ||
      rate < 8000 || rate > 48000)
    {
      syslog(LOG_ERR, "[dm-voice] playback invalid WAV format\n");
      goto out;
    }

  source_frames = data_size / ((uint32_t)ch * (bits / 8));
  if (source_frames == 0 || data_size > WAV_INPUT_MAX ||
      fseek(f, data_offset, SEEK_SET) != 0)
    {
      syslog(LOG_ERR, "[dm-voice] playback invalid WAV payload bytes=%u\n",
             data_size);
      goto out;
    }

  failed_at = "create normalized PCM";
  o = fopen(PCM_RAW, "wb");
  if (!o)
    {
      syslog(LOG_ERR, "[dm-voice] playback cannot create PCM errno=%d\n",
             errno);
      goto out;
    }

  /* Nearest-neighbour resampling is sufficient for speech previews and avoids
   * pulling a large resampler into the small embedded app. Mix stereo inputs
   * to mono, then duplicate to left/right at a fixed 16 kHz output rate. */
  for (i = 0; i < source_frames; i++)
    {
      int32_t sample;

      if (fread(in_frame, 1, (size_t)ch * (bits / 8), f) !=
          (size_t)ch * (bits / 8))
        {
          syslog(LOG_ERR, "[dm-voice] playback WAV read failed frame=%u\n", i);
          goto out;
        }

      if (bits == 16)
        {
          int32_t left = (int16_t)voice_get_le16(in_frame);
          sample = left;
          if (ch == 2)
            {
              int32_t right = (int16_t)voice_get_le16(in_frame + 2);
              sample = (left + right) / 2;
            }
        }
      else
        {
          int32_t left = ((int32_t)in_frame[0] - 128) * 256;
          sample = left;
          if (ch == 2)
            {
              int32_t right = ((int32_t)in_frame[1] - 128) * 256;
              sample = (left + right) / 2;
            }
        }

      phase += SPEAKER_RATE;
      while (phase >= rate)
        {
          uint16_t encoded = (uint16_t)(int16_t)sample;
          if (out_used + 4 > sizeof(outbuf))
            {
              if (fwrite(outbuf, 1, out_used, o) != out_used)
                {
                  syslog(LOG_ERR, "[dm-voice] playback PCM write failed\n");
                  goto out;
                }
              out_used = 0;
            }
          outbuf[out_used++] = (uint8_t)encoded;
          outbuf[out_used++] = (uint8_t)(encoded >> 8);
          outbuf[out_used++] = (uint8_t)encoded;
          outbuf[out_used++] = (uint8_t)(encoded >> 8);
          out_frames++;
          phase -= rate;
        }
    }

  if (out_used > 0 && fwrite(outbuf, 1, out_used, o) != out_used)
    {
      syslog(LOG_ERR, "[dm-voice] playback final PCM write failed\n");
      goto out;
    }
  if (fclose(o) != 0)
    {
      o = NULL;
      syslog(LOG_ERR, "[dm-voice] playback PCM close failed\n");
      goto out;
    }
  o = NULL;
  fclose(f);
  f = NULL;

  failed_at = "create player";
  p = nxplayer_create();
  if (!p)
    {
      syslog(LOG_ERR, "[dm-voice] playback nxplayer_create failed\n");
      goto out;
    }
  if (nxplayer_setdevice(p, "/dev/audio/pcm0p") != OK)
    {
      syslog(LOG_ERR, "[dm-voice] playback cannot select speaker device\n");
      goto out;
    }

  /* Re-apply the app's persisted codec gain before starting a new audio
   * session. Some board configurations also expose nxplayer's volume ioctl. */
  dm_vol_apply();
#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
  if (nxplayer_setvolume(p, (uint16_t)(s_speaker_vol * 10)) != OK)
    {
      syslog(LOG_WARNING, "[dm-voice] nxplayer volume ioctl unavailable\n");
    }
#endif
  syslog(LOG_INFO,
         "[dm-voice] playback start source=%uHz/%uch/%ubit output=16000Hz/2ch/16bit bytes=%llu\n",
         (unsigned int)rate, (unsigned int)ch, (unsigned int)bits,
         (unsigned long long)(out_frames * 4));
  failed_at = "start playback";
  rc = nxplayer_playraw(p, PCM_RAW, AUDIO_FMT_PCM, 0, 2, 16,
                        SPEAKER_RATE, 0);
  if (rc != OK)
    {
      syslog(LOG_ERR, "[dm-voice] nxplayer_playraw failed rc=%d errno=%d\n",
             rc, errno);
      goto out;
    }

  /* nxplayer_playraw() returns once its worker starts. Hold the audio gate
   * through the normalized PCM duration and join before another stream opens. */
  {
    uint64_t play_ms = out_frames * 1000 / SPEAKER_RATE + 200;
    /* Permit a full ten-minute recording plus playback completion margin. */
    if (play_ms > 605000)
      {
        play_ms = 605000;
      }
    while (play_ms >= 1000)
      {
        sleep(1);
        play_ms -= 1000;
      }
    if (play_ms > 0)
      {
        usleep((useconds_t)(play_ms * 1000));
      }
  }

  (void)nxplayer_stop(p);
  rc = 0;

out:
  if (o)
    {
      fclose(o);
    }
  if (f)
    {
      fclose(f);
    }
  if (p)
    {
      nxplayer_release(p);
    }
  unlink(PCM_RAW);
  if (rc != 0)
    {
      syslog(LOG_WARNING, "[dm-voice] playback failed at %s\n", failed_at);
    }
  pthread_mutex_unlock(&s_audio_lock);
  return rc;
}

/* Play one short 440Hz test tone.  This is synchronous by design; callers
 * run it on a worker so neither LVGL nor the recorder UI can block. */
int dm_voice_beep(void)
{
  struct nxplayer_s *p = NULL;
  int rc = -1;

  if (pthread_mutex_trylock(&s_audio_lock) != 0)
    {
      return -EBUSY;
    }
  if (s_rec_active)
    {
      pthread_mutex_unlock(&s_audio_lock);
      return -EBUSY;
    }

  p = nxplayer_create();
  if (!p || nxplayer_setdevice(p, "/dev/audio/pcm0p") != OK)
    {
      syslog(LOG_ERR, "[dm-voice] beep cannot open speaker player\n");
      goto out;
    }

  dm_vol_apply();
#ifndef CONFIG_AUDIO_EXCLUDE_VOLUME
  (void)nxplayer_setvolume(p, (uint16_t)(s_speaker_vol * 10));
#endif
  if (nxplayer_playtone(p, SPEAKER_RATE, 440, 1) != OK)
    {
      syslog(LOG_ERR, "[dm-voice] nxplayer_playtone failed errno=%d\n", errno);
      goto out;
    }

  /* Tone playback is asynchronous; wait slightly beyond its duration and
   * join it before allowing the next audio operation. */
  usleep(1150 * 1000);
  (void)nxplayer_stop(p);
  rc = 0;

out:
  if (p)
    {
      nxplayer_release(p);
    }
  pthread_mutex_unlock(&s_audio_lock);
  return rc;
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

static int json_escape(const char *in, char *out, size_t cap)
{
  size_t o = 0;
  size_t i;

  if (!in || !out || cap < 2)
    {
      if (out && cap)
        {
          out[0] = '\0';
        }
      return -1;
    }

  for (i = 0; in[i]; i++)
    {
      unsigned char ch = (unsigned char)in[i];
      size_t need = (ch == '"' || ch == '\\' || ch == '\n') ? 2 :
                    (ch >= 0x20 ? 1 : 0);

      if (o + need >= cap)
        {
          out[o] = '\0';
          return -1;
        }
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
  return (int)o;
}

/* Load and validate the key supplied at runtime. Never log or compile it. */
static int voice_key_load(char *key, size_t key_cap)
{
  FILE *f;
  size_t i;
  size_t n;
  int valid = 0;

  if (!key || key_cap < 16)
    {
      return 0;
    }
  key[0] = '\0';
  f = fopen(MIMO_KEY_PATH, "rb");
  if (!f)
    {
      return 0;
    }

  if (fgets(key, (int)key_cap, f))
    {
      n = strlen(key);
      while (n > 0 && (key[n - 1] == '\r' || key[n - 1] == '\n' ||
                       key[n - 1] == ' ' || key[n - 1] == '\t'))
        {
          key[--n] = '\0';
        }
      valid = n > 8 && n < key_cap - 1;
      for (i = 0; valid && i < n; i++)
        {
          unsigned char ch = (unsigned char)key[i];
          if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                ch == '.'))
            {
              valid = 0;
            }
        }
    }

  fclose(f);
  if (!valid)
    {
      memset(key, 0, key_cap);
    }
  return valid;
}

/* Put the complete curl request in curl's config file. NuttX NSH has a small
 * command argument limit, so passing every curl option through system() makes
 * the shell split one logical request into too many arguments. The command
 * below deliberately contains only: curl -K <config>. The API key stays out
 * of argv and the temporary config is removed after the request. */
static int run_curl_command(const char *url, const char *request_path,
                            const char *out_path, int timeout_sec)
{
  char key[128];
  FILE *kf;
  int config_ok;
  int close_rc;
  int rc;
  int saved_errno;

  if (!url || !request_path || !out_path || timeout_sec < 1 ||
      timeout_sec > 120 || strchr(url, '"') || strchr(url, '\\') ||
      strchr(url, '\r') || strchr(url, '\n') || strchr(request_path, '"') ||
      strchr(request_path, '\\') || strchr(request_path, '\r') ||
      strchr(request_path, '\n') || strchr(out_path, '"') ||
      strchr(out_path, '\\') || strchr(out_path, '\r') ||
      strchr(out_path, '\n') || !voice_key_load(key, sizeof(key)))
    {
      s_last_failure = DM_VOICE_FAILURE_API_KEY;
      syslog(LOG_ERR, "[dm-voice] invalid curl request configuration\n");
      return -1;
    }

  kf = fopen(MIMO_CURL_CONFIG, "w");
  if (!kf)
    {
      syslog(LOG_ERR, "[dm-voice] cannot create curl config errno=%d\n", errno);
      memset(key, 0, sizeof(key));
      return -1;
    }
  if (chmod(MIMO_CURL_CONFIG, S_IRUSR | S_IWUSR) != 0)
    {
      /* NuttX tmpfs may not implement chmod (ENOSYS); do not block a request. */
      syslog(LOG_WARNING,
             "[dm-voice] chmod unavailable for curl config errno=%d; continuing\n",
             errno);
    }

  config_ok = fputs("silent\nshow-error\nfail\n",
                    kf) != EOF &&
              fprintf(kf, "max-time = %d\n", timeout_sec) > 0 &&
              fprintf(kf, "url = \"%s\"\n", url) > 0 &&
              fputs("cacert = \"/resource/etc/ssl/curl/ca-certificates.crt\"\n"
                    "request = \"POST\"\n"
                    "header = \"Content-Type: application/json\"\n",
                    kf) != EOF &&
              fprintf(kf, "header = \"Authorization: Bearer %s\"\n",
                      key) > 0 &&
              fprintf(kf, "data = \"@%s\"\noutput = \"%s\"\n",
                      request_path, out_path) > 0 &&
              fputs("write-out = \"HTTP_STATUS:%{http_code}\\n\"\n",
                    kf) != EOF;
  close_rc = fclose(kf);
  memset(key, 0, sizeof(key));
  if (!config_ok || close_rc != 0)
    {
      syslog(LOG_ERR, "[dm-voice] cannot write curl config\n");
      unlink(MIMO_CURL_CONFIG);
      return -1;
    }

  errno = 0;
  rc = system("curl -K " MIMO_CURL_CONFIG);
  saved_errno = errno;
  unlink(MIMO_CURL_CONFIG);
  if (rc != 0)
    {
      syslog(LOG_ERR,
             "[dm-voice] curl command failed system_status=%d errno=%d\n",
             rc, saved_errno);
      return -1;
    }
  return 0;
}


static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const unsigned char *in, size_t inlen, char *out,
                      size_t outcap)
{
  size_t i = 0;
  size_t o = 0;

  if (!in || !out || outcap < 2 || inlen > (outcap - 1) / 4 * 3)
    {
      return -1;
    }

  while (i < inlen)
    {
      size_t remain = inlen - i;
      unsigned int v = (unsigned int)in[i++] << 16;
      if (remain > 1)
        {
          v |= (unsigned int)in[i++] << 8;
        }
      if (remain > 2)
        {
          v |= (unsigned int)in[i++];
        }
      out[o++] = b64tab[(v >> 18) & 63];
      out[o++] = b64tab[(v >> 12) & 63];
      out[o++] = remain > 1 ? b64tab[(v >> 6) & 63] : '=';
      out[o++] = remain > 2 ? b64tab[v & 63] : '=';
    }
  out[o] = '\0';
  return (int)o;
}

static int b64_decode(const char *in, unsigned char *out, size_t outcap)
{
  const char *p;
  unsigned int val = 0;
  int bits = 0;
  size_t o = 0;
  size_t symbols = 0;
  size_t padding = 0;
  int ended = 0;

  if (!in || !out)
    {
      return -1;
    }

  /* Some compatible gateways return a data URL instead of bare Base64. */
  if (strncmp(in, "data:", 5) == 0)
    {
      const char *comma = strchr(in, ',');
      const char *base64 = strstr(in, ";base64");
      if (!comma || !base64 || base64 > comma)
        {
          return -1;
        }
      in = comma + 1;
    }

  for (; *in; in++)
    {
      unsigned char ch = (unsigned char)*in;
      if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
        {
          continue;
        }
      if (ch == '=')
        {
          ended = 1;
          padding++;
          if (padding > 2)
            {
              return -1;
            }
          continue;
        }
      if (ended)
        {
          return -1;
        }

      p = strchr(b64tab, ch);
      if (!p)
        {
          return -1;
        }
      symbols++;
      val = (val << 6) | (unsigned int)(p - b64tab);
      bits += 6;
      if (bits >= 8)
        {
          bits -= 8;
          if (o >= outcap)
            {
              return -1;
            }
          out[o++] = (unsigned char)((val >> bits) & 0xff);
          val &= bits ? ((1u << bits) - 1u) : 0u;
        }
    }

  if (symbols == 0 || symbols % 4 == 1 ||
      (padding == 1 && symbols % 4 != 3) ||
      (padding == 2 && symbols % 4 != 2) ||
      (padding > 0 && (symbols + padding) % 4 != 0) ||
      (bits != 0 && bits != 2 && bits != 4) || val != 0)
    {
      return -1;
    }
  return (int)o;
}

/* The chat UI can become available before boot auto-connect finishes.  Let
 * the worker wait briefly for the app's verified WLAN/IP state before trying
 * a cloud request; never block the LVGL thread. */
static int voice_wait_for_wifi(void)
{
  int i;

  for (i = 0; i <= VOICE_WIFI_WAIT_SEC; i++)
    {
      if (dm_wifi_connected())
        {
          return 0;
        }
      if (i < VOICE_WIFI_WAIT_SEC)
        {
          sleep(1);
        }
    }
  return -1;
}

static int http_post_json_checked(const char *path, const char *json_body,
                                  const char *out_path, bool health_required)
{
  char url[160];
  FILE *f;
  size_t body_len;
  int write_ok;
  int close_rc;
  int url_len;
  int rc;

  if (!path || !json_body || !out_path)
    {
      return -1;
    }

  if (!dm_wifi_connected())
    {
      syslog(LOG_INFO, "[dm-voice] waiting up to %d seconds for Wi-Fi\n",
             VOICE_WIFI_WAIT_SEC);
      if (voice_wait_for_wifi() != 0)
        {
          s_last_failure = DM_VOICE_FAILURE_WIFI;
          syslog(LOG_WARNING, "[dm-voice] Wi-Fi did not become ready\n");
          return -1;
        }
    }
  if (dm_wifi_wait_time_sync(40) != 0)
    {
      s_last_failure = DM_VOICE_FAILURE_TIME;
      syslog(LOG_WARNING, "[dm-voice] secure request deferred: clock not ready\n");
      return -1;
    }

  /* Wi-Fi / clock waits may outlive the permission that allowed this body. */
  if (health_required && !dm_ai_permission_allowed(DM_AI_READ_HEALTH)) return -2;
  f = fopen(JSON_REQ, "w");
  if (!f)
    {
      s_last_failure = DM_VOICE_FAILURE_CHAT;
      syslog(LOG_ERR, "[dm-voice] cannot create HTTP request body\n");
      return -1;
    }
  body_len = strlen(json_body);
  write_ok = fwrite(json_body, 1, body_len, f) == body_len;
  close_rc = fclose(f);
  if (!write_ok || close_rc != 0)
    {
      s_last_failure = DM_VOICE_FAILURE_CHAT;
      syslog(LOG_ERR, "[dm-voice] cannot write HTTP request body\n");
      return -1;
    }
  unlink(out_path);
  url_len = snprintf(url, sizeof(url), "%s%s", VOICE_BASE, path);
  if (url_len <= 0 || url_len >= (int)sizeof(url))
    {
      s_last_failure = DM_VOICE_FAILURE_CHAT;
      syslog(LOG_ERR, "[dm-voice] HTTP URL buffer is too small\n");
      return -1;
    }

  syslog(LOG_INFO, "[dm-voice] curl request configured; NSH argv kept minimal\n");
  if (health_required && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
    { unlink(JSON_REQ); return -2; }
  rc = run_curl_command(url, JSON_REQ, out_path, 25);

  if (rc != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_CHAT;
        }
      syslog(LOG_WARNING,
             "[dm-voice] HTTP request failed; curl error/status is above\n");
    }
  else
    {
      syslog(LOG_INFO, "[dm-voice] HTTP request completed\n");
    }
  return rc;
}

static int http_post_json(const char *path, const char *json_body,
                          const char *out_path)
{
  return http_post_json_checked(path, json_body, out_path, false);
}

static cJSON *voice_json_load(const char *path, long max_size)
{
  FILE *f;
  char *buf;
  long size;
  cJSON *root;

  f = fopen(path, "rb");
  if (!f)
    {
      return NULL;
    }
  if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) <= 0 ||
      size > max_size || fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      return NULL;
    }
  buf = malloc((size_t)size + 1);
  if (!buf)
    {
      fclose(f);
      return NULL;
    }
  if (fread(buf, 1, (size_t)size, f) != (size_t)size)
    {
      free(buf);
      fclose(f);
      return NULL;
    }
  fclose(f);
  buf[size] = '\0';
  root = cJSON_Parse(buf);
  free(buf);
  return root;
}

static cJSON *voice_first_message(cJSON *root)
{
  cJSON *choices;
  cJSON *choice;

  if (!root)
    {
      return NULL;
    }
  choices = cJSON_GetObjectItem(root, "choices");
  if (!choices || !cJSON_IsArray(choices))
    {
      return NULL;
    }
  choice = cJSON_GetArrayItem(choices, 0);
  return choice ? cJSON_GetObjectItem(choice, "message") : NULL;
}

static void put_le16(uint8_t *p, uint16_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value)
{
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

/* nxrecorder's AUDIO_FMT_PCM encoder writes headerless PCM even when the
 * filename ends in .wav.  Wrap that payload in a standards-compliant WAV
 * header before sending `format: wav` to the MiMo chat-completions ASR API. */
static int voice_prepare_wav(const char *src, const char **wav_out)
{
  FILE *in;
  FILE *out;
  uint8_t header[44];
  char buf[1024];
  long size;
  size_t n;
  uint32_t data_size;
  unsigned int byte_rate = REC_RATE * REC_CH * REC_BITS / 8;
  int rc = -1;

  if (!src || !wav_out)
    {
      return -1;
    }

  *wav_out = src;
  in = fopen(src, "rb");
  if (!in)
    {
      return -1;
    }

  if (fread(header, 1, 12, in) == 12 &&
      memcmp(header, "RIFF", 4) == 0 && memcmp(header + 8, "WAVE", 4) == 0)
    {
      fclose(in);
      return 0;
    }

  if (fseek(in, 0, SEEK_END) != 0 || (size = ftell(in)) <= 0 ||
      size > VOICE_MAX_RECORD_BYTES || fseek(in, 0, SEEK_SET) != 0)
    {
      fclose(in);
      return -1;
    }

  data_size = (uint32_t)size;
  memset(header, 0, sizeof(header));
  memcpy(header, "RIFF", 4);
  put_le32(header + 4, data_size + 36);
  memcpy(header + 8, "WAVEfmt ", 8);
  put_le32(header + 16, 16);
  put_le16(header + 20, 1);
  put_le16(header + 22, REC_CH);
  put_le32(header + 24, REC_RATE);
  put_le32(header + 28, byte_rate);
  put_le16(header + 32, REC_CH * REC_BITS / 8);
  put_le16(header + 34, REC_BITS);
  memcpy(header + 36, "data", 4);
  put_le32(header + 40, data_size);

  out = fopen(WAV_ASR, "wb");
  if (!out)
    {
      fclose(in);
      return -1;
    }

  if (fwrite(header, 1, sizeof(header), out) == sizeof(header))
    {
      rc = 0;
      while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        {
          if (fwrite(buf, 1, n, out) != n)
            {
              rc = -1;
              break;
            }
        }
      if (ferror(in))
        {
          rc = -1;
        }
    }

  if (fclose(out) != 0)
    {
      rc = -1;
    }
  fclose(in);
  if (rc == 0)
    {
      *wav_out = WAV_ASR;
    }
  else
    {
      unlink(WAV_ASR);
    }
  return rc;
}

/* Convert the latest microphone capture from raw PCM to a WAV file and persist
 * it on /data.  Use a temporary sibling and rename so an interrupted save does
 * not leave a partial file masquerading as a completed recording. */
int dm_voice_record_save(const char *wav_path)
{
  const char *prepared_path = NULL;
  char temp_path[160];
  FILE *in = NULL;
  FILE *out = NULL;
  unsigned char buf[1024];
  size_t n;
  int rc = -1;
  int close_in_rc = 0;
  int close_out_rc = 0;
  int path_len;

  if (!wav_path || !wav_path[0])
    {
      return -EINVAL;
    }
  path_len = snprintf(temp_path, sizeof(temp_path), "%s.tmp", wav_path);
  if (path_len < 0 || path_len >= (int)sizeof(temp_path))
    {
      return -ENAMETOOLONG;
    }

  pthread_mutex_lock(&s_voice_api_lock);
  if (voice_prepare_wav(WAV_REC, &prepared_path) != 0)
    {
      syslog(LOG_ERR, "[dm-voice] recorder save: WAV preparation failed\n");
      goto out;
    }

  in = fopen(prepared_path, "rb");
  out = fopen(temp_path, "wb");
  if (!in || !out)
    {
      syslog(LOG_ERR, "[dm-voice] recorder save: cannot open source/destination\n");
      goto out;
    }

  rc = 0;
  while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
    {
      if (fwrite(buf, 1, n, out) != n)
        {
          rc = -EIO;
          break;
        }
    }
  if (ferror(in))
    {
      rc = -EIO;
    }

  close_in_rc = fclose(in);
  in = NULL;
  close_out_rc = fclose(out);
  out = NULL;
  if (close_in_rc != 0 || close_out_rc != 0)
    {
      rc = -EIO;
    }
  if (rc == 0 && rename(temp_path, wav_path) != 0)
    {
      rc = -errno;
    }

out:
  if (in)
    {
      fclose(in);
    }
  if (out)
    {
      fclose(out);
    }
  if (rc != 0)
    {
      unlink(temp_path);
    }
  else
    {
      syslog(LOG_INFO, "[dm-voice] saved recording to persistent storage\n");
      if (prepared_path && strcmp(prepared_path, WAV_ASR) == 0)
        {
          unlink(WAV_ASR);
        }
      unlink(WAV_REC);
    }
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

static int asr_wav(const char *wav, char *text, int cap)
{
  FILE *f;
  unsigned char *raw;
  char *b64;
  char *body;
  size_t body_cap;
  long sz;
  int n;
  cJSON *root;
  cJSON *message;
  cJSON *content;

  if (!wav || !text || cap < 2)
    {
      return -1;
    }
  text[0] = '\0';
  f = fopen(wav, "rb");
  if (!f)
    {
      return -1;
    }
  if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 44 ||
      sz > 2 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      syslog(LOG_ERR, "[dm-voice] ASR WAV size/seek invalid\n");
      return -1;
    }
  syslog(LOG_INFO, "[dm-voice] ASR WAV bytes=%ld\n", sz);
  raw = malloc((size_t)sz);
  b64 = malloc((size_t)sz * 2 + 8);
  body_cap = (size_t)sz * 2 + 512;
  body = malloc(body_cap);
  if (!raw || !b64 || !body)
    {
      free(raw);
      free(b64);
      free(body);
      fclose(f);
      return -1;
    }
  if (fread(raw, 1, (size_t)sz, f) != (size_t)sz)
    {
      free(raw);
      free(b64);
      free(body);
      fclose(f);
      return -1;
    }
  fclose(f);
  if (b64_encode(raw, (size_t)sz, b64, (size_t)sz * 2 + 8) <= 0)
    {
      free(raw);
      free(b64);
      free(body);
      return -1;
    }
  free(raw);

  /* Official MiMo ASR schema: data URL + one input_audio content part. */
  n = snprintf(body, body_cap,
               "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":"
               "[{\"type\":\"input_audio\",\"input_audio\":{\"data\":"
               "\"data:audio/wav;base64,%s\"}}]}],"
               "\"asr_options\":{\"language\":\"auto\"}}",
               ASR_MODEL, b64);
  free(b64);
  if (n <= 0 || (size_t)n >= body_cap)
    {
      free(body);
      return -1;
    }
  syslog(LOG_INFO, "[dm-voice] ASR request start\n");
  if (http_post_json("/chat/completions", body, TXT_ASR) != 0)
    {
      free(body);
      syslog(LOG_ERR, "[dm-voice] ASR HTTP request failed\n");
      return -1;
    }
  free(body);

  root = voice_json_load(TXT_ASR, 256 * 1024);
  if (!root)
    {
      syslog(LOG_ERR, "[dm-voice] ASR response is not valid JSON\n");
      return -1;
    }
  message = voice_first_message(root);
  content = message ? cJSON_GetObjectItem(message, "content") : NULL;
  if (content && cJSON_IsString(content) && content->valuestring)
    {
      size_t content_len = strlen(content->valuestring);
      if (content_len >= (size_t)cap)
        {
          cJSON_Delete(root);
          syslog(LOG_ERR, "[dm-voice] ASR segment text exceeds buffer\n");
          return -ENOSPC;
        }
      snprintf(text, (size_t)cap, "%s", content->valuestring);
    }
  cJSON_Delete(root);
  if (!text[0])
    {
      syslog(LOG_ERR,
             "[dm-voice] ASR response has no choices[0].message.content text\n");
      return -1;
    }
  syslog(LOG_INFO, "[dm-voice] ASR success text_bytes=%u\n",
         (unsigned int)strlen(text));
  return 0;
}

static int chat_reply(const char *user, char *out, int cap,
                       bool *health_context)
{
  char esc[400];
  char hist[1400];
  char body[2400];
  int n;
  bool private_context;
  cJSON *root;
  cJSON *message;
  cJSON *content;

  if (!user || !out || cap < 2)
    {
      return -1;
    }
  out[0] = '\0';
  if (json_escape(user, esc, sizeof(esc)) < 0)
    {
      return -1;
    }
  private_context = dm_ai_permission_allowed(DM_AI_READ_HEALTH);
  *health_context = private_context;
  dm_chat_hist_json_for_cloud(hist, sizeof(hist), private_context);

  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"system\",\"content\":\"你是openvela语音小助手，口语化简短回答，不超过60字。只提供建议；不要声称已启动计时、保存备忘或创建提醒，除非应用明确反馈执行成功。情绪困扰时给予温和、非诊断性支持。\"},"
               "%s"
               "{\"role\":\"user\",\"content\":\"%s\"}],"
               "\"max_completion_tokens\":400,\"thinking\":{\"type\":\"disabled\"}}",
               CHAT_MODEL, hist, esc);
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return -1;
    }
  if (private_context && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
    {
      snprintf(out, (size_t)cap, "%s", "权限已更新，请重新发送本次聊天。");
      return 0;
    }
  syslog(LOG_INFO, "[dm-voice] chat completion request start\n");
  if (http_post_json_checked("/chat/completions", body, TXT_CHAT, private_context) != 0)
    {
      syslog(LOG_ERR, "[dm-voice] chat completion HTTP request failed\n");
      return -1;
    }

  root = voice_json_load(TXT_CHAT, 64 * 1024);
  if (!root)
    {
      syslog(LOG_ERR, "[dm-voice] chat completion response is not valid JSON\n");
      return -1;
    }
  message = voice_first_message(root);
  content = message ? cJSON_GetObjectItem(message, "content") : NULL;
  if (content && cJSON_IsString(content) && content->valuestring)
    {
      snprintf(out, (size_t)cap, "%s", content->valuestring);
    }
  cJSON_Delete(root);

  if (out[0])
    {
      syslog(LOG_INFO, "[dm-voice] chat completion success reply_bytes=%u\n",
             (unsigned int)strlen(out));
      if (private_context) dm_chat_hist_push_health(user, out);
      else dm_chat_hist_push(user, out);
      return 0;
    }
  syslog(LOG_ERR,
         "[dm-voice] chat completion response has no choices[0].message.content\n");
  return -1;
}

static int tts_synth(const char *text, const char *wav_path,
                     const char *voice_id, bool health_required)
{
  char esc[1200];
  char voice_esc[64];
  char body[2400];
  unsigned char *raw = NULL;
  FILE *f = NULL;
  long sz;
  int n;
  int rawlen;
  int write_ok;
  int close_rc;
  int rc = -1;
  cJSON *root = NULL;
  cJSON *message;
  cJSON *audio;
  cJSON *data;

  if (!text || !text[0] || !wav_path || !voice_id || !voice_id[0])
    {
      return -1;
    }
  if (json_escape(text, esc, sizeof(esc)) < 0 ||
      json_escape(voice_id, voice_esc, sizeof(voice_esc)) < 0)
    {
      return -1;
    }
  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"user\",\"content\":\"请使用指定音色，以自然清晰的语气朗读。\"},"
               "{\"role\":\"assistant\",\"content\":\"%s\"}],"
               "\"audio\":{\"format\":\"wav\",\"voice\":\"%s\"}}",
               TTS_MODEL, esc, voice_esc);
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return -1;
    }
  syslog(LOG_INFO, "[dm-voice] TTS request start\n");
  n = http_post_json_checked("/chat/completions", body, TXT_CHAT, health_required);
  if (n != 0)
    {
      syslog(LOG_ERR, "[dm-voice] TTS HTTP request failed\n");
      return n == -2 ? -2 : -1;
    }

  root = voice_json_load(TXT_CHAT, 4 * 1024 * 1024);
  if (!root)
    {
      goto out;
    }
  message = voice_first_message(root);
  audio = message ? cJSON_GetObjectItem(message, "audio") : NULL;
  data = audio ? cJSON_GetObjectItem(audio, "data") : NULL;
  if (!data || !cJSON_IsString(data) || !data->valuestring)
    {
      goto out;
    }

  sz = (long)strlen(data->valuestring);
  if (sz <= 0 || sz > 4 * 1024 * 1024)
    {
      goto out;
    }
  raw = malloc((size_t)sz);
  if (!raw)
    {
      goto out;
    }
  rawlen = b64_decode(data->valuestring, raw, (size_t)sz);
  if (rawlen < 44 || memcmp(raw, "RIFF", 4) != 0 ||
      memcmp(raw + 8, "WAVE", 4) != 0)
    {
      goto out;
    }

  f = fopen(wav_path, "wb");
  if (!f)
    {
      goto out;
    }
  write_ok = fwrite(raw, 1, (size_t)rawlen, f) == (size_t)rawlen;
  close_rc = fclose(f);
  f = NULL;
  if (write_ok && close_rc == 0)
    {
      rc = 0;
      syslog(LOG_INFO, "[dm-voice] TTS WAV decoded bytes=%d\n", rawlen);
    }

out:
  if (f)
    {
      fclose(f);
    }
  free(raw);
  if (root)
    {
      cJSON_Delete(root);
    }
  if (rc != 0)
    {
      syslog(LOG_ERR, "[dm-voice] TTS response missing/invalid WAV data\n");
      unlink(wav_path);
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_TTS;
        }
    }
  return rc;
}

static int voice_play_wake_samples_unlocked(void)
{
  static const char *const samples[] = {
    WAV_PREVIEW_BINGTANG,
    WAV_PREVIEW_JASMINE,
    WAV_PREVIEW_SODA,
  };
  int i;

  for (i = 0; i < (int)(sizeof(samples) / sizeof(samples[0])); i++)
    {
      syslog(LOG_INFO, "[dm-voice] playing wake sample index=%d\n", i);
      if (play_wav(samples[i]) != 0)
        {
          syslog(LOG_ERR, "[dm-voice] wake sample playback failed index=%d\n",
                 i);
          return -1;
        }
    }
  return 0;
}

static int voice_asr_chat_tts_unlocked(const char *wav_path, char *reply,
                                       int cap)
{
  char text[256];
  const char *asr_path;
  int play_rc = -1;
  int intent_handled;
  bool health_context = false;

  s_health_reply = 0;

  if (!wav_path || !reply || cap < 8)
    {
      return -1;
    }
  s_last_failure = DM_VOICE_FAILURE_NONE;
  reply[0] = '\0';

  if (voice_prepare_wav(wav_path, &asr_path) != 0)
    {
      s_last_failure = DM_VOICE_FAILURE_ASR;
      s_st = DM_VOICE_ERR;
      return -1;
    }

  s_st = DM_VOICE_ASR;
  if (asr_wav(asr_path, text, sizeof(text)) != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_ASR;
        }
      s_st = DM_VOICE_ERR;
      return -1;
    }

  s_st = DM_VOICE_CHAT;
  intent_handled = dm_assistant_intent_try(text, reply, cap);
  s_health_reply = intent_handled == 3 || intent_handled == 5;
  if (intent_handled > 0)
    {
      if (intent_handled == 4)
        {
          s_st = DM_VOICE_IDLE;
          return 3; /* Existing life records stay local: no history or TTS. */
        }
      if (intent_handled == 5) { /* A real health query is not chat history. */ }
      else if (intent_handled == 3) dm_chat_hist_push_health(text, reply);
      else dm_chat_hist_push(text, reply);
    }
  else if (chat_reply(text, reply, cap, &health_context) != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_CHAT;
        }
      strncpy(reply, "（网络忙，没听清）", (size_t)cap - 1);
      reply[cap - 1] = '\0';
      s_st = DM_VOICE_ERR;
      return -1;
    }

  s_health_reply = s_health_reply || health_context;
  if (s_health_reply &&
      !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
    {
      s_st = DM_VOICE_IDLE;
      return 2; /* Local display only: no health-derived text sent to cloud TTS. */
    }

  /* Preserve the text answer even if playback fails; the caller can show
   * that the reply is text-only rather than pretending the speaker worked. */
  if (intent_handled == 2)
    {
      /* Wake phrase response demonstrates all three user-provided voices in
       * their existing order; regular profile audition behavior is unchanged. */
      s_st = DM_VOICE_PLAY;
      play_rc = voice_play_wake_samples_unlocked();
      if (play_rc != 0 && s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_TTS;
        }
    }
  else
    {
      if (s_health_reply && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
        { s_st = DM_VOICE_IDLE; return 2; }
      s_st = DM_VOICE_TTS;
      int synth_rc = tts_synth(reply, WAV_TTS, dm_voice_profile_api_id(), s_health_reply);
      if (synth_rc == -2) { s_st = DM_VOICE_IDLE; return 2; }
      if (synth_rc == 0)
        {
          s_st = DM_VOICE_PLAY;
          play_rc = play_wav(WAV_TTS);
        }
      else if (s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_TTS;
        }
    }
  s_st = DM_VOICE_IDLE;
  return play_rc == 0 ? 0 : 1;
}

int dm_voice_asr_chat_tts(const char *wav_path, char *reply, int cap)
{
  int rc;

  pthread_mutex_lock(&s_voice_api_lock);
  rc = voice_asr_chat_tts_unlocked(wav_path, reply, cap);
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

static int voice_tts_play_unlocked(const char *text, bool health_required)
{
  int synth_rc;
  s_last_failure = DM_VOICE_FAILURE_NONE;
  if (!text || !text[0])
    {
      return -1;
    }
  s_st = DM_VOICE_TTS;
  synth_rc = tts_synth(text, WAV_TTS, dm_voice_profile_api_id(), health_required);
  if (synth_rc == -2) { s_st = DM_VOICE_IDLE; return 2; }
  if (synth_rc != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        {
          s_last_failure = DM_VOICE_FAILURE_TTS;
        }
      s_st = DM_VOICE_ERR;
      return -1;
    }
  s_st = DM_VOICE_PLAY;
  if (play_wav(WAV_TTS) != 0)
    {
      s_last_failure = DM_VOICE_FAILURE_TTS;
      s_st = DM_VOICE_ERR;
      return -1;
    }
  s_st = DM_VOICE_IDLE;
  return 0;
}

int dm_voice_tts_play(const char *text)
{
  int rc;

  pthread_mutex_lock(&s_voice_api_lock);
  rc = voice_tts_play_unlocked(text, false);
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

int dm_voice_tts_play_private(const char *text)
{
  int rc;
  pthread_mutex_lock(&s_voice_api_lock);
  /* Recheck after waiting for other audio/network users. */
  if (!dm_ai_permission_allowed(DM_AI_READ_HEALTH)) rc = 2;
  else rc = voice_tts_play_unlocked(text, true);
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

static void voice_meeting_progress_set(int completed, int total)
{
  pthread_mutex_lock(&s_meeting_progress_lock);
  s_meeting_progress_done = completed;
  s_meeting_progress_total = total;
  pthread_mutex_unlock(&s_meeting_progress_lock);
}

void dm_voice_meeting_progress(int *completed, int *total)
{
  pthread_mutex_lock(&s_meeting_progress_lock);
  if (completed) *completed = s_meeting_progress_done;
  if (total) *total = s_meeting_progress_total;
  pthread_mutex_unlock(&s_meeting_progress_lock);
}

static int meeting_write_wav_header(FILE *out, uint32_t data_size)
{
  uint8_t header[44];
  uint32_t byte_rate = REC_RATE * REC_CH * REC_BITS / 8;

  memset(header, 0, sizeof(header));
  memcpy(header, "RIFF", 4);
  put_le32(header + 4, data_size + 36);
  memcpy(header + 8, "WAVEfmt ", 8);
  put_le32(header + 16, 16);
  put_le16(header + 20, 1);
  put_le16(header + 22, REC_CH);
  put_le32(header + 24, REC_RATE);
  put_le32(header + 28, byte_rate);
  put_le16(header + 32, REC_CH * REC_BITS / 8);
  put_le16(header + 34, REC_BITS);
  memcpy(header + 36, "data", 4);
  put_le32(header + 40, data_size);
  return fwrite(header, 1, sizeof(header), out) == sizeof(header) ? 0 : -EIO;
}

static int meeting_asr_transcribe(const char *wav_path, char *transcript,
                                  size_t transcript_cap)
{
  FILE *in = NULL;
  uint16_t channels;
  uint16_t bits;
  uint32_t rate;
  uint32_t data_size;
  uint32_t aligned_size;
  uint32_t offset;
  long data_offset;
  uint32_t segment_index;
  uint32_t segment_count;
  size_t transcript_used = 0;
  char segment_text[1024];
  char copybuf[4096];
  int rc = -1;

  if (!wav_path || !transcript || transcript_cap < 2)
    {
      return -EINVAL;
    }
  transcript[0] = '\0';
  in = fopen(wav_path, "rb");
  if (!in)
    {
      return -errno;
    }
  if (voice_wav_find_data(in, &channels, &rate, &bits, &data_offset,
                          &data_size) != 0 || channels != REC_CH ||
      rate != REC_RATE || bits != REC_BITS ||
      data_size > VOICE_MAX_RECORD_BYTES)
    {
      syslog(LOG_ERR, "[dm-voice] meeting WAV format/size unsupported\n");
      goto out;
    }

  aligned_size = data_size - data_size % (REC_CH * REC_BITS / 8);
  if (aligned_size == 0)
    {
      goto out;
    }
  segment_count = (aligned_size + MEETING_ASR_SEGMENT_BYTES - 1) /
                  MEETING_ASR_SEGMENT_BYTES;
  voice_meeting_progress_set(0, (int)segment_count + 2);

  for (segment_index = 0, offset = 0; offset < aligned_size;
       segment_index++)
    {
      FILE *chunk = NULL;
      uint32_t chunk_size = aligned_size - offset;
      uint32_t copied = 0;

      if (chunk_size > MEETING_ASR_SEGMENT_BYTES)
        {
          chunk_size = MEETING_ASR_SEGMENT_BYTES;
        }
      if (fseek(in, data_offset + (long)offset, SEEK_SET) != 0)
        {
          goto out;
        }
      chunk = fopen(MEETING_CHUNK_WAV, "wb");
      if (!chunk)
        {
          goto out;
        }
      if (meeting_write_wav_header(chunk, chunk_size) != 0)
        {
          fclose(chunk);
          unlink(MEETING_CHUNK_WAV);
          goto out;
        }
      while (copied < chunk_size)
        {
          size_t want = chunk_size - copied;
          size_t got;
          if (want > sizeof(copybuf)) want = sizeof(copybuf);
          got = fread(copybuf, 1, want, in);
          if (got != want || fwrite(copybuf, 1, got, chunk) != got)
            {
              fclose(chunk);
              unlink(MEETING_CHUNK_WAV);
              goto out;
            }
          copied += (uint32_t)got;
        }
      if (fclose(chunk) != 0)
        {
          unlink(MEETING_CHUNK_WAV);
          goto out;
        }

      if (asr_wav(MEETING_CHUNK_WAV, segment_text,
                  sizeof(segment_text)) != 0)
        {
          unlink(MEETING_CHUNK_WAV);
          goto out;
        }
      unlink(MEETING_CHUNK_WAV);
      if (segment_text[0])
        {
          size_t n = strlen(segment_text);
          size_t separator = transcript_used ? 1 : 0;
          if (transcript_used + separator + n >= transcript_cap)
            {
              rc = -ENOSPC;
              goto out;
            }
          if (separator)
            {
              transcript[transcript_used++] = '\n';
            }
          memcpy(transcript + transcript_used, segment_text, n);
          transcript_used += n;
          transcript[transcript_used] = '\0';
        }
      offset += chunk_size;
      voice_meeting_progress_set((int)segment_index + 1,
                                 (int)segment_count + 2);
    }

  rc = transcript_used ? 0 : -EIO;

out:
  unlink(MEETING_CHUNK_WAV);
  if (in) fclose(in);
  return rc;
}

static int meeting_summary_reply(const char *transcript, char *summary,
                                 size_t summary_cap)
{
  static const char system_prompt[] =
               "你是严谨的会议纪要助手。只依据ASR转写，不推测或补造信息。用中文依次输出主题概述、关键结论、待办事项；负责人和截止时间只在转写明确提及时记录，不确定内容标注待确认。重点精炼，整体控制在约150个汉字内，适合屏幕阅读和语音播报。";
  static const char user_prefix[] = "请整理以下会议逐段转写，合并重复内容并保留重点：\\n\\n";
  size_t transcript_len;
  size_t escaped_cap;
  size_t body_cap;
  char *escaped = NULL;
  char *body = NULL;
  int n;
  cJSON *root = NULL;
  cJSON *message;
  cJSON *content;
  int rc = -1;

  if (!transcript || !transcript[0] || !summary || summary_cap < 2)
    {
      return -EINVAL;
    }
  summary[0] = '\0';
  transcript_len = strlen(transcript);
  if (transcript_len > (SIZE_MAX - 1) / 2)
    {
      return -EOVERFLOW;
    }
  escaped_cap = transcript_len * 2 + 1;
  body_cap = escaped_cap + sizeof(system_prompt) + sizeof(user_prefix) + 512;
  escaped = malloc(escaped_cap);
  body = malloc(body_cap);
  if (!escaped || !body)
    {
      rc = -ENOMEM;
      goto out;
    }
  if (json_escape(transcript, escaped, escaped_cap) < 0)
    {
      rc = -ENOSPC;
      goto out;
    }
  n = snprintf(body, body_cap,
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"system\",\"content\":\"%s\"},"
               "{\"role\":\"user\",\"content\":\"%s%s\"}],"
               "\"max_completion_tokens\":1200,"
               "\"thinking\":{\"type\":\"disabled\"}}",
               MEETING_SUMMARY_MODEL, system_prompt, user_prefix, escaped);
  if (n <= 0 || (size_t)n >= body_cap)
    {
      rc = -ENOSPC;
      goto out;
    }
  if (http_post_json("/chat/completions", body, MEETING_CHAT_RSP) != 0)
    {
      goto out;
    }
  root = voice_json_load(MEETING_CHAT_RSP, 256 * 1024);
  if (!root)
    {
      goto out;
    }
  message = voice_first_message(root);
  content = message ? cJSON_GetObjectItem(message, "content") : NULL;
  if (!content || !cJSON_IsString(content) || !content->valuestring ||
      !content->valuestring[0])
    {
      goto out;
    }
  if (strlen(content->valuestring) >= summary_cap)
    {
      rc = -ENOSPC;
      goto out;
    }
  snprintf(summary, summary_cap, "%s", content->valuestring);
  rc = 0;

out:
  if (root) cJSON_Delete(root);
  free(escaped);
  free(body);
  return rc;
}

static void meeting_speech_excerpt(const char *summary, char *speech,
                                   size_t speech_cap)
{
  static const char prefix[] = "会议摘要。";
  size_t prefix_len = sizeof(prefix) - 1;
  size_t source_len;
  size_t keep;

  if (!speech || speech_cap == 0)
    {
      return;
    }
  speech[0] = '\0';
  if (!summary || !summary[0] || prefix_len >= speech_cap)
    {
      return;
    }
  source_len = strlen(summary);
  keep = source_len;
  if (keep > speech_cap - prefix_len - 1)
    {
      keep = speech_cap - prefix_len - 1;
    }
  if (keep > 500) keep = 500;
  while (keep > 0 && (((unsigned char)summary[keep] & 0xc0) == 0x80))
    {
      keep--;
    }
  memcpy(speech, prefix, prefix_len);
  memcpy(speech + prefix_len, summary, keep);
  speech[prefix_len + keep] = '\0';
}

int dm_voice_meeting_analyze(const char *wav_path,
                             char *transcript, int transcript_cap,
                             char *summary, int summary_cap)
{
  char speech[600];
  int rc;
  int tts_rc;
  int progress_total;

  if (!wav_path || !transcript || transcript_cap < 2 || !summary ||
      summary_cap < 2)
    {
      return -EINVAL;
    }
  transcript[0] = '\0';
  summary[0] = '\0';
  voice_meeting_progress_set(0, 0);
  pthread_mutex_lock(&s_voice_api_lock);
  s_last_failure = DM_VOICE_FAILURE_NONE;
  s_st = DM_VOICE_ASR;
  rc = meeting_asr_transcribe(wav_path, transcript, (size_t)transcript_cap);
  if (rc != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        s_last_failure = DM_VOICE_FAILURE_ASR;
      goto out;
    }

  s_st = DM_VOICE_CHAT;
  rc = meeting_summary_reply(transcript, summary, (size_t)summary_cap);
  if (rc != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        s_last_failure = DM_VOICE_FAILURE_CHAT;
      goto out;
    }
  dm_voice_meeting_progress(NULL, &progress_total);
  if (progress_total > 0)
    {
      voice_meeting_progress_set(progress_total - 1, progress_total);
    }

  meeting_speech_excerpt(summary, speech, sizeof(speech));
  s_st = DM_VOICE_TTS;
  tts_rc = voice_tts_play_unlocked(speech, false);
  dm_voice_meeting_progress(NULL, &progress_total);
  voice_meeting_progress_set(progress_total, progress_total);
  if (tts_rc != 0)
    {
      if (s_last_failure == DM_VOICE_FAILURE_NONE)
        s_last_failure = DM_VOICE_FAILURE_TTS;
      rc = 1; /* transcript and summary remain successful and should be saved */
    }
  else
    {
      rc = 0;
    }

out:
  s_st = DM_VOICE_IDLE;
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

static int voice_wav_cache_valid(const char *path)
{
  FILE *f = fopen(path, "rb");
  unsigned char hdr[44];
  long size;
  int valid = 0;

  if (!f)
    {
      return 0;
    }
  if (fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) > 44 &&
      fseek(f, 0, SEEK_SET) == 0 &&
      fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
      memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0)
    {
      valid = 1;
    }
  fclose(f);
  return valid;
}

static const char *voice_builtin_preview_path(int profile_idx)
{
  switch (profile_idx)
    {
      case 1:
        return WAV_PREVIEW_BINGTANG;
      case 2:
        return WAV_PREVIEW_JASMINE;
      case 3:
        return WAV_PREVIEW_SODA;
      default:
        return NULL;
    }
}

/* Prefer bundled offline audition files for the supplied presets; other voice
 * profiles retain the existing generated-and-cached preview behavior. */
static int voice_profile_preview_unlocked(int profile_idx)
{
  static const char sample_text[] =
      "你好，我是你的语音助手。这是当前音色试听。";
  const dm_voice_profile_t *profile;
  const char *builtin_path = NULL;
  char path[64];

  if (profile_idx < 0 || profile_idx >= dm_voice_profile_count())
    {
      return -1;
    }
  profile = dm_voice_profile_get(profile_idx);
  builtin_path = voice_builtin_preview_path(profile_idx);

  s_last_failure = DM_VOICE_FAILURE_NONE;
  /* Use the exact user-provided board assets for these three presets before
   * consulting a prior generated cache or making any cloud request. */
  if (builtin_path && voice_wav_cache_valid(builtin_path))
    {
      syslog(LOG_INFO, "[dm-voice] playing bundled preset preview index=%d\n",
             profile_idx);
      s_st = DM_VOICE_PLAY;
      if (play_wav(builtin_path) == 0)
        {
          s_st = DM_VOICE_IDLE;
          return 0;
        }
      s_last_failure = DM_VOICE_FAILURE_TTS;
      s_st = DM_VOICE_ERR;
      return -1;
    }

  if (builtin_path)
    {
      syslog(LOG_ERR,
             "[dm-voice] bundled preset WAV missing/invalid index=%d; using generated fallback\n",
             profile_idx);
    }
  snprintf(path, sizeof(path), "/data/dm_voice_sample_%d.wav", profile_idx);
  if (!voice_wav_cache_valid(path))
    {
      unlink(path);
      s_st = DM_VOICE_TTS;
      if (tts_synth(sample_text, path, profile->api_id, false) != 0 ||
          !voice_wav_cache_valid(path))
        {
          if (s_last_failure == DM_VOICE_FAILURE_NONE)
            {
              s_last_failure = DM_VOICE_FAILURE_TTS;
            }
          s_st = DM_VOICE_ERR;
          return -1;
        }
    }

  s_st = DM_VOICE_PLAY;
  if (play_wav(path) != 0)
    {
      s_last_failure = DM_VOICE_FAILURE_TTS;
      s_st = DM_VOICE_ERR;
      return -1;
    }
  s_st = DM_VOICE_IDLE;
  return 0;
}

int dm_voice_profile_preview(int profile_idx)
{
  int rc;

  pthread_mutex_lock(&s_voice_api_lock);
  rc = voice_profile_preview_unlocked(profile_idx);
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

/* Reuse bundled preset WAVs on the volume page; generate a test phrase only
 * for profiles that do not have an offline sample. */
static int voice_volume_test_unlocked(void)
{
  static const char sample_text[] = "音量试听。请确认这个音量听起来是否舒适。";
  char path[64];
  const char *builtin_path;
  int profile_idx = dm_voice_profile_sel();

  if (profile_idx < 0 || profile_idx >= dm_voice_profile_count())
    {
      profile_idx = 0;
    }
  s_last_failure = DM_VOICE_FAILURE_NONE;
  builtin_path = voice_builtin_preview_path(profile_idx);
  if (builtin_path && voice_wav_cache_valid(builtin_path))
    {
      syslog(LOG_INFO, "[dm-voice] volume test using bundled preset index=%d\n",
             profile_idx);
      return play_wav(builtin_path) == 0 ? 0 : -1;
    }

  snprintf(path, sizeof(path), WAV_VOL_TEST_FMT, profile_idx);
  if (!voice_wav_cache_valid(path))
    {
      unlink(path);
      if (tts_synth(sample_text, path, dm_voice_profile_api_id(), false) != 0 ||
          !voice_wav_cache_valid(path))
        {
          int rc;
          if (s_last_failure == DM_VOICE_FAILURE_NONE)
            {
              s_last_failure = DM_VOICE_FAILURE_TTS;
            }
          syslog(LOG_WARNING,
                 "[dm-voice] cloud volume sample unavailable; using tone\n");
          rc = dm_voice_beep();
          return rc == 0 ? 1 : -1;
        }
    }

  if (play_wav(path) == 0)
    {
      return 0;
    }
  return dm_voice_beep() == 0 ? 1 : -1;
}

int dm_voice_volume_test(void)
{
  int rc;

  pthread_mutex_lock(&s_voice_api_lock);
  rc = voice_volume_test_unlocked();
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

/* Play local alert files away from the LVGL timer thread. */
struct voice_file_job_s
{
  char path[128];
};

static void *play_file_worker(void *arg)
{
  struct voice_file_job_s *job = arg;
  char path[sizeof(job->path)];

  snprintf(path, sizeof(path), "%s", job->path);
  free(job);
  (void)play_wav(path);
  return NULL;
}

int dm_voice_play_file(const char *wav_path)
{
  struct voice_file_job_s *job;
  pthread_t th;
  pthread_attr_t attr;
  int ret;

  if (!wav_path || !wav_path[0])
    {
      return -1;
    }

  job = malloc(sizeof(*job));
  if (!job)
    {
      return -1;
    }
  snprintf(job->path, sizeof(job->path), "%s", wav_path);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  ret = pthread_create(&th, &attr, play_file_worker, job);
  pthread_attr_destroy(&attr);
  if (ret != 0)
    {
      free(job);
      return -1;
    }

  pthread_detach(th);
  return 0;
}

int dm_voice_play_file_sync(const char *wav_path)
{
  int rc;

  if (!wav_path || !wav_path[0])
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&s_voice_api_lock);
  rc = play_wav(wav_path);
  pthread_mutex_unlock(&s_voice_api_lock);
  return rc;
}

#endif /* CONFIG_DESKMATE_APP */
