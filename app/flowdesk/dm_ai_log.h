#ifndef DM_AI_LOG_H
#define DM_AI_LOG_H

#include "dm_ai_permissions.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define DM_AI_LOG_MAX 50

/* Append IDs only: these numeric values are persisted, never user content. */
typedef enum {
  DM_AI_LOG_FOCUS, DM_AI_LOG_TODO, DM_AI_LOG_ALARM, DM_AI_LOG_NOTE,
  DM_AI_LOG_COUNTDOWN, DM_AI_LOG_MED_OPEN, DM_AI_LOG_MED_TAKE,
  DM_AI_LOG_MOOD_RECORD, DM_AI_LOG_ADVICE, DM_AI_LOG_GAME_PICKER,
  DM_AI_LOG_2048, DM_AI_LOG_GUESS, DM_AI_LOG_24, DM_AI_LOG_GAME_CANCEL,
  DM_AI_LOG_MBTI, DM_AI_LOG_GREETING, DM_AI_LOG_CANCEL,
  DM_AI_LOG_DELETE, DM_AI_LOG_TIME_ERROR, DM_AI_LOG_READ_HEALTH,
  DM_AI_LOG_MOOD_OPEN,
  DM_AI_LOG_READ_NOTES, DM_AI_LOG_READ_TODOS, DM_AI_LOG_READ_ALARMS,
  DM_AI_LOG_READ_COUNTDOWNS,
  DM_AI_LOG_READ_WATER, DM_AI_LOG_READ_LAST_MOOD,
  DM_AI_LOG_DELETE_NOTE, DM_AI_LOG_DELETE_TODO, DM_AI_LOG_DELETE_ALARM,
  DM_AI_LOG_DELETE_COUNTDOWN, DM_AI_LOG_DELETE_MOOD, DM_AI_LOG_DELETE_WATER,
  DM_AI_LOG_DELETE_MEDICATION,
  DM_AI_LOG_ACTION_COUNT
} dm_ai_log_action_t;

typedef enum {
  DM_AI_LOG_SUCCESS, DM_AI_LOG_FAILED, DM_AI_LOG_DENIED, DM_AI_LOG_SKIPPED,
  DM_AI_LOG_RESULT_COUNT
} dm_ai_log_result_t;

typedef enum {
  DM_AI_LOG_REASON_NONE, DM_AI_LOG_REASON_PERMISSION,
  DM_AI_LOG_REASON_FULL, DM_AI_LOG_REASON_SAVE,
  DM_AI_LOG_REASON_CLOCK, DM_AI_LOG_REASON_INVALID,
  DM_AI_LOG_REASON_TOO_LONG, DM_AI_LOG_REASON_BUSY,
  DM_AI_LOG_REASON_MED_NAME, DM_AI_LOG_REASON_MED_SLOT,
  DM_AI_LOG_REASON_UNSUPPORTED, DM_AI_LOG_REASON_CANCELLED,
  DM_AI_LOG_REASON_MODULE, DM_AI_LOG_REASON_NO_GAME,
  DM_AI_LOG_REASON_NOT_FOUND, DM_AI_LOG_REASON_AMBIGUOUS, DM_AI_LOG_REASON_COUNT
} dm_ai_log_reason_t;

typedef struct {
  uint32_t sequence;
  time_t timestamp;
  bool clock_valid;
  dm_ai_log_action_t action;
  dm_ai_log_result_t result;
  dm_ai_log_reason_t reason;
  dm_ai_permission_t permission; /* COUNT means no denied permission. */
  int seconds;
  int operation_rc;
} dm_ai_log_entry_t;

void dm_ai_log_init(void);
/* No API accepts transcripts, reply strings, passwords or record bodies. */
void dm_ai_log_add(dm_ai_log_action_t action, dm_ai_log_result_t result,
                   dm_ai_log_reason_t reason, dm_ai_permission_t permission,
                   int seconds, int operation_rc);
size_t dm_ai_log_snapshot(dm_ai_log_entry_t *out, size_t cap,
                           uint32_t *revision, bool *persisted);
uint32_t dm_ai_log_revision(void);
const char *dm_ai_log_action_name(dm_ai_log_action_t action);
const char *dm_ai_log_result_name(dm_ai_log_result_t result);
void dm_ai_log_time(const dm_ai_log_entry_t *entry, char *out, size_t cap);
void dm_ai_log_detail(const dm_ai_log_entry_t *entry, char *out, size_t cap);

#endif
