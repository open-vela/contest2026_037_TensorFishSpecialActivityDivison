/****************************************************************************
 * dm_ui_wifi.c — Settings + WiFi independent pages
 *
 * Fixes applied:
 *   1. Scrollable AP list supporting up to 16 nearby hotspots with clear
 *      24px touch rows and signal strength display.
 *   2. Non-overlapping layout on 320x240 screen (with bottom dock hidden on
 *      PAGE_WIFI so "连接所选" / Join button is never blocked).
 *   3. Password entry uses a dedicated page (PAGE_WIFI_KB) with a keyboard
 *      built from ordinary buttons: the stock LVGL keyboard rendered as a
 *      corrupted block on this SPI LCD.
 *      plus password show/hide toggle and Ready/Cancel event handling.
 *   4. Auto-triggers scan when opening PAGE_WIFI if list is empty.
 ****************************************************************************/

#include "deskmate.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#ifdef CONFIG_DESKMATE_APP

#define WIFI_AP_ROWS 16

static lv_obj_t *s_set_page;
static lv_obj_t *s_wifi_sw;
static bool s_sw_sync;
static lv_obj_t *s_wifi_sub;
static lv_obj_t *s_wifi_page;
static lv_obj_t *s_cur_ssid_l;
static lv_obj_t *s_cur_ip_l;
static lv_obj_t *s_cur_state_l;
static lv_obj_t *s_tip_l;
static lv_obj_t *s_ap_sc;
static lv_obj_t *s_ap_btns[WIFI_AP_ROWS];
static lv_obj_t *s_ap_lbls[WIFI_AP_ROWS];
static lv_obj_t *s_join_btn;
static lv_obj_t *s_kb_page;
static lv_obj_t *s_kb_ssid_l;
static lv_obj_t *s_kb_disp_l;
static lv_obj_t *s_kb_tip_l;
static lv_obj_t *s_conn_page;
static lv_obj_t *s_conn_ssid_l;
static lv_obj_t *s_done_page;
static lv_obj_t *s_done_icon;
static lv_obj_t *s_done_title;
static lv_obj_t *s_done_sub;
static lv_obj_t *s_time_now_l;
static lv_obj_t *s_time_hint_l;
static lv_obj_t *s_time_value_l[5];
static int s_time_value[5];
static int s_time_tick;

static char s_kb_buf[64];
static int s_kb_len;
static bool s_kb_upper;
static bool s_kb_mask;
static char s_sel_ssid[33];
static bool s_sel_open;

static lv_obj_t *mk_page(dm_page_t id)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(C_BG), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  g_dm_pages[id] = page;
  return page;
}

static lv_obj_t *card(lv_obj_t *p, int h)
{
  lv_obj_t *c = lv_obj_create(p);
  lv_obj_set_width(c, 304);
  lv_obj_set_height(c, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x111111), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(c, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 8, LV_PART_MAIN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void refresh_wifi_switch(void)
{
  if (!s_wifi_sw)
    {
      return;
    }

  s_sw_sync = true;
  if (dm_wifi_enabled())
    {
      lv_obj_add_state(s_wifi_sw, LV_STATE_CHECKED);
    }
  else
    {
      lv_obj_clear_state(s_wifi_sw, LV_STATE_CHECKED);
    }
  s_sw_sync = false;
}

static void refresh_wifi_header(void)
{
  int en = dm_wifi_enabled();
  int conn = en && dm_wifi_connected();
  int lost = en && dm_wifi_link_lost();

  refresh_wifi_switch();

  if (s_cur_ssid_l)
    {
      const char *txt;
      if (!en)
        {
          txt = dm_t("Wi-Fi 已关闭", "Wi-Fi off");
        }
      else if (conn)
        {
          txt = dm_wifi_cur_ssid();
        }
      else if (lost)
        {
          txt = dm_t("连接已断开", "Disconnected");
        }
      else
        {
          txt = dm_t("未连接", "Not connected");
        }
      lv_label_set_text(s_cur_ssid_l, txt);
      lv_obj_set_style_text_color(s_cur_ssid_l,
                                  lv_color_hex(MD_ON_SURFACE), LV_PART_MAIN);
    }

  if (s_cur_ip_l)
    {
      lv_label_set_text(s_cur_ip_l, conn ? dm_wifi_cur_ip() : "—");
      lv_obj_set_style_text_color(s_cur_ip_l,
                                  lv_color_hex(MD_ON_SURFACE_VARIANT),
                                  LV_PART_MAIN);
    }

  if (s_cur_state_l)
    {
      const char *txt;
      uint32_t col;
      if (!en)
        {
          txt = dm_t("已关闭", "Off");
          col = MD_ON_SURFACE_VARIANT;
        }
      else if (conn)
        {
          txt = dm_t("已连接", "Online");
          col = C_OK;
        }
      else if (lost)
        {
          txt = dm_t("已断开", "Lost");
          col = MD_ERROR;
        }
      else
        {
          txt = dm_t("离线", "Offline");
          col = MD_ON_SURFACE_VARIANT;
        }
      lv_label_set_text(s_cur_state_l, txt);
      lv_obj_set_style_text_color(s_cur_state_l, lv_color_hex(col),
                                  LV_PART_MAIN);
    }

  if (s_wifi_sub)
    {
      const char *txt = dm_t("未连接", "Disconnected");
      if (!en)
        {
          txt = dm_t("已关闭", "Off");
        }
      else if (conn)
        {
          txt = dm_wifi_cur_ssid();
        }
      else if (lost)
        {
          txt = dm_t("已断开", "Disconnected");
        }
      lv_label_set_text(s_wifi_sub, txt);
      lv_obj_set_style_text_color(s_wifi_sub,
                                  lv_color_hex(MD_ON_SURFACE_VARIANT),
                                  LV_PART_MAIN);
    }
}

static void refresh_ap_list(void)
{
  int n = dm_wifi_ap_count();
  int sel = dm_wifi_sel();
  int i;

  if (sel >= 0 && sel < n)
    {
      const char *ssid = NULL;
      bool open = false;
      if (dm_wifi_ap_get(sel, &ssid, NULL, &open) == 0 && ssid)
        {
          strncpy(s_sel_ssid, ssid, sizeof(s_sel_ssid) - 1);
          s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
          s_sel_open = open;
        }
    }

  if (!dm_wifi_enabled())
    {
      for (i = 0; i < WIFI_AP_ROWS; i++)
        {
          if (s_ap_btns[i])
            {
              lv_obj_add_flag(s_ap_btns[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
      if (s_join_btn)
        {
          lv_obj_set_style_bg_opa(s_join_btn, LV_OPA_40, LV_PART_MAIN);
        }
      return;
    }

  for (i = 0; i < WIFI_AP_ROWS; i++)
    {
      const char *ssid = NULL;
      int rssi = 0;
      bool open = false;

      if (!s_ap_btns[i])
        {
          continue;
        }

      if (i < n && dm_wifi_ap_get(i, &ssid, &rssi, &open) == 0 && ssid)
        {
          char line[56];
          lv_obj_clear_flag(s_ap_btns[i], LV_OBJ_FLAG_HIDDEN);
          lv_snprintf(line, sizeof(line), "%s%s (%ddBm)",
                      open ? "  " : "* ", ssid, rssi);
          if (s_ap_lbls[i])
            {
              lv_label_set_text(s_ap_lbls[i], line);
              lv_obj_set_style_text_color(
                  s_ap_lbls[i],
                  lv_color_hex(i == sel ? C_ACCENT : C_INK),
                  LV_PART_MAIN);
            }
          lv_obj_set_style_bg_color(
              s_ap_btns[i],
              lv_color_hex(i == sel ? 0x0d3a4a : 0x1a1a1a),
              LV_PART_MAIN);
        }
      else
        {
          lv_obj_add_flag(s_ap_btns[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (s_join_btn)
    {
      if (sel >= 0 && sel < n)
        {
          lv_obj_set_style_bg_opa(s_join_btn, LV_OPA_COVER, LV_PART_MAIN);
        }
      else
        {
          lv_obj_set_style_bg_opa(s_join_btn, LV_OPA_50, LV_PART_MAIN);
        }
    }
}

static void set_tip(const char *zh, const char *en)
{
  if (s_tip_l)
    {
      lv_label_set_text(s_tip_l, dm_t(zh, en));
    }
}

static void wifi_switch_cb(lv_event_t *e)
{
  lv_obj_t *sw = lv_event_get_target(e);
  int on;

  if (s_sw_sync)
    {
      return;
    }

  on = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
  (void)dm_wifi_set_enabled(on);
  refresh_wifi_header();
  refresh_ap_list();

  if (on)
    {
      set_tip("Wi-Fi 已打开，正在扫描…", "Wi-Fi on, scanning…");
      (void)dm_wifi_start_scan();
    }
  else
    {
      set_tip("Wi-Fi 已关闭", "Wi-Fi off");
    }
}

static void open_app_conn(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_APP_CONNECT);
}

static void open_voice_sel(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_VOICE_SEL);
}

static void open_volume(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_VOLUME);
}

static void open_about(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_ABOUT);
}

static void open_time_set(lv_event_t *e);
static void create_time_set_page(void);

static void open_wifi(lv_event_t *e)
{
  (void)e;
  refresh_wifi_header();
  refresh_ap_list();
  dm_show(PAGE_WIFI);

  if (dm_wifi_scan_busy())
    {
      set_tip("正在扫描附近热点…", "Scanning nearby APs…");
    }
  else if (dm_wifi_ap_count() == 0)
    {
      if (dm_wifi_start_scan() == 0)
        {
          set_tip("正在扫描附近热点…", "Scanning nearby APs…");
        }
      else
        {
          set_tip("点右上角「扫描」搜索热点", "Tap Scan for nearby APs");
        }
    }
  else
    {
      set_tip("点选热点后点「连接所选」", "Select AP then tap Join");
    }
}

static void back_set(lv_event_t *e)
{
  (void)e;
  refresh_wifi_header();
  dm_show(PAGE_SETTINGS);
}

static void back_wifi(lv_event_t *e)
{
  (void)e;
  refresh_wifi_header();
  dm_show(PAGE_WIFI);
}

static void scan_cb(lv_event_t *e)
{
  (void)e;
  if (!dm_wifi_enabled())
    {
      set_tip("Wi-Fi 已关闭，请先打开开关", "Wi-Fi off, turn it on first");
      return;
    }
  if (dm_wifi_start_scan() == 0)
    {
      set_tip("正在扫描附近热点…", "Scanning nearby APs…");
    }
  else
    {
      set_tip("当前正忙，请稍候重试", "Busy, try again shortly");
    }
}

static void ap_click(lv_event_t *e)
{
  int i = (int)(uintptr_t)lv_event_get_user_data(e);
  const char *ssid = NULL;
  bool open = false;

  if (i < 0 || i >= dm_wifi_ap_count())
    {
      return;
    }
  dm_wifi_set_sel(i);
  dm_wifi_ap_get(i, &ssid, NULL, &open);
  if (ssid)
    {
      strncpy(s_sel_ssid, ssid, sizeof(s_sel_ssid) - 1);
      s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
      s_sel_open = open;
    }
  refresh_ap_list();
}

static void kb_refresh(void);
static void kb_set_tip(const char *zh, const char *en);

static void join_cb(lv_event_t *e)
{
  int sel = dm_wifi_sel();
  const char *ssid = NULL;
  bool open = false;

  (void)e;
  if (!dm_wifi_enabled())
    {
      set_tip("Wi-Fi 已关闭，请先打开开关", "Wi-Fi off, turn it on first");
      return;
    }
  if (sel < 0 || sel >= dm_wifi_ap_count())
    {
      set_tip("请先扫描并点选一个热点", "Select an AP first");
      return;
    }

  if (dm_wifi_ap_get(sel, &ssid, NULL, &open) == 0 && ssid)
    {
      strncpy(s_sel_ssid, ssid, sizeof(s_sel_ssid) - 1);
      s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
      s_sel_open = open;
    }

  if (s_sel_open)
    {
      if (dm_wifi_connect(s_sel_ssid, NULL) == 0)
        {
          if (s_conn_ssid_l)
            {
              lv_label_set_text(s_conn_ssid_l, s_sel_ssid);
            }
          dm_show(PAGE_WIFI_CONN);
        }
      return;
    }

  s_kb_len = 0;
  s_kb_buf[0] = 0;
  s_kb_upper = false;
  s_kb_mask = true;
  if (s_kb_ssid_l)
    {
      lv_label_set_text(s_kb_ssid_l, s_sel_ssid);
    }
  kb_set_tip("", "");
  kb_refresh();
  dm_show(PAGE_WIFI_KB);
}

/* ---------- WiFi password keyboard (custom page) ----------
 * The stock LVGL keyboard renders as a corrupted block on this SPI LCD, so
 * the password is entered with a keyboard built from ordinary buttons.
 */

static void kb_refresh(void)
{
  char disp[64];
  int i;
  int n = s_kb_len;

  if (n > 24)
    {
      n = 24;
    }
  for (i = 0; i < n; i++)
    {
      disp[i] = s_kb_mask ? '*' : s_kb_buf[s_kb_len - n + i];
    }
  disp[n] = 0;
  if (s_kb_disp_l)
    {
      lv_label_set_text(s_kb_disp_l, disp);
    }
}

static void kb_set_tip(const char *zh, const char *en)
{
  if (s_kb_tip_l)
    {
      lv_label_set_text(s_kb_tip_l, dm_t(zh, en));
    }
}

static void kb_add_char(char c)
{
  if (s_kb_len >= (int)sizeof(s_kb_buf) - 1)
    {
      return;
    }
  if (s_kb_upper && c >= 'a' && c <= 'z')
    {
      c = (char)(c - 'a' + 'A');
    }
  s_kb_buf[s_kb_len++] = c;
  s_kb_buf[s_kb_len] = 0;
  kb_set_tip("", "");
  kb_refresh();
}

static void kb_key_cb(lv_event_t *e)
{
  char c = (char)(uintptr_t)lv_event_get_user_data(e);
  kb_add_char(c);
}

static void kb_case_cb(lv_event_t *e)
{
  (void)e;
  s_kb_upper = !s_kb_upper;
  kb_set_tip(s_kb_upper ? "大写" : "小写", s_kb_upper ? "UPPER" : "lower");
}

static void kb_back_cb(lv_event_t *e)
{
  (void)e;
  if (s_kb_len > 0)
    {
      s_kb_buf[--s_kb_len] = 0;
      kb_refresh();
    }
}

static void kb_clear_cb(lv_event_t *e)
{
  (void)e;
  s_kb_len = 0;
  s_kb_buf[0] = 0;
  kb_refresh();
}

static void kb_show_cb(lv_event_t *e)
{
  (void)e;
  s_kb_mask = !s_kb_mask;
  kb_refresh();
}

static void kb_cancel_cb(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_WIFI);
}

static void kb_ok_cb(lv_event_t *e)
{
  (void)e;
  if (!dm_wifi_enabled())
    {
      kb_set_tip("Wi-Fi 已关闭", "Wi-Fi off");
      return;
    }
  if (s_kb_len < 8 || s_kb_len > 63)
    {
      kb_set_tip("需 8-63 位", "8-63 chars");
      return;
    }
  if (dm_wifi_connect(s_sel_ssid, s_kb_buf) == 0)
    {
      if (s_conn_ssid_l)
        {
          lv_label_set_text(s_conn_ssid_l, s_sel_ssid);
        }
      dm_show(PAGE_WIFI_CONN);
    }
  else
    {
      kb_set_tip("设备忙，重试", "Busy, retry");
    }
}

static void kb_mk_key(lv_obj_t *p, int x, int y, char c)
{
  char txt[2];
  lv_obj_t *b;

  txt[0] = c;
  txt[1] = 0;
  b = dm_btn(p, txt, txt, 30, 21, MD_SURFACE_HIGH, MD_ON_SURFACE,
             kb_key_cb, (void *)(uintptr_t)(unsigned char)c);
  lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
  lv_obj_set_pos(b, x, y);
}

static void kb_mk_row(lv_obj_t *p, int y, const char *keys)
{
  int i;

  for (i = 0; i < 10 && keys[i]; i++)
    {
      kb_mk_key(p, 5 + i * 31, y, keys[i]);
    }
}

static void done_ok(lv_event_t *e)
{
  (void)e;
  dm_wifi_clear_conn_flags();
  refresh_wifi_header();
  dm_show(PAGE_WIFI);
}

static int time_days_in_month(int year, int month)
{
  static const int days[] = { 31, 28, 31, 30, 31, 30,
                              31, 31, 30, 31, 30, 31 };
  int leap;

  if (month < 1 || month > 12)
    {
      return 31;
    }
  leap = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0);
  if (month == 2 && leap)
    {
      return 29;
    }
  return days[month - 1];
}

static void time_seed_from_build(void)
{
  static const char *const months[] =
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char mon[4] = { 0 };
  int day = 1;
  int year = 2026;
  int hour = 12;
  int minute = 0;
  int second = 0;
  int i;

  s_time_value[0] = year;
  s_time_value[1] = 1;
  s_time_value[2] = 1;
  if (sscanf(__DATE__, "%3s %d %d", mon, &day, &year) == 3)
    {
      for (i = 0; i < 12; i++)
        {
          if (strcmp(mon, months[i]) == 0)
            {
              s_time_value[1] = i + 1;
              break;
            }
        }
      s_time_value[0] = year;
      s_time_value[2] = day;
    }
  else
    {
      s_time_value[0] = year;
      s_time_value[1] = 1;
      s_time_value[2] = 1;
    }

  if (sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3)
    {
      hour = 12;
      minute = 0;
    }
  s_time_value[3] = hour;
  s_time_value[4] = minute;
}

static void time_init_fields(void)
{
  time_t now = time(NULL);
  struct tm current;

  if (now > 0 && localtime_r(&now, &current) != NULL &&
      current.tm_year + 1900 >= 2024)
    {
      s_time_value[0] = current.tm_year + 1900;
      s_time_value[1] = current.tm_mon + 1;
      s_time_value[2] = current.tm_mday;
      s_time_value[3] = current.tm_hour;
      s_time_value[4] = current.tm_min;
    }
  else
    {
      time_seed_from_build();
    }
}

static void time_refresh_labels(void)
{
  char buf[64];
  time_t now = time(NULL);
  struct tm tmv;
  int i;

  for (i = 0; i < 5; i++)
    {
      if (s_time_value_l[i])
        {
          lv_snprintf(buf, sizeof(buf), "%d", s_time_value[i]);
          lv_label_set_text(s_time_value_l[i], buf);
        }
    }

  if (!s_time_now_l)
    {
      return;
    }

  if (now > 0 && localtime_r(&now, &tmv) != NULL)
    {
      lv_snprintf(buf, sizeof(buf), "%s %04d-%02d-%02d %02d:%02d",
                  dm_t("系统时间", "Device"), tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    }
  else
    {
      lv_snprintf(buf, sizeof(buf), "%s", dm_t("系统时间不可用", "Clock unavailable"));
    }
  lv_label_set_text(s_time_now_l, buf);
}

static void time_back_cb(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_SETTINGS);
}

static void time_step_cb(lv_event_t *e)
{
  int action = (int)(intptr_t)lv_event_get_user_data(e);
  int field = action >> 1;
  int delta = (action & 1) ? 1 : -1;

  if (field < 0 || field >= 5)
    {
      return;
    }

  switch (field)
    {
      case 0:
        s_time_value[0] += delta;
        if (s_time_value[0] < 2024) s_time_value[0] = 2024;
        if (s_time_value[0] > 2099) s_time_value[0] = 2099;
        if (s_time_value[2] > time_days_in_month(s_time_value[0],
                                                  s_time_value[1]))
          {
            s_time_value[2] = time_days_in_month(s_time_value[0],
                                                  s_time_value[1]);
          }
        break;
      case 1:
        s_time_value[1] += delta;
        if (s_time_value[1] < 1) s_time_value[1] = 12;
        if (s_time_value[1] > 12) s_time_value[1] = 1;
        if (s_time_value[2] > time_days_in_month(s_time_value[0],
                                                   s_time_value[1]))
          {
            s_time_value[2] = time_days_in_month(s_time_value[0],
                                                  s_time_value[1]);
          }
        break;
      case 2:
        s_time_value[2] += delta;
        if (s_time_value[2] < 1)
          {
            s_time_value[2] = time_days_in_month(s_time_value[0],
                                                  s_time_value[1]);
          }
        if (s_time_value[2] > time_days_in_month(s_time_value[0],
                                                  s_time_value[1]))
          {
            s_time_value[2] = 1;
          }
        break;
      case 3:
        s_time_value[3] = (s_time_value[3] + delta + 24) % 24;
        break;
      case 4:
        s_time_value[4] = (s_time_value[4] + delta + 60) % 60;
        break;
    }
  time_refresh_labels();
}

static void time_make_field(lv_obj_t *page, int field, int x, int y,
                            int width, const char *zh, const char *en)
{
  lv_obj_t *label;
  lv_obj_t *minus;
  lv_obj_t *plus;
  int button_w = width >= 110 ? 26 : 22;
  int value_w = width - 2 * button_w - 8;

  label = dm_lbl(page, zh, en, g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_width(label, width);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(label, x, y);

  minus = dm_btn(page, "−", "-", button_w, 27, MD_SURFACE_HIGH,
                 MD_ON_SURFACE, time_step_cb,
                 (void *)(intptr_t)(field << 1));
  lv_obj_set_pos(minus, x, y + 18);

  s_time_value_l[field] = dm_lbl(page, "0", "0", g_dm_font_m,
                                 MD_ON_SURFACE);
  lv_obj_set_width(s_time_value_l[field], value_w);
  lv_obj_set_style_text_align(s_time_value_l[field], LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_set_pos(s_time_value_l[field], x + button_w + 4, y + 18);

  plus = dm_btn(page, "+", "+", button_w, 27, MD_SURFACE_HIGH,
                MD_ON_SURFACE, time_step_cb,
                (void *)(intptr_t)((field << 1) | 1));
  lv_obj_set_pos(plus, x + width - button_w, y + 18);
}

static void time_apply_cb(lv_event_t *e)
{
  struct tm requested;
  struct tm verify;
  time_t epoch;
  int rc;

  (void)e;
  if (s_time_value[0] < 2024 || s_time_value[0] > 2099 ||
      s_time_value[1] < 1 || s_time_value[1] > 12 ||
      s_time_value[2] < 1 ||
      s_time_value[2] > time_days_in_month(s_time_value[0],
                                            s_time_value[1]) ||
      s_time_value[3] < 0 || s_time_value[3] > 23 ||
      s_time_value[4] < 0 || s_time_value[4] > 59)
    {
      if (s_time_hint_l)
        {
          lv_label_set_text(s_time_hint_l,
                            dm_t("日期或时间超出范围", "Date/time out of range"));
        }
      return;
    }

  memset(&requested, 0, sizeof(requested));
  requested.tm_year = s_time_value[0] - 1900;
  requested.tm_mon = s_time_value[1] - 1;
  requested.tm_mday = s_time_value[2];
  requested.tm_hour = s_time_value[3];
  requested.tm_min = s_time_value[4];
  requested.tm_isdst = -1;
  epoch = mktime(&requested);
  if (epoch == (time_t)-1 || localtime_r(&epoch, &verify) == NULL ||
      verify.tm_year != s_time_value[0] - 1900 ||
      verify.tm_mon != s_time_value[1] - 1 ||
      verify.tm_mday != s_time_value[2] ||
      verify.tm_hour != s_time_value[3] ||
      verify.tm_min != s_time_value[4])
    {
      if (s_time_hint_l)
        {
          lv_label_set_text(s_time_hint_l,
                            dm_t("系统无法识别该日期", "Invalid local date"));
        }
      return;
    }

  rc = dm_wifi_set_manual_time(epoch);
  if (rc != 0)
    {
      if (s_time_hint_l)
        {
          lv_label_set_text(s_time_hint_l,
                            dm_t("时间未生效，请查看串口校时日志",
                                 "Clock not applied; check serial log"));
        }
      return;
    }

  time_refresh_labels();
  if (s_time_hint_l)
    {
      lv_label_set_text(s_time_hint_l,
                        dm_t("本次开机已校准，可使用语音聊天",
                             "Clock ready for voice this boot"));
    }
}

static void open_time_set(lv_event_t *e)
{
  (void)e;
  time_init_fields();
  time_refresh_labels();
  if (s_time_hint_l)
    {
      lv_label_set_text(s_time_hint_l,
                        dm_t("按设备本地时间调整", "Adjust device local time"));
    }
  dm_show(PAGE_TIME_SET);
}

static void create_time_set_page(void)
{
  lv_obj_t *page = mk_page(PAGE_TIME_SET);
  lv_obj_t *back;
  lv_obj_t *title;
  lv_obj_t *apply;

  lv_obj_set_style_bg_color(page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  back = dm_btn(page, "←", "<", 34, 24, MD_SURFACE_HIGH,
                MD_ON_SURFACE, time_back_cb, NULL);
  lv_obj_set_pos(back, 8, 6);
  title = dm_lbl(page, "日期与时间", "Date & time", g_dm_font_m,
                 MD_ON_SURFACE);
  lv_obj_set_pos(title, 48, 7);

  s_time_now_l = dm_lbl(page, "系统时间不可用", "Clock unavailable",
                        g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_width(s_time_now_l, 304);
  lv_obj_set_style_text_align(s_time_now_l, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_set_pos(s_time_now_l, 8, 34);

  time_make_field(page, 0, 8, 62, 104, "年", "Year");
  time_make_field(page, 1, 112, 62, 92, "月", "Month");
  time_make_field(page, 2, 204, 62, 108, "日", "Day");
  time_make_field(page, 3, 48, 119, 104, "时", "Hour");
  time_make_field(page, 4, 168, 119, 104, "分", "Minute");

  apply = dm_btn(page, "应用时间", "Apply time", 132, 30,
                 MD_PRIMARY, MD_ON_PRIMARY, time_apply_cb, NULL);
  lv_obj_align(apply, LV_ALIGN_TOP_MID, 0, 173);

  s_time_hint_l = dm_lbl(page, "手动设定后 NTP 可自动校正",
                         "NTP can correct a manual time later",
                         g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_width(s_time_hint_l, 312);
  lv_obj_set_style_text_align(s_time_hint_l, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_set_pos(s_time_hint_l, 4, 210);
  time_init_fields();
  time_refresh_labels();
}

/* ---------- pages ---------- */

static void open_permissions(lv_event_t *event)
{
  (void)event;
  dm_show(PAGE_PERMISSIONS);
}

static void open_ai_log(lv_event_t *event)
{
  (void)event;
  dm_show(PAGE_AI_LOG);
  dm_ai_log_tick();
}

void dm_create_settings(void)
{
  lv_obj_t *title;
  lv_obj_t *c1;
  lv_obj_t *c2;
  lv_obj_t *name;
  lv_obj_t *arrow;

  s_set_page = mk_page(PAGE_SETTINGS);
  title = dm_lbl(s_set_page, "设定", "Settings", g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 12, 8);
  /* Scrollable settings container */
  lv_obj_t *sc = lv_obj_create(s_set_page);
  lv_obj_set_size(sc, 304, 160);
  lv_obj_set_pos(sc, 8, 38);
  lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(sc, 8, LV_PART_MAIN);
  lv_obj_set_scroll_dir(sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(sc, LV_FLEX_FLOW_COLUMN);

  c1 = card(sc, 60);
  lv_obj_set_width(c1, 288);
  lv_obj_set_style_bg_color(c1, lv_color_hex(MD_SURFACE_CONTAINER),
                            LV_PART_MAIN);
  lv_obj_set_style_radius(c1, 16, LV_PART_MAIN);
  lv_obj_add_flag(c1, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c1, open_wifi, LV_EVENT_CLICKED, NULL);
  name = dm_lbl(c1, "Wi-Fi", "Wi-Fi", g_dm_font_s, MD_ON_SURFACE);
  lv_obj_set_pos(name, 0, 2);
  s_wifi_sub = dm_lbl(c1, "未连接", "Disconnected", g_dm_font_s,
                      MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_wifi_sub, 0, 22);

  s_wifi_sw = lv_switch_create(c1);
  lv_obj_set_size(s_wifi_sw, 48, 24);
  lv_obj_align(s_wifi_sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(s_wifi_sw, lv_color_hex(MD_SURFACE_HIGHEST),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_wifi_sw, lv_color_hex(MD_PRIMARY),
                            LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s_wifi_sw, lv_color_hex(MD_ON_PRIMARY),
                            LV_PART_KNOB);
  lv_obj_add_event_cb(s_wifi_sw, wifi_switch_cb, LV_EVENT_VALUE_CHANGED,
                      NULL);

  /* Manual date/time page, available when network time is unreachable. */
  {
    lv_obj_t *c_time = card(sc, 56);
    lv_obj_set_width(c_time, 288);
    lv_obj_add_flag(c_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c_time, open_time_set, LV_EVENT_CLICKED, NULL);
    name = dm_lbl(c_time, "日期与时间", "Date & time", g_dm_font_s, C_INK);
    lv_obj_set_pos(name, 0, 2);
    {
      lv_obj_t *sub = dm_lbl(c_time, "手动校准设备本地时钟",
                             "Set device local clock manually",
                             g_dm_font_s, C_MUTED);
      lv_obj_set_pos(sub, 0, 22);
    }
    arrow = dm_lbl(c_time, "›", "›", g_dm_font_m, C_MUTED);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
  }

  /* App Connect entry */
  lv_obj_t *c_app = card(sc, 56);
  /* Temporarily hide App Connect without removing its implementation. */
  lv_obj_add_flag(c_app, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_width(c_app, 288);
  lv_obj_add_flag(c_app, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c_app, open_app_conn, LV_EVENT_CLICKED, NULL);
  name = dm_lbl(c_app, "App 连接", "App Connect", g_dm_font_s, C_INK);
  lv_obj_set_pos(name, 0, 2);
  {
    lv_obj_t *sub = dm_lbl(c_app, "通过手机 App 共享上网", "Phone app sharing",
                           g_dm_font_s, C_MUTED);
    lv_obj_set_pos(sub, 0, 22);
  }
  arrow = dm_lbl(c_app, "›", "›", g_dm_font_m, C_MUTED);
  lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);

  {
    lv_obj_t *permissions = card(sc, 56);
    lv_obj_set_width(permissions, 288);
    lv_obj_add_flag(permissions, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(permissions, open_permissions, LV_EVENT_CLICKED, NULL);
    name = dm_lbl(permissions, "权限管理", "AI permissions", g_dm_font_s, C_INK);
    lv_obj_set_pos(name, 0, 2);
    {
      lv_obj_t *sub = dm_lbl(permissions, "管理 AI 本地操作与应用访问",
                             "Manage AI local actions", g_dm_font_s, C_MUTED);
      lv_obj_set_pos(sub, 0, 22);
    }
    arrow = dm_lbl(permissions, "›", "›", g_dm_font_m, C_MUTED);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
  }

  /* AI activity follows permission management in the settings list. */
  {
    lv_obj_t *activity = card(sc, 56);
    lv_obj_set_width(activity, 288);
    lv_obj_add_flag(activity, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(activity, open_ai_log, LV_EVENT_CLICKED, NULL);
    name = dm_lbl(activity, "AI操作记录", "AI activity", g_dm_font_s, C_INK);
    lv_obj_set_pos(name, 0, 2);
    {
      lv_obj_t *sub = dm_lbl(activity, "查看访问对象、结果与拦截原因",
                             "Access, results and denials", g_dm_font_s, C_MUTED);
      lv_obj_set_pos(sub, 0, 22);
    }
    arrow = dm_lbl(activity, "›", "›", g_dm_font_m, C_MUTED);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
  }

  /* Volume settings entry */
  {
    lv_obj_t *c_vol = card(sc, 56);
    lv_obj_set_width(c_vol, 288);
    lv_obj_add_flag(c_vol, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c_vol, open_volume, LV_EVENT_CLICKED, NULL);
    name = dm_lbl(c_vol, "音量", "Volume", g_dm_font_s, C_INK);
    lv_obj_set_pos(name, 0, 2);
    {
      lv_obj_t *sub = dm_lbl(c_vol, "喇叭音量 / 静音 / 试听",
                             "Speaker / mute / test",
                             g_dm_font_s, C_MUTED);
      lv_obj_set_pos(sub, 0, 22);
    }
    arrow = dm_lbl(c_vol, "›", "›", g_dm_font_m, C_MUTED);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
  }

  /* Voice selection entry */
  {
    lv_obj_t *c_vo = card(sc, 56);
    lv_obj_set_width(c_vo, 288);
    lv_obj_add_flag(c_vo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c_vo, open_voice_sel, LV_EVENT_CLICKED, NULL);
    name = dm_lbl(c_vo, "播报声音", "Voice", g_dm_font_s, C_INK);
    lv_obj_set_pos(name, 0, 2);
    {
      lv_obj_t *sub = dm_lbl(c_vo, "冰糖 / 茉莉 / 苏打 / 白桦 等",
                             "Multiple TTS voices",
                             g_dm_font_s, C_MUTED);
      lv_obj_set_pos(sub, 0, 22);
    }
    arrow = dm_lbl(c_vo, "›", "›", g_dm_font_m, C_MUTED);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -4, 0);
  }

  c2 = card(sc, 56);
  lv_obj_set_width(c2, 288);
  lv_obj_add_flag(c2, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(c2, open_about, LV_EVENT_CLICKED, NULL);
  name = dm_lbl(c2, "关于", "About", g_dm_font_s, C_INK);
  lv_obj_set_pos(name, 0, 2);
  {
    lv_obj_t *v = dm_lbl(c2, "FlowDesk " DM_VER, "FlowDesk " DM_VER,
                         g_dm_font_s, C_MUTED);
    lv_obj_set_pos(v, 0, 20);
  }

  create_time_set_page();
}

void dm_create_wifi(void)
{
  lv_obj_t *title;
  lv_obj_t *back;
  lv_obj_t *scan;
  lv_obj_t *c_cur;
  lv_obj_t *c_list;
  lv_obj_t *lab;
  int i;

  s_wifi_page = mk_page(PAGE_WIFI);
  lv_obj_set_style_bg_color(s_wifi_page, lv_color_hex(MD_SURFACE),
                            LV_PART_MAIN);

  back = dm_btn(s_wifi_page, "←", "<", 34, 24, MD_SURFACE_HIGH,
                MD_ON_SURFACE, back_set, NULL);
  lv_obj_set_pos(back, 8, 8);
  title = dm_lbl(s_wifi_page, "Wi-Fi", "Wi-Fi", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(title, 48, 8);

  scan = dm_btn(s_wifi_page, "扫描", "Scan", 58, 24, MD_SECONDARY_CONTAINER,
                MD_ON_SECONDARY_CONT, scan_cb, NULL);
  lv_obj_set_pos(scan, 252, 8);

  /* Current connection status card */
  c_cur = card(s_wifi_page, 50);
  lv_obj_set_style_bg_color(c_cur, lv_color_hex(MD_SURFACE_CONTAINER),
                            LV_PART_MAIN);
  lv_obj_set_style_radius(c_cur, 16, LV_PART_MAIN);
  lv_obj_align(c_cur, LV_ALIGN_TOP_MID, 0, 42);
  s_cur_ssid_l = dm_lbl(c_cur, "未连接", "Disconnected", g_dm_font_s,
                        MD_ON_SURFACE);
  lv_obj_set_pos(s_cur_ssid_l, 0, 0);
  s_cur_ip_l = dm_lbl(c_cur, "—", "—", g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_cur_ip_l, 0, 16);
  s_cur_state_l = dm_lbl(c_cur, "离线", "Offline", g_dm_font_s,
                         MD_ON_SURFACE_VARIANT);
  lv_obj_align(s_cur_state_l, LV_ALIGN_RIGHT_MID, 0, 0);

  /* Nearby networks list */
  c_list = card(s_wifi_page, 86);
  lv_obj_set_style_bg_color(c_list, lv_color_hex(MD_SURFACE_CONTAINER),
                            LV_PART_MAIN);
  lv_obj_set_style_radius(c_list, 16, LV_PART_MAIN);
  lv_obj_align(c_list, LV_ALIGN_TOP_MID, 0, 98);
  lab = dm_lbl(c_list, "附近网络", "Nearby networks", g_dm_font_s,
               MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(lab, 0, 0);

  s_ap_sc = lv_obj_create(c_list);
  lv_obj_set_size(s_ap_sc, 288, 56);
  lv_obj_set_pos(s_ap_sc, 0, 18);
  lv_obj_set_style_bg_opa(s_ap_sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_ap_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_ap_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_ap_sc, 4, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_ap_sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_ap_sc, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(s_ap_sc, LV_FLEX_FLOW_COLUMN);

  for (i = 0; i < WIFI_AP_ROWS; i++)
    {
      s_ap_btns[i] = dm_btn(s_ap_sc, "", "", 280, 26, MD_SURFACE_HIGH,
                            MD_ON_SURFACE, ap_click, (void *)(uintptr_t)i);
      lv_obj_set_style_radius(s_ap_btns[i], 10, LV_PART_MAIN);
      if (lv_obj_get_child_count(s_ap_btns[i]) > 0)
        {
          s_ap_lbls[i] = lv_obj_get_child(s_ap_btns[i], 0);
          lv_obj_align(s_ap_lbls[i], LV_ALIGN_LEFT_MID, 8, 0);
        }
      lv_obj_add_flag(s_ap_btns[i], LV_OBJ_FLAG_HIDDEN);
    }

  s_tip_l = dm_lbl(s_wifi_page, "点右上角「扫描」搜索热点",
                   "Tap Scan for nearby APs", g_dm_font_s,
                   MD_ON_SURFACE_VARIANT);
  lv_obj_set_style_text_align(s_tip_l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_tip_l, LV_ALIGN_TOP_MID, 0, 186);

  s_join_btn = dm_btn(s_wifi_page, "连接所选", "Join Selected", 150, 30,
                      MD_PRIMARY, MD_ON_PRIMARY, join_cb, NULL);
  lv_obj_align(s_join_btn, LV_ALIGN_TOP_MID, 0, 204);

  /* ---------- Password keyboard page (MD3) ---------- */
  s_kb_page = mk_page(PAGE_WIFI_KB);
  lv_obj_set_style_bg_color(s_kb_page, lv_color_hex(MD_SURFACE),
                            LV_PART_MAIN);
  back = dm_btn(s_kb_page, "←", "<", 32, 24, MD_SURFACE_HIGH,
                MD_ON_SURFACE, back_wifi, NULL);
  lv_obj_set_pos(back, 6, 6);
  title = dm_lbl(s_kb_page, "输入密码", "Password", g_dm_font_s,
                 MD_ON_SURFACE);
  lv_obj_set_pos(title, 46, 8);
  s_kb_tip_l = dm_lbl(s_kb_page, "", "", g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_kb_tip_l, 150, 8);

  s_kb_ssid_l = dm_lbl(s_kb_page, "SSID", "SSID", g_dm_font_s,
                       MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_kb_ssid_l, 10, 36);

  {
    lv_obj_t *box = lv_obj_create(s_kb_page);
    lv_obj_set_size(box, 300, 28);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(box, lv_color_hex(MD_SURFACE_HIGHEST),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(MD_OUTLINE),
                                  LV_PART_MAIN);
    lv_obj_set_style_radius(box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    s_kb_disp_l = dm_lbl(box, "", "", g_dm_font_s, MD_ON_SURFACE);
    lv_obj_set_pos(s_kb_disp_l, 8, 4);
  }

  {
    lv_obj_t *b;
    b = dm_btn(s_kb_page, "显/隐", "Show", 46, 24, MD_SECONDARY_CONTAINER,
               MD_ON_SECONDARY_CONT, kb_show_cb, NULL);
    lv_obj_set_pos(b, 6, 90);
    b = dm_btn(s_kb_page, "大小", "Aa", 46, 24, MD_SECONDARY_CONTAINER,
               MD_ON_SECONDARY_CONT, kb_case_cb, NULL);
    lv_obj_set_pos(b, 56, 90);
    b = dm_btn(s_kb_page, "退格", "Del", 52, 24, MD_SURFACE_HIGH,
               MD_ON_SURFACE_VARIANT, kb_back_cb, NULL);
    lv_obj_set_pos(b, 106, 90);
    b = dm_btn(s_kb_page, "清空", "Clr", 52, 24, MD_SURFACE_HIGH,
               MD_ON_SURFACE_VARIANT, kb_clear_cb, NULL);
    lv_obj_set_pos(b, 162, 90);
    b = dm_btn(s_kb_page, "取消", "Esc", 44, 24, MD_SURFACE_HIGH,
               MD_ON_SURFACE_VARIANT, kb_cancel_cb, NULL);
    lv_obj_set_pos(b, 218, 90);
    b = dm_btn(s_kb_page, "确定", "Join", 54, 24, MD_PRIMARY,
               MD_ON_PRIMARY, kb_ok_cb, NULL);
    lv_obj_set_pos(b, 264, 90);
  }

  kb_mk_row(s_kb_page, 120, "1234567890");
  kb_mk_row(s_kb_page, 144, "qwertyuiop");
  kb_mk_row(s_kb_page, 168, "asdfghjkl");
  {
    lv_obj_t *b = dm_btn(s_kb_page, "Aa", "Aa", 30, 21, MD_SURFACE_HIGHEST,
                         MD_PRIMARY, kb_case_cb, NULL);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_pos(b, 284, 168);
  }
  kb_mk_row(s_kb_page, 192, "zxcvbnm.-@");
  kb_mk_row(s_kb_page, 216, "_#?!~:/*&");

  s_kb_len = 0;
  s_kb_buf[0] = 0;
  s_kb_upper = false;
  s_kb_mask = true;
  kb_refresh();

  /* ---------- Connecting page ---------- */
  s_conn_page = mk_page(PAGE_WIFI_CONN);
  lv_obj_set_style_bg_color(s_conn_page, lv_color_hex(MD_SURFACE),
                            LV_PART_MAIN);
  title = dm_lbl(s_conn_page, "连接中", "Connecting", g_dm_font_s,
                 MD_ON_SURFACE);
  lv_obj_set_pos(title, 12, 10);
  s_conn_ssid_l = dm_lbl(s_conn_page, "", "", g_dm_font_l, MD_ON_SURFACE);
  lv_obj_set_style_text_align(s_conn_ssid_l, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(s_conn_ssid_l, LV_ALIGN_TOP_MID, 0, 88);
  {
    lv_obj_t *hint = dm_lbl(s_conn_page, "认证并获取 IP…", "Auth + DHCP…",
                            g_dm_font_s, MD_ON_SURFACE_VARIANT);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 128);
  }

  /* ---------- Result page ---------- */
  s_done_page = mk_page(PAGE_WIFI_DONE);
  lv_obj_set_style_bg_color(s_done_page, lv_color_hex(MD_SURFACE),
                            LV_PART_MAIN);
  s_done_icon = dm_lbl(s_done_page, "OK", "OK", g_dm_font_l, C_OK);
  lv_obj_set_style_text_align(s_done_icon, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(s_done_icon, LV_ALIGN_TOP_MID, 0, 64);
  s_done_title = dm_lbl(s_done_page, "已连接", "Connected", g_dm_font_m,
                        MD_ON_SURFACE);
  lv_obj_set_style_text_align(s_done_title, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(s_done_title, LV_ALIGN_TOP_MID, 0, 104);
  s_done_sub = dm_lbl(s_done_page, "", "", g_dm_font_s,
                      MD_ON_SURFACE_VARIANT);
  lv_obj_set_style_text_align(s_done_sub, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(s_done_sub, LV_ALIGN_TOP_MID, 0, 132);
  {
    lv_obj_t *ok = dm_btn(s_done_page, "返回 Wi-Fi", "Back", 130, 30,
                          MD_PRIMARY, MD_ON_PRIMARY, done_ok, NULL);
    lv_obj_align(ok, LV_ALIGN_TOP_MID, 0, 164);
  }
}

void dm_wifi_tick(void)
{
  dm_wifi_poll();

  if (g_dm.page == PAGE_TIME_SET && (++s_time_tick & 3) == 0)
    {
      time_refresh_labels();
    }

  if (dm_wifi_link_lost())
    {
      dm_wifi_clear_link_lost();
      refresh_wifi_header();
      refresh_ap_list();
      if (g_dm.page == PAGE_WIFI)
        {
          set_tip("Wi-Fi 连接已断开，请重新连接",
                  "Connection lost, reconnect");
        }
    }

  if (g_dm.page == PAGE_SETTINGS || g_dm.page == PAGE_WIFI)
    {
      refresh_wifi_header();
    }

  if (g_dm.page == PAGE_WIFI)
    {
      if (dm_wifi_scan_ready())
        {
          dm_wifi_clear_scan_ready();
          refresh_ap_list();
          if (dm_wifi_ap_count() > 0)
            {
              char b[48];
              lv_snprintf(b, sizeof(b), "%s %d %s",
                          dm_t("找到", "Found"), dm_wifi_ap_count(),
                          dm_t("个热点，点选后连接", "APs, tap to select"));
              set_tip(b, b);
            }
          else
            {
              set_tip("未找到热点，可再点「扫描」", "No APs, tap Scan again");
            }
        }
    }

  if (g_dm.page == PAGE_WIFI_CONN)
    {
      if (dm_wifi_conn_ok())
        {
          dm_wifi_clear_conn_flags();
          refresh_wifi_header();
          if (s_done_title)
            {
              lv_label_set_text(s_done_title, dm_t("已连接", "Connected"));
              lv_obj_set_style_text_color(s_done_title, lv_color_hex(C_OK),
                                          LV_PART_MAIN);
            }
          if (s_done_icon)
            {
              lv_label_set_text(s_done_icon, "OK");
              lv_obj_set_style_text_color(s_done_icon, lv_color_hex(C_OK),
                                          LV_PART_MAIN);
            }
          if (s_done_sub)
            {
              lv_label_set_text(s_done_sub, dm_wifi_cur_ip());
            }
          dm_show(PAGE_WIFI_DONE);
        }
      else if (dm_wifi_conn_fail())
        {
          dm_wifi_clear_conn_flags();
          refresh_wifi_header();
          if (s_done_title)
            {
              lv_label_set_text(s_done_title,
                                dm_t("连接失败", "Failed"));
              lv_obj_set_style_text_color(s_done_title, lv_color_hex(C_HEART),
                                          LV_PART_MAIN);
            }
          if (s_done_icon)
            {
              lv_label_set_text(s_done_icon, "X");
              lv_obj_set_style_text_color(s_done_icon, lv_color_hex(C_HEART),
                                          LV_PART_MAIN);
            }
          if (s_done_sub)
            {
              lv_label_set_text(s_done_sub,
                                dm_t("请检查密码或重试", "Check password"));
            }
          dm_show(PAGE_WIFI_DONE);
        }
    }
}

#endif /* CONFIG_DESKMATE_APP */
