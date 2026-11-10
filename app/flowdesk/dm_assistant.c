/* Local voice/text intent routing for FlowDesk. */

#include "deskmate.h"
#include "dm_voice.h"
#include "dm_ai_permissions.h"
#include "dm_ai_log.h"
#include "dm_ai_local_read.h"
#include "dm_ai_data.h"

#ifdef CONFIG_DESKMATE_APP

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INTENT_TEXT_MAX 256
#define INTENT_REPLY_MAX 240
#define INTENT_WAIT_STEPS 160
#define INTENT_WAIT_US 50000
#define ASSISTANT_TODO_MAX_SECONDS (7 * 24 * 60 * 60)

typedef enum
{
  INTENT_NONE = 0,
  INTENT_CANCEL,
  INTENT_MED_OPEN,
  INTENT_FOCUS,
  INTENT_COUNTDOWN,
  INTENT_NOTE,
  INTENT_MOOD,
  INTENT_MOOD_RECORD,
  INTENT_MED_TAKE,
  INTENT_MBTI_OPEN,
  INTENT_WAKE,
  INTENT_GAME_ASK,
  INTENT_GAME,
  INTENT_GAME_CANCEL,
  INTENT_TODO_AFTER,
  INTENT_TODO_AT,
  INTENT_ALARM,
  INTENT_ALARM_AFTER,
  INTENT_TIME_ERROR,
  INTENT_DELETE_UNSUPPORTED,
  INTENT_READ_LOCAL,
  INTENT_READ_HEALTH,
  INTENT_DELETE_DATA
} assistant_intent_type_t;

typedef struct
{
  assistant_intent_type_t type;
  int seconds;
  int repeat_daily;
  dm_page_t game_page;
  int year;
  int month;
  int day;
  int hour;
  int minute;
  char text[INTENT_TEXT_MAX];
  int operation_rc;
  dm_ai_permission_t denied_permission;
  dm_ai_read_kind_t read_kind;
  dm_ai_health_query_t health_query;
  dm_ai_delete_request_t deletion;
} assistant_intent_t;

static pthread_mutex_t s_intent_lock = PTHREAD_MUTEX_INITIALIZER;
static assistant_intent_t s_intent;
static int s_intent_active;
static int s_intent_executing;
static int s_intent_done;
static int s_intent_rc;
static int s_waiting_game_choice;
static char s_intent_reply[INTENT_REPLY_MAX];

static int contains_any(const char *text, const char *const *words, size_t count)
{
  size_t i;
  for (i = 0; i < count; i++)
    {
      if (strstr(text, words[i]))
        {
          return 1;
        }
    }
  return 0;
}

static int chinese_digit(const char *p, size_t left, int *value)
{
  static const struct
  {
    const char *text;
    int value;
  } digits[] = {
    {"零", 0}, {"〇", 0}, {"一", 1}, {"二", 2}, {"两", 2},
    {"三", 3}, {"四", 4}, {"五", 5}, {"六", 6}, {"七", 7},
    {"八", 8}, {"九", 9}
  };
  size_t i;

  if (left < 3)
    {
      return 0;
    }
  for (i = 0; i < sizeof(digits) / sizeof(digits[0]); i++)
    {
      if (memcmp(p, digits[i].text, 3) == 0)
        {
          *value = digits[i].value;
          return 1;
        }
    }
  return 0;
}

static int chinese_unit(const char *p, size_t left, int *unit)
{
  if (left < 3)
    {
      return 0;
    }
  if (memcmp(p, "十", 3) == 0)
    {
      *unit = 10;
      return 1;
    }
  if (memcmp(p, "百", 3) == 0)
    {
      *unit = 100;
      return 1;
    }
  if (memcmp(p, "千", 3) == 0)
    {
      *unit = 1000;
      return 1;
    }
  return 0;
}

static int parse_number_slice(const char *text, size_t begin, size_t end,
                              int *value)
{
  size_t i;
  int ascii_only = 1;
  int total = 0;
  int section = 0;
  int number = -1;
  int saw = 0;

  if (!text || !value || begin >= end)
    {
      return 0;
    }
  for (i = begin; i < end; i++)
    {
      if (!isdigit((unsigned char)text[i]))
        {
          ascii_only = 0;
          break;
        }
    }
  if (ascii_only)
    {
      long parsed = 0;
      for (i = begin; i < end; i++)
        {
          int digit = text[i] - '0';
          if (parsed > (1000000 - digit) / 10)
            {
              return 0;
            }
          parsed = parsed * 10 + digit;
        }
      *value = (int)parsed;
      return 1;
    }

  i = begin;
  while (i < end)
    {
      int digit;
      int unit;
      if (chinese_digit(text + i, end - i, &digit))
        {
          number = digit;
          i += 3;
          saw = 1;
        }
      else if (chinese_unit(text + i, end - i, &unit))
        {
          int multiplier = number >= 0 ? number : 1;
          section += multiplier * unit;
          number = -1;
          i += 3;
          saw = 1;
        }
      else
        {
          return 0;
        }
    }
  if (!saw)
    {
      return 0;
    }
  if (number >= 0)
    {
      section += number;
    }
  total += section;
  if (total > 1000000)
    {
      return 0;
    }
  *value = total;
  return 1;
}

static int parse_number_before(const char *text, size_t end, size_t *start,
                               int *value)
{
  size_t begin;
  size_t number_end = end;

  while (number_end > 0 &&
         (text[number_end - 1] == ' ' || text[number_end - 1] == '\t' ||
          text[number_end - 1] == '\r' || text[number_end - 1] == '\n'))
    {
      number_end--;
    }
  begin = number_end;
  while (begin > 0)
    {
      int ignored;
      if (isdigit((unsigned char)text[begin - 1]))
        {
          begin--;
          continue;
        }
      if (begin >= 3 &&
          (chinese_digit(text + begin - 3, 3, &ignored) ||
           chinese_unit(text + begin - 3, 3, &ignored)))
        {
          begin -= 3;
          continue;
        }
      break;
    }
  if (!parse_number_slice(text, begin, number_end, value))
    {
      return 0;
    }
  if (start)
    {
      *start = begin;
    }
  return 1;
}

static const char *first_of(const char *text, const char *a, const char *b)
{
  const char *pa = a ? strstr(text, a) : NULL;
  const char *pb = b ? strstr(text, b) : NULL;
  if (!pa)
    {
      return pb;
    }
  if (!pb)
    {
      return pa;
    }
  return pa < pb ? pa : pb;
}

static int parse_duration_part(const char *text, const char *unit,
                               int unit_bytes, int multiplier,
                               int allow_half_hour, int max_seconds,
                               int *seconds)
{
  size_t start;
  int number;

  if (!unit)
    {
      return 0;
    }
  if (allow_half_hour && unit_bytes == 6 && unit >= text + 3 &&
      memcmp(unit - 3, "半", 3) == 0)
    {
      if (unit >= text + 6 && memcmp(unit - 6, "个", 3) == 0 &&
          parse_number_before(text, (size_t)(unit - text - 6), &start,
                              &number))
        {
          if (number < 0 || number > max_seconds / multiplier ||
              number * multiplier > max_seconds - 30 * 60)
            {
              return -1;
            }
          *seconds += number * multiplier + 30 * 60;
        }
      else
        {
          *seconds += 30 * 60;
        }
      return 1;
    }
  if (!parse_number_before(text, (size_t)(unit - text), &start, &number))
    {
      /* Accept the natural Mandarin form "一个小时". */
      if (allow_half_hour && unit_bytes == 6 && unit >= text + 3 &&
          memcmp(unit - 3, "个", 3) == 0 &&
          parse_number_before(text, (size_t)(unit - text - 3), &start,
                              &number))
        {
          /* parsed the number before 个 */
        }
      else
        {
          return -1;
        }
    }
  if (number < 0 || number > max_seconds / multiplier)
    {
      return -1;
    }
  *seconds += number * multiplier;
  return 1;
}

static int parse_duration_seconds(const char *text, int *seconds,
                                  int max_seconds)
{
  const char *hour_word = strstr(text, "小时");
  const char *minute_word = strstr(text, "分钟");
  const char *second_word = strstr(text, "秒");
  int total = 0;
  int parts = 0;
  int rc;

  if (!minute_word)
    {
      minute_word = strstr(text, "分");
    }
  rc = parse_duration_part(text, hour_word, 6, 60 * 60, 1,
                           max_seconds, &total);
  if (rc < 0)
    {
      return 0;
    }
  parts += rc;

  /* "半小时" is consumed as an hour component above; support an additional
   * half hour in forms such as "1小时半" as well. */
  if (!hour_word && strstr(text, "半小时"))
    {
      total += 30 * 60;
      parts++;
    }
  else if (hour_word && strstr(hour_word + 6, "半") &&
           (!minute_word || strstr(hour_word + 6, "半") < minute_word))
    {
      total += 30 * 60;
    }

  rc = parse_duration_part(text, minute_word,
                           minute_word && strstr(text, "分钟") == minute_word
                             ? 6 : 3,
                           60, 0, max_seconds, &total);
  if (rc < 0)
    {
      return 0;
    }
  parts += rc;

  rc = parse_duration_part(text, second_word, 3, 1, 0,
                           max_seconds, &total);
  if (rc < 0)
    {
      return 0;
    }
  parts += rc;

  if (parts == 0 || total < 1 || total > max_seconds)
    {
      return 0;
    }
  *seconds = total;
  return 1;
}

static int ascii_contains_ci(const char *text, const char *needle);
static int parse_datetime(const char *text, int *year, int *month, int *day,
                          int *hour, int *minute, size_t *date_start,
                          size_t *date_end, size_t *time_start,
                          size_t *time_end);

static const char *relative_marker(const char *text, size_t *bytes)
{
  const char *p = strstr(text, "之后");
  const char *q = strstr(text, "以后");
  const char *r = strstr(text, "后");
  const char *best = NULL;
  *bytes = 0;
  if (p) { best = p; *bytes = 6; }
  if (q && (!best || q < best)) { best = q; *bytes = 6; }
  if (r && (!best || r < best)) { best = r; *bytes = 3; }
  return best;
}

static void clean_relative_name(const char *text, char *out, size_t cap)
{
  const char *marker;
  const char *content;
  size_t marker_bytes;
  size_t len;
  static const char *const prefixes[] = {
    "提醒我", "提醒一下", "提醒", "叫醒我", "叫我", "帮我", "帮忙",
    "安排我", "记得", "请", "在", "我", "一下"
  };
  size_t i;
  if (!out || cap == 0)
    {
      return;
    }
  marker = relative_marker(text, &marker_bytes);
  content = marker ? marker + marker_bytes : NULL;
  if (!content)
    {
      snprintf(out, cap, "%s", "待办事项");
      return;
    }
  while (*content == ' ' || *content == '\t' || *content == ',' || *content == ':')
    {
      content++;
    }
  while (strncmp(content, "，", 3) == 0 || strncmp(content, "：", 3) == 0 ||
         strncmp(content, "、", 3) == 0)
    {
      content += 3;
    }
  while (*content)
    {
      int changed = 0;
      for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        {
          size_t n = strlen(prefixes[i]);
          if (strncmp(content, prefixes[i], n) == 0)
            {
              content += n;
              changed = 1;
              break;
            }
        }
      if (!changed)
        {
          break;
        }
      while (*content == ' ' || *content == '\t' || *content == ',' || *content == ':')
        {
          content++;
        }
      while (strncmp(content, "，", 3) == 0 || strncmp(content, "：", 3) == 0 ||
             strncmp(content, "、", 3) == 0)
        {
          content += 3;
        }
    }
  len = strlen(content);
  while (len > 0)
    {
      if (len >= 3 && (memcmp(content + len - 3, "。", 3) == 0 ||
                       memcmp(content + len - 3, "，", 3) == 0 ||
                       memcmp(content + len - 3, "！", 3) == 0 ||
                       memcmp(content + len - 3, "？", 3) == 0))
        {
          len -= 3;
        }
      else if (content[len - 1] == ' ' || content[len - 1] == '\t' ||
               content[len - 1] == ',' || content[len - 1] == '.')
        {
          len--;
        }
      else
        {
          break;
        }
    }
  if (len == 0)
    {
      snprintf(out, cap, "%s", "待办事项");
      return;
    }
  if (len >= cap)
    {
      len = cap - 1;
      while (len > 0 && ((unsigned char)content[len] & 0xc0u) == 0x80u)
        {
          len--;
        }
    }
  memcpy(out, content, len);
  out[len] = '\0';
}

static int contains_alarm_word(const char *text)
{
  static const char *const words[] = {
    "闹钟", "alarm", "起床", "叫醒", "叫我", "每天叫我", "每日闹钟"
  };
  return contains_any(text, words, sizeof(words) / sizeof(words[0])) ||
         ascii_contains_ci(text, "alarm");
}

static void clean_alarm_name(const char *text, char *out, size_t cap)
{
  const char *time_mark = first_of(text, "点", "时");
  const char *minute_mark;
  const char *content;
  size_t len;
  static const char *const prefixes[] = {
    "叫醒我", "叫我", "提醒我", "闹钟", "请", "帮我", "设置", "设个"
  };
  size_t i;
  if (!time_mark)
    {
      snprintf(out, cap, "%s", "闹钟");
      return;
    }
  content = time_mark + 3;
  minute_mark = strstr(content, "分");
  if (minute_mark)
    {
      content = minute_mark + 3;
    }
  else if (strncmp(content, "半", 3) == 0)
    {
      content += 3;
    }
  while (*content == ' ' || *content == '\t' || *content == ',' || *content == ':')
    {
      content++;
    }
  while (strncmp(content, "，", 3) == 0 || strncmp(content, "：", 3) == 0 ||
         strncmp(content, "、", 3) == 0)
    {
      content += 3;
    }
  while (*content)
    {
      int changed = 0;
      for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        {
          size_t n = strlen(prefixes[i]);
          if (strncmp(content, prefixes[i], n) == 0)
            {
              content += n;
              changed = 1;
              break;
            }
        }
      if (!changed) break;
      while (*content == ' ' || *content == '\t' || *content == ',' || *content == ':')
        {
          content++;
        }
      while (strncmp(content, "，", 3) == 0 || strncmp(content, "：", 3) == 0 ||
             strncmp(content, "、", 3) == 0)
        {
          content += 3;
        }
    }
  len = strlen(content);
  if (len == 0)
    {
      snprintf(out, cap, "%s", "闹钟");
      return;
    }
  if (len >= cap)
    {
      len = cap - 1;
      while (len > 0 && ((unsigned char)content[len] & 0xc0u) == 0x80u)
        {
          len--;
        }
    }
  memcpy(out, content, len);
  out[len] = '\0';
}

static int alarm_date_was_spoken(const char *text)
{
  const char *day_mark = first_of(text, "号", "日");
  const char *month_mark = strstr(text, "月");
  const char *year_mark = strstr(text, "年");
  size_t start;
  int value;

  if (day_mark && parse_number_before(text, (size_t)(day_mark - text),
                                      &start, &value))
    {
      return 1;
    }
  if (month_mark && parse_number_before(text, (size_t)(month_mark - text),
                                        &start, &value))
    {
      return 1;
    }
  if (year_mark && parse_number_before(text, (size_t)(year_mark - text),
                                       &start, &value))
    {
      return 1;
    }
  return 0;
}

static int parse_alarm_time(const char *text, assistant_intent_t *intent)
{
  size_t ds;
  size_t de;
  size_t ts;
  size_t te;
  const char *time_mark;
  const char *minute_mark;
  size_t hour_start;
  int hour;
  int minute = 0;
  int day_offset = 0;
  int next_day = 0;
  int has_datetime;
  time_t now;
  struct tm current;
  struct tm target;
  struct tm verify;
  time_t when;

  has_datetime = parse_datetime(text, &intent->year, &intent->month, &intent->day,
                                &intent->hour, &intent->minute, &ds, &de,
                                &ts, &te);
  if (has_datetime == 1)
    {
      const char *year_mark = strstr(text, "年");
      int explicit_year = year_mark != NULL;
      int explicit_month = strstr(text, "月") != NULL;
      int repeat_daily = strstr(text, "每天") || strstr(text, "每日") ||
                         strstr(text, "天天");
      int target_day = intent->day;
      int midnight_roll = 0;
      int attempt;

      if (explicit_year)
        {
          size_t year_start;
          int year_value;
          if (!parse_number_before(text, (size_t)(year_mark - text),
                                   &year_start, &year_value) ||
              year_value < 2024 || year_value > 2099)
            {
              return -2;
            }
          intent->year = year_value;
        }
      if ((strstr(text, "凌晨") || strstr(text, "早上") || strstr(text, "上午")) &&
          intent->hour == 12)
        {
          intent->hour = 0;
        }
      else if ((strstr(text, "下午") || strstr(text, "晚上") ||
                strstr(text, "傍晚") || strstr(text, "明晚")) &&
               intent->hour < 12)
        {
          intent->hour += 12;
        }
      else if ((strstr(text, "晚上") || strstr(text, "明晚")) &&
               intent->hour == 12)
        {
          intent->hour = 0;
          midnight_roll = 1;
        }

      now = time(NULL);
      if (!localtime_r(&now, &current) || current.tm_year + 1900 < 2024)
        {
          return -1;
        }
      if (!explicit_year)
        {
          intent->year = current.tm_year + 1900;
        }
      if (!explicit_month)
        {
          intent->month = current.tm_mon + 1;
        }
      for (attempt = 0; attempt < (explicit_year ? 1 : 13); attempt++)
        {
          memset(&target, 0, sizeof(target));
          target.tm_year = intent->year - 1900;
          target.tm_mon = intent->month - 1;
          target.tm_mday = target_day;
          target.tm_hour = 12;
          target.tm_isdst = -1;
          when = mktime(&target);
          if (when != (time_t)-1)
            {
              localtime_r(&when, &verify);
              if (verify.tm_year == intent->year - 1900 &&
                  verify.tm_mon == intent->month - 1 && verify.tm_mday == target_day)
                {
                  if (midnight_roll)
                    {
                      verify.tm_mday++;
                    }
                  verify.tm_hour = intent->hour;
                  verify.tm_min = intent->minute;
                  verify.tm_sec = 0;
                  verify.tm_isdst = -1;
                  when = mktime(&verify);
                  if (when != (time_t)-1 && when <= now && repeat_daily)
                    {
                      localtime_r(&when, &verify);
                      verify.tm_mday++;
                      verify.tm_hour = intent->hour;
                      verify.tm_min = intent->minute;
                      verify.tm_sec = 0;
                      verify.tm_isdst = -1;
                      when = mktime(&verify);
                    }
                  if (when != (time_t)-1)
                    {
                      localtime_r(&when, &target);
                      if (target.tm_hour == intent->hour &&
                          target.tm_min == intent->minute && when > now)
                        {
                          intent->year = target.tm_year + 1900;
                          intent->month = target.tm_mon + 1;
                          intent->day = target.tm_mday;
                          break;
                        }
                    }
                }
            }
          if (explicit_year)
            {
              return 0;
            }
          if (explicit_month)
            {
              intent->year++;
            }
          else
            {
              intent->month++;
              if (intent->month > 12)
                {
                  intent->month = 1;
                  intent->year++;
                }
            }
        }
      if (attempt >= (explicit_year ? 1 : 13))
        {
          return 0;
        }
      intent->repeat_daily = repeat_daily ? 1 : 0;
      clean_alarm_name(text, intent->text, sizeof(intent->text));
      return 1;
    }
  if (has_datetime < 0)
    {
      return -1;
    }
  if (alarm_date_was_spoken(text))
    {
      /* A spoken but invalid date must never silently become today's alarm. */
      return -2;
    }

  time_mark = first_of(text, "点", "时");
  if (!time_mark ||
      !parse_number_before(text, (size_t)(time_mark - text), &hour_start, &hour) ||
      hour < 0 || hour > 23)
    {
      return 0;
    }
  minute_mark = strstr(time_mark + 3, "分");
  if (minute_mark)
    {
      size_t minute_start;
      int parsed;
      if (!parse_number_before(text, (size_t)(minute_mark - text), &minute_start,
                               &parsed) ||
          minute_start < (size_t)(time_mark + 3 - text) || parsed < 0 || parsed > 59)
        {
          return 0;
        }
      minute = parsed;
    }
  else if (strncmp(time_mark + 3, "半", 3) == 0)
    {
      minute = 30;
    }

  if ((strstr(text, "凌晨") || strstr(text, "早上") || strstr(text, "上午")) &&
      hour == 12)
    {
      hour = 0;
    }
  else if ((strstr(text, "下午") || strstr(text, "晚上") ||
            strstr(text, "傍晚") || strstr(text, "明晚")) && hour < 12)
    {
      hour += 12;
    }

  now = time(NULL);
  if (!localtime_r(&now, &current) || current.tm_year + 1900 < 2024)
    {
      return -1;
    }
  if (strstr(text, "后天")) day_offset = 2;
  else if (strstr(text, "明天") || strstr(text, "明早") ||
           strstr(text, "明晚")) day_offset = 1;
  if (strstr(text, "晚上") && hour == 12)
    {
      hour = 0;
      next_day = 1;
    }
  else if (strstr(text, "夜里") && hour == 12)
    {
      hour = 0;
      next_day = 1;
    }
  memset(&target, 0, sizeof(target));
  target.tm_year = current.tm_year;
  target.tm_mon = current.tm_mon;
  target.tm_mday = current.tm_mday + day_offset + next_day;
  target.tm_hour = hour;
  target.tm_min = minute;
  target.tm_isdst = -1;
  when = mktime(&target);
  if (when == (time_t)-1)
    {
      return 0;
    }
  if (day_offset == 0 && when <= now &&
      !(strstr(text, "每天") || strstr(text, "每日") || strstr(text, "天天")))
    {
      target.tm_mday++;
      target.tm_isdst = -1;
      when = mktime(&target);
    }
  localtime_r(&when, &target);
  intent->year = target.tm_year + 1900;
  intent->month = target.tm_mon + 1;
  intent->day = target.tm_mday;
  intent->hour = hour;
  intent->minute = minute;
  intent->repeat_daily = strstr(text, "每天") || strstr(text, "每日") ||
                         strstr(text, "天天") ? 1 : 0;
  clean_alarm_name(text, intent->text, sizeof(intent->text));
  return 1;
}

static int ascii_contains_ci(const char *text, const char *needle)
{
  size_t n;
  const char *p;

  if (!text || !needle || !needle[0])
    {
      return 0;
    }
  n = strlen(needle);
  for (p = text; *p; p++)
    {
      size_t i;
      for (i = 0; i < n; i++)
        {
          unsigned char a = (unsigned char)p[i];
          unsigned char b = (unsigned char)needle[i];
          if (!a || tolower(a) != tolower(b))
            {
              break;
            }
        }
      if (i == n)
        {
          return 1;
        }
    }
  return 0;
}

static int is_wake_phrase(const char *text)
{
  int mentions_openvela = ascii_contains_ci(text, "openvela");
  if (!mentions_openvela)
    {
      return 0;
    }
  return strstr(text, "你好") != NULL ||
         (ascii_contains_ci(text, "hello") && mentions_openvela);
}

static int is_mbti_open_request(const char *text)
{
  static const char *const targets[] = {
    "mbti", "人格测试", "性格测试", "人格类型测试"
  };
  static const char *const actions[] = {
    "打开", "进入", "开始", "启动", "做", "测", "测试"
  };
  static const char *const questions[] = {
    "是什么意思", "是什么", "代表什么", "有什么含义"
  };
  int has_target = ascii_contains_ci(text, "mbti") ||
      contains_any(text, targets + 1,
                   sizeof(targets) / sizeof(targets[0]) - 1);

  if (!has_target || contains_any(text, questions,
                                  sizeof(questions) / sizeof(questions[0])))
    {
      return 0;
    }
  return contains_any(text, actions, sizeof(actions) / sizeof(actions[0]));
}

static int is_med_take_request(const char *text)
{
  static const char *const completed[] = {
    "吃了", "吃过", "吃完", "吃药了", "服了", "服过", "服用过",
    "已服", "已吃", "已经吃", "已经服", "刚吃", "刚刚吃",
    "刚服", "刚刚服", "服药了", "服完"
  };
  static const char *const medicine[] = {
    "药", "服用", "维生素", "胶囊", "钙片", "阿莫西林",
    "阿司匹林", "胰岛素"
  };
  return contains_any(text, completed, sizeof(completed) / sizeof(completed[0])) &&
         (contains_any(text, medicine, sizeof(medicine) / sizeof(medicine[0])) ||
          dm_med_voice_has_name(text));
}

static dm_page_t game_page_from_text(const char *text)
{
  if (strstr(text, "2048") || strstr(text, "二零四八") ||
      strstr(text, "两千零四十八"))
    {
      return PAGE_2048;
    }
  if (strstr(text, "猜数字") || strstr(text, "猜个数") ||
      strstr(text, "猜数"))
    {
      return PAGE_GUESS;
    }
  if (strstr(text, "24点") || strstr(text, "24 点") ||
      strstr(text, "二十四点"))
    {
      return PAGE_24;
    }
  return PAGE_COUNT;
}

static int is_game_request(const char *text)
{
  static const char *const triggers[] = {
    "小游戏", "玩游戏", "玩个游戏", "玩一局", "玩一下游戏", "一起玩",
    "想玩", "来一局"
  };
  return contains_any(text, triggers, sizeof(triggers) / sizeof(triggers[0]));
}

static int parse_datetime(const char *text, int *year, int *month, int *day,
                          int *hour, int *minute, size_t *date_start,
                          size_t *date_end, size_t *time_start,
                          size_t *time_end)
{
  const char *day_mark = first_of(text, "号", "日");
  const char *month_mark = strstr(text, "月");
  const char *time_mark;
  const char *minute_mark;
  const char *year_mark;
  size_t year_start = 0;
  size_t day_start;
  size_t hour_start;
  size_t month_start = 0;
  int explicit_month = 0;
  int explicit_year = 0;
  int month_value = 0;
  int year_value = 0;
  int day_value;
  int hour_value;
  int minute_value = 0;
  time_t now;
  struct tm current;
  struct tm target_tm;
  time_t target;
  struct tm verify;
  int attempt;
  int resolved = 0;

  if (!day_mark || !parse_number_before(text, (size_t)(day_mark - text),
                                        &day_start, &day_value) ||
      day_value < 1 || day_value > 31)
    {
      return 0;
    }
  if (month_mark && month_mark < day_mark)
    {
      if (!parse_number_before(text, (size_t)(month_mark - text), &month_start,
                               &month_value) || month_value < 1 ||
          month_value > 12)
        {
          return 0;
        }
      explicit_month = 1;
    }
  else
    {
      month_mark = NULL;
    }

  year_mark = month_mark ? strstr(text, "年") : NULL;
  if (year_mark && year_mark < month_mark)
    {
      if (!parse_number_before(text, (size_t)(year_mark - text), &year_start,
                               &year_value) || year_value < 2024 ||
          year_value > 2099)
        {
          return 0;
        }
      explicit_year = 1;
    }
  time_mark = first_of(day_mark + 3, "点", "时");
  if (!time_mark ||
      !parse_number_before(text, (size_t)(time_mark - text), &hour_start,
                           &hour_value) || hour_value < 0 || hour_value > 23)
    {
      return 0;
    }

  minute_value = 0;
  minute_mark = strstr(time_mark + 3, "分");
  if (minute_mark)
    {
      size_t minute_start;
      int parsed_minute;
      if (parse_number_before(text, (size_t)(minute_mark - text),
                              &minute_start, &parsed_minute) &&
          minute_start >= (size_t)(time_mark + 3 - text) &&
          parsed_minute >= 0 && parsed_minute <= 59)
        {
          minute_value = parsed_minute;
        }
      else
        {
          minute_mark = NULL;
        }
    }
  else if (strncmp(time_mark + 3, "半", 3) == 0)
    {
      minute_value = 30;
    }

  /* Resolve spoken 12-hour markers before choosing a future calendar date. */
  if ((strstr(text, "凌晨") || strstr(text, "早上") || strstr(text, "上午")) &&
      hour_value == 12)
    {
      hour_value = 0;
    }
  else if ((strstr(text, "下午") || strstr(text, "晚上") ||
            strstr(text, "傍晚") || strstr(text, "明晚")) && hour_value < 12)
    {
      hour_value += 12;
    }

  now = time(NULL);
  localtime_r(&now, &current);
  if (current.tm_year + 1900 < 2024)
    {
      return -1;
    }
  *year = explicit_year ? year_value : current.tm_year + 1900;
  *month = explicit_month ? month_value : current.tm_mon + 1;
  *day = day_value;
  *hour = hour_value;
  *minute = minute_value;

  /* For an omitted month, roll forward to the next valid occurrence. This
   * also handles dates such as the 31st when the current month is shorter. */
  for (attempt = 0; attempt < (explicit_year ? 1 : 12); attempt++)
    {
      memset(&target_tm, 0, sizeof(target_tm));
      target_tm.tm_year = *year - 1900;
      target_tm.tm_mon = *month - 1;
      target_tm.tm_mday = *day;
      target_tm.tm_hour = *hour;
      target_tm.tm_min = *minute;
      target_tm.tm_isdst = -1;
      target = mktime(&target_tm);
      if (target != (time_t)-1)
        {
          localtime_r(&target, &verify);
          if (verify.tm_year == *year - 1900 &&
              verify.tm_mon == *month - 1 && verify.tm_mday == *day &&
              verify.tm_hour == *hour && verify.tm_min == *minute &&
              (explicit_year || target > now))
            {
              resolved = 1;
              break;
            }
        }

      if (explicit_year)
        {
          break;
        }
      if (explicit_month)
        {
          (*year)++;
        }
      else
        {
          (*month)++;
          if (*month > 12)
            {
              *month = 1;
              (*year)++;
            }
        }
    }
  if (!resolved)
    {
      return 0;
    }

  if (date_start)
    {
      *date_start = explicit_year ? year_start
                                  : (explicit_month ? month_start : day_start);
    }
  if (date_end)
    {
      *date_end = (size_t)(day_mark - text) + 3;
    }
  if (time_start)
    {
      *time_start = hour_start;
    }
  if (time_end)
    {
      *time_end = (minute_mark ? (size_t)(minute_mark - text) + 3
                               : (size_t)(time_mark - text) + 3 +
                                 (minute_value == 30 ? 3 : 0));
    }
  return 1;
}

static void trim_reminder_punctuation(char *text, size_t *len)
{
  int changed = 1;
  while (*len > 0 && changed)
    {
      changed = 0;
      if (text[*len - 1] == ' ' || text[*len - 1] == '\t' ||
          text[*len - 1] == ',' || text[*len - 1] == ':' ||
          text[*len - 1] == '-' || text[*len - 1] == '.')
        {
          text[--(*len)] = '\0';
          changed = 1;
        }
      else if (*len >= 3 &&
               (memcmp(text + *len - 3, "。", 3) == 0 ||
                memcmp(text + *len - 3, "，", 3) == 0 ||
                memcmp(text + *len - 3, "！", 3) == 0 ||
                memcmp(text + *len - 3, "？", 3) == 0 ||
                memcmp(text + *len - 3, "；", 3) == 0 ||
                memcmp(text + *len - 3, "：", 3) == 0))
        {
          *len -= 3;
          text[*len] = '\0';
          changed = 1;
        }
    }
}

static void trim_reminder_clutter(char *text, size_t *len)
{
  static const char *const suffixes[] = {
    "帮我记录一下", "帮我记下来", "帮我记一下", "帮我记下",
    "帮我添加待办", "添加到待办", "记一下", "记下来", "记下",
    "提醒我", "提醒一下", "帮我提醒", "到时候", "请", "在"
  };
  size_t i;
  int changed = 1;

  while (*len > 0 && changed)
    {
      changed = 0;
      for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
        {
          size_t n = strlen(suffixes[i]);
          if (*len >= n && memcmp(text + *len - n, suffixes[i], n) == 0)
            {
              *len -= n;
              text[*len] = '\0';
              trim_reminder_punctuation(text, len);
              changed = 1;
              break;
            }
        }
    }
}

static void clean_relative_todo_name(const char *text, char *out, size_t cap)
{
  static const char *const leading[] = {
    "有一个", "有一场", "有个", "有场", "的"
  };
  size_t len;
  size_t i;

  if (!out || cap == 0)
    {
      return;
    }
  clean_relative_name(text, out, cap);
  len = strlen(out);
  trim_reminder_punctuation(out, &len);
  trim_reminder_clutter(out, &len);
  trim_reminder_punctuation(out, &len);
  for (i = 0; i < sizeof(leading) / sizeof(leading[0]); i++)
    {
      size_t n = strlen(leading[i]);
      if (len >= n && memcmp(out, leading[i], n) == 0)
        {
          memmove(out, out + n, len - n + 1);
          len -= n;
          break;
        }
    }
  if (strcmp(out, "会") == 0 || strcmp(out, "有会") == 0 ||
      strcmp(out, "要开会") == 0 || strcmp(out, "开个会") == 0)
    {
      snprintf(out, cap, "%s", "开会");
    }
  else if (len == 0)
    {
      snprintf(out, cap, "%s", "待办事项");
    }
}

static int is_meeting_schedule(const char *text)
{
  static const char *const meeting_words[] = {
    "开会", "会议", "参会", "有个会", "有一个会", "有场会", "有一场会"
  };
  static const char *const questions[] = {
    "怎么", "如何", "什么", "为什么", "哪", "能不能", "可不可以"
  };

  return contains_any(text, meeting_words,
                      sizeof(meeting_words) / sizeof(meeting_words[0])) &&
         !contains_any(text, questions, sizeof(questions) / sizeof(questions[0]));
}

static void clean_todo_at_name(const char *text, char *out, size_t cap)
{
  size_t len;

  if (!out || cap == 0)
    {
      return;
    }

  clean_alarm_name(text, out, cap);
  if (strcmp(out, "闹钟") == 0)
    {
      snprintf(out, cap, "%s", "待办事项");
      return;
    }

  len = strlen(out);
  trim_reminder_clutter(out, &len);
  trim_reminder_punctuation(out, &len);
  if (len == 0)
    {
      snprintf(out, cap, "%s", "待办事项");
    }
}

static void clean_reminder_name(const char *text, size_t date_start,
                                size_t date_end, size_t time_start,
                                size_t time_end, char *out, size_t cap)
{
  char tmp[INTENT_TEXT_MAX];
  size_t len = strlen(text);
  size_t start = date_start;
  size_t end = date_end;
  size_t tstart = time_start;
  size_t tend = time_end;
  size_t out_len = 0;
  static const char *const leading[] = {
    "帮我设置提醒", "帮我", "给我", "提醒一下", "提醒我", "我提醒我", "倒数日",
    "记一个", "计一个", "记一下", "记下来", "在", "请", "把", "到时候"
  };
  size_t i;

  if (len >= sizeof(tmp))
    {
      snprintf(out, cap, "%s", "提醒事项");
      return;
    }
  memcpy(tmp, text, len);
  tmp[len] = '\0';

  if (tstart >= end)
    {
      memmove(tmp + tstart, tmp + tend, len - tend + 1);
      len -= tend - tstart;
      memmove(tmp + start, tmp + end, len - end + 1);
      len -= end - start;
    }
  else if (start >= tend)
    {
      memmove(tmp + start, tmp + end, len - end + 1);
      len -= end - start;
      memmove(tmp + tstart, tmp + tend, len - tend + 1);
      len -= tend - tstart;
    }
  else
    {
      memmove(tmp + start, tmp + end, len - end + 1);
      len -= end - start;
      if (tstart > end)
        {
          tstart -= end - start;
          tend -= end - start;
        }
      memmove(tmp + tstart, tmp + tend, len - tend + 1);
      len -= tend - tstart;
    }

  while (len > 0)
    {
      int changed = 0;
      while (len > 0 && (tmp[0] == ' ' || tmp[0] == '\t' || tmp[0] == ',' ||
                         tmp[0] == ':' || tmp[0] == '-'))
        {
          memmove(tmp, tmp + 1, len);
          len--;
        }
      while (len >= 3 &&
             (memcmp(tmp, "，", 3) == 0 || memcmp(tmp, "：", 3) == 0 ||
              memcmp(tmp, "、", 3) == 0))
        {
          memmove(tmp, tmp + 3, len - 2);
          len -= 3;
        }
      for (i = 0; i < sizeof(leading) / sizeof(leading[0]); i++)
        {
          size_t n = strlen(leading[i]);
          if (len >= n && memcmp(tmp, leading[i], n) == 0)
            {
              memmove(tmp, tmp + n, len - n + 1);
              len -= n;
              changed = 1;
              break;
            }
        }
      if (!changed)
        {
          break;
        }
    }
  trim_reminder_clutter(tmp, &len);
  trim_reminder_punctuation(tmp, &len);

  if (len == 0)
    {
      snprintf(out, cap, "%s", "提醒事项");
      return;
    }
  out_len = len < cap - 1 ? len : cap - 1;
  if (out_len < len)
    {
      while (out_len > 0 && ((unsigned char)tmp[out_len] & 0xc0u) == 0x80u)
        {
          out_len--;
        }
    }
  memcpy(out, tmp, out_len);
  out[out_len] = '\0';
}

static int parse_local_read_request(const char *text, dm_ai_read_kind_t *kind)
{
  static const char *const objects[] = {"备忘", "待办", "闹钟", "倒数日"};
  static const char *const verbs[] = {
    "读取", "读一下", "读出", "读给我", "念一下", "查看", "查询",
    "看看", "看一下", "有哪些", "有什么", "是什么", "列出", "告诉我", "内容", "列表"
  };
  static const char *const writes[] = {
    "添加", "新建", "保存", "记一下", "记下", "设置", "设定",
    "删除", "删掉", "清空", "移除"
  };
  size_t i;
  if (!contains_any(text, verbs, sizeof(verbs) / sizeof(verbs[0])) ||
      contains_any(text, writes, sizeof(writes) / sizeof(writes[0]))) return 0;
  for (i = 0; i < sizeof(objects) / sizeof(objects[0]); i++)
    if (strstr(text, objects[i])) { *kind = (dm_ai_read_kind_t)i; return 1; }
  return 0;
}

static int parse_health_read_request(const char *text, dm_ai_health_query_t *query)
{
  static const char *const writes[] = {
    "删除", "删掉", "移除", "清空", "清除", "添加", "新建", "保存", "记录一下", "记一下", "记下"
  };
  static const char *const asks[] = {
    "多少", "几杯", "什么时候", "几点", "哪天", "多久", "最近一条", "最新一条",
    "最后一条", "上次", "最近一次", "最新的", "查看", "查询", "读一下", "读出", "告诉我",
    "是什么", "怎么样", "有哪些", "有什么"
  };
  int water, mood;
  if (contains_any(text, writes, sizeof(writes) / sizeof(writes[0]))) return 0;
  if (!contains_any(text, asks, sizeof(asks) / sizeof(asks[0]))) return 0;
  if (strstr(text, "应该") || strstr(text, "建议") || strstr(text, "合适") ||
      strstr(text, "每天应该")) return 0;
  water = strstr(text, "喝水") || strstr(text, "饮水") || strstr(text, "杯水") ||
          (strstr(text, "喝") && strstr(text, "水")) ||
          (strstr(text, "喝了") && strstr(text, "杯"));
  mood = strstr(text, "心情") || strstr(text, "情绪记录");
  if (!water && !mood) return 0;
  *query = water ? DM_AI_HEALTH_WATER : DM_AI_HEALTH_LAST_MOOD;
  if (water && mood) *query = DM_AI_HEALTH_WATER_AND_MOOD;
  if (strstr(text, "昨天") || strstr(text, "前天") ||
      strstr(text, "上周") || strstr(text, "上个月") || strstr(text, "历史") ||
      strstr(text, "哪天") || strstr(text, "这周") || strstr(text, "本周") ||
      strstr(text, "明天") || strstr(text, "后天") || strstr(text, "星期") ||
      strstr(text, "毫升") || strstr(text, "多少升") || strstr(text, "每次") ||
      strstr(text, "月") || strstr(text, "号") ||
      strstr(text, "一共") || strstr(text, "总共") ||
      (water && !mood && (strstr(text, "什么时候") || strstr(text, "几点") || strstr(text, "多久"))))
    *query = DM_AI_HEALTH_UNAVAILABLE;
  return 1;
}

static void delete_trim(char *text)
{
  size_t n = strlen(text);
  while (n && (text[0] == ' ' || text[0] == '\t'))
    { memmove(text, text + 1, n); n--; }
  while (n && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '.' ||
               text[n - 1] == '!' || text[n - 1] == ',')) text[--n] = '\0';
  while (n >= 3 && (memcmp(text + n - 3, "。", 3) == 0 ||
                    memcmp(text + n - 3, "！", 3) == 0 ||
                    memcmp(text + n - 3, "，", 3) == 0))
    { n -= 3; text[n] = '\0'; }
}

static void delete_strip_context(char *text)
{
  static const char *const words[] = {"今天", "今日", "我的", "的"};
  size_t i;
  int changed;
  do {
    changed = 0;
    delete_trim(text);
    for (i = 0; i < sizeof(words) / sizeof(words[0]); i++)
      {
        size_t n = strlen(words[i]);
        if (strncmp(text, words[i], n) == 0)
          { memmove(text, text + n, strlen(text + n) + 1); changed = 1; break; }
      }
  } while (changed);
}

static int delete_index(const char *text)
{
  size_t len = strlen(text), start = strlen("第"), end;
  int value;
  if (strncmp(text, "第", start) != 0) return 0;
  for (end = start; end + 3 <= len; end++)
    if ((memcmp(text + end, "条", 3) == 0 || memcmp(text + end, "个", 3) == 0 ||
         memcmp(text + end, "项", 3) == 0) &&
        (end + 3 == len || strcmp(text + end + 3, "的") == 0) &&
        parse_number_slice(text, start, end, &value))
      return value > 0 && value <= 12 ? value : 0;
  return 0;
}

static int delete_title(const char *text, char *out, size_t cap)
{
  size_t n = strlen(text), begin = 0, end = n;
  if (!n) return 0;
  if (text[0] == '"' || text[0] == '\'')
    {
      if (n < 3 || text[n - 1] != text[0] || strchr(text + 1, text[0]) != text + n - 1) return 0;
      begin = 1; end--;
    }
  else if (strncmp(text, "“", 3) == 0 || strncmp(text, "‘", 3) == 0)
    {
      const char *close = strncmp(text, "“", 3) == 0 ? "”" : "’";
      if (n < 7 || memcmp(text + n - 3, close, 3) || strstr(text + 3, close) != text + n - 3) return 0;
      begin = 3; end -= 3;
    }
  else if (strstr(text, "和") || strstr(text, "以及") || strstr(text, "然后") ||
           strstr(text, "全部") || strstr(text, "所有") || strstr(text, "第") ||
           strstr(text, "最近") || strstr(text, "这条") || strstr(text, "那条")) return 0;
  if (end <= begin || end - begin >= cap) return 0;
  memcpy(out, text + begin, end - begin);
  out[end - begin] = '\0';
  return 1;
}

/* 0=no delete command, -1=needs a precise target, 1=one explicit target. */
static int parse_delete_request(const char *text, dm_ai_delete_request_t *request)
{
  static const char *const verbs[] = {"删除", "删掉", "清空", "清除", "移除"};
  static const struct {const char *name; dm_ai_data_kind_t kind;} objects[] = {
    {"备忘录", DM_AI_DATA_NOTE}, {"备忘", DM_AI_DATA_NOTE},
    {"待办事项", DM_AI_DATA_TODO}, {"待办", DM_AI_DATA_TODO},
    {"闹钟", DM_AI_DATA_ALARM}, {"倒数日", DM_AI_DATA_COUNTDOWN},
    {"心情记录", DM_AI_DATA_MOOD}, {"心情", DM_AI_DATA_MOOD},
    {"喝水记录", DM_AI_DATA_WATER}, {"饮水记录", DM_AI_DATA_WATER},
    {"喝水", DM_AI_DATA_WATER}, {"饮水", DM_AI_DATA_WATER},
    {"服药记录", DM_AI_DATA_MEDICATION}, {"吃药记录", DM_AI_DATA_MEDICATION}
  };
  const char *command = NULL, *object = NULL, *p;
  size_t verb_len = 0, object_len = 0, i, len;
  char before[INTENT_TEXT_MAX], after[INTENT_TEXT_MAX];
  int index, today;
  memset(request, 0, sizeof(*request));
  request->kind = DM_AI_DATA_COUNT;
  for (i = 0; i < sizeof(verbs) / sizeof(verbs[0]); i++)
    {
      const char *found = strstr(text, verbs[i]);
      if (found && (!command || found < command))
        { command = found; verb_len = strlen(verbs[i]); }
    }
  if (!command) return 0;
  /* A note containing the word delete remains a note, not an operation. */
  p = strstr(text, "记");
  if (p && p < command) return 0;
  /* A quoted example, conditional or explanation is not a command. */
  {
    static const char *const prefixes[] = {
      "麻烦你", "麻烦", "帮我", "替我", "给我", "请", "把", "我想", "我要", "我需要", "现在", "先"
    };
    size_t prefix_len = (size_t)(command - text);
    int changed;
    if (prefix_len >= sizeof(before)) return -1;
    memcpy(before, text, prefix_len); before[prefix_len] = '\0';
    do {
      changed = 0; delete_trim(before);
      for (i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        {
          size_t n = strlen(prefixes[i]);
          if (strncmp(before, prefixes[i], n) == 0)
            { memmove(before, before + n, strlen(before + n) + 1); changed = 1; break; }
        }
    } while (changed);
    if (before[0]) return -1;
  }
  if (strstr(text, "吗") || strstr(text, "？") || strchr(text, '?') ||
      strstr(text, "是否") || strstr(text, "怎么") || strstr(text, "如何") ||
      strstr(text, "能不能") || strstr(text, "可不可以")) return -1;
  p = command + verb_len;
  for (i = 0; i < sizeof(objects) / sizeof(objects[0]); i++)
    {
      const char *found = strstr(p, objects[i].name);
      if (found && (!object || found < object))
        { object = found; object_len = strlen(objects[i].name); request->kind = objects[i].kind; }
    }
  if (!object) return -1;
  len = (size_t)(object - p);
  if (len >= sizeof(before) || strlen(object + object_len) >= sizeof(after)) return -1;
  memcpy(before, p, len); before[len] = '\0';
  snprintf(after, sizeof(after), "%s", object + object_len);
  today = strstr(before, "今天") || strstr(before, "今日");
  delete_strip_context(before);
  delete_trim(after);
  if (strcmp(after, "记录") == 0) after[0] = '\0';
  if (request->kind < DM_AI_DATA_MOOD && today) return -1;
  if (request->kind == DM_AI_DATA_WATER)
    {
      if (!today || after[0]) return -1;
      if (!before[0]) request->selector = DM_AI_SELECT_TODAY;
      else
        {
          const char *cup = strstr(before, "杯");
          if (!cup || cup[3] || !parse_number_slice(before, 0, (size_t)(cup - before), &index) ||
              index < 1 || index > 1000) return -1;
          request->selector = DM_AI_SELECT_CUPS; request->number = index;
        }
      return 1;
    }
  if (strncmp(command, "清空", 6) == 0 || strstr(before, "全部") || strstr(before, "所有")) return -1;
  index = delete_index(before[0] ? before : after);
  if (index && !(before[0] && after[0]))
    { request->selector = DM_AI_SELECT_INDEX; request->number = index; return 1; }
  if (request->kind == DM_AI_DATA_MOOD)
    {
      const char *part = before[0] ? before : after;
      if (before[0] && after[0]) return -1;
      if (strcmp(part, "最近一条") && strcmp(part, "最新一条") &&
          strcmp(part, "最近一次") && strcmp(part, "最后一条")) return -1;
      request->selector = DM_AI_SELECT_LATEST;
      return 1;
    }
  if (request->kind == DM_AI_DATA_MEDICATION && before[0] && !after[0])
    {
      len = strlen(before);
      if (len >= 3 && strcmp(before + len - 3, "的") == 0) before[len - 3] = '\0';
      snprintf(after, sizeof(after), "%s", before); before[0] = '\0';
    }
  if (before[0] || !delete_title(after, request->title, sizeof(request->title))) return -1;
  request->selector = DM_AI_SELECT_TITLE;
  return 1;
}

static int parse_intent(const char *text, assistant_intent_t *intent)
{
  static const char *const note_triggers[] = {
    "帮我记一下", "帮我记下", "记一下", "记下来", "记下", "记一个",
    "记个", "备忘里"
  };
  static const char *const reminder_triggers[] = {
    "提醒", "闹钟", "倒数日", "记一下", "记一个", "计一个", "安排",
    "设置提醒", "设个提醒"
  };
  static const char *const mood_words[] = {
    "不开心", "不太开心", "不高兴", "不太高兴", "心情不好",
    "心情不太好", "心情差", "状态不好", "状态不太好", "情绪不好",
    "情绪不太好", "情绪低落", "低落", "提不起劲", "心累", "心烦",
    "难受", "难过", "伤心", "焦虑", "压力大", "压力很大", "有压力", "很累",
    "疲惫", "孤独", "烦躁", "沮丧"
  };
  static const char *const mood_state_words[] = {
    "不开心", "不太开心", "不高兴", "不太高兴", "心情不好",
    "心情不太好", "心情差", "状态不好", "状态不太好", "情绪不好",
    "情绪不太好", "情绪低落", "不太好", "不好受", "低落", "提不起劲",
    "心累", "心烦",
    "难受", "难过", "伤心", "焦虑", "压力大", "压力很大", "有压力", "很累",
    "疲惫", "孤独", "烦躁", "沮丧", "开心", "高兴", "愉快", "兴奋", "激动",
    "自信", "期待", "满足", "满意", "感动", "平静", "平和",
    "放松", "安心", "若有所思", "思考", "惊讶", "生气", "愤怒",
    "恼火", "累", "困", "紧张", "担心", "不安", "孤单", "有压力",
    "心情不错", "心情很好", "心情挺好", "心情一般", "感觉一般",
    "一般般", "心平气和",
    "坐立不安", "充满活力"
  };
  static const char *const mood_record_triggers[] = {
    "记录心情", "记录一下心情", "记录一下我的心情", "记下心情",
    "记一下心情", "记一下我的心情", "记录我的心情", "记下我的心情",
    "保存心情", "保存我的心情", "心情记录", "打卡心情",
    "帮我记心情", "帮我记录心情"
  };
  static const char *const mood_self_report[] = {
    "我现在", "我今天", "今天我", "最近我", "我最近", "我的心情",
    "我心情", "最近心情", "今天心情", "这几天心情", "我觉得",
    "我感觉", "今天感觉", "现在感觉", "我不太好", "我不好受",
    "我状态不好", "我状态不太好", "我很", "我有点", "我挺", "我好"
  };
  static const char *const no_reminder[] = {
    "不要提醒", "别提醒", "不用提醒", "取消提醒", "不要闹钟", "取消闹钟"
  };
  static const char *const no_focus[] = {
    "不要专注", "别专注", "不用专注", "取消专注", "不要计时", "别计时"
  };
  static const char *const todo_triggers[] = {
    "提醒", "待办", "记得", "到时候", "todo"
  };
  static const char *const note_destinations[] = {
    "到备忘", "在备忘", "入备忘", "备忘里", "备忘录里",
    "添加备忘", "新建备忘"
  };
  size_t date_start = 0;
  size_t date_end = 0;
  size_t time_start = 0;
  size_t time_end = 0;
  int has_dt;
  int seconds;
  int alarm_rc;
  size_t relative_bytes;
  int alarm_requested;
  dm_page_t game_page;
  int cancel_reminder;
  int cancel_focus;
  int todo_requested;
  int explicit_note;

  memset(intent, 0, sizeof(*intent));
  /* Reject ambiguous negative requests before any state-changing rule. */
  if (strstr(text, "不要") || strstr(text, "不用") || strstr(text, "取消") ||
      strstr(text, "别设置") || strstr(text, "别记录") || strstr(text, "别提醒") ||
      strstr(text, "别计时") || strstr(text, "不玩了") ||
      strstr(text, "还没吃") || strstr(text, "没有吃") || strstr(text, "没吃") ||
      strstr(text, "没服") || strstr(text, "没有服") ||
      strstr(text, "准备吃") || strstr(text, "准备服") || strstr(text, "别删") ||
      strstr(text, "不删除") || strstr(text, "不删掉") || strstr(text, "别移除") ||
      strstr(text, "别清空") || strstr(text, "不清空") || strstr(text, "别清除") ||
      strstr(text, "不清除") || strstr(text, "不移除") || strstr(text, "不想删") ||
      strstr(text, "不需要删") || strstr(text, "无需删") || strstr(text, "不能删") ||
      strstr(text, "禁止删") || strstr(text, "不允许删") || strstr(text, "不是要删"))
    {
      intent->type = INTENT_CANCEL;
      return 1;
    }
  /* Read-only questions are handled locally; they never create records. */
  if (parse_local_read_request(text, &intent->read_kind))
    { intent->type = INTENT_READ_LOCAL; return 1; }
  if (parse_health_read_request(text, &intent->health_query))
    { intent->type = INTENT_READ_HEALTH; return 1; }
  {
    int deletion = parse_delete_request(text, &intent->deletion);
    if (deletion)
      { intent->type = deletion > 0 ? INTENT_DELETE_DATA : INTENT_DELETE_UNSUPPORTED; return 1; }
  }
  /* Other questions never confirm a state-changing action. */
  if (strstr(text, "吗") || strstr(text, "是否") || strstr(text, "有没有") ||
      strstr(text, "是不是") || strstr(text, "要不要") ||
      strchr(text, '?') || strstr(text, "？"))
    {
      return 0;
    }
  {
    static const char *const delete_words[] = {
      "删除", "删掉", "清空", "清除", "移除"
    };
    const char *deletion = NULL;
    const char *note = NULL;
    size_t i;
    for (i = 0; i < sizeof(delete_words) / sizeof(delete_words[0]); i++)
      {
        const char *found = strstr(text, delete_words[i]);
        if (found && (!deletion || found < deletion)) deletion = found;
      }
    for (i = 0; i < sizeof(note_triggers) / sizeof(note_triggers[0]); i++)
      {
        const char *found = strstr(text, note_triggers[i]);
        if (found && (!note || found < note)) note = found;
      }
    if (deletion && (!note || deletion < note))
      {
        intent->type = INTENT_DELETE_UNSUPPORTED;
        return 1;
      }
  }
  cancel_reminder = contains_any(
      text, no_reminder, sizeof(no_reminder) / sizeof(no_reminder[0]));
  cancel_focus = contains_any(text, no_focus,
                              sizeof(no_focus) / sizeof(no_focus[0]));

  has_dt = parse_datetime(text, &intent->year, &intent->month, &intent->day,
                          &intent->hour, &intent->minute, &date_start,
                          &date_end, &time_start, &time_end);
  relative_marker(text, &relative_bytes);
  alarm_requested = contains_alarm_word(text) ||
      ((strstr(text, "每天") || strstr(text, "每日") || strstr(text, "天天")) &&
       first_of(text, "点", "时") && !relative_bytes);
  explicit_note = contains_any(text, note_destinations,
                               sizeof(note_destinations) /
                                 sizeof(note_destinations[0]));
  /* A timed "remember this" is a TODO unless a note destination is explicit.
   * Meeting statements also carry enough information to save immediately;
   * never send them to cloud chat merely to ask for a meeting name. */
  todo_requested = contains_any(text, todo_triggers,
                                sizeof(todo_triggers) / sizeof(todo_triggers[0])) ||
                   contains_any(text, note_triggers,
                                sizeof(note_triggers) / sizeof(note_triggers[0])) ||
                   is_meeting_schedule(text);
  if (!cancel_reminder && alarm_requested)
    {
      relative_marker(text, &relative_bytes);
      if (relative_bytes &&
          parse_duration_seconds(text, &seconds, ASSISTANT_TODO_MAX_SECONDS))
        {
          intent->type = INTENT_ALARM_AFTER;
          intent->seconds = seconds;
          clean_relative_name(text, intent->text, sizeof(intent->text));
          if (strcmp(intent->text, "待办事项") == 0)
            {
              snprintf(intent->text, sizeof(intent->text), "%s", "闹钟");
            }
          return 1;
        }
      alarm_rc = parse_alarm_time(text, intent);
      if (alarm_rc > 0)
        {
          intent->type = INTENT_ALARM;
          return 1;
        }
      if (alarm_rc < 0)
        {
          intent->type = INTENT_TIME_ERROR;
          return 1;
        }
    }

  if (!cancel_reminder && !alarm_requested && !cancel_focus && !explicit_note &&
      !(strstr(text, "专注") || strstr(text, "计时") || strstr(text, "番茄钟")) &&
      todo_requested)
    {
      const char *clock_mark = first_of(text, "点", "时");
      size_t clock_start;
      int clock_hour;
      /* "会议要点" and "小时" are not clock times. */
      if (clock_mark &&
          !parse_number_before(text, (size_t)(clock_mark - text),
                                &clock_start, &clock_hour))
        {
          clock_mark = NULL;
        }
      relative_marker(text, &relative_bytes);
      if (relative_bytes &&
          parse_duration_seconds(text, &seconds, ASSISTANT_TODO_MAX_SECONDS))
        {
          intent->type = INTENT_TODO_AFTER;
          intent->seconds = seconds;
          clean_relative_todo_name(text, intent->text, sizeof(intent->text));
          return 1;
        }

      /* A clock-only request such as "11点提醒我开会" is a timed TODO,
       * not an alarm. Resolve it to the next occurrence of that local time. */
      if (!relative_bytes && has_dt == 0 && clock_mark)
        {
          alarm_rc = parse_alarm_time(text, intent);
          if (alarm_rc > 0)
            {
              intent->type = INTENT_TODO_AT;
              clean_todo_at_name(text, intent->text, sizeof(intent->text));
              return 1;
            }
          if (alarm_rc <= 0)
            {
              intent->type = INTENT_TIME_ERROR;
              return 1;
            }
        }
    }

  if (has_dt < 0 && !cancel_reminder &&
      contains_any(text, reminder_triggers,
                   sizeof(reminder_triggers) / sizeof(reminder_triggers[0])))
    {
      intent->type = INTENT_TIME_ERROR;
      return 1;
    }
  if (has_dt == 1 && !cancel_reminder &&
      contains_any(text, reminder_triggers,
                   sizeof(reminder_triggers) / sizeof(reminder_triggers[0])))
    {
      intent->type = INTENT_COUNTDOWN;
      clean_reminder_name(text, date_start, date_end, time_start, time_end,
                          intent->text, sizeof(intent->text));
      return 1;
    }

  if (strcmp(text, "吃药") == 0 || strcmp(text, "吃药。") == 0 ||
      strstr(text, "打开吃药") || strstr(text, "打开服药"))
    {
      intent->type = INTENT_MED_OPEN;
      return 1;
    }
  if (is_med_take_request(text))
    {
      intent->type = INTENT_MED_TAKE;
      snprintf(intent->text, sizeof(intent->text), "%s", text);
      return 1;
    }
  if (is_mbti_open_request(text))
    {
      intent->type = INTENT_MBTI_OPEN;
      return 1;
    }

  game_page = game_page_from_text(text);
  if (game_page != PAGE_COUNT &&
      (is_game_request(text) || strstr(text, "玩") != NULL))
    {
      intent->type = INTENT_GAME;
      intent->game_page = game_page;
      return 1;
    }
  if (is_game_request(text))
    {
      intent->type = INTENT_GAME_ASK;
      return 1;
    }
  if (is_wake_phrase(text))
    {
      intent->type = INTENT_WAKE;
      return 1;
    }

  if (contains_any(text, mood_record_triggers,
                   sizeof(mood_record_triggers) /
                     sizeof(mood_record_triggers[0])))
    {
      intent->type = INTENT_MOOD_RECORD;
      intent->seconds = contains_any(
          text, mood_state_words,
          sizeof(mood_state_words) / sizeof(mood_state_words[0])) ? 0 : 1;
      snprintf(intent->text, sizeof(intent->text), "%s", text);
      return 1;
    }

  if (contains_any(text, note_triggers,
                   sizeof(note_triggers) / sizeof(note_triggers[0])))
    {
      const char *trigger = NULL;
      size_t trigger_len = 0;
      size_t i;
      for (i = 0; i < sizeof(note_triggers) / sizeof(note_triggers[0]); i++)
        {
          const char *found = strstr(text, note_triggers[i]);
          if (found && (!trigger || found < trigger))
            {
              trigger = found;
              trigger_len = strlen(note_triggers[i]);
            }
        }
      if (trigger)
        {
          const char *content = trigger + trigger_len;
          while (*content == ' ' || *content == '\t' || *content == ':' ||
                 *content == ',')
            {
              content++;
            }
          while (strncmp(content, "，", 3) == 0 ||
                 strncmp(content, "：", 3) == 0 ||
                 strncmp(content, "、", 3) == 0)
            {
              content += 3;
            }
          while (strncmp(content, "我", 3) == 0 ||
                 strncmp(content, "一下", 6) == 0 ||
                 strncmp(content, "记得", 6) == 0)
            {
              content += strncmp(content, "我", 3) == 0 ? 3 : 6;
            }
          if (!*content)
            {
              return 0;
            }
          intent->type = INTENT_NOTE;
          snprintf(intent->text, sizeof(intent->text), "%s", content);
          return 1;
        }
    }

  if (!cancel_focus &&
      (strstr(text, "专注") || strstr(text, "计时") || strstr(text, "番茄钟")) &&
      parse_duration_seconds(text, &seconds, 120 * 60))
    {
      intent->type = INTENT_FOCUS;
      intent->seconds = seconds;
      return 1;
    }

  if (contains_any(text, mood_state_words,
                   sizeof(mood_state_words) / sizeof(mood_state_words[0])) &&
      contains_any(text, mood_self_report,
                   sizeof(mood_self_report) / sizeof(mood_self_report[0])))
    {
      intent->type = INTENT_MOOD_RECORD;
      intent->seconds = 0;
      snprintf(intent->text, sizeof(intent->text), "%s", text);
      return 1;
    }

  if (contains_any(text, mood_words, sizeof(mood_words) / sizeof(mood_words[0])))
    {
      intent->type = INTENT_MOOD;
      snprintf(intent->text, sizeof(intent->text), "%s", text);
      return 1;
    }

  return 0;
}

static dm_ai_log_action_t intent_log_action(const assistant_intent_t *intent)
{
  switch (intent->type)
    {
      case INTENT_FOCUS: return DM_AI_LOG_FOCUS;
      case INTENT_TODO_AFTER:
      case INTENT_TODO_AT: return DM_AI_LOG_TODO;
      case INTENT_ALARM:
      case INTENT_ALARM_AFTER: return DM_AI_LOG_ALARM;
      case INTENT_NOTE: return DM_AI_LOG_NOTE;
      case INTENT_COUNTDOWN: return DM_AI_LOG_COUNTDOWN;
      case INTENT_MED_OPEN: return DM_AI_LOG_MED_OPEN;
      case INTENT_MED_TAKE: return DM_AI_LOG_MED_TAKE;
      case INTENT_MOOD_RECORD: return DM_AI_LOG_MOOD_RECORD;
      case INTENT_MOOD: return DM_AI_LOG_ADVICE;
      case INTENT_GAME_ASK: return DM_AI_LOG_GAME_PICKER;
      case INTENT_GAME:
        return intent->game_page == PAGE_2048 ? DM_AI_LOG_2048 :
               intent->game_page == PAGE_GUESS ? DM_AI_LOG_GUESS : DM_AI_LOG_24;
      case INTENT_GAME_CANCEL: return DM_AI_LOG_GAME_CANCEL;
      case INTENT_MBTI_OPEN: return DM_AI_LOG_MBTI;
      case INTENT_WAKE: return DM_AI_LOG_GREETING;
      case INTENT_CANCEL: return DM_AI_LOG_CANCEL;
      case INTENT_DELETE_UNSUPPORTED: return DM_AI_LOG_DELETE;
      case INTENT_DELETE_DATA:
        return intent->deletion.kind >= 0 && intent->deletion.kind < DM_AI_DATA_COUNT
                 ? (dm_ai_log_action_t)(DM_AI_LOG_DELETE_NOTE + intent->deletion.kind)
                 : DM_AI_LOG_DELETE;
      case INTENT_READ_HEALTH:
        return intent->health_query == DM_AI_HEALTH_WATER ? DM_AI_LOG_READ_WATER :
               intent->health_query == DM_AI_HEALTH_LAST_MOOD ? DM_AI_LOG_READ_LAST_MOOD : DM_AI_LOG_READ_HEALTH;
      case INTENT_TIME_ERROR: return DM_AI_LOG_TIME_ERROR;
      case INTENT_READ_LOCAL:
        return intent->read_kind == DM_AI_READ_NOTES ? DM_AI_LOG_READ_NOTES :
               intent->read_kind == DM_AI_READ_TODOS ? DM_AI_LOG_READ_TODOS :
               intent->read_kind == DM_AI_READ_ALARMS ? DM_AI_LOG_READ_ALARMS : DM_AI_LOG_READ_COUNTDOWNS;
      default: return DM_AI_LOG_ACTION_COUNT;
    }
}

static void record_intent_result(const assistant_intent_t *intent, int rc,
                                  const char *reply)
{
  dm_ai_log_action_t action = intent_log_action(intent);
  dm_ai_log_result_t result = rc == 0 ? DM_AI_LOG_SUCCESS : DM_AI_LOG_FAILED;
  dm_ai_log_reason_t reason = rc == 0 ? DM_AI_LOG_REASON_NONE : DM_AI_LOG_REASON_MODULE;
  dm_ai_permission_t denied = DM_AI_PERMISSION_COUNT;
  int raw_rc = rc == 0 ? 0 : (intent->operation_rc ? intent->operation_rc : rc);
  int seconds = intent->type == INTENT_FOCUS || intent->type == INTENT_TODO_AFTER ||
                intent->type == INTENT_ALARM_AFTER ? intent->seconds : 0;

  if (intent->type == INTENT_MOOD_RECORD && rc == 0 &&
      intent->operation_rc != 0 && intent->seconds)
    action = DM_AI_LOG_MOOD_OPEN;

  if (rc == -2)
    {
      result = DM_AI_LOG_DENIED;
      reason = DM_AI_LOG_REASON_PERMISSION;
      denied = intent->denied_permission;
    }
  else if (action == DM_AI_LOG_CANCEL || action == DM_AI_LOG_GAME_CANCEL)
    { result = DM_AI_LOG_SKIPPED; reason = DM_AI_LOG_REASON_CANCELLED; }
  else if (action == DM_AI_LOG_DELETE)
    { result = DM_AI_LOG_SKIPPED; reason = DM_AI_LOG_REASON_UNSUPPORTED; }
  else if (intent->type == INTENT_DELETE_DATA || intent->type == INTENT_READ_HEALTH)
    {
      if (raw_rc == DM_AI_DATA_SAVE) reason = DM_AI_LOG_REASON_SAVE;
      else if (raw_rc == DM_AI_DATA_CLOCK) reason = DM_AI_LOG_REASON_CLOCK;
      else if (raw_rc == DM_AI_DATA_NOT_FOUND) reason = DM_AI_LOG_REASON_NOT_FOUND;
      else if (raw_rc == DM_AI_DATA_AMBIGUOUS) reason = DM_AI_LOG_REASON_AMBIGUOUS;
      else if (raw_rc == DM_AI_DATA_BUSY) reason = DM_AI_LOG_REASON_BUSY;
      else if (raw_rc == DM_AI_DATA_UNSUPPORTED) reason = DM_AI_LOG_REASON_UNSUPPORTED;
      else if (raw_rc) reason = DM_AI_LOG_REASON_INVALID;
    }
  else if (action == DM_AI_LOG_TIME_ERROR)
    reason = DM_AI_LOG_REASON_INVALID;
  else if (rc != 0)
    {
      if (action == DM_AI_LOG_GAME_PICKER)
        { result = DM_AI_LOG_DENIED; reason = DM_AI_LOG_REASON_NO_GAME; }
      else if (action == DM_AI_LOG_FOCUS) reason = DM_AI_LOG_REASON_BUSY;
      else if (action == DM_AI_LOG_MED_TAKE)
        {
          if (raw_rc == -2 || raw_rc == -3) reason = DM_AI_LOG_REASON_MED_NAME;
          else if (raw_rc == -5) reason = DM_AI_LOG_REASON_SAVE;
          else if (raw_rc == -4)
            reason = reply && strstr(reply, "校准") ? DM_AI_LOG_REASON_CLOCK : DM_AI_LOG_REASON_MED_SLOT;
          else reason = DM_AI_LOG_REASON_INVALID;
        }
      else if (action == DM_AI_LOG_NOTE || action == DM_AI_LOG_TODO ||
               action == DM_AI_LOG_ALARM || action == DM_AI_LOG_COUNTDOWN)
        {
          if (raw_rc == -2) reason = DM_AI_LOG_REASON_FULL;
          else if (raw_rc == -3) reason = DM_AI_LOG_REASON_SAVE;
          else if (raw_rc == -4)
            reason = action == DM_AI_LOG_NOTE ? DM_AI_LOG_REASON_TOO_LONG : DM_AI_LOG_REASON_CLOCK;
          else reason = DM_AI_LOG_REASON_INVALID;
        }
      else if (action == DM_AI_LOG_MOOD_RECORD && raw_rc == -2)
        reason = DM_AI_LOG_REASON_SAVE;
    }
  dm_ai_log_add(action, result, reason, denied, seconds, raw_rc);
}

static dm_ai_permission_t intent_permission(const assistant_intent_t *intent)
{
  switch (intent->type)
    {
      case INTENT_READ_LOCAL: return DM_AI_READ_LOCAL_DATA;
      case INTENT_READ_HEALTH: return DM_AI_READ_HEALTH;
      case INTENT_DELETE_DATA: return dm_ai_data_delete_permission(intent->deletion.kind);
      case INTENT_FOCUS: return DM_AI_FOCUS;
      case INTENT_TODO_AFTER:
      case INTENT_TODO_AT: return DM_AI_TODO;
      case INTENT_ALARM:
      case INTENT_ALARM_AFTER: return DM_AI_ALARM;
      case INTENT_NOTE: return DM_AI_NOTE;
      case INTENT_COUNTDOWN: return DM_AI_COUNTDOWN;
      case INTENT_MED_OPEN:
      case INTENT_MED_TAKE: return DM_AI_MEDICATION;
      case INTENT_MOOD_RECORD: return DM_AI_MOOD_RECORD;
      case INTENT_MBTI_OPEN: return DM_AI_MBTI;
      case INTENT_WAKE:
      case INTENT_GAME_ASK:
      case INTENT_MOOD: return DM_AI_COMPANION;
      case INTENT_GAME:
        if (intent->game_page == PAGE_2048) return DM_AI_GAME_2048;
        if (intent->game_page == PAGE_GUESS) return DM_AI_GAME_GUESS;
        if (intent->game_page == PAGE_24) return DM_AI_GAME_24;
        return DM_AI_PERMISSION_COUNT;
      default: return DM_AI_PERMISSION_COUNT;
    }
}

static int intent_check_permission(assistant_intent_t *intent,
                                    char *reply, size_t cap)
{
  dm_ai_permission_t required;
  dm_ai_permission_t denied;
  if (intent->type == INTENT_CANCEL || intent->type == INTENT_GAME_CANCEL ||
      intent->type == INTENT_TIME_ERROR ||
      intent->type == INTENT_DELETE_UNSUPPORTED) return 1;
  required = intent_permission(intent);
  if (!dm_ai_permission_allowed(DM_AI_LOCAL)) denied = DM_AI_LOCAL;
  else if (required == DM_AI_PERMISSION_COUNT)
    {
      snprintf(reply, cap, "%s", "这个本地操作暂不支持。");
      return 0;
    }
  else if (!dm_ai_permission_allowed(required)) denied = required;
  else if (intent->type == INTENT_DELETE_DATA &&
           !dm_ai_permission_allowed(dm_ai_data_read_permission(intent->deletion.kind)))
    denied = dm_ai_data_read_permission(intent->deletion.kind);
  else if (intent->type != INTENT_MOOD && intent->type != INTENT_READ_LOCAL &&
           intent->type != INTENT_READ_HEALTH &&
           !dm_ai_permission_allowed(DM_AI_OPEN_APPS)) denied = DM_AI_OPEN_APPS;
  else return 1;

  intent->denied_permission = denied;
  pthread_mutex_lock(&s_intent_lock);
  s_waiting_game_choice = 0;
  pthread_mutex_unlock(&s_intent_lock);
  snprintf(reply, cap, "已关闭“%s”权限，请到设定的权限管理中开启。",
           dm_ai_permission_name(denied));
  return 0;
}

static int run_intent(assistant_intent_t *intent, char *reply, size_t cap)
{
  int rc;

  intent->operation_rc = 0;
  intent->denied_permission = DM_AI_PERMISSION_COUNT;
  /* Recheck on LVGL immediately before any device-side action. */
  if (!intent_check_permission(intent, reply, cap)) return -2;
  if (intent->type != INTENT_GAME_ASK && intent->type != INTENT_GAME &&
      intent->type != INTENT_GAME_CANCEL)
    {
      pthread_mutex_lock(&s_intent_lock);
      s_waiting_game_choice = 0;
      pthread_mutex_unlock(&s_intent_lock);
    }

  switch (intent->type)
    {
      case INTENT_READ_HEALTH:
        rc = dm_ai_read_health(intent->health_query, reply, cap);
        intent->operation_rc = rc;
        if (rc == DM_AI_DATA_DENIED)
          {
            intent->denied_permission = dm_ai_permission_allowed(DM_AI_LOCAL)
                                         ? DM_AI_READ_HEALTH : DM_AI_LOCAL;
            snprintf(reply, cap, "%s", "读取健康记录权限已关闭，未返回已有健康数据。");
          }
        else if (rc == DM_AI_DATA_CLOCK)
          snprintf(reply, cap, "%s", "设备时间尚未校准，无法确定今日喝水或最近心情的日期，请先校准时间。");
        else if (rc)
          snprintf(reply, cap, "%s", "本地健康记录读取失败，无法确认具体数值或时间，请在健康页核对。");
        return rc;
      case INTENT_DELETE_DATA:
        rc = dm_ai_delete_data(&intent->deletion);
        intent->operation_rc = rc;
        if (!rc)
          {
            static const char *const replies[DM_AI_DATA_COUNT] = {
              "已删除指定的一条备忘。", "已删除指定的一条待办。",
              "已删除指定的一条闹钟。", "已删除指定的一条倒数日。",
              "已删除指定的一条今日心情记录。", "已删除指定范围的今日喝水记录。",
              "已清除指定药品今天的服药打卡，药品和用药计划已保留。"
            };
            static const dm_page_t pages[DM_AI_DATA_COUNT] = {
              PAGE_NOTE, PAGE_TODO, PAGE_ALARM, PAGE_COUNTDOWN,
              PAGE_HEALTH, PAGE_WATER, PAGE_MED
            };
            dm_show(pages[intent->deletion.kind]);
            snprintf(reply, cap, "%s", replies[intent->deletion.kind]);
          }
        else if (rc == DM_AI_DATA_DENIED)
          {
            intent->denied_permission = !dm_ai_permission_allowed(DM_AI_LOCAL) ? DM_AI_LOCAL :
              !dm_ai_permission_allowed(dm_ai_data_read_permission(intent->deletion.kind))
                ? dm_ai_data_read_permission(intent->deletion.kind) :
              !dm_ai_permission_allowed(dm_ai_data_delete_permission(intent->deletion.kind))
                ? dm_ai_data_delete_permission(intent->deletion.kind) : DM_AI_OPEN_APPS;
            snprintf(reply, cap, "已关闭“%s”权限，未查找或删除记录。", dm_ai_permission_name(intent->denied_permission));
          }
        else if (rc == DM_AI_DATA_SAVE)
          snprintf(reply, cap, "%s", "删除未保存，原记录已恢复，请稍后重试。");
        else if (rc == DM_AI_DATA_NOT_FOUND)
          snprintf(reply, cap, "%s", "没有找到指定的可删除记录，未修改其他数据。请核对完整名称或当前列表编号。");
        else if (rc == DM_AI_DATA_AMBIGUOUS)
          snprintf(reply, cap, "%s", "存在同名记录，不能确定删除哪一条。请按当前列表编号指定。");
        else if (rc == DM_AI_DATA_BUSY)
          snprintf(reply, cap, "%s", "该条提醒正在显示，请先结束提醒，再请求删除。");
        else if (rc == DM_AI_DATA_CLOCK)
          snprintf(reply, cap, "%s", "设备时间未校准，不能确定今日健康记录，未执行删除。");
        else
          snprintf(reply, cap, "%s", "删除范围或定位条件不支持，未修改任何记录。");
        return rc;
      case INTENT_READ_LOCAL:
        rc = dm_ai_read_local(intent->read_kind, reply, cap);
        intent->operation_rc = rc;
        if (rc == -2)
          {
            intent->denied_permission = dm_ai_permission_allowed(DM_AI_LOCAL)
                                         ? DM_AI_READ_LOCAL_DATA : DM_AI_LOCAL;
            snprintf(reply, cap, "%s", "本地生活数据读取权限已关闭，未返回已有内容。");
            return -2;
          }
        if (rc != 0) snprintf(reply, cap, "%s", "本地记录读取失败，请稍后重试。");
        return rc;
      case INTENT_DELETE_UNSUPPORTED:
        snprintf(reply, cap, "%s", "请明确指定一条记录，例如“删除第2条备忘”“删除最近一条心情记录”。不支持模糊目标或批量删除；喝水可明确删除今天的杯数或今日累计。");
        return 0;

      case INTENT_CANCEL:
        snprintf(reply, cap, "%s", "本次没有新建、修改或删除记录。若要删除已有记录，请明确指定类别和编号。");
        return 0;

      case INTENT_MED_OPEN:
        dm_show(PAGE_MED);
        snprintf(reply, cap, "%s", "已打开吃药页面，请核对药品和服药记录。");
        return 0;

      case INTENT_FOCUS:
        rc = dm_focus_start_seconds(intent->seconds);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            int minutes = intent->seconds / 60;
            int seconds = intent->seconds % 60;
            if (seconds == 0)
              {
                snprintf(reply, cap, "好，%d分钟专注已开始。我陪你。",
                         minutes);
              }
            else if (minutes == 0)
              {
                snprintf(reply, cap, "好，%d秒专注已开始。我陪你。", seconds);
              }
            else
              {
                snprintf(reply, cap,
                         "好，%d分钟%02d秒专注已开始。我陪你。",
                         minutes, seconds);
              }
            return 0;
          }
        snprintf(reply, cap, "已有专注计时正在进行，我没有重置它。先结束当前专注再试。");
        return -1;

      case INTENT_TODO_AFTER:
        rc = dm_todo_add_voice(intent->text, intent->seconds);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            int hours = intent->seconds / 3600;
            int minutes = (intent->seconds % 3600) / 60;
            int seconds = intent->seconds % 60;
            dm_show(PAGE_TODO);
            if (hours > 0)
              {
                snprintf(reply, cap, "记好了：%s，%d小时%d分钟后提醒你，到时会语音播报。",
                         intent->text, hours, minutes);
              }
            else if (minutes > 0 && seconds > 0)
              {
                snprintf(reply, cap, "记好了：%s，%d分钟%02d秒后语音提醒你。",
                         intent->text, minutes, seconds);
              }
            else if (minutes > 0)
              {
                snprintf(reply, cap, "已添加待办：%s，%d分钟后提醒你。",
                         intent->text, minutes);
              }
            else
              {
                snprintf(reply, cap, "已添加待办：%s，%d秒后提醒你。",
                         intent->text, seconds);
              }
            return 0;
          }
        if (rc == -2)
          {
            snprintf(reply, cap, "待办列表已满，请先删除一项再添加。");
          }
        else if (rc == -4)
          {
            snprintf(reply, cap, "设备时间还没校准，暂时不能设定待办提醒。");
          }
        else
          {
            snprintf(reply, cap, "待办没有保存成功，请换一种说法再试。");
          }
        return -1;

      case INTENT_TODO_AT:
        rc = dm_todo_add_at_voice(intent->text, intent->year, intent->month,
                                  intent->day, intent->hour, intent->minute);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            dm_show(PAGE_TODO);
            snprintf(reply, cap, "已添加待办：%s，%d月%d日%02d:%02d提醒你。",
                     intent->text, intent->month, intent->day, intent->hour,
                     intent->minute);
            return 0;
          }
        if (rc == -2)
          {
            snprintf(reply, cap, "待办列表已满，请先删除一项再添加。");
          }
        else if (rc == -4)
          {
            snprintf(reply, cap, "设备时间还没校准，暂时不能设定待办提醒。");
          }
        else
          {
            snprintf(reply, cap, "这个时间无效或已经过去，请说一个未来的时间。");
          }
        return -1;

      case INTENT_ALARM:
        rc = dm_alarm_add_voice(intent->text, intent->year, intent->month,
                                intent->day, intent->hour, intent->minute,
                                intent->repeat_daily);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            dm_show(PAGE_ALARM);
            snprintf(reply, cap, "闹钟设好了：%s，%d月%d日%02d:%02d%s。",
                     intent->text, intent->month, intent->day, intent->hour,
                     intent->minute,
                     intent->repeat_daily ? "，每天重复" : "，响一次");
            return 0;
          }
        if (rc == -2)
          {
            snprintf(reply, cap, "闹钟数量已满，请先删除一个再添加。");
          }
        else if (rc == -4)
          {
            snprintf(reply, cap, "设备时间还没校准，暂时不能设闹钟。");
          }
        else
          {
            snprintf(reply, cap, "闹钟时间无效或已过，请换个时间再试。");
          }
        return -1;

      case INTENT_ALARM_AFTER:
        rc = dm_alarm_add_after_voice(intent->text, intent->seconds);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            dm_show(PAGE_ALARM);
            snprintf(reply, cap, "闹钟已设好：%s，%d分钟%d秒后响铃。",
                     intent->text, intent->seconds / 60, intent->seconds % 60);
            return 0;
          }
        if (rc == -2)
          {
            snprintf(reply, cap, "闹钟数量已满，请先删除一个再添加。");
          }
        else if (rc == -4)
          {
            snprintf(reply, cap, "设备时间还没校准，暂时不能设闹钟。");
          }
        else
          {
            snprintf(reply, cap, "闹钟没有保存成功，请再试一次。");
          }
        return -1;

      case INTENT_WAKE:
        dm_show(PAGE_CHAT);
        snprintf(reply, cap, "%s", "你好呀，有什么事可以帮到你？");
        return 0;

      case INTENT_MBTI_OPEN:
        dm_show(PAGE_MBTI);
        snprintf(reply, cap, "%s", "已打开 MBTI 测试，可以开始答题了。");
        return 0;

      case INTENT_MED_TAKE:
        rc = dm_med_voice_take(intent->text, reply, (int)cap);
        intent->operation_rc = rc;
        if (rc == 0) dm_show(PAGE_MED);
        return rc == 0 ? 0 : -1;

      case INTENT_GAME_ASK:
        if (!dm_ai_permission_allowed(DM_AI_GAME_2048) &&
            !dm_ai_permission_allowed(DM_AI_GAME_GUESS) &&
            !dm_ai_permission_allowed(DM_AI_GAME_24))
          {
            pthread_mutex_lock(&s_intent_lock);
            s_waiting_game_choice = 0;
            pthread_mutex_unlock(&s_intent_lock);
            snprintf(reply, cap, "%s", "小游戏权限均已关闭，请在权限管理中开启后再试。");
            return -1;
          }
        pthread_mutex_lock(&s_intent_lock);
        s_waiting_game_choice = 1;
        pthread_mutex_unlock(&s_intent_lock);
        dm_show(PAGE_CHAT);
        snprintf(reply, cap,
                 "可以选择已授权的小游戏：%s%s%s。",
                 dm_ai_permission_allowed(DM_AI_GAME_2048) ? "2048 " : "",
                 dm_ai_permission_allowed(DM_AI_GAME_GUESS) ? "猜数字 " : "",
                 dm_ai_permission_allowed(DM_AI_GAME_24) ? "24点" : "");
        return 0;

      case INTENT_GAME:
        pthread_mutex_lock(&s_intent_lock);
        s_waiting_game_choice = 0;
        pthread_mutex_unlock(&s_intent_lock);
        dm_show(intent->game_page);
        if (intent->game_page == PAGE_2048)
          {
            snprintf(reply, cap, "%s", "好，我们来玩2048。游戏已经打开。");
          }
        else if (intent->game_page == PAGE_GUESS)
          {
            snprintf(reply, cap, "%s", "好，我们来玩猜数字。游戏已经打开。");
          }
        else
          {
            snprintf(reply, cap, "%s", "好，我们来玩24点。游戏已经打开。");
          }
        return 0;

      case INTENT_GAME_CANCEL:
        pthread_mutex_lock(&s_intent_lock);
        s_waiting_game_choice = 0;
        pthread_mutex_unlock(&s_intent_lock);
        if (dm_ai_permission_allowed(DM_AI_OPEN_APPS)) dm_show(PAGE_CHAT);
        snprintf(reply, cap, "%s", "好，那我们先不玩。想玩的时候再告诉我。");
        return 0;

      case INTENT_COUNTDOWN:
        rc = dm_countdown_add_voice(intent->text, intent->year, intent->month,
                                    intent->day, intent->hour,
                                    intent->minute);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            snprintf(reply, cap, "记好了：%s，%d月%d日%02d:%02d提醒你。",
                     intent->text, intent->month, intent->day, intent->hour,
                     intent->minute);
            return 0;
          }
        if (rc == -2)
          {
            snprintf(reply, cap, "倒数日列表已满，暂时不能添加。请先删除一项。");
          }
        else if (rc == -4)
          {
            snprintf(reply, cap, "设备日期时间还没校准，无法设置准确提醒。");
          }
        else
          {
            snprintf(reply, cap, "提醒没有保存成功，请检查日期后再试。");
          }
        return -1;

      case INTENT_NOTE:
        rc = dm_note_add_voice(intent->text);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            snprintf(reply, cap, "%s", "记下了，已经保存到备忘。");
            return 0;
          }
        if (rc == -4)
          {
            snprintf(reply, cap, "%s", "备忘内容超过31字节，请缩短后再说，尚未保存。");
          }
        else if (rc == -2)
          {
            snprintf(reply, cap, "备忘已满，暂时不能添加。请先删除一条。");
          }
        else
          {
            snprintf(reply, cap, "这条备忘没有保存成功，请再说短一点。");
          }
        return -1;

      case INTENT_MOOD_RECORD:
        rc = dm_health_record_mood_voice(intent->text);
        intent->operation_rc = rc;
        if (rc == 0)
          {
            dm_show(PAGE_HEALTH);
            if (intent->seconds == 0)
              {
                char advice[160];
                dm_health_voice_advice(intent->text, advice, sizeof(advice));
                snprintf(reply, cap, "记好了，心情已保存。%s", advice);
              }
            else
              {
                snprintf(reply, cap, "%s", "记好了，心情已保存到健康记录。");
              }
            return 0;
          }
        if (rc == -2)
          {
            if (intent->seconds == 0)
              {
                char advice[160];
                dm_health_voice_advice(intent->text, advice, sizeof(advice));
                snprintf(reply, cap, "心情暂未保存。%s", advice);
              }
            else
              {
                snprintf(reply, cap, "%s", "心情记录没有保存成功，请稍后重试。");
              }
            return -1;
          }
        if (intent->seconds)
          {
            dm_show(PAGE_MOOD_LOG);
            snprintf(reply, cap,
                     "%s", "我打开了心情记录页，请选择当前心情后点保存。");
            return 0;
          }
        dm_health_voice_advice(intent->text, reply, (int)cap);
        return 0;

      case INTENT_MOOD:
        dm_health_voice_advice(intent->text, reply, (int)cap);
        return 0;

      case INTENT_TIME_ERROR:
        snprintf(reply, cap, "设备时间未校准，或提醒时间无效；请检查日期时间后再试。");
        return -1;

      default:
        return -1;
    }
}

int dm_assistant_intent_try(const char *text, char *reply, int reply_cap)
{
  static const char *const cancel_game_words[] = {
    "算了", "不玩了", "取消", "先不了", "不用了"
  };
  assistant_intent_t parsed;
  dm_page_t game_page;
  int parsed_ok;
  int i;

  if (!text || !text[0] || !reply || reply_cap < 8)
    {
      return 0;
    }
  parsed_ok = parse_intent(text, &parsed);

  pthread_mutex_lock(&s_intent_lock);
  if (!parsed_ok && s_waiting_game_choice)
    {
      game_page = game_page_from_text(text);
      if (game_page != PAGE_COUNT)
        {
          memset(&parsed, 0, sizeof(parsed));
          parsed.type = INTENT_GAME;
          parsed.game_page = game_page;
          parsed_ok = 1;
        }
      else if (contains_any(text, cancel_game_words,
                            sizeof(cancel_game_words) /
                              sizeof(cancel_game_words[0])))
        {
          memset(&parsed, 0, sizeof(parsed));
          parsed.type = INTENT_GAME_CANCEL;
          parsed_ok = 1;
        }
    }
  if (!parsed_ok)
    {
      pthread_mutex_unlock(&s_intent_lock);
      return 0;
    }
  if (s_intent_active)
    {
      snprintf(reply, (size_t)reply_cap, "%s", "上一项操作仍在处理，请稍后重试。");
      pthread_mutex_unlock(&s_intent_lock);
      dm_ai_log_add(intent_log_action(&parsed), DM_AI_LOG_SKIPPED,
                     DM_AI_LOG_REASON_BUSY, DM_AI_PERMISSION_COUNT, 0, -1);
      return 1;
    }
  s_intent = parsed;
  s_intent_active = 1;
  s_intent_executing = 0;
  s_intent_done = 0;
  s_intent_rc = -1;
  s_intent_reply[0] = '\0';
  pthread_mutex_unlock(&s_intent_lock);

  for (i = 0; ; i++)
    {
      int result;
      usleep(INTENT_WAIT_US);
      pthread_mutex_lock(&s_intent_lock);
      if (s_intent_done)
        {
          result = 1;
          if (s_intent.type == INTENT_WAKE) result = 2;
          else if (s_intent.type == INTENT_READ_LOCAL) result = 4;
          else if (s_intent.type == INTENT_READ_HEALTH) result = 5;
          else if (s_intent.type == INTENT_FOCUS ||
                   (s_intent.type == INTENT_DELETE_DATA && s_intent.deletion.kind >= DM_AI_DATA_MOOD) ||
                   s_intent.type == INTENT_MED_TAKE ||
                   s_intent.type == INTENT_MED_OPEN ||
                   s_intent.type == INTENT_MOOD_RECORD ||
                   s_intent.type == INTENT_MOOD) result = 3;
          if (result == 4 && !dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA))
            snprintf(reply, (size_t)reply_cap, "%s", "本地生活数据读取权限已关闭，内容不会返回。");
          else if (result == 5 && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
            snprintf(reply, (size_t)reply_cap, "%s", "读取健康记录权限已关闭，未返回已有健康数据。");
          else if (result == 3 && s_intent_rc != -2 &&
              !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
            {
              snprintf(reply, (size_t)reply_cap, "%s",
                       s_intent_rc == 0
                         ? "本地操作已完成，健康记录详情请在设备查看。"
                         : "操作未完成，请在设备核对药名、时段或设备状态。");
            }
          else snprintf(reply, (size_t)reply_cap, "%s", s_intent_reply);
          s_intent_active = 0;
          s_intent_done = 0;
          pthread_mutex_unlock(&s_intent_lock);
          return result;
        }
      if (i >= INTENT_WAIT_STEPS && !s_intent_executing)
        {
          s_intent_active = 0;
          snprintf(reply, (size_t)reply_cap, "%s", "设备忙，本次操作未执行，请稍后重试。");
          pthread_mutex_unlock(&s_intent_lock);
          dm_ai_log_add(intent_log_action(&parsed), DM_AI_LOG_SKIPPED,
                         DM_AI_LOG_REASON_BUSY, DM_AI_PERMISSION_COUNT, 0, -1);
          return 1;
        }
      pthread_mutex_unlock(&s_intent_lock);
      /* Once execution starts, its result must be collected before reuse. */
      if (i >= INTENT_WAIT_STEPS) i = INTENT_WAIT_STEPS;
    }
}

void dm_assistant_intent_tick(void)
{
  assistant_intent_t intent;
  char reply[INTENT_REPLY_MAX];
  int rc;

  pthread_mutex_lock(&s_intent_lock);
  if (!s_intent_active || s_intent_executing || s_intent_done)
    {
      pthread_mutex_unlock(&s_intent_lock);
      return;
    }
  intent = s_intent;
  s_intent_executing = 1;
  pthread_mutex_unlock(&s_intent_lock);

  reply[0] = '\0';
  rc = run_intent(&intent, reply, sizeof(reply));
  record_intent_result(&intent, rc, reply);

  pthread_mutex_lock(&s_intent_lock);
  s_intent_rc = rc;
  snprintf(s_intent_reply, sizeof(s_intent_reply), "%s", reply);
  s_intent_done = 1;
  s_intent_executing = 0;
  pthread_mutex_unlock(&s_intent_lock);
}

#endif /* CONFIG_DESKMATE_APP */
