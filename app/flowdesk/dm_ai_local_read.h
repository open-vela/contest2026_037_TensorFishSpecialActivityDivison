#ifndef DM_AI_LOCAL_READ_H
#define DM_AI_LOCAL_READ_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef enum {
  DM_AI_READ_NOTES, DM_AI_READ_TODOS, DM_AI_READ_ALARMS,
  DM_AI_READ_COUNTDOWNS, DM_AI_READ_KIND_COUNT
} dm_ai_read_kind_t;

typedef struct {
  char title[64];
  time_t due;
  int year, month, day, hour, minute;
  bool has_time;
  bool repeat_daily;
  bool pending;
} dm_ai_local_record_t;

/* LVGL thread only. Every provider checks read permission before its data. */
int dm_note_ai_snapshot(dm_ai_local_record_t *out, int cap);
int dm_reminder_ai_snapshot(bool alarm, dm_ai_local_record_t *out, int cap);
int dm_countdown_ai_snapshot(dm_ai_local_record_t *out, int cap);
/* 0=local result, -2=permission denied, -1=invalid request. */
int dm_ai_read_local(dm_ai_read_kind_t kind, char *out, size_t cap);

#endif
