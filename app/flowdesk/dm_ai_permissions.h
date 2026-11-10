#ifndef DM_AI_PERMISSIONS_H
#define DM_AI_PERMISSIONS_H

#include <stdbool.h>

/* Append new permissions to preserve the on-device bit assignments. */
typedef enum
{
  DM_AI_LOCAL = 0,
  DM_AI_OPEN_APPS,
  DM_AI_FOCUS,
  DM_AI_TODO,
  DM_AI_ALARM,
  DM_AI_NOTE,
  DM_AI_COUNTDOWN,
  DM_AI_MEDICATION,
  DM_AI_MOOD_RECORD,
  DM_AI_GAME_2048,
  DM_AI_GAME_GUESS,
  DM_AI_GAME_24,
  DM_AI_MBTI,
  DM_AI_COMPANION,
  DM_AI_READ_HEALTH,
  DM_AI_READ_LOCAL_DATA,
  DM_AI_DELETE_LOCAL_DATA,
  DM_AI_DELETE_HEALTH,
  DM_AI_PERMISSION_COUNT
} dm_ai_permission_t;

void dm_ai_permissions_init(void);
bool dm_ai_permission_enabled(dm_ai_permission_t permission);
bool dm_ai_permission_allowed(dm_ai_permission_t permission);
int dm_ai_permission_set(dm_ai_permission_t permission, bool enabled);
const char *dm_ai_permission_name(dm_ai_permission_t permission);
const char *dm_ai_permission_explanation(dm_ai_permission_t permission);

#endif
