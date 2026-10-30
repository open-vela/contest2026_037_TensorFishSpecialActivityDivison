/*
 * dm_ui_btnet.c — Bluetooth Headset (A2DP Source)
 * Reference: Canopus-Module-BluetoothAudio (A2DP/AVDTP headphones)
 * Scan / pair / connect headset so board audio plays on BT earphones.
 */

#include "deskmate.h"
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#define HS_ROWS 8

static lv_obj_t *hs_page_new(dm_page_t id)
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

static lv_obj_t *hs_card(lv_obj_t *p, int h)
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

static lv_obj_t *s_hs_page;
static lv_obj_t *s_hs_cur_l;
static lv_obj_t *s_hs_state_l;
static lv_obj_t *s_hs_sc;
static lv_obj_t *s_hs_devs[HS_ROWS];
static lv_obj_t *s_hs_tip_l;
static volatile int s_hs_scan_busy;
static volatile int s_hs_sel = -1;
static char s_hs_addrs[HS_ROWS][18];
static char s_hs_names[HS_ROWS][24];
static int s_hs_n;
static volatile int s_hs_scan_done;
static char s_hs_connected[18];

static void hs_back(lv_event_t *e);
static void hs_scan(lv_event_t *e);
static void hs_connect(lv_event_t *e);
static void hs_disconnect(lv_event_t *e);
static void *hs_scan_thread(void *arg);

static void hs_set_tip(const char *zh, const char *en)
{
  lv_label_set_text(s_hs_tip_l, dm_t(zh, en));
}

static void hs_update_list(void)
{
  int i;
  for (i = 0; i < HS_ROWS; i++)
    {
      if (i < s_hs_n)
        {
          char line[64];
          snprintf(line, sizeof(line), "%s  [%s]", s_hs_names[i], s_hs_addrs[i]);
          lv_label_set_text(lv_obj_get_child(s_hs_devs[i], 0), line);
          lv_obj_clear_flag(s_hs_devs[i], LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(s_hs_devs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void hs_dev_click(lv_event_t *e)
{
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  if (idx >= 0 && idx < s_hs_n)
    {
      s_hs_sel = idx;
      char msg[80];
      snprintf(msg, sizeof(msg), "已选择: %s", s_hs_names[idx]);
      hs_set_tip(msg, msg);
    }
}

static void *hs_scan_thread(void *arg)
{
  (void)arg;
  /* bttool is line-oriented stdin CLI (see tools/bt_tools.c) */
  {
    FILE *cmd = fopen("/tmp/hs_cmd.txt", "w");
    if (cmd)
      {
        fputs("enable\n", cmd);
        fputs("inquiry start 8\n", cmd);
        fputs("quit\n", cmd);
        fclose(cmd);
      }
  }
  system("bttool < /tmp/hs_cmd.txt > /tmp/hs_scan.txt 2>&1");

  FILE *fp = fopen("/tmp/hs_scan.txt", "r");
  s_hs_n = 0;
  s_hs_sel = -1;
  if (fp)
    {
      char line[160];
      while (fgets(line, sizeof(line), fp) && s_hs_n < HS_ROWS)
        {
          /* Inquiring: device [aa:bb:cc:dd:ee:ff], name: XXX, cod: ... */
          char *dev = strstr(line, "device [");
          if (!dev)
            {
              continue;
            }
          char addr[18];
          char name[24];
          addr[0] = 0;
          name[0] = 0;
          if (sscanf(dev, "device [%17[0-9a-fA-F:]]", addr) != 1 ||
              strlen(addr) != 17)
            {
              continue;
            }
          {
            char *nm = strstr(line, "name:");
            if (nm)
              {
                nm += 5;
                while (*nm == ' ')
                  {
                    nm++;
                  }
                snprintf(name, sizeof(name), "%.20s", nm);
                name[strcspn(name, ",\r\n")] = 0;
              }
          }
          if (!name[0])
            {
              snprintf(name, sizeof(name), "BT-%02d", s_hs_n);
            }
          snprintf(s_hs_addrs[s_hs_n], 18, "%s", addr);
          snprintf(s_hs_names[s_hs_n], 24, "%s", name);
          s_hs_n++;
        }
      fclose(fp);
    }

  s_hs_scan_busy = 0;
  s_hs_scan_done = 1;
  return NULL;
}

void dm_btnet_tick(void)
{
  if (!s_hs_scan_done)
    {
      return;
    }
  s_hs_scan_done = 0;
  if (s_hs_n > 0)
    {
      hs_set_tip("扫描完成，点选耳机后连接", "Scan done, select headset");
    }
  else
    {
      hs_set_tip("未发现耳机，请让耳机进入配对模式",
                 "No headset — put it in pairing mode");
    }
  hs_update_list();
}

static void hs_scan(lv_event_t *e)
{
  (void)e;
  if (s_hs_scan_busy)
    return;
  s_hs_scan_busy = 1;
  s_hs_n = 0;
  s_hs_sel = -1;
  hs_update_list();
  hs_set_tip("正在扫描蓝牙耳机…", "Scanning headsets…");

  pthread_t th;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 16384);
  if (pthread_create(&th, &attr, hs_scan_thread, NULL) == 0)
    pthread_detach(th);
  pthread_attr_destroy(&attr);
}

static void hs_connect(lv_event_t *e)
{
  (void)e;
  if (s_hs_sel < 0 || s_hs_sel >= s_hs_n)
    {
      hs_set_tip("请先选择耳机", "Select a headset first");
      return;
    }

  char msg[80];
  FILE *cmdf;
  snprintf(msg, sizeof(msg), "正在连接 %s…", s_hs_names[s_hs_sel]);
  hs_set_tip(msg, msg);

  /* bttool: createbond then a2dpsrc connect (headset in pairing mode) */
  cmdf = fopen("/tmp/hs_conn.txt", "w");
  if (cmdf)
    {
      fputs("enable\n", cmdf);
      fprintf(cmdf, "createbond %s 1\n", s_hs_addrs[s_hs_sel]);
      fputs("quit\n", cmdf);
      fclose(cmdf);
      system("bttool < /tmp/hs_conn.txt > /tmp/hs_conn.log 2>&1");
      sleep(2);
    }

  cmdf = fopen("/tmp/hs_a2dp.txt", "w");
  if (cmdf)
    {
      fputs("enable\n", cmdf);
      fprintf(cmdf, "a2dpsrc connect %s\n", s_hs_addrs[s_hs_sel]);
      fputs("quit\n", cmdf);
      fclose(cmdf);
      system("bttool < /tmp/hs_a2dp.txt > /tmp/hs_a2dp.log 2>&1");
    }

  snprintf(s_hs_connected, sizeof(s_hs_connected), "%s", s_hs_addrs[s_hs_sel]);
  { FILE *ff = fopen("/tmp/hs_connected.flag", "w"); if (ff) fclose(ff); }
  if (s_hs_cur_l)
    {
      lv_label_set_text(s_hs_cur_l, s_hs_names[s_hs_sel]);
    }
  if (s_hs_state_l)
    {
      lv_label_set_text(s_hs_state_l, dm_t("已连接", "Connected"));
    }
  hs_set_tip("耳机已连接，音频将走蓝牙耳机",
             "Headset connected, audio via BT");
}

static void hs_disconnect(lv_event_t *e)
{
  (void)e;
  if (s_hs_connected[0])
    {
      FILE *cmdf = fopen("/tmp/hs_disc.txt", "w");
      if (cmdf)
        {
          fputs("enable\n", cmdf);
          fprintf(cmdf, "a2dpsrc disconnect %s\n", s_hs_connected);
          fputs("quit\n", cmdf);
          fclose(cmdf);
          system("bttool < /tmp/hs_disc.txt > /tmp/hs_disc.log 2>&1");
        }
      s_hs_connected[0] = 0;
      unlink("/tmp/hs_connected.flag");
    }
  if (s_hs_cur_l)
    {
      lv_label_set_text(s_hs_cur_l, dm_t("未连接", "Disconnected"));
    }
  if (s_hs_state_l)
    {
      lv_label_set_text(s_hs_state_l, dm_t("空闲", "Idle"));
    }
  hs_set_tip("已断开耳机", "Headset disconnected");
}

static void hs_back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_SETTINGS);
}

void dm_create_btnet(void)
{
  lv_obj_t *title, *back, *scan, *c_cur, *c_list, *lab, *conn, *disc;
  int i;

  s_hs_page = hs_page_new(PAGE_BT_NET);
  back = dm_btn(s_hs_page, "←", "<", 36, 24, C_BTN, C_MUTED, hs_back, NULL);
  lv_obj_set_pos(back, 8, 6);
  title = dm_lbl(s_hs_page, "蓝牙耳机", "BT Headset", g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 50, 8);
  scan = dm_btn(s_hs_page, "扫描", "Scan", 64, 24, C_BTN_HI, C_ACCENT,
                hs_scan, NULL);
  lv_obj_set_pos(scan, 248, 6);

  c_cur = hs_card(s_hs_page, 42);
  lv_obj_align(c_cur, LV_ALIGN_TOP_MID, 0, 34);
  s_hs_cur_l = dm_lbl(c_cur, "未连接", "Disconnected", g_dm_font_s, C_INK);
  lv_obj_set_pos(s_hs_cur_l, 0, 0);
  s_hs_state_l = dm_lbl(c_cur, "A2DP", "A2DP", g_dm_font_s, C_MUTED);
  lv_obj_align(s_hs_state_l, LV_ALIGN_RIGHT_MID, 0, 0);

  c_list = hs_card(s_hs_page, 104);
  lv_obj_align(c_list, LV_ALIGN_TOP_MID, 0, 80);
  lab = dm_lbl(c_list, "附近耳机 (上下滑动选择)", "Nearby headsets (scroll)",
               g_dm_font_s, C_MUTED);
  lv_obj_set_pos(lab, 0, 0);

  s_hs_sc = lv_obj_create(c_list);
  lv_obj_set_size(s_hs_sc, 288, 70);
  lv_obj_set_pos(s_hs_sc, 0, 18);
  lv_obj_set_style_bg_opa(s_hs_sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_hs_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_hs_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_hs_sc, 4, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_hs_sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_hs_sc, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(s_hs_sc, LV_FLEX_FLOW_COLUMN);

  for (i = 0; i < HS_ROWS; i++)
    {
      s_hs_devs[i] = dm_btn(s_hs_sc, "", "", 280, 24, 0x1a1a1a, C_INK,
                            hs_dev_click, (void *)(intptr_t)i);
      lv_obj_set_style_radius(s_hs_devs[i], 6, LV_PART_MAIN);
      lv_obj_add_flag(s_hs_devs[i], LV_OBJ_FLAG_HIDDEN);
    }

  conn = dm_btn(s_hs_page, "连接耳机", "Connect", 130, 28, C_BTN_HI, C_ACCENT,
                hs_connect, NULL);
  lv_obj_set_pos(conn, 20, 188);
  disc = dm_btn(s_hs_page, "断开", "Disconnect", 130, 28, C_BTN, C_MUTED,
                hs_disconnect, NULL);
  lv_obj_set_pos(disc, 170, 188);

  s_hs_tip_l = dm_lbl(s_hs_page, "请将耳机置于配对模式后点击扫描",
                      "Put headset in pairing mode, then scan",
                      g_dm_font_s, C_MUTED);
  lv_obj_set_pos(s_hs_tip_l, 12, 220);

  hs_update_list();
}
