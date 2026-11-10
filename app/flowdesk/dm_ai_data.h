#ifndef DM_AI_DATA_H
#define DM_AI_DATA_H

#include "dm_ai_permissions.h"
#include <stddef.h>

typedef enum {
  DM_AI_HEALTH_WATER, DM_AI_HEALTH_LAST_MOOD, DM_AI_HEALTH_UNAVAILABLE,
  DM_AI_HEALTH_WATER_AND_MOOD
} dm_ai_health_query_t;

typedef enum {
  DM_AI_DATA_NOTE, DM_AI_DATA_TODO, DM_AI_DATA_ALARM, DM_AI_DATA_COUNTDOWN,
  DM_AI_DATA_MOOD, DM_AI_DATA_WATER, DM_AI_DATA_MEDICATION, DM_AI_DATA_COUNT
} dm_ai_data_kind_t;

typedef enum {
  DM_AI_SELECT_NONE, DM_AI_SELECT_INDEX, DM_AI_SELECT_TITLE,
  DM_AI_SELECT_LATEST, DM_AI_SELECT_TODAY, DM_AI_SELECT_CUPS
} dm_ai_selector_t;

typedef struct {
  dm_ai_data_kind_t kind;
  dm_ai_selector_t selector;
  int number; /* One-based visible index, or a positive number of cups. */
  char title[96]; /* Exact user-supplied title. Never returned by deletion. */
} dm_ai_delete_request_t;

enum {
  DM_AI_DATA_INVALID = -1, DM_AI_DATA_DENIED = -2,
  DM_AI_DATA_SAVE = -3, DM_AI_DATA_CLOCK = -4,
  DM_AI_DATA_NOT_FOUND = -5, DM_AI_DATA_AMBIGUOUS = -6,
  DM_AI_DATA_BUSY = -7, DM_AI_DATA_UNSUPPORTED = -8
};

/* All record access runs on the LVGL thread. Providers repeat the gates. */
dm_ai_permission_t dm_ai_data_read_permission(dm_ai_data_kind_t kind);
dm_ai_permission_t dm_ai_data_delete_permission(dm_ai_data_kind_t kind);
int dm_ai_data_delete_allowed(dm_ai_data_kind_t kind);
int dm_ai_data_match(const dm_ai_delete_request_t *request, const char *title,
                     int visible_index);
int dm_ai_delete_data(const dm_ai_delete_request_t *request);
int dm_ai_read_health(dm_ai_health_query_t query, char *out, size_t cap);

int dm_note_ai_delete(const dm_ai_delete_request_t *request);
int dm_reminder_ai_delete(const dm_ai_delete_request_t *request);
int dm_countdown_ai_delete(const dm_ai_delete_request_t *request);
int dm_health_ai_delete(const dm_ai_delete_request_t *request);
int dm_water_ai_delete(const dm_ai_delete_request_t *request);
int dm_med_ai_delete(const dm_ai_delete_request_t *request);
int dm_health_ai_read(dm_ai_health_query_t query, char *out, size_t cap);
int dm_water_ai_cups(int *cups);

#endif
