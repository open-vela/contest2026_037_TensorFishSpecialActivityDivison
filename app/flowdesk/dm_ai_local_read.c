#include <nuttx/config.h>
#include "dm_ai_local_read.h"
#include "dm_ai_permissions.h"

#ifdef CONFIG_DESKMATE_APP
#include <stdio.h>
#include <string.h>

int dm_ai_read_local(dm_ai_read_kind_t kind, char *out, size_t cap)
{
  static const char *const names[] = {"备忘", "待办", "闹钟", "倒数日"};
  const char *more = "其余请在对应功能页查看。";
  dm_ai_local_record_t rows[12];
  int n, i;
  size_t used;
  if (!out || cap < 80 || kind < 0 || kind >= DM_AI_READ_KIND_COUNT) return -1;
  out[0] = '\0';
  if (!dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA)) return -2;
  memset(rows, 0, sizeof(rows));
  switch (kind)
    {
      case DM_AI_READ_NOTES: n = dm_note_ai_snapshot(rows, 12); break;
      case DM_AI_READ_TODOS: n = dm_reminder_ai_snapshot(false, rows, 12); break;
      case DM_AI_READ_ALARMS: n = dm_reminder_ai_snapshot(true, rows, 12); break;
      case DM_AI_READ_COUNTDOWNS: n = dm_countdown_ai_snapshot(rows, 12); break;
      default: return -1;
    }
  /* Do not return a snapshot if permission was revoked while collecting it. */
  if (!dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA) || n == -2)
    { memset(rows, 0, sizeof(rows)); return -2; }
  if (n < 0 || n > 12) return -1;
  if (!n)
    {
      snprintf(out, cap, "当前没有%s记录。", names[kind]);
      return 0;
    }
  used = (size_t)snprintf(out, cap, "%s共%d条：", names[kind], n);
  for (i = 0; i < n; i++)
    {
      char item[160], date[48];
      struct tm tm;
      size_t len;
      date[0] = '\0';
      if (rows[i].year)
        snprintf(date, sizeof(date), " %04d-%02d-%02d", rows[i].year,
                 rows[i].month, rows[i].day);
      else if (rows[i].due > 0 && localtime_r(&rows[i].due, &tm))
        snprintf(date, sizeof(date), " %02d-%02d %02d:%02d", tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min);
      snprintf(item, sizeof(item), "%d.%s%s%s%s；", i + 1, rows[i].title, date,
               rows[i].repeat_daily ? "（每天）" : "",
               rows[i].pending ? "（已到期）" : "");
      len = strlen(item);
      if (used + len + strlen(more) + 1 > cap) break;
      memcpy(out + used, item, len + 1);
      used += len;
    }
  if (i < n) snprintf(out + used, cap - used, "%s", more);
  memset(rows, 0, sizeof(rows));
  return 0;
}
#endif
