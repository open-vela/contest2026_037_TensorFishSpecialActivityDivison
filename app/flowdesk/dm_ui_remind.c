/* App-layer voice TODOs and alarms. */

#include "deskmate.h"
#include "dm_audio_vol.h"
#include "dm_voice.h"
#include "dm_ai_permissions.h"
#include "dm_ai_local_read.h"
#include "dm_ai_data.h"

#ifdef CONFIG_DESKMATE_APP

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define REMINDER_PATH "/data/flowdesk_reminders.txt"
#define REMINDER_MAX 12
#define REMINDER_TEXT_MAX 64
#define TODO_MAX_SECONDS (7 * 24 * 60 * 60)

typedef enum
{
  REMINDER_TODO = 0,
  REMINDER_ALARM = 1
} reminder_type_t;

typedef struct
{
  time_t due;
  int type;
  int repeat_daily;
  int fired;
  int pending;
  int hour;
  int minute;
  char text[REMINDER_TEXT_MAX];
} reminder_t;

typedef struct
{
  int is_alarm;
  unsigned int generation;
  char text[REMINDER_TEXT_MAX];
} reminder_speech_t;

static reminder_t s_items[REMINDER_MAX];
static int s_count;
static int s_todo_sel = -1;
static int s_alarm_sel = -1;
static int s_todo_map[REMINDER_MAX];
static int s_alarm_map[REMINDER_MAX];
static int s_alert_idx = -1;
static int s_alarm_hour = 7;
static int s_alarm_minute;
static int s_alarm_repeat;
static int s_voice_busy;
static unsigned int s_alert_generation;
static int s_alert_sound_started;
static time_t s_alert_shown_at;
static int s_persistence_dirty;
static time_t s_persistence_retry;
static int s_voice_done;
static int s_alarm_ring_active;
static pthread_mutex_t s_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static lv_obj_t *s_todo_rows[REMINDER_MAX];
static lv_obj_t *s_todo_text[REMINDER_MAX];
static lv_obj_t *s_alarm_rows[REMINDER_MAX];
static lv_obj_t *s_alarm_text[REMINDER_MAX];
static lv_obj_t *s_todo_page;
static lv_obj_t *s_alarm_time;
static lv_obj_t *s_alarm_repeat_btn;
static lv_obj_t *s_alarm_repeat_label;
static lv_obj_t *s_alarm_hint;
static lv_obj_t *s_alarm_empty;
static lv_obj_t *s_alarm_empty_add;
static lv_obj_t *s_alarm_plus;
static lv_obj_t *s_alarm_delete;
static lv_obj_t *s_alert;
static lv_obj_t *s_alert_title;
static lv_obj_t *s_alert_text;

static void reminder_paint(void);

static int reminder_save(void)
{
  FILE *f = fopen(REMINDER_PATH ".tmp", "w");
  int i;
  int ok = 1;
  if (!f)
    {
      return -1;
    }
  if (fputs("#flowdesk-reminders-v2\n", f) == EOF) ok = 0;
  for (i = 0; ok && i < s_count; i++)
    {
      if (fprintf(f, "%d %d %d %ld %d %d %d %s\n", s_items[i].type,
                  s_items[i].repeat_daily, s_items[i].fired,
                  (long)s_items[i].due, s_items[i].hour, s_items[i].minute,
                  s_items[i].pending, s_items[i].text) < 0)
        {
          ok = 0;
          break;
        }
    }
  if (fclose(f) != 0)
    {
      ok = 0;
    }
  if (ok && rename(REMINDER_PATH ".tmp", REMINDER_PATH) != 0) ok = 0;
  if (!ok) unlink(REMINDER_PATH ".tmp");
  return ok ? 0 : -1;
}

static void reminder_load(void)
{
  FILE *f = fopen(REMINDER_PATH, "r");
  char line[160];
  int version = 1;

  s_count = 0;
  if (!f)
    {
      return;
    }
  while (s_count < REMINDER_MAX && fgets(line, sizeof(line), f))
    {
      int type;
      int pending = 0;
      int repeat;
      int fired;
      int hour;
      int minute;
      long due;
      char text[REMINDER_TEXT_MAX];
      int fields;
      if (strcmp(line, "#flowdesk-reminders-v2\n") == 0)
        {
          version = 2;
          continue;
        }
      if (version == 2)
        fields = sscanf(line, "%d %d %d %ld %d %d %d %63[^\r\n]",
                        &type, &repeat, &fired, &due, &hour, &minute, &pending, text);
      else
        fields = sscanf(line, "%d %d %d %ld %d %d %63[^\r\n]",
                        &type, &repeat, &fired, &due, &hour, &minute, text);
      if (fields != (version == 2 ? 8 : 7) || pending < 0 || pending > 1 || (type != REMINDER_TODO && type != REMINDER_ALARM) ||
          repeat < 0 || repeat > 1 || fired < 0 || fired > 1 || due <= 0 ||
          hour < 0 || hour > 23 || minute < 0 || minute > 59 || !text[0])
        {
          continue;
        }
      memset(&s_items[s_count], 0, sizeof(s_items[s_count]));
      s_items[s_count].type = type;
      s_items[s_count].repeat_daily = repeat;
      s_items[s_count].fired = fired;
      s_items[s_count].pending = pending;
      s_items[s_count].due = (time_t)due;
      s_items[s_count].hour = hour;
      s_items[s_count].minute = minute;
      snprintf(s_items[s_count].text, sizeof(s_items[s_count].text), "%s", text);
      s_count++;
    }
  fclose(f);
}

static int reminder_clock_valid(time_t now)
{
  struct tm tmv;
  if (!localtime_r(&now, &tmv))
    {
      return 0;
    }
  return tmv.tm_year + 1900 >= 2024 && dm_wifi_clock_valid();
}

int dm_reminder_ai_snapshot(bool alarm, dm_ai_local_record_t *out, int cap)
{
  int i, n = 0;
  if (!dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA)) return -2;
  if (!out || cap < 1) return -1;
  for (i = 0; i < s_count && n < cap; i++)
    if (s_items[i].type == (alarm ? REMINDER_ALARM : REMINDER_TODO) &&
        (!s_items[i].fired || s_items[i].pending))
      {
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].title, sizeof(out[n].title), "%s", s_items[i].text);
        out[n].due = s_items[i].due;
        out[n].repeat_daily = s_items[i].repeat_daily != 0;
        out[n].pending = s_items[i].pending != 0;
        n++;
      }
  return n;
}

static int reminder_copy_text(const char *in, char *out, size_t cap)
{
  size_t n;
  if (!in || !out || cap < 2)
    {
      return -1;
    }
  while (*in == ' ' || *in == '\t')
    {
      in++;
    }
  n = strlen(in);
  while (n > 0 && (in[n - 1] == ' ' || in[n - 1] == '\t' ||
                   in[n - 1] == '\r' || in[n - 1] == '\n'))
    {
      n--;
    }
  if (n == 0)
    {
      return -1;
    }
  if (n >= cap)
    {
      n = cap - 1;
      while (n > 0 && ((unsigned char)in[n] & 0xc0u) == 0x80u)
        {
          n--;
        }
    }
  memcpy(out, in, n);
  out[n] = '\0';
  {
    size_t i;
    for (i = 0; i < n; i++)
      {
        if (out[i] == '\r' || out[i] == '\n' || (unsigned char)out[i] < 0x20)
          {
            out[i] = ' ';
          }
      }
  }
  return n > 0 ? 0 : -1;
}

static int reminder_append(int type, const char *text, time_t due,
                           int repeat_daily, int hour, int minute)
{
  reminder_t item;
  reminder_t replaced;
  int index = s_count;
  int i;
  if (s_count >= REMINDER_MAX)
    {
      index = -1;
      for (i = 0; i < s_count; i++)
        if (s_items[i].fired && !s_items[i].pending && i != s_alert_idx)
          {
            index = i;
            break;
          }
      if (index < 0) return -2;
    }
  if (due <= time(NULL))
    {
      return -1;
    }
  memset(&item, 0, sizeof(item));
  item.type = type;
  item.repeat_daily = repeat_daily ? 1 : 0;
  item.due = due;
  item.hour = hour;
  item.minute = minute;
  if (reminder_copy_text(text, item.text, sizeof(item.text)) != 0)
    {
      return -1;
    }
  replaced = s_items[index];
  s_items[index] = item;
  if (index == s_count) s_count++;
  if (reminder_save() != 0)
    {
      s_items[index] = replaced;
      if (index == s_count - 1 && !replaced.due) s_count--;
      return -3;
    }
  reminder_paint();
  return 0;
}

int dm_todo_add_voice(const char *name, int delay_seconds)
{
  time_t now = time(NULL);
  if (!reminder_clock_valid(now))
    {
      return -4;
    }
  if (delay_seconds < 1 || delay_seconds > TODO_MAX_SECONDS)
    {
      return -1;
    }
  return reminder_append(REMINDER_TODO, name, now + delay_seconds, 0, 0, 0);
}

int dm_todo_add_at_voice(const char *name, int year, int month, int day,
                         int hour, int minute)
{
  time_t now = time(NULL);
  time_t due;
  struct tm target;
  struct tm verify;

  if (!reminder_clock_valid(now))
    {
      return -4;
    }
  if (year < 2024 || year > 2099 || month < 1 || month > 12 ||
      day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59)
    {
      return -1;
    }

  memset(&target, 0, sizeof(target));
  target.tm_year = year - 1900;
  target.tm_mon = month - 1;
  target.tm_mday = day;
  target.tm_hour = hour;
  target.tm_min = minute;
  target.tm_isdst = -1;
  due = mktime(&target);
  if (due == (time_t)-1 || !localtime_r(&due, &verify) ||
      verify.tm_year != year - 1900 || verify.tm_mon != month - 1 ||
      verify.tm_mday != day || verify.tm_hour != hour ||
      verify.tm_min != minute || due <= now)
    {
      return -1;
    }

  return reminder_append(REMINDER_TODO, name, due, 0, 0, 0);
}

int dm_alarm_add_voice(const char *name, int year, int month, int day,
                       int hour, int minute, int repeat_daily)
{
  time_t now = time(NULL);
  struct tm tmv;
  struct tm verify;
  time_t due;

  if (!reminder_clock_valid(now))
    {
      return -4;
    }
  if (year < 2024 || year > 2099 || month < 1 || month > 12 ||
      day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 ||
      minute > 59)
    {
      return -1;
    }
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = year - 1900;
  tmv.tm_mon = month - 1;
  tmv.tm_mday = day;
  tmv.tm_hour = hour;
  tmv.tm_min = minute;
  tmv.tm_isdst = -1;
  due = mktime(&tmv);
  if (due == (time_t)-1)
    {
      return -1;
    }
  localtime_r(&due, &verify);
  if (verify.tm_year != year - 1900 || verify.tm_mon != month - 1 ||
      verify.tm_mday != day || verify.tm_hour != hour || verify.tm_min != minute)
    {
      return -1;
    }
  if (repeat_daily && due <= now)
    {
      verify.tm_mday++;
      verify.tm_isdst = -1;
      due = mktime(&verify);
    }
  else if (due <= now)
    {
      return -1;
    }
  return reminder_append(REMINDER_ALARM, name, due, repeat_daily, hour, minute);
}

int dm_alarm_add_after_voice(const char *name, int delay_seconds)
{
  time_t now = time(NULL);
  struct tm tmv;
  if (!reminder_clock_valid(now))
    {
      return -4;
    }
  if (delay_seconds < 1 || delay_seconds > TODO_MAX_SECONDS)
    {
      return -1;
    }
  now += delay_seconds;
  if (!localtime_r(&now, &tmv))
    {
      return -1;
    }
  return reminder_append(REMINDER_ALARM, name, now, 0,
                         tmv.tm_hour, tmv.tm_min);
}

static void reminder_paint(void);
static void alarm_time_paint(void);
static void alarm_adjust_cb(lv_event_t *e);
static void alarm_repeat_cb(lv_event_t *e);
static void alarm_add_cb(lv_event_t *e);
static void alarm_delete_cb(lv_event_t *e);
static void alarm_open_add_cb(lv_event_t *e);
static void alarm_form_back_cb(lv_event_t *e);

static void reminder_back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_FEATURES);
}

static void alarm_open_add_cb(lv_event_t *e)
{
  time_t now = time(NULL);
  struct tm tmv;
  (void)e;

  s_alarm_repeat = 0;
  if (localtime_r(&now, &tmv) && tmv.tm_year + 1900 >= 2024)
    {
      tmv.tm_min++;
      if (tmv.tm_min >= 60)
        {
          tmv.tm_min = 0;
          tmv.tm_hour = (tmv.tm_hour + 1) % 24;
        }
      s_alarm_hour = tmv.tm_hour;
      s_alarm_minute = tmv.tm_min;
    }
  else
    {
      s_alarm_hour = 7;
      s_alarm_minute = 0;
    }
  alarm_time_paint();
  if (s_alarm_hint) lv_label_set_text(s_alarm_hint, "");
  dm_show(PAGE_ALARM_ADD);
}

static void alarm_form_back_cb(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_ALARM);
}

static void reminder_select(lv_event_t *e)
{
  uintptr_t data = (uintptr_t)lv_event_get_user_data(e);
  int row = (int)(data & 0xffffu);
  int type = (int)(data >> 16);
  int index;

  if (row < 0 || row >= REMINDER_MAX)
    {
      return;
    }
  index = type == REMINDER_TODO ? s_todo_map[row] : s_alarm_map[row];
  if (index < 0 || index >= s_count || s_items[index].type != type)
    {
      return;
    }
  if (type == REMINDER_TODO)
    {
      s_todo_sel = index;
    }
  else
    {
      s_alarm_sel = index;
    }
  reminder_paint();
}

static void reminder_delete(int type)
{
  int index = type == REMINDER_TODO ? s_todo_sel : s_alarm_sel;
  int old_count = s_count;
  int old_alert = s_alert_idx;
  reminder_t removed;
  int i;
  if (index < 0 || index >= s_count || s_items[index].type != type)
    {
      return;
    }
  if (index == s_alert_idx)
    {
      return;
    }
  removed = s_items[index];
  for (i = index; i + 1 < s_count; i++)
    {
      s_items[i] = s_items[i + 1];
    }
  s_count--;
  memset(&s_items[s_count], 0, sizeof(s_items[s_count]));
  if (reminder_save() != 0)
    {
      for (i = s_count; i > index; i--)
        {
          s_items[i] = s_items[i - 1];
        }
      s_items[index] = removed;
      s_count = old_count;
      s_alert_idx = old_alert;
      return;
    }
  if (s_alert_idx > index)
    {
      s_alert_idx--;
    }
  s_todo_sel = -1;
  s_alarm_sel = -1;
  reminder_paint();
}

int dm_reminder_ai_delete(const dm_ai_delete_request_t *request)
{
  reminder_t previous[REMINDER_MAX];
  int i, visible = 0, found = -1, type, old_count;
  if (!request || (request->kind != DM_AI_DATA_TODO &&
                   request->kind != DM_AI_DATA_ALARM)) return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(request->kind)) return DM_AI_DATA_DENIED;
  type = request->kind == DM_AI_DATA_ALARM ? REMINDER_ALARM : REMINDER_TODO;
  for (i = 0; i < s_count; i++)
    if (s_items[i].type == type && (!s_items[i].fired || s_items[i].pending))
      {
        visible++;
        if (dm_ai_data_match(request, s_items[i].text, visible))
          {
            if (found >= 0) return DM_AI_DATA_AMBIGUOUS;
            found = i;
          }
      }
  if (found < 0) return DM_AI_DATA_NOT_FOUND;
  if (found == s_alert_idx) return DM_AI_DATA_BUSY;
  if (!dm_ai_data_delete_allowed(request->kind)) return DM_AI_DATA_DENIED;
  memcpy(previous, s_items, sizeof(previous));
  old_count = s_count;
  for (i = found; i + 1 < s_count; i++) s_items[i] = s_items[i + 1];
  s_count--;
  memset(&s_items[s_count], 0, sizeof(s_items[s_count]));
  if (reminder_save() != 0)
    {
      memcpy(s_items, previous, sizeof(previous));
      s_count = old_count;
      return DM_AI_DATA_SAVE;
    }
  if (s_alert_idx > found) s_alert_idx--;
  s_todo_sel = s_alarm_sel = -1;
  reminder_paint();
  return 0;
}

static void todo_delete_cb(lv_event_t *e)
{
  (void)e;
  reminder_delete(REMINDER_TODO);
}

static void alarm_delete_cb(lv_event_t *e)
{
  (void)e;
  reminder_delete(REMINDER_ALARM);
}

static void format_remaining(time_t due, char *out, size_t cap)
{
  time_t now = time(NULL);
  long left = (long)(due - now);
  if (left <= 0)
    {
      snprintf(out, cap, "%s", "到时间了");
    }
  else if (left >= 86400)
    {
      snprintf(out, cap, "%ld天后", (left + 86399) / 86400);
    }
  else if (left >= 3600)
    {
      snprintf(out, cap, "%ld小时后", (left + 3599) / 3600);
    }
  else if (left >= 60)
    {
      snprintf(out, cap, "%ld分钟后", (left + 59) / 60);
    }
  else
    {
      snprintf(out, cap, "%ld秒后", left);
    }
}

static void reminder_paint(void)
{
  int todo_row = 0;
  int alarm_row = 0;
  int i;
  char detail[48];
  char line[REMINDER_TEXT_MAX + 56];
  char time_text[32];

  for (i = 0; i < REMINDER_MAX; i++)
    {
      s_todo_map[i] = -1;
      s_alarm_map[i] = -1;
      if (s_todo_rows[i]) lv_obj_add_flag(s_todo_rows[i], LV_OBJ_FLAG_HIDDEN);
      if (s_alarm_rows[i]) lv_obj_add_flag(s_alarm_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
  for (i = 0; i < s_count; i++)
    {
      lv_obj_t *row;
      lv_obj_t *label;
      if (s_items[i].type == REMINDER_TODO)
        {
          if (todo_row >= REMINDER_MAX || !s_todo_rows[todo_row]) continue;
          row = s_todo_rows[todo_row];
          label = s_todo_text[todo_row];
          s_todo_map[todo_row] = i;
          format_remaining(s_items[i].due, detail, sizeof(detail));
          snprintf(line, sizeof(line), "%s · %s%s", s_items[i].text, detail,
                   s_items[i].fired ? " · 已提醒" : "");
          lv_label_set_text(label, line);
          lv_obj_set_style_bg_color(row,
              lv_color_hex(i == s_todo_sel ? 0x0d3a4a : C_BTN), LV_PART_MAIN);
          lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
          todo_row++;
        }
      else
        {
          struct tm tmv;
          if (alarm_row >= REMINDER_MAX || !s_alarm_rows[alarm_row]) continue;
          row = s_alarm_rows[alarm_row];
          label = s_alarm_text[alarm_row];
          s_alarm_map[alarm_row] = i;
          localtime_r(&s_items[i].due, &tmv);
          snprintf(time_text, sizeof(time_text), "%02d:%02d %s",
                   tmv.tm_hour, tmv.tm_min,
                   s_items[i].repeat_daily ? "每天" : "一次");
          snprintf(line, sizeof(line), "%s · %s%s", time_text, s_items[i].text,
                   s_items[i].fired && !s_items[i].repeat_daily ? " · 已响" : "");
          lv_label_set_text(label, line);
          lv_obj_set_style_bg_color(row,
              lv_color_hex(i == s_alarm_sel ? 0x0d3a4a : C_BTN), LV_PART_MAIN);
          lv_obj_clear_flag(row, LV_OBJ_FLAG_HIDDEN);
          alarm_row++;
        }
    }

  if (s_alarm_empty)
    {
      if (alarm_row == 0)
        lv_obj_clear_flag(s_alarm_empty, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(s_alarm_empty, LV_OBJ_FLAG_HIDDEN);
    }
  if (s_alarm_empty_add)
    {
      if (alarm_row == 0)
        lv_obj_clear_flag(s_alarm_empty_add, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(s_alarm_empty_add, LV_OBJ_FLAG_HIDDEN);
    }
  if (s_alarm_plus)
    {
      if (alarm_row > 0)
        lv_obj_clear_flag(s_alarm_plus, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(s_alarm_plus, LV_OBJ_FLAG_HIDDEN);
    }
  if (s_alarm_delete)
    {
      if (alarm_row > 0 && s_alarm_sel >= 0)
        lv_obj_clear_flag(s_alarm_delete, LV_OBJ_FLAG_HIDDEN);
      else
        lv_obj_add_flag(s_alarm_delete, LV_OBJ_FLAG_HIDDEN);
    }
}

static int reminder_speech_current(unsigned int generation)
{
  int active;
  pthread_mutex_lock(&s_voice_lock);
  active = generation == s_alert_generation;
  pthread_mutex_unlock(&s_voice_lock);
  return active;
}

static int reminder_alarm_is_ringing(void)
{
  int active;
  pthread_mutex_lock(&s_voice_lock);
  active = s_alarm_ring_active;
  pthread_mutex_unlock(&s_voice_lock);
  return active;
}

static void *reminder_speech_worker(void *arg)
{
  reminder_speech_t *speech = arg;
  char line[REMINDER_TEXT_MAX + 48];
  int rc = -1;
  int beep_rc;
  int attempt;
  int speech_attempted = 0;

  if (!speech)
    {
      return NULL;
    }
  if (speech->is_alarm)
    {
      snprintf(line, sizeof(line), "闹钟响了：%s。", speech->text);
      dm_vol_alarm_begin();
    }
  else
    {
      snprintf(line, sizeof(line), "待办提醒：%s。", speech->text);
    }

  /* TODOs notify once with optional speech. Alarms repeat offline tones
   * until dismissed; never block their ringing on a cloud TTS request. */
  do
    {
      if (speech->is_alarm && !reminder_alarm_is_ringing())
        {
          break;
        }

      beep_rc = -EBUSY;
      for (; reminder_speech_current(speech->generation) &&
           (!speech->is_alarm || reminder_alarm_is_ringing()); )
        {
          beep_rc = dm_voice_beep();
          if (beep_rc != -EBUSY)
            {
              break;
            }
          usleep(250000);
        }
      if (beep_rc != 0 &&
          (!speech->is_alarm || reminder_alarm_is_ringing()))
        {
          syslog(LOG_WARNING, "[dm-remind] alert tone playback failed rc=%d\n",
                 beep_rc);
        }

      if (!speech->is_alarm && !speech_attempted &&
          reminder_speech_current(speech->generation))
        {
          speech_attempted = 1;
          if (dm_wifi_connected() &&
              dm_ai_permission_allowed(DM_AI_READ_HEALTH))
            {
              rc = dm_voice_tts_play_private(line);
            }
          if (rc != 0)
            {
              syslog(LOG_WARNING, "[dm-remind] spoken reminder unavailable\n");
            }
        }

      if (!speech->is_alarm || !reminder_alarm_is_ringing())
        {
          break;
        }

      /* Keep only a 100 ms gap between tones and recheck dismissal. */
      for (attempt = 0; attempt < 1 && reminder_alarm_is_ringing(); attempt++)
        {
          usleep(100000);
        }
    }
  while (speech->is_alarm && reminder_alarm_is_ringing());

  if (speech->is_alarm)
    {
      pthread_mutex_lock(&s_voice_lock);
      s_alarm_ring_active = 0;
      pthread_mutex_unlock(&s_voice_lock);
      dm_vol_alarm_end();
    }
  free(speech);
  pthread_mutex_lock(&s_voice_lock);
  s_voice_done = 1;
  pthread_mutex_unlock(&s_voice_lock);
  return NULL;
}

static int reminder_speak(int index)
{
  reminder_speech_t *speech;
  pthread_t th;
  pthread_attr_t attr;
  int rc;

  if (index < 0 || index >= s_count)
    {
      return -1;
    }
  pthread_mutex_lock(&s_voice_lock);
  if (s_voice_busy)
    {
      pthread_mutex_unlock(&s_voice_lock);
      return -1;
    }
  s_voice_busy = 1;
  s_voice_done = 0;
  pthread_mutex_unlock(&s_voice_lock);
  speech = malloc(sizeof(*speech));
  if (!speech)
    {
      pthread_mutex_lock(&s_voice_lock);
      s_voice_busy = 0;
      pthread_mutex_unlock(&s_voice_lock);
      return -1;
    }
  pthread_mutex_lock(&s_voice_lock);
  speech->generation = s_alert_generation;
  pthread_mutex_unlock(&s_voice_lock);
  speech->is_alarm = s_items[index].type == REMINDER_ALARM;
  snprintf(speech->text, sizeof(speech->text), "%s", s_items[index].text);
  pthread_mutex_lock(&s_voice_lock);
  s_alarm_ring_active = speech->is_alarm;
  pthread_mutex_unlock(&s_voice_lock);
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, DM_VOICE_NET_WORKER_STACK);
  rc = pthread_create(&th, &attr, reminder_speech_worker, speech);
  pthread_attr_destroy(&attr);
  if (rc != 0)
    {
      pthread_mutex_lock(&s_voice_lock);
      s_voice_busy = 0;
      s_alarm_ring_active = 0;
      pthread_mutex_unlock(&s_voice_lock);
      free(speech);
      return -1;
    }
  pthread_detach(th);
  return 0;
}

static void reminder_alert_dismiss(lv_event_t *e)
{
  (void)e;
  pthread_mutex_lock(&s_voice_lock);
  s_alarm_ring_active = 0;
  s_alert_generation++;
  pthread_mutex_unlock(&s_voice_lock);
  s_alert_idx = -1;
  s_alert_sound_started = 0;
  s_persistence_dirty = 1;
  s_persistence_retry = 0;
  if (s_alert)
    {
      lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    }
  reminder_paint();
}

static lv_obj_t *reminder_page(dm_page_t id, const char *title)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_t *back;
  lv_obj_t *label;
  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  g_dm_pages[id] = page;
  back = dm_btn(page, "←", "<", 36, 24, C_BTN, C_MUTED,
                reminder_back, NULL);
  lv_obj_set_pos(back, 8, 6);
  label = dm_lbl(page, title, title, g_dm_font_m, C_INK);
  lv_obj_set_pos(label, 50, 8);
  return page;
}

static lv_obj_t *reminder_list(lv_obj_t *page, int y, int height)
{
  lv_obj_t *sc = lv_obj_create(page);
  lv_obj_set_size(sc, 304, height);
  lv_obj_set_pos(sc, 8, y);
  lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sc, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_row(sc, 4, LV_PART_MAIN);
  lv_obj_set_scroll_dir(sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  return sc;
}

void dm_create_reminders(void)
{
  lv_obj_t *page;
  lv_obj_t *sc;
  lv_obj_t *row;
  lv_obj_t *label;
  lv_obj_t *button;
  lv_obj_t *time_panel;
  int i;

  mkdir("/data", 0755);
  reminder_load();

  s_todo_page = reminder_page(PAGE_TODO, "待办提醒");
  sc = reminder_list(s_todo_page, 34, 142);
  for (i = 0; i < REMINDER_MAX; i++)
    {
      row = lv_obj_create(sc);
      lv_obj_set_size(row, 292, 28);
      lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 3, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, reminder_select, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)(i | (REMINDER_TODO << 16)));
      s_todo_text[i] = dm_lbl(row, "", "", g_dm_font_s, C_INK);
      lv_obj_set_width(s_todo_text[i], 280);
      lv_label_set_long_mode(s_todo_text[i], LV_LABEL_LONG_DOT);
      lv_obj_set_pos(s_todo_text[i], 3, 4);
      s_todo_rows[i] = row;
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }
  label = dm_lbl(s_todo_page, "说：20分钟后开会，或11点提醒我开会",
                 "Say: meeting in 20 min, or remind me at 11",
                 g_dm_font_s, C_DIM);
  lv_obj_set_width(label, 302);
  lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
  lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 180);
  button = dm_btn(s_todo_page, "删除已选待办", "Delete selected", 288, 28,
                  C_BTN, C_INK, todo_delete_cb, NULL);
  lv_obj_set_pos(button, 16, 204);

  page = reminder_page(PAGE_ALARM, "闹钟");
  sc = reminder_list(page, 38, 142);
  for (i = 0; i < REMINDER_MAX; i++)
    {
      row = lv_obj_create(sc);
      lv_obj_set_size(row, 292, 32);
      lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, reminder_select, LV_EVENT_CLICKED,
                          (void *)(uintptr_t)(i | (REMINDER_ALARM << 16)));
      s_alarm_text[i] = dm_lbl(row, "", "", g_dm_font_s, C_INK);
      lv_obj_set_width(s_alarm_text[i], 280);
      lv_label_set_long_mode(s_alarm_text[i], LV_LABEL_LONG_DOT);
      lv_obj_set_pos(s_alarm_text[i], 3, 5);
      s_alarm_rows[i] = row;
      lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    }

  s_alarm_empty = dm_lbl(page, "暂无内容", "No alarms yet", g_dm_font_l,
                         MD_ON_SURFACE);
  lv_obj_set_width(s_alarm_empty, 300);
  lv_obj_set_style_text_align(s_alarm_empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(s_alarm_empty, 10, 82);
  s_alarm_empty_add = dm_btn(page, "＋  点击添加", "+  Add alarm", 180, 46,
                             MD_PRIMARY, MD_ON_PRIMARY,
                             alarm_open_add_cb, NULL);
  lv_obj_set_style_radius(s_alarm_empty_add, 23, LV_PART_MAIN);
  lv_obj_align(s_alarm_empty_add, LV_ALIGN_TOP_MID, 0, 128);
  s_alarm_delete = dm_btn(page, "删除", "Delete", 76, 34,
                          MD_ERROR, C_INK, alarm_delete_cb, NULL);
  lv_obj_set_pos(s_alarm_delete, 22, 194);
  lv_obj_add_flag(s_alarm_delete, LV_OBJ_FLAG_HIDDEN);
  s_alarm_plus = dm_btn(page, "+", "+", 48, 40,
                        MD_PRIMARY, MD_ON_PRIMARY, alarm_open_add_cb, NULL);
  lv_obj_set_style_radius(s_alarm_plus, 20, LV_PART_MAIN);
  lv_obj_align(s_alarm_plus, LV_ALIGN_TOP_MID, 0, 190);
  lv_obj_add_flag(s_alarm_plus, LV_OBJ_FLAG_HIDDEN);

  page = lv_obj_create(g_dm_root);
  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  g_dm_pages[PAGE_ALARM_ADD] = page;
  button = dm_btn(page, "←", "<", 36, 26, MD_SURFACE_HIGH,
                  MD_ON_SURFACE, alarm_form_back_cb, NULL);
  lv_obj_set_pos(button, 8, 6);
  label = dm_lbl(page, "新建闹钟", "New alarm", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(label, 54, 8);
  label = dm_lbl(page, "设置响铃时间", "Set alarm time", g_dm_font_s,
                 MD_ON_SURFACE_VARIANT);
  lv_obj_set_width(label, 300);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(label, 10, 40);

  time_panel = lv_obj_create(page);
  lv_obj_set_size(time_panel, 260, 54);
  lv_obj_align(time_panel, LV_ALIGN_TOP_MID, 0, 62);
  lv_obj_set_style_bg_color(time_panel, lv_color_hex(MD_SURFACE_CONTAINER),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(time_panel, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(time_panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(time_panel, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(time_panel, 0, LV_PART_MAIN);
  lv_obj_clear_flag(time_panel, LV_OBJ_FLAG_SCROLLABLE);
  s_alarm_time = dm_lbl(time_panel, "07:00", "07:00", g_dm_font_xl, MD_ON_SURFACE);
  lv_obj_set_width(s_alarm_time, 240);
  lv_obj_set_style_text_align(s_alarm_time, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_alarm_time, LV_ALIGN_CENTER, 0, 0);

  button = dm_btn(page, "− 时", "- hour", 62, 28, MD_SURFACE_HIGH,
                  MD_ON_SURFACE, alarm_adjust_cb, (void *)(intptr_t)-1);
  lv_obj_set_pos(button, 10, 124);
  button = dm_btn(page, "＋ 时", "+ hour", 62, 28, MD_SURFACE_HIGH,
                  MD_ON_SURFACE, alarm_adjust_cb, (void *)(intptr_t)1);
  lv_obj_set_pos(button, 78, 124);
  button = dm_btn(page, "− 分", "- min", 62, 28, MD_SURFACE_HIGH,
                  MD_ON_SURFACE, alarm_adjust_cb, (void *)(intptr_t)-60);
  lv_obj_set_pos(button, 180, 124);
  button = dm_btn(page, "＋ 分", "+ min", 62, 28, MD_SURFACE_HIGH,
                  MD_ON_SURFACE, alarm_adjust_cb, (void *)(intptr_t)60);
  lv_obj_set_pos(button, 248, 124);

  s_alarm_repeat_btn = dm_btn(page, "单次", "Once", 140, 30,
                              MD_SURFACE_HIGH, C_STAR, alarm_repeat_cb, NULL);
  lv_obj_align(s_alarm_repeat_btn, LV_ALIGN_TOP_MID, 0, 160);
  s_alarm_repeat_label = lv_obj_get_child(s_alarm_repeat_btn, 0);
  s_alarm_hint = dm_lbl(page, "", "", g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_width(s_alarm_hint, 300);
  lv_obj_set_style_text_align(s_alarm_hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(s_alarm_hint, 10, 190);
  button = dm_btn(page, "保存闹钟", "Save alarm", 180, 26,
                  MD_PRIMARY, MD_ON_PRIMARY, alarm_add_cb, NULL);
  lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 208);

  s_alert = lv_obj_create(g_dm_root);
  lv_obj_set_size(s_alert, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(s_alert, 0, 0);
  lv_obj_set_style_bg_color(s_alert, lv_color_hex(0x14202b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_alert, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_alert, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_alert, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_alert, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_alert, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
  s_alert_title = dm_lbl(s_alert, "提醒", "Reminder", g_dm_font_l, C_ACCENT);
  lv_obj_set_width(s_alert_title, 300);
  lv_obj_set_style_text_align(s_alert_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_alert_title, LV_ALIGN_TOP_MID, 0, 54);
  s_alert_text = dm_lbl(s_alert, "", "", g_dm_font_m, C_INK);
  lv_label_set_long_mode(s_alert_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_alert_text, 292);
  lv_obj_set_style_text_align(s_alert_text, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_alert_text, LV_ALIGN_TOP_MID, 0, 104);
  button = dm_btn(s_alert, "知道了", "Dismiss", 176, 38, C_ACCENT, C_EYE,
                  reminder_alert_dismiss, NULL);
  lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 162);

  alarm_time_paint();
  reminder_paint();
}

static void alarm_time_paint(void)
{
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", s_alarm_hour, s_alarm_minute);
  if (s_alarm_time) lv_label_set_text(s_alarm_time, buf);
  if (s_alarm_repeat_label)
    {
      lv_label_set_text(s_alarm_repeat_label,
                        s_alarm_repeat ? "每天重复" : "单次");
    }
}

static void alarm_adjust_cb(lv_event_t *e)
{
  int code = (int)(intptr_t)lv_event_get_user_data(e);
  if (code == 1) s_alarm_hour = (s_alarm_hour + 1) % 24;
  else if (code == -1) s_alarm_hour = (s_alarm_hour + 23) % 24;
  else if (code == 60) s_alarm_minute = (s_alarm_minute + 1) % 60;
  else if (code == -60) s_alarm_minute = (s_alarm_minute + 59) % 60;
  alarm_time_paint();
}

static void alarm_repeat_cb(lv_event_t *e)
{
  (void)e;
  s_alarm_repeat = !s_alarm_repeat;
  alarm_time_paint();
}

static void alarm_add_cb(lv_event_t *e)
{
  int rc;
  time_t now = time(NULL);
  struct tm tmv;
  (void)e;
  if (!localtime_r(&now, &tmv) || tmv.tm_year + 1900 < 2024)
    {
      if (s_alarm_hint) lv_label_set_text(s_alarm_hint, "请先校准设备时间");
      return;
    }
  tmv.tm_sec = 0;
  if (!s_alarm_repeat &&
      (s_alarm_hour < tmv.tm_hour ||
       (s_alarm_hour == tmv.tm_hour && s_alarm_minute <= tmv.tm_min)))
    {
      tmv.tm_mday++;
    }
  rc = dm_alarm_add_voice("闹钟", tmv.tm_year + 1900, tmv.tm_mon + 1,
                          tmv.tm_mday, s_alarm_hour, s_alarm_minute,
                          s_alarm_repeat);
  if (rc == 0)
    {
      s_alarm_sel = -1;
      reminder_paint();
      dm_show(PAGE_ALARM);
      return;
    }
  if (s_alarm_hint)
    {
      lv_label_set_text(s_alarm_hint,
                        rc == -2 ? "闹钟数量已满" :
                        rc == -4 ? "请先校准设备时间" : "设置失败");
    }
}

void dm_reminder_tick(void)
{
  time_t now = time(NULL);
  int i;
  int voice_busy;
  int changed = 0;
  int selected = -1;

  pthread_mutex_lock(&s_voice_lock);
  if (s_voice_done)
    {
      s_voice_done = 0;
      s_voice_busy = 0;
    }
  voice_busy = s_voice_busy;
  pthread_mutex_unlock(&s_voice_lock);
  if (!reminder_clock_valid(now)) return;

  /* Mark every due item, even while another alert is visible. */
  for (i = 0; i < s_count; i++)
    {
      reminder_t *item = &s_items[i];
      if (item->fired || item->due > now) continue;
      item->pending = 1;
      if (item->type == REMINDER_ALARM && item->repeat_daily)
        {
          struct tm next;
          localtime_r(&now, &next);
          next.tm_hour = item->hour;
          next.tm_min = item->minute;
          next.tm_sec = 0;
          next.tm_isdst = -1;
          if (mktime(&next) <= now) next.tm_mday++;
          item->due = mktime(&next);
        }
      else item->fired = 1;
      changed = 1;
    }

  if (changed || (s_persistence_dirty && now >= s_persistence_retry))
    {
      s_persistence_dirty = reminder_save() != 0;
      s_persistence_retry = now + 5;
      if (s_persistence_dirty)
        syslog(LOG_ERR, "[dm-remind] alert active; persistence retry pending\n");
    }

  for (i = 0; i < s_count; i++)
    if (s_items[i].pending &&
        (selected < 0 || (s_items[i].type == REMINDER_ALARM &&
                         s_items[selected].type != REMINDER_ALARM)))
      selected = i;

  /* An alarm preempts an ordinary TODO. The interrupted TODO stays queued. */
  if (s_alert_idx >= 0 && selected >= 0 &&
      s_items[s_alert_idx].type == REMINDER_TODO &&
      s_items[selected].type == REMINDER_ALARM)
    {
      s_items[s_alert_idx].pending = 1;
      reminder_alert_dismiss(NULL);
    }
  if (s_alert_idx >= 0 && s_items[s_alert_idx].type == REMINDER_TODO &&
      s_alert_sound_started && !voice_busy && now - s_alert_shown_at >= 10)
    reminder_alert_dismiss(NULL);

  if (s_alert_idx < 0 && selected >= 0)
    {
      s_alert_idx = selected;
      s_items[selected].pending = 0;
      s_alert_sound_started = 0;
      s_alert_shown_at = now;
      pthread_mutex_lock(&s_voice_lock);
      s_alert_generation++;
      pthread_mutex_unlock(&s_voice_lock);
      if (s_alert_title)
        lv_label_set_text(s_alert_title,
                          s_items[selected].type == REMINDER_ALARM
                            ? "闹钟响了" : "待办提醒");
      if (s_alert_text) lv_label_set_text(s_alert_text, s_items[selected].text);
      if (s_alert)
        {
          lv_obj_clear_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(s_alert);
        }
    }
  if (s_alert_idx >= 0 && !s_alert_sound_started && !voice_busy)
    {
      if (reminder_speak(s_alert_idx) == 0) s_alert_sound_started = 1;
    }
  reminder_paint();
}

#endif /* CONFIG_DESKMATE_APP */
