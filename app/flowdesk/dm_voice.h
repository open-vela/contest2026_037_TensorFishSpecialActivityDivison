/*
 * dm_voice.h — ASR / TTS + toggle mic capture
 */
#ifndef DM_VOICE_H
#define DM_VOICE_H

#include <stdbool.h>

/* Run MiMo/cURL + TLS + JSON work on a larger task stack than lightweight UI
 * callbacks; this is separate from handling transport/formatting failures. */
#define DM_VOICE_NET_WORKER_STACK 65536
#define DM_VOICE_RECORD_PATH "/data/flowdesk_recording.wav"
#define DM_VOICE_MEETING_TRANSCRIPT_MAX 12288
#define DM_VOICE_MEETING_SUMMARY_MAX 2048

typedef enum {
  DM_VOICE_FAILURE_NONE = 0,
  DM_VOICE_FAILURE_WIFI,
  DM_VOICE_FAILURE_TIME,
  DM_VOICE_FAILURE_API_KEY,
  DM_VOICE_FAILURE_ASR,
  DM_VOICE_FAILURE_CHAT,
  DM_VOICE_FAILURE_TTS,
} dm_voice_failure_t;

typedef enum {
  DM_VOICE_IDLE = 0,
  DM_VOICE_REC,
  DM_VOICE_ASR,
  DM_VOICE_CHAT,
  DM_VOICE_TTS,
  DM_VOICE_PLAY,
  DM_VOICE_ERR,
} dm_voice_state_t;

int dm_voice_mic_available(void);
dm_voice_state_t dm_voice_state(void);
dm_voice_failure_t dm_voice_last_failure(void);
/* Most recent voice result contains health context; UI must recheck access. */
int dm_voice_health_reply(void);

/* tap-to-talk: start then stop */
int dm_voice_rec_start(int max_seconds);
int dm_voice_rec_stop(void);          /* 0 = got audio */
int dm_voice_rec_active(void);

/* 0=spoken, 1=TTS failed, 2=health kept local, 3=life records kept local,
 * <0=ASR/chat failed. */
int dm_voice_asr_chat_tts(const char *wav_path, char *reply, int reply_cap);
int dm_voice_tts_play(const char *text);
/* Returns 2 when health-record privacy blocks uploading local text. */
int dm_voice_tts_play_private(const char *text);
/* Analyze a locally saved meeting WAV. Returns 0 when spoken, 1 when the
 * transcript/summary succeeded but TTS failed, and negative on analysis failure. */
int dm_voice_meeting_analyze(const char *wav_path,
                             char *transcript, int transcript_cap,
                             char *summary, int summary_cap);
void dm_voice_meeting_progress(int *completed, int *total);
int dm_voice_profile_preview(int profile_idx); /* generates/caches once per voice */
int dm_voice_play_file(const char *wav_path); /* schedules local WAV playback */
int dm_voice_play_file_sync(const char *wav_path); /* blocking; worker only */
int dm_voice_record_save(const char *wav_path); /* WAV wrapper + persistent copy */
int dm_voice_beep(void);                      /* synchronous; call from worker */
int dm_voice_volume_test(void);                /* 0=speech, 1=tone fallback, <0=fail */
void dm_voice_set_speaker_volume(int percent);
void dm_voice_reset(void);

#endif /* DM_VOICE_H */
