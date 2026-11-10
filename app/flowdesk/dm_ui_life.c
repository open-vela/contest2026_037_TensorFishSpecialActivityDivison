/****************************************************************************
 * dm_ui_life.c — Water reminder + Countdown days (file persistence)
 ****************************************************************************/

#include "deskmate.h"
#include "dm_voice.h"
#include "dm_ai_local_read.h"
#include "dm_ai_permissions.h"
#include "dm_ai_data.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

#ifdef CONFIG_DESKMATE_APP

#define WATER_PATH "/data/deskmate_water.txt"
#define COUNTDOWN_PATH "/data/deskmate_countdown.txt"
#define CD_MAX 8
#define CD_NAME_MAX 48

static void life_back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_FEATURES);
}

static lv_obj_t *mk_life_page(dm_page_t id, const char *zh, const char *en)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_t *title;
  lv_obj_t *back;

  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  back = dm_btn(page, "←", "<", 36, 24, C_BTN, C_MUTED, life_back, NULL);
  lv_obj_set_pos(back, 8, 6);

  title = dm_lbl(page, zh, en, g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 50, 8);

  g_dm_pages[id] = page;
  return page;
}

/* ---------- water ---------- */

#define WATER_GOAL 8

static int s_w_cups;
static int s_w_interval = 45;
static int s_w_left;
static int s_w_day;
static time_t s_w_due;
static bool s_w_alert_on;
static lv_obj_t *s_w_big;
static lv_obj_t *s_w_next;
static lv_obj_t *s_w_tip;
static lv_obj_t *s_w_iv_lab;
static lv_obj_t *s_w_alert;

#define WATER_IV_MIN 5
#define WATER_IV_MAX 60
#define WATER_IV_STEP 5

static int water_iv_clamp(int iv)
{
  if (iv < WATER_IV_MIN)
    {
      iv = WATER_IV_MIN;
    }
  if (iv > WATER_IV_MAX)
    {
      iv = WATER_IV_MAX;
    }
  iv = (iv / WATER_IV_STEP) * WATER_IV_STEP;
  if (iv < WATER_IV_MIN)
    {
      iv = WATER_IV_MIN;
    }
  return iv;
}

static int today_yday(void)
{
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  return tmv.tm_yday + tmv.tm_year * 1000;
}

static int water_save(void)
{
  FILE *f;
  time_t due = s_w_due;
  int left = s_w_left;
  int ok, closed;
  if (!dm_wifi_clock_valid()) return -1;

  if (s_w_left > 0)
    {
      due = time(NULL) + s_w_left;
    }
  else if (!s_w_alert_on)
    {
      due = time(NULL) + s_w_interval * 60;
      left = s_w_interval * 60;
    }
  /* when alert is on, keep due in the past until user taps 好滴 */

  f = fopen(WATER_PATH ".tmp", "w");
  if (!f)
    {
      return -1;
    }
  ok = fprintf(f, "%d %d %ld %d\n", s_w_cups, s_w_interval, (long)due,
               s_w_day) > 0;
  closed = fclose(f) == 0;
  if (!ok || !closed || rename(WATER_PATH ".tmp", WATER_PATH) != 0)
    {
      unlink(WATER_PATH ".tmp");
      return -1;
    }
  s_w_due = due;
  s_w_left = left;
  return 0;
}

static void water_load(void)
{
  FILE *f = fopen(WATER_PATH, "r");
  int c = 0;
  int iv = 45;
  long due = 0;
  int day = today_yday();
  time_t now = time(NULL);

  if (f)
    {
      if (fscanf(f, "%d %d %ld %d", &c, &iv, &due, &day) != 4)
        {
          day = today_yday();
          due = 0;
        }
      fclose(f);
    }
  iv = water_iv_clamp(iv);
  if (iv < WATER_IV_MIN || iv > WATER_IV_MAX)
    {
      iv = 45;
    }
  s_w_cups = c < 0 ? 0 : c;
  s_w_interval = iv;
  s_w_day = dm_wifi_clock_valid() ? today_yday() : day;

  if (dm_wifi_clock_valid() && day != today_yday())
    {
      s_w_cups = 0;
      s_w_due = now + iv * 60;
    }
  else if (due <= 0)
    {
      s_w_due = now + iv * 60;
    }
  else
    {
      s_w_due = (time_t)due;
    }

  s_w_left = (int)(s_w_due - now);
  if (s_w_left < 0)
    {
      s_w_left = 0; /* overdue → tick will raise full-screen alert */
    }
}

static void water_paint(void)
{
  char buf[32];
  int m;
  int s;

  if (s_w_big)
    {
      snprintf(buf, sizeof(buf), "%d / %d", s_w_cups, WATER_GOAL);
      lv_label_set_text(s_w_big, buf);
    }
  m = s_w_left / 60;
  s = s_w_left % 60;
  if (s_w_next)
    {
      if (s_w_alert_on || s_w_left <= 0)
        {
          snprintf(buf, sizeof(buf), "下次提醒 --:--");
        }
      else
        {
          snprintf(buf, sizeof(buf), "下次提醒 %02d:%02d", m, s);
        }
      lv_label_set_text(s_w_next, buf);
    }
  if (s_w_iv_lab)
    {
      snprintf(buf, sizeof(buf), "间隔 %d 分", s_w_interval);
      lv_label_set_text(s_w_iv_lab, buf);
    }
}

static void water_alert_hide_restart(void)
{
  if (s_w_alert)
    {
      lv_obj_add_flag(s_w_alert, LV_OBJ_FLAG_HIDDEN);
    }
  s_w_alert_on = false;
  s_w_left = s_w_interval * 60;
  s_w_due = time(NULL) + s_w_left;
  water_save();
  water_paint();
}

static void water_ok_cb(lv_event_t *e)
{
  (void)e;
  /* 好滴 = 已喝水，计 1 杯并重新倒计时 */
  s_w_cups++;
  water_alert_hide_restart();
  if (s_w_tip)
    {
      if (s_w_cups >= WATER_GOAL)
        {
          lv_label_set_text(s_w_tip,
                            dm_t("好滴，今日喝水达标！", "OK, daily goal done!"));
        }
      else
        {
          char buf[32];
          snprintf(buf, sizeof(buf), "好滴，已记 %d/%d 杯",
                   s_w_cups, WATER_GOAL);
          lv_label_set_text(s_w_tip, buf);
        }
    }
  water_save();
  water_paint();
  dm_health_refresh();
  dm_show(g_dm.page);
}

static void water_alert_show(void)
{
  if (!s_w_alert || s_w_alert_on)
    {
      return;
    }
  s_w_alert_on = true;
  s_w_left = 0;
  lv_obj_clear_flag(s_w_alert, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_w_alert);
  if (g_dm_dock)
    {
      lv_obj_add_flag(g_dm_dock, LV_OBJ_FLAG_HIDDEN);
    }
  if (s_w_tip)
    {
      lv_label_set_text(s_w_tip, dm_t("该喝水啦！", "Time to drink!"));
    }
  if (g_dm.page == PAGE_WATER)
    {
      water_paint();
    }
}

static void water_iv_cb(lv_event_t *e)
{
  int delta = (int)(intptr_t)lv_event_get_user_data(e);
  int iv = water_iv_clamp(s_w_interval + delta);
  s_w_interval = iv;
  water_alert_hide_restart();
  if (s_w_tip)
    {
      char buf[24];
      snprintf(buf, sizeof(buf), "间隔 %d 分", iv);
      lv_label_set_text(s_w_tip, buf);
    }
  water_save();
  water_paint();
}

static void water_drink_cb(lv_event_t *e)
{
  (void)e;
  s_w_cups++;
  s_w_left = s_w_interval * 60;
  s_w_due = time(NULL) + s_w_left;
  if (s_w_alert)
    {
      lv_obj_add_flag(s_w_alert, LV_OBJ_FLAG_HIDDEN);
    }
  s_w_alert_on = false;
  if (s_w_tip)
    {
      lv_label_set_text(s_w_tip,
                        s_w_cups >= WATER_GOAL ? dm_t("今日目标达成！",
                                                      "Goal done!")
                                               : dm_t("记下了，记得休息眼睛",
                                                      "Logged, rest your eyes"));
    }
  water_save();
  water_paint();
}

static void water_reset_cb(lv_event_t *e)
{
  (void)e;
  s_w_cups = 0;
  s_w_left = s_w_interval * 60;
  s_w_due = time(NULL) + s_w_left;
  if (s_w_alert)
    {
      lv_obj_add_flag(s_w_alert, LV_OBJ_FLAG_HIDDEN);
    }
  s_w_alert_on = false;
  if (s_w_tip)
    {
      lv_label_set_text(s_w_tip, " ");
    }
  water_save();
  water_paint();
}

int dm_water_today_cups(void)
{
  return dm_wifi_clock_valid() && s_w_day == today_yday() ? s_w_cups : 0;
}

int dm_water_ai_cups(int *cups)
{
  if (!dm_ai_permission_allowed(DM_AI_READ_HEALTH)) return DM_AI_DATA_DENIED;
  if (!cups) return DM_AI_DATA_INVALID;
  if (!dm_wifi_clock_valid()) return DM_AI_DATA_CLOCK;
  *cups = s_w_day == today_yday() ? s_w_cups : 0;
  return 0;
}

int dm_water_ai_delete(const dm_ai_delete_request_t *request)
{
  int old_cups;
  if (!request || request->kind != DM_AI_DATA_WATER) return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_WATER)) return DM_AI_DATA_DENIED;
  if (!dm_wifi_clock_valid()) return DM_AI_DATA_CLOCK;
  if (request->selector != DM_AI_SELECT_TODAY &&
      request->selector != DM_AI_SELECT_CUPS) return DM_AI_DATA_UNSUPPORTED;
  if (s_w_day != today_yday() || s_w_cups <= 0) return DM_AI_DATA_NOT_FOUND;
  if (request->selector == DM_AI_SELECT_CUPS &&
      (request->number < 1 || request->number > s_w_cups)) return DM_AI_DATA_NOT_FOUND;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_WATER)) return DM_AI_DATA_DENIED;
  old_cups = s_w_cups;
  s_w_cups = request->selector == DM_AI_SELECT_TODAY ? 0 : s_w_cups - request->number;
  if (water_save() != 0) { s_w_cups = old_cups; return DM_AI_DATA_SAVE; }
  water_paint();
  dm_health_refresh();
  return 0;
}

void dm_create_water(void)
{
  lv_obj_t *page = mk_life_page(PAGE_WATER, "喝水提醒", "Water");
  lv_obj_t *b;
  lv_obj_t *al;
  lv_obj_t *t;

  s_w_big = dm_lbl(page, "0 / 8", "0 / 8", g_dm_font_xl, C_ACCENT);
  lv_obj_align(s_w_big, LV_ALIGN_TOP_MID, 0, 36);

  s_w_next = dm_lbl(page, "下次提醒 45:00", "Next 45:00", g_dm_font_s,
                    C_MUTED);
  lv_obj_align(s_w_next, LV_ALIGN_TOP_MID, 0, 84);

  /* custom interval: 5~60 min, step 5 */
  b = dm_btn(page, "−5", "-5", 64, 28, C_BTN, C_INK, water_iv_cb,
             (void *)(intptr_t)-5);
  lv_obj_set_pos(b, 40, 108);
  s_w_iv_lab = dm_lbl(page, "间隔 45 分", "Every 45 min", g_dm_font_m,
                      C_STAR);
  lv_obj_set_style_text_align(s_w_iv_lab, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(s_w_iv_lab, LV_ALIGN_TOP_MID, 0, 114);
  b = dm_btn(page, "+5", "+5", 64, 28, C_BTN, C_INK, water_iv_cb,
             (void *)(intptr_t)5);
  lv_obj_set_pos(b, 216, 108);

  b = dm_btn(page, "喝了一杯", "Drink", 148, 36, 0x0d3a4a, C_ACCENT,
             water_drink_cb, NULL);
  lv_obj_set_pos(b, 8, 150);
  b = dm_btn(page, "今日清零", "Reset", 148, 36, C_BTN, C_INK,
             water_reset_cb, NULL);
  lv_obj_set_pos(b, 164, 150);

  s_w_tip = dm_lbl(page, " ", " ", g_dm_font_s, C_STAR);
  lv_obj_align(s_w_tip, LV_ALIGN_TOP_MID, 0, 198);

  /* full-screen reminder overlay (any page) */
  al = lv_obj_create(g_dm_root);
  lv_obj_set_size(al, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(al, 0, 0);
  lv_obj_set_style_bg_color(al, lv_color_hex(0x062a3d), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(al, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(al, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(al, 0, LV_PART_MAIN);
  lv_obj_clear_flag(al, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(al, LV_OBJ_FLAG_HIDDEN);
  s_w_alert = al;

  t = dm_lbl(al, "喝水啦！", "Drink water!", g_dm_font_xl, C_ACCENT);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 78);

  t = dm_lbl(al, "起来接杯水，活动一下", "Take a sip and stretch",
             g_dm_font_s, C_STAR);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 130);

  b = dm_btn(al, "好滴", "OK", 160, 40, C_STAR, C_EYE, water_ok_cb, NULL);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 168);

  water_load();
  s_w_alert_on = false;
  water_paint();
}

void dm_water_tick(void)
{
  time_t now = time(NULL);
  int left;
  if (!dm_wifi_clock_valid()) return;

  if (s_w_day != today_yday())
    {
      s_w_cups = 0;
      s_w_day = today_yday();
      s_w_due = now + s_w_interval * 60;
      water_save();
    }

  left = (int)(s_w_due - now);
  if (left < 0)
    {
      left = 0;
    }

  if (left <= 0 && !s_w_alert_on)
    {
      water_alert_show();
    }

  if (left != s_w_left)
    {
      s_w_left = left;
      if (g_dm.page == PAGE_WATER)
        {
          water_paint();
        }
    }
}

/* ---------- countdown ---------- */

typedef struct
{
  int y;
  int m;
  int d;
  int hour;
  int minute;
  int has_alarm;
  int alarm_fired;
  char name[CD_NAME_MAX];
} cd_item_t;

static cd_item_t s_cd[CD_MAX];
static int s_cd_n;
static int s_cd_sel;
static bool s_cd_form;
static int s_cd_fy;
static int s_cd_fm;
static int s_cd_fd;
static int s_cd_fname; /* 0..4 preset index */
static lv_obj_t *s_cd_rows[CD_MAX];
static lv_obj_t *s_cd_days[CD_MAX];
static lv_obj_t *s_cd_names[CD_MAX];
static lv_obj_t *s_cd_form_box;
static lv_obj_t *s_cd_date_l;
static lv_obj_t *s_cd_name_btns[5];
static lv_obj_t *s_cd_alert;
static lv_obj_t *s_cd_alert_name;
static int s_cd_alert_idx = -1;

static const char *const s_cd_presets[5] = {
  "生日", "考试", "旅行", "纪念日", "项目"
};

static int cd_save(void)
{
  FILE *f = fopen(COUNTDOWN_PATH, "w");
  int i;
  int ok = 1;
  if (!f)
    {
      return -1;
    }
  for (i = 0; i < s_cd_n; i++)
    {
      int rc;
      if (s_cd[i].has_alarm)
        {
          rc = fprintf(f, "%04d-%02d-%02d %02d:%02d %d %s\n",
                       s_cd[i].y, s_cd[i].m, s_cd[i].d, s_cd[i].hour,
                       s_cd[i].minute, s_cd[i].alarm_fired, s_cd[i].name);
        }
      else
        {
          rc = fprintf(f, "%04d-%02d-%02d %s\n", s_cd[i].y, s_cd[i].m,
                       s_cd[i].d, s_cd[i].name);
        }
      if (rc < 0)
        {
          ok = 0;
          break;
        }
    }
  if (fclose(f) != 0)
    {
      ok = 0;
    }
  return ok ? 0 : -1;
}

static void cd_load(void)
{
  FILE *f = fopen(COUNTDOWN_PATH, "r");
  char line[128];
  s_cd_n = 0;
  s_cd_sel = 0;
  if (!f)
    {
      /* seed demo */
      time_t t = time(NULL);
      struct tm tmv;
      localtime_r(&t, &tmv);
      memset(s_cd, 0, sizeof(s_cd));
      s_cd[0].y = tmv.tm_year + 1900;
      s_cd[0].m = 10;
      s_cd[0].d = 1;
      snprintf(s_cd[0].name, CD_NAME_MAX, "生日");
      s_cd[1].y = tmv.tm_year + 1900;
      s_cd[1].m = 12;
      s_cd[1].d = 20;
      snprintf(s_cd[1].name, CD_NAME_MAX, "考试");
      s_cd_n = 2;
      return;
    }
  memset(s_cd, 0, sizeof(s_cd));
  while (s_cd_n < CD_MAX && fgets(line, sizeof(line), f))
    {
      int y;
      int m;
      int d;
      int hour;
      int minute;
      int fired;
      char name[CD_NAME_MAX];
      int fields;

      fields = sscanf(line, "%d-%d-%d %d:%d %d %47[^\r\n]",
                      &y, &m, &d, &hour, &minute, &fired, name);
      if (fields == 7 && m >= 1 && m <= 12 && d >= 1 && d <= 31 &&
          hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59)
        {
          s_cd[s_cd_n].y = y;
          s_cd[s_cd_n].m = m;
          s_cd[s_cd_n].d = d;
          s_cd[s_cd_n].hour = hour;
          s_cd[s_cd_n].minute = minute;
          s_cd[s_cd_n].has_alarm = 1;
          s_cd[s_cd_n].alarm_fired = fired ? 1 : 0;
          snprintf(s_cd[s_cd_n].name, CD_NAME_MAX, "%s", name);
          s_cd_n++;
        }
      else
        {
          fields = sscanf(line, "%d-%d-%d %47[^\r\n]", &y, &m, &d, name);
          if (fields == 4 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
            {
              s_cd[s_cd_n].y = y;
              s_cd[s_cd_n].m = m;
              s_cd[s_cd_n].d = d;
              snprintf(s_cd[s_cd_n].name, CD_NAME_MAX, "%s", name);
              s_cd_n++;
            }
        }
    }
  fclose(f);
}

static int cd_days(const cd_item_t *it)
{
  time_t now = time(NULL);
  struct tm tmv;
  struct tm t2;
  time_t target;

  localtime_r(&now, &tmv);
  memset(&t2, 0, sizeof(t2));
  t2.tm_year = it->y - 1900;
  t2.tm_mon = it->m - 1;
  t2.tm_mday = it->d;
  t2.tm_hour = 12;
  target = mktime(&t2);
  if (target == (time_t)-1)
    {
      return 0;
    }
  {
    struct tm a;
    struct tm b;
    localtime_r(&target, &a);
    localtime_r(&now, &b);
    a.tm_hour = a.tm_min = a.tm_sec = 0;
    b.tm_hour = b.tm_min = b.tm_sec = 0;
    return (int)difftime(mktime(&a), mktime(&b)) / 86400;
  }
}

static void cd_paint(void);

static time_t cd_alarm_time(const cd_item_t *item)
{
  struct tm tmv;
  struct tm verify;
  time_t target;

  if (!item || !item->has_alarm || item->m < 1 || item->m > 12 ||
      item->d < 1 || item->d > 31 || item->hour < 0 || item->hour > 23 ||
      item->minute < 0 || item->minute > 59)
    {
      return (time_t)-1;
    }
  memset(&tmv, 0, sizeof(tmv));
  tmv.tm_year = item->y - 1900;
  tmv.tm_mon = item->m - 1;
  tmv.tm_mday = item->d;
  tmv.tm_hour = item->hour;
  tmv.tm_min = item->minute;
  tmv.tm_isdst = -1;
  target = mktime(&tmv);
  if (target == (time_t)-1)
    {
      return (time_t)-1;
    }
  localtime_r(&target, &verify);
  if (verify.tm_year != item->y - 1900 || verify.tm_mon != item->m - 1 ||
      verify.tm_mday != item->d || verify.tm_hour != item->hour ||
      verify.tm_min != item->minute)
    {
      return (time_t)-1;
    }
  return target;
}

int dm_countdown_ai_snapshot(dm_ai_local_record_t *out, int cap)
{
  int i, n;
  if (!dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA)) return -2;
  if (!out || cap < 1) return -1;
  n = s_cd_n < cap ? s_cd_n : cap;
  for (i = 0; i < n; i++)
    {
      memset(&out[i], 0, sizeof(out[i]));
      snprintf(out[i].title, sizeof(out[i].title), "%s", s_cd[i].name);
      out[i].year = s_cd[i].y;
      out[i].month = s_cd[i].m;
      out[i].day = s_cd[i].d;
      out[i].hour = s_cd[i].hour;
      out[i].minute = s_cd[i].minute;
      out[i].has_time = s_cd[i].has_alarm != 0;
    }
  return n;
}

int dm_countdown_ai_delete(const dm_ai_delete_request_t *request)
{
  cd_item_t previous[CD_MAX];
  int i, found = -1, old_n, old_sel;
  if (!request || request->kind != DM_AI_DATA_COUNTDOWN) return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_COUNTDOWN)) return DM_AI_DATA_DENIED;
  for (i = 0; i < s_cd_n; i++)
    if (dm_ai_data_match(request, s_cd[i].name, i + 1))
      {
        if (found >= 0) return DM_AI_DATA_AMBIGUOUS;
        found = i;
      }
  if (found < 0) return DM_AI_DATA_NOT_FOUND;
  if (found == s_cd_alert_idx) return DM_AI_DATA_BUSY;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_COUNTDOWN)) return DM_AI_DATA_DENIED;
  memcpy(previous, s_cd, sizeof(previous));
  old_n = s_cd_n;
  old_sel = s_cd_sel;
  for (i = found; i + 1 < s_cd_n; i++) s_cd[i] = s_cd[i + 1];
  s_cd_n--;
  memset(&s_cd[s_cd_n], 0, sizeof(s_cd[s_cd_n]));
  s_cd_sel = s_cd_n ? (found < s_cd_n ? found : s_cd_n - 1) : 0;
  if (cd_save() != 0)
    {
      memcpy(s_cd, previous, sizeof(previous));
      s_cd_n = old_n;
      s_cd_sel = old_sel;
      return DM_AI_DATA_SAVE;
    }
  if (s_cd_alert_idx > found) s_cd_alert_idx--;
  cd_paint();
  return 0;
}

int dm_countdown_add_voice(const char *name, int year, int month, int day,
                           int hour, int minute)
{
  time_t now = time(NULL);
  struct tm current;
  struct tm target_tm;
  struct tm verify;
  time_t target;
  size_t len;

  if (!name || !name[0] || month < 1 || month > 12 || day < 1 || day > 31 ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
      year < 2024 || year > 2099)
    {
      return -1;
    }
  if (s_cd_n >= CD_MAX)
    {
      return -2;
    }
  localtime_r(&now, &current);
  if (current.tm_year + 1900 < 2024)
    {
      return -4;
    }

  memset(&target_tm, 0, sizeof(target_tm));
  target_tm.tm_year = year - 1900;
  target_tm.tm_mon = month - 1;
  target_tm.tm_mday = day;
  target_tm.tm_hour = hour;
  target_tm.tm_min = minute;
  target_tm.tm_isdst = -1;
  target = mktime(&target_tm);
  if (target == (time_t)-1)
    {
      return -1;
    }
  localtime_r(&target, &verify);
  if (verify.tm_year != year - 1900 || verify.tm_mon != month - 1 ||
      verify.tm_mday != day || verify.tm_hour != hour ||
      verify.tm_min != minute || target <= now)
    {
      return -1;
    }

  len = strlen(name);
  if (len >= CD_NAME_MAX)
    {
      len = CD_NAME_MAX - 1;
      while (len > 0 && ((unsigned char)name[len] & 0xc0u) == 0x80u)
        {
          len--;
        }
    }
  if (len == 0)
    {
      return -1;
    }

  memset(&s_cd[s_cd_n], 0, sizeof(s_cd[s_cd_n]));
  s_cd[s_cd_n].y = year;
  s_cd[s_cd_n].m = month;
  s_cd[s_cd_n].d = day;
  s_cd[s_cd_n].hour = hour;
  s_cd[s_cd_n].minute = minute;
  s_cd[s_cd_n].has_alarm = 1;
  memcpy(s_cd[s_cd_n].name, name, len);
  s_cd[s_cd_n].name[len] = '\0';
  s_cd_sel = s_cd_n;
  s_cd_n++;
  if (cd_save() != 0)
    {
      s_cd_n--;
      memset(&s_cd[s_cd_n], 0, sizeof(s_cd[s_cd_n]));
      return -3;
    }
  cd_paint();
  dm_show(PAGE_COUNTDOWN);
  return 0;
}

static void cd_alarm_dismiss_cb(lv_event_t *e)
{
  (void)e;
  s_cd_alert_idx = -1;
  if (s_cd_alert)
    {
      lv_obj_add_flag(s_cd_alert, LV_OBJ_FLAG_HIDDEN);
    }
  dm_show(PAGE_COUNTDOWN);
}

static void *cd_alarm_beep_worker(void *arg)
{
  (void)arg;
  (void)dm_voice_beep();
  return NULL;
}

static void cd_alarm_beep_async(void)
{
  pthread_t th;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  if (pthread_create(&th, &attr, cd_alarm_beep_worker, NULL) == 0)
    {
      pthread_detach(th);
    }
  pthread_attr_destroy(&attr);
}

static void cd_alarm_tick(void)
{
  time_t now;
  int i;

  if (s_cd_alert_idx >= 0)
    {
      return;
    }
  now = time(NULL);
  for (i = 0; i < s_cd_n; i++)
    {
      time_t due;
      if (!s_cd[i].has_alarm || s_cd[i].alarm_fired)
        {
          continue;
        }
      due = cd_alarm_time(&s_cd[i]);
      if (due == (time_t)-1 || now < due)
        {
          continue;
        }

      s_cd[i].alarm_fired = 1;
      (void)cd_save();
      s_cd_alert_idx = i;
      dm_show(PAGE_COUNTDOWN);
      if (s_cd_alert_name)
        {
          lv_label_set_text(s_cd_alert_name, s_cd[i].name);
        }
      if (s_cd_alert)
        {
          lv_obj_clear_flag(s_cd_alert, LV_OBJ_FLAG_HIDDEN);
          lv_obj_move_foreground(s_cd_alert);
        }
      if (g_dm_dock)
        {
          lv_obj_add_flag(g_dm_dock, LV_OBJ_FLAG_HIDDEN);
        }
      dm_say("提醒时间到了", "Reminder time");
      cd_alarm_beep_async();
      return;
    }
}

static void cd_paint(void)
{
  int i;
  char buf[32];
  char name_line[64];

  for (i = 0; i < CD_MAX; i++)
    {
      if (!s_cd_rows[i])
        {
          continue;
        }
      if (i < s_cd_n)
        {
          int days = cd_days(&s_cd[i]);
          lv_obj_clear_flag(s_cd_rows[i], LV_OBJ_FLAG_HIDDEN);
          lv_obj_set_style_bg_color(
              s_cd_rows[i],
              lv_color_hex(i == s_cd_sel ? 0x0d3a4a : C_BTN), LV_PART_MAIN);
          if (s_cd_names[i])
            {
              if (s_cd[i].has_alarm)
                {
                  snprintf(name_line, sizeof(name_line), "%s %02d:%02d",
                           s_cd[i].name, s_cd[i].hour, s_cd[i].minute);
                  lv_label_set_text(s_cd_names[i], name_line);
                }
              else
                {
                  lv_label_set_text(s_cd_names[i], s_cd[i].name);
                }
            }
          if (s_cd_days[i])
            {
              if (days < 0)
                {
                  lv_label_set_text(s_cd_days[i], "已过");
                  lv_obj_set_style_text_color(s_cd_days[i],
                                              lv_color_hex(C_MUTED),
                                              LV_PART_MAIN);
                }
              else
                {
                  snprintf(buf, sizeof(buf), "%d", days);
                  lv_label_set_text(s_cd_days[i], buf);
                  lv_obj_set_style_text_color(s_cd_days[i],
                                              lv_color_hex(C_ACCENT),
                                              LV_PART_MAIN);
                }
            }
        }
      else
        {
          lv_obj_add_flag(s_cd_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (s_cd_date_l)
    {
      snprintf(buf, sizeof(buf), "%04d-%02d-%02d", s_cd_fy, s_cd_fm, s_cd_fd);
      lv_label_set_text(s_cd_date_l, buf);
    }
  if (s_cd_form_box)
    {
      if (s_cd_form)
        {
          lv_obj_clear_flag(s_cd_form_box, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(s_cd_form_box, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void cd_row_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  s_cd_sel = i;
  cd_paint();
}

static void cd_add_cb(lv_event_t *e)
{
  int i;
  (void)e;
  s_cd_form = !s_cd_form;
  if (s_cd_form)
    {
      s_cd_fname = 0;
      for (i = 0; i < 5; i++)
        {
          if (s_cd_name_btns[i])
            {
              lv_obj_set_style_bg_color(s_cd_name_btns[i],
                                        lv_color_hex(i == 0 ? 0x0d3a4a
                                                            : 0x222222),
                                        LV_PART_MAIN);
            }
        }
    }
  cd_paint();
}

static void cd_del_cb(lv_event_t *e)
{
  int i;
  (void)e;
  if (s_cd_n <= 0)
    {
      return;
    }
  for (i = s_cd_sel; i < s_cd_n - 1; i++)
    {
      s_cd[i] = s_cd[i + 1];
    }
  s_cd_n--;
  memset(&s_cd[s_cd_n], 0, sizeof(s_cd[s_cd_n]));
  if (s_cd_sel >= s_cd_n)
    {
      s_cd_sel = s_cd_n > 0 ? s_cd_n - 1 : 0;
    }
  cd_save();
  cd_paint();
}

static void cd_preset_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  int j;
  s_cd_fname = i;
  for (j = 0; j < 5; j++)
    {
      if (s_cd_name_btns[j])
        {
          lv_obj_set_style_bg_color(s_cd_name_btns[j],
                                    lv_color_hex(j == i ? 0x0d3a4a
                                                        : 0x222222),
                                    LV_PART_MAIN);
        }
    }
  cd_paint();
}

static void cd_date_cb(lv_event_t *e)
{
  const char *k = (const char *)lv_event_get_user_data(e);
  if (!k)
    {
      return;
    }
  if (strcmp(k, "y-") == 0)
    {
      s_cd_fy--;
    }
  else if (strcmp(k, "y+") == 0)
    {
      s_cd_fy++;
    }
  else if (strcmp(k, "m-") == 0)
    {
      s_cd_fm = s_cd_fm <= 1 ? 12 : s_cd_fm - 1;
    }
  else if (strcmp(k, "m+") == 0)
    {
      s_cd_fm = s_cd_fm >= 12 ? 1 : s_cd_fm + 1;
    }
  else if (strcmp(k, "d-") == 0)
    {
      s_cd_fd = s_cd_fd <= 1 ? 31 : s_cd_fd - 1;
    }
  else if (strcmp(k, "d+") == 0)
    {
      s_cd_fd = s_cd_fd >= 31 ? 1 : s_cd_fd + 1;
    }
  cd_paint();
}

static void cd_save_cb(lv_event_t *e)
{
  (void)e;
  if (s_cd_n >= CD_MAX)
    {
      return;
    }
  memset(&s_cd[s_cd_n], 0, sizeof(s_cd[s_cd_n]));
  s_cd[s_cd_n].y = s_cd_fy;
  s_cd[s_cd_n].m = s_cd_fm;
  s_cd[s_cd_n].d = s_cd_fd;
  snprintf(s_cd[s_cd_n].name, CD_NAME_MAX, "%s", s_cd_presets[s_cd_fname]);
  s_cd_sel = s_cd_n;
  s_cd_n++;
  s_cd_form = false;
  cd_save();
  cd_paint();
}

void dm_create_countdown(void)
{
  lv_obj_t *page = mk_life_page(PAGE_COUNTDOWN, "倒数日", "Countdown");
  lv_obj_t *sc;
  lv_obj_t *row;
  lv_obj_t *b;
  int i;
  time_t t = time(NULL);
  struct tm tmv;

  localtime_r(&t, &tmv);
  s_cd_fy = tmv.tm_year + 1900;
  s_cd_fm = tmv.tm_mon + 1;
  s_cd_fd = tmv.tm_mday;

  sc = lv_obj_create(page);
  lv_obj_set_size(sc, 304, 164);
  lv_obj_set_pos(sc, 8, 34);
  lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(sc, 4, LV_PART_MAIN);
  lv_obj_set_scroll_dir(sc, LV_DIR_VER);
  lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  for (i = 0; i < CD_MAX; i++)
    {
      row = lv_obj_create(sc);
      lv_obj_set_size(row, 300, 22);
      lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 2, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, cd_row_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      s_cd_names[i] = dm_lbl(row, "", "", g_dm_font_s, C_INK);
      lv_obj_set_width(s_cd_names[i], 220);
      lv_label_set_long_mode(s_cd_names[i], LV_LABEL_LONG_DOT);
      lv_obj_set_pos(s_cd_names[i], 4, 3);
      s_cd_days[i] = dm_lbl(row, "", "", g_dm_font_s, C_ACCENT);
      lv_obj_align(s_cd_days[i], LV_ALIGN_RIGHT_MID, -4, 0);
      s_cd_rows[i] = row;
    }

  b = dm_btn(page, "添加", "Add", 140, 24, 0x0d3a4a, C_ACCENT, cd_add_cb,
             NULL);
  lv_obj_set_pos(b, 16, 206);
  b = dm_btn(page, "删除选中", "Del", 140, 24, C_BTN, C_INK, cd_del_cb,
             NULL);
  lv_obj_set_pos(b, 164, 206);

  s_cd_form_box = lv_obj_create(page);
  lv_obj_set_size(s_cd_form_box, 304, 164);
  lv_obj_set_pos(s_cd_form_box, 8, 34);
  lv_obj_set_style_bg_color(s_cd_form_box, lv_color_hex(0x111111),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_cd_form_box, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_cd_form_box, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_cd_form_box, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_cd_form_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_cd_form_box, LV_OBJ_FLAG_HIDDEN);

  for (i = 0; i < 5; i++)
    {
      b = dm_btn(s_cd_form_box, s_cd_presets[i], s_cd_presets[i], 52, 22,
                 0x222222, C_DIM, cd_preset_cb, (void *)(intptr_t)i);
      lv_obj_set_pos(b, 8 + i * 58, 10);
      s_cd_name_btns[i] = b;
    }

  s_cd_date_l = dm_lbl(s_cd_form_box, "2026-01-01", "2026-01-01",
                       g_dm_font_m, C_INK);
  lv_obj_set_pos(s_cd_date_l, 12, 48);

  b = dm_btn(s_cd_form_box, "−", "-", 28, 22, C_BTN, C_DIM, cd_date_cb,
             "y-");
  lv_obj_set_pos(b, 12, 80);
  b = dm_btn(s_cd_form_box, "+", "+", 28, 22, C_BTN, C_DIM, cd_date_cb,
             "y+");
  lv_obj_set_pos(b, 44, 80);
  b = dm_btn(s_cd_form_box, "−月", "-M", 36, 22, C_BTN, C_DIM, cd_date_cb,
             "m-");
  lv_obj_set_pos(b, 90, 80);
  b = dm_btn(s_cd_form_box, "+月", "+M", 36, 22, C_BTN, C_DIM, cd_date_cb,
             "m+");
  lv_obj_set_pos(b, 130, 80);
  b = dm_btn(s_cd_form_box, "−日", "-D", 36, 22, C_BTN, C_DIM, cd_date_cb,
             "d-");
  lv_obj_set_pos(b, 176, 80);
  b = dm_btn(s_cd_form_box, "+日", "+D", 36, 22, C_BTN, C_DIM, cd_date_cb,
             "d+");
  lv_obj_set_pos(b, 216, 80);

  b = dm_btn(s_cd_form_box, "保存", "Save", 288, 28, C_ACCENT, C_EYE,
             cd_save_cb, NULL);
  lv_obj_set_pos(b, 8, 122);

  s_cd_alert = lv_obj_create(g_dm_root);
  lv_obj_set_size(s_cd_alert, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(s_cd_alert, 0, 0);
  lv_obj_set_style_bg_color(s_cd_alert, lv_color_hex(0x14202b), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_cd_alert, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_cd_alert, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_cd_alert, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_cd_alert, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_cd_alert, LV_OBJ_FLAG_HIDDEN);
  {
    lv_obj_t *title = dm_lbl(s_cd_alert, "提醒时间到了", "Reminder time",
                             g_dm_font_l, C_ACCENT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(title, 300);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 54);
    s_cd_alert_name = dm_lbl(s_cd_alert, "", "", g_dm_font_m, C_INK);
    lv_obj_set_style_text_align(s_cd_alert_name, LV_TEXT_ALIGN_CENTER,
                                LV_PART_MAIN);
    lv_obj_set_width(s_cd_alert_name, 300);
    lv_obj_align(s_cd_alert_name, LV_ALIGN_TOP_MID, 0, 104);
    b = dm_btn(s_cd_alert, "知道了", "Dismiss", 180, 38, C_ACCENT,
               C_EYE, cd_alarm_dismiss_cb, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 162);
  }

  cd_load();
  cd_paint();
}

/* ---------- entry ---------- */

void dm_create_life(void)
{
  mkdir("/data", 0755);
  dm_create_water();
  dm_create_countdown();
}

void dm_life_tick(void)
{
  dm_water_tick();
  cd_alarm_tick();
}

#endif /* CONFIG_DESKMATE_APP */
