#include <nuttx/config.h>
#include "dm_ai_data.h"

#ifdef CONFIG_DESKMATE_APP
#include <stdio.h>
#include <string.h>

dm_ai_permission_t dm_ai_data_read_permission(dm_ai_data_kind_t kind)
{
  if (kind < 0 || kind >= DM_AI_DATA_COUNT) return DM_AI_PERMISSION_COUNT;
  return kind >= DM_AI_DATA_MOOD ? DM_AI_READ_HEALTH : DM_AI_READ_LOCAL_DATA;
}

dm_ai_permission_t dm_ai_data_delete_permission(dm_ai_data_kind_t kind)
{
  if (kind < 0 || kind >= DM_AI_DATA_COUNT) return DM_AI_PERMISSION_COUNT;
  return kind >= DM_AI_DATA_MOOD ? DM_AI_DELETE_HEALTH : DM_AI_DELETE_LOCAL_DATA;
}

int dm_ai_data_delete_allowed(dm_ai_data_kind_t kind)
{
  return dm_ai_permission_allowed(dm_ai_data_delete_permission(kind)) &&
         dm_ai_permission_allowed(dm_ai_data_read_permission(kind)) &&
         dm_ai_permission_allowed(DM_AI_OPEN_APPS);
}

int dm_ai_data_match(const dm_ai_delete_request_t *request, const char *title,
                     int visible_index)
{
  if (!request || visible_index < 1) return 0;
  if (request->selector == DM_AI_SELECT_INDEX)
    return request->number == visible_index;
  if (request->selector == DM_AI_SELECT_LATEST)
    return request->kind == DM_AI_DATA_MOOD && visible_index == 1;
  return request->selector == DM_AI_SELECT_TITLE && request->title[0] &&
         memchr(request->title, '\0', sizeof(request->title)) != NULL &&
         title && strcmp(request->title, title) == 0;
}

int dm_ai_delete_data(const dm_ai_delete_request_t *request)
{
  if (!request || request->kind < 0 || request->kind >= DM_AI_DATA_COUNT ||
      request->selector <= DM_AI_SELECT_NONE ||
      request->selector > DM_AI_SELECT_CUPS) return DM_AI_DATA_INVALID;
  if ((request->selector == DM_AI_SELECT_INDEX &&
       (request->number < 1 || request->number > 12)) ||
      (request->selector == DM_AI_SELECT_TITLE &&
       (!request->title[0] || !memchr(request->title, '\0', sizeof(request->title)))) ||
      (request->selector == DM_AI_SELECT_LATEST && request->kind != DM_AI_DATA_MOOD) ||
      ((request->selector == DM_AI_SELECT_TODAY || request->selector == DM_AI_SELECT_CUPS) &&
       request->kind != DM_AI_DATA_WATER) ||
      (request->selector == DM_AI_SELECT_CUPS && (request->number < 1 || request->number > 1000)))
    return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(request->kind)) return DM_AI_DATA_DENIED;
  switch (request->kind)
    {
      case DM_AI_DATA_NOTE: return dm_note_ai_delete(request);
      case DM_AI_DATA_TODO:
      case DM_AI_DATA_ALARM: return dm_reminder_ai_delete(request);
      case DM_AI_DATA_COUNTDOWN: return dm_countdown_ai_delete(request);
      case DM_AI_DATA_MOOD: return dm_health_ai_delete(request);
      case DM_AI_DATA_WATER: return dm_water_ai_delete(request);
      case DM_AI_DATA_MEDICATION: return dm_med_ai_delete(request);
      default: return DM_AI_DATA_INVALID;
    }
}

int dm_ai_read_health(dm_ai_health_query_t query, char *out, size_t cap)
{
  int rc;
  if (!out || cap < 80) return DM_AI_DATA_INVALID;
  out[0] = '\0';
  if (!dm_ai_permission_allowed(DM_AI_READ_HEALTH)) return DM_AI_DATA_DENIED;
  if (query == DM_AI_HEALTH_UNAVAILABLE)
    {
      snprintf(out, cap, "%s", "设备目前仅支持查询今日喝水杯数和今日保存的最近一条心情，不能推测未保存的历史记录。");
      return 0;
    }
  rc = dm_health_ai_read(query, out, cap);
  if (!dm_ai_permission_allowed(DM_AI_READ_HEALTH) || rc == DM_AI_DATA_DENIED)
    {
      memset(out, 0, cap);
      return DM_AI_DATA_DENIED;
    }
  return rc;
}
#endif
