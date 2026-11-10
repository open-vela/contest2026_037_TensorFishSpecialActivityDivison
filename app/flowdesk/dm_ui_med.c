/****************************************************************************
 * dm_ui_med.c — 吃药提醒
 *
 * List page + Add page + independent keyboard page (pinyin / English).
 * Times/day uses 1/2/3 segmented control + dynamic hour slots.
 ****************************************************************************/

#include "deskmate.h"
#include "dm_ai_data.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CONFIG_DESKMATE_APP

#include <pthread.h>

#define MED_PATH "/data/deskmate_med.txt"
#define MED_TMP_PATH "/data/deskmate_med.txt.tmp"
#define MED_MAX 8
#define MED_NAME_MAX 32
#define MED_KB_PIN_MAX 10
#define MED_KB_CAND_MAX 8
#define MED_PY_DICT_N 28

typedef struct
{
  char name[MED_NAME_MAX];
  uint8_t times;
  uint8_t hours[3];
  uint8_t taken;
  uint16_t day;
} med_item_t;

typedef struct
{
  const char *py;
  const char *word;
} med_py_t;

static med_item_t s_med[MED_MAX];
static int s_med_n;
static int s_med_sel;
static int s_med_loaded;
static pthread_mutex_t s_names_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_voice_names[MED_MAX][MED_NAME_MAX];
static int s_voice_name_count;

static void med_publish_names(void)
{
  int i;
  pthread_mutex_lock(&s_names_lock);
  s_voice_name_count = s_med_n;
  for (i = 0; i < s_med_n; i++)
    snprintf(s_voice_names[i], MED_NAME_MAX, "%s", s_med[i].name);
  pthread_mutex_unlock(&s_names_lock);
}

int dm_med_voice_has_name(const char *text)
{
  int i;
  int found = 0;
  if (!text) return 0;
  pthread_mutex_lock(&s_names_lock);
  for (i = 0; i < s_voice_name_count; i++)
    if (s_voice_names[i][0] && strstr(text, s_voice_names[i]))
      {
        found = 1;
        break;
      }
  pthread_mutex_unlock(&s_names_lock);
  return found;
}

static char s_add_name[MED_NAME_MAX];
static int s_add_name_len;
static int s_add_times = 1;
static int s_add_h0 = 8;
static int s_add_h1 = 12;
static int s_add_h2 = 20;
static int s_add_preset = -1;

static lv_obj_t *s_med_rows[MED_MAX];
static lv_obj_t *s_med_names[MED_MAX];
static lv_obj_t *s_med_stat[MED_MAX];
static lv_obj_t *s_med_tip;
static lv_obj_t *s_med_list;

static lv_obj_t *s_add_sc;
static lv_obj_t *s_add_name_l;
static lv_obj_t *s_add_times_l;
static lv_obj_t *s_add_hours_l;
static lv_obj_t *s_add_slot_box;
static lv_obj_t *s_add_preset_btns[8];
static lv_obj_t *s_add_times_btns[3];
static lv_obj_t *s_add_slot_val[3];
static lv_obj_t *s_add_slot_tag[3];

static lv_obj_t *s_kb_name_l;
static lv_obj_t *s_kb_pin_l;
static lv_obj_t *s_kb_cand_box;
static lv_obj_t *s_kb_mode_btns[2];
static char s_kb_buf[MED_NAME_MAX];
static int s_kb_buf_len;
static char s_kb_pin[MED_KB_PIN_MAX];
static int s_kb_pin_len;
static int s_kb_mode; /* 0 pinyin, 1 english */

static const char *const s_med_presets[8] = {
  "感冒药", "维生素", "胃药", "降压药",
  "消炎药", "钙片", "眼药水", "其他",
};

static const med_py_t s_med_py[MED_PY_DICT_N] = {
  { "ganmao", "感冒药" },
  { "ganmaoling", "感冒灵" },
  { "weishengsu", "维生素" },
  { "weiyao", "胃药" },
  { "jiangya", "降压药" },
  { "jiangyayao", "降压药" },
  { "xiaoyan", "消炎药" },
  { "gaipian", "钙片" },
  { "yanyaoshui", "眼药水" },
  { "chuangketie", "创可贴" },
  { "gan", "感冒药" },
  { "mao", "感冒药" },
  { "wei", "胃药" },
  { "yao", "药" },
  { "su", "素" },
  { "pian", "片" },
  { "shui", "水" },
  { "yan", "消炎" },
  { "xiao", "消炎药" },
  { "jiang", "降压药" },
  { "ya", "压" },
  { "gai", "钙片" },
  { "weisu", "维生素" },
  { "sheng", "生" },
  { "yanjing", "眼药水" },
  { "yao2", "胃药" },
  { "chuang", "创可贴" },
  { "tie", "贴" },
};

static int add_hours[3];

static int med_today(void)
{
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  return (tmv.tm_yday + tmv.tm_year * 1000) & 0xffff;
}

static int med_save(void)
{
  FILE *f = fopen(MED_TMP_PATH, "w");
  int i;
  int rc = 0;

  if (!f)
    {
      return -1;
    }
  for (i = 0; i < s_med_n; i++)
    {
      if (fprintf(f, "%s|%d|%d,%d,%d|%d|%d\n", s_med[i].name,
                  s_med[i].times, s_med[i].hours[0], s_med[i].hours[1],
                  s_med[i].hours[2], s_med[i].taken, s_med[i].day) < 0)
        {
          rc = -1;
          break;
        }
    }
  if (fclose(f) != 0)
    {
      rc = -1;
    }
  if (rc == 0 && rename(MED_TMP_PATH, MED_PATH) != 0)
    {
      rc = -1;
    }
  if (rc != 0)
    {
      unlink(MED_TMP_PATH);
    }
  if (rc == 0) med_publish_names();
  return rc;
}

static void med_load(void)
{
  FILE *f = fopen(MED_PATH, "r");
  char line[96];
  int today = med_today();

  s_med_n = 0;
  s_med_sel = 0;
  s_med_loaded = 1;
  med_publish_names();
  if (!f)
    {
      return;
    }
  while (s_med_n < MED_MAX && fgets(line, sizeof(line), f))
    {
      med_item_t *m = &s_med[s_med_n];
      int t;
      int h0;
      int h1;
      int h2;
      int taken;
      int day;
      char name[MED_NAME_MAX];

      if (sscanf(line, "%31[^|]|%d|%d,%d,%d|%d|%d", name, &t, &h0, &h1,
                 &h2, &taken, &day) != 7)
        {
          continue;
        }
      snprintf(m->name, MED_NAME_MAX, "%s", name);
      m->times = (uint8_t)(t > 3 ? 3 : (t < 1 ? 1 : t));
      m->hours[0] = (uint8_t)h0;
      m->hours[1] = (uint8_t)h1;
      m->hours[2] = (uint8_t)h2;
      m->taken = (uint8_t)taken;
      m->day = (uint16_t)day;
      if (dm_wifi_clock_valid() && day != today)
        {
          m->taken = 0;
          m->day = (uint16_t)today;
        }
      s_med_n++;
    }
  fclose(f);
  med_publish_names();
}

static void med_back(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_FEATURES);
}

static lv_obj_t *mk_med_page(dm_page_t id, const char *zh, const char *en)
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

  back = dm_btn(page, "←", "<", 36, 24, C_BTN, C_MUTED, med_back, NULL);
  lv_obj_set_pos(back, 8, 6);

  title = dm_lbl(page, zh, en, g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 50, 8);

  g_dm_pages[id] = page;
  return page;
}

static void med_hours_from_state(void)
{
  add_hours[0] = s_add_h0;
  add_hours[1] = s_add_h1;
  add_hours[2] = s_add_h2;
}

static void med_hours_to_state(void)
{
  s_add_h0 = add_hours[0];
  s_add_h1 = add_hours[1];
  s_add_h2 = add_hours[2];
}

static const char *med_hour_tag(int h)
{
  if (h < 12)
    {
      return "上午";
    }
  if (h < 18)
    {
      return "下午";
    }
  return "晚上";
}

static void med_unique_hours(void)
{
  int i;
  int j;

  for (i = 0; i < s_add_times; i++)
    {
      for (j = 0; j < i; j++)
        {
          if (add_hours[i] == add_hours[j])
            {
              add_hours[i] = (add_hours[i] + 1) % 24;
              j = -1;
            }
        }
    }
}

static void med_slot_paint(void)
{
  int i;
  char b[24];

  if (s_add_times_l)
    {
      lv_label_set_text_fmt(s_add_times_l, "%d 次 / 天", s_add_times);
    }

  for (i = 0; i < 3; i++)
    {
      if (!s_add_times_btns[i])
        {
          continue;
        }
      lv_obj_set_style_bg_color(
          s_add_times_btns[i],
          lv_color_hex((i + 1) == s_add_times ? C_FACE : C_BTN_HI),
          LV_PART_MAIN);
      {
        lv_obj_t *lab = lv_obj_get_child(s_add_times_btns[i], 0);
        if (lab)
          {
            lv_obj_set_style_text_color(
                lab,
                lv_color_hex((i + 1) == s_add_times ? C_EYE : C_DIM),
                LV_PART_MAIN);
          }
      }
    }

  for (i = 0; i < 3; i++)
    {
      if (!s_add_slot_val[i])
        {
          continue;
        }
      if (i < s_add_times)
        {
          lv_snprintf(b, sizeof(b), "%02d:00", add_hours[i]);
          lv_label_set_text(s_add_slot_val[i], b);
          lv_obj_set_style_text_color(s_add_slot_val[i],
                                      lv_color_hex(C_STAR), LV_PART_MAIN);
          if (s_add_slot_tag[i])
            {
              lv_label_set_text(s_add_slot_tag[i], med_hour_tag(add_hours[i]));
            }
        }
      else
        {
          lv_label_set_text(s_add_slot_val[i], "--:--");
          lv_obj_set_style_text_color(s_add_slot_val[i],
                                      lv_color_hex(0x333333), LV_PART_MAIN);
          if (s_add_slot_tag[i])
            {
              lv_label_set_text(s_add_slot_tag[i], " ");
            }
        }
    }

  if (s_add_hours_l)
    {
      if (s_add_times == 1)
        {
          lv_snprintf(b, sizeof(b), "每天 %02d:00 %s", add_hours[0],
                      med_hour_tag(add_hours[0]));
        }
      else if (s_add_times == 2)
        {
          lv_snprintf(b, sizeof(b), "%02d:00 · %02d:00", add_hours[0],
                      add_hours[1]);
        }
      else
        {
          lv_snprintf(b, sizeof(b), "%02d · %02d · %02d", add_hours[0],
                      add_hours[1], add_hours[2]);
        }
      lv_label_set_text(s_add_hours_l, b);
    }

  if (s_kb_name_l)
    {
      if (s_kb_buf_len > 0)
        {
          lv_label_set_text(s_kb_name_l, s_kb_buf);
        }
      else
        {
          lv_label_set_text(s_kb_name_l, dm_t("未输入", "Empty"));
        }
    }
  if (s_kb_pin_l)
    {
      if (s_kb_pin_len > 0)
        {
          lv_label_set_text(s_kb_pin_l, s_kb_pin);
        }
      else
        {
          lv_label_set_text(s_kb_pin_l, "…");
        }
    }
}

static void med_paint(void)
{
  int i;
  time_t now = time(NULL);
  struct tm tmv;
  int hour;

  localtime_r(&now, &tmv);
  hour = tmv.tm_hour;

  for (i = 0; i < MED_MAX; i++)
    {
      if (!s_med_rows[i])
        {
          continue;
        }
      if (i < s_med_n)
        {
          med_item_t *m = &s_med[i];
          char st[48];
          int done = 0;
          int due = 0;
          int k;

          for (k = 0; k < m->times; k++)
            {
              if (m->taken & (1u << k))
                {
                  done++;
                }
              else if (hour >= m->hours[k])
                {
                  due = 1;
                }
            }

          lv_obj_clear_flag(s_med_rows[i], LV_OBJ_FLAG_HIDDEN);
          lv_obj_set_style_bg_color(
              s_med_rows[i],
              lv_color_hex(i == s_med_sel ? 0x0d3a4a : C_BTN), LV_PART_MAIN);
          if (s_med_names[i])
            {
              lv_label_set_text(s_med_names[i], m->name);
            }
          if (s_med_stat[i])
            {
              if (done >= m->times)
                {
                  snprintf(st, sizeof(st), "已服 %d/%d", done, m->times);
                  lv_label_set_text(s_med_stat[i], st);
                  lv_obj_set_style_text_color(s_med_stat[i],
                                              lv_color_hex(C_OK),
                                              LV_PART_MAIN);
                }
              else if (due)
                {
                  snprintf(st, sizeof(st), "待服 %d/%d", done, m->times);
                  lv_label_set_text(s_med_stat[i], st);
                  lv_obj_set_style_text_color(s_med_stat[i],
                                              lv_color_hex(C_STAR),
                                              LV_PART_MAIN);
                }
              else
                {
                  size_t n;
                  snprintf(st, sizeof(st), "%d/%d ", done, m->times);
                  for (k = 0; k < m->times; k++)
                    {
                      n = strlen(st);
                      snprintf(st + n, sizeof(st) - n, "%02d ",
                               m->hours[k]);
                    }
                  lv_label_set_text(s_med_stat[i], st);
                  lv_obj_set_style_text_color(s_med_stat[i],
                                              lv_color_hex(C_MUTED),
                                              LV_PART_MAIN);
                }
            }
        }
      else
        {
          lv_obj_add_flag(s_med_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (s_add_name_l)
    {
      if (s_add_name_len > 0)
        {
          lv_label_set_text(s_add_name_l, s_add_name);
        }
      else
        {
          lv_label_set_text(s_add_name_l, "未选择");
        }
    }

  med_slot_paint();

  for (i = 0; i < 8; i++)
    {
      if (!s_add_preset_btns[i])
        {
          continue;
        }
      lv_obj_set_style_bg_color(
          s_add_preset_btns[i],
          lv_color_hex(s_add_preset == i ? 0x0d3a4a : C_BTN_HI),
          LV_PART_MAIN);
    }
}

/* ---- list actions ---- */

static void med_row_cb(lv_event_t *e)
{
  s_med_sel = (int)(intptr_t)lv_event_get_user_data(e);
  med_paint();
}

static void med_take_cb(lv_event_t *e)
{
  med_item_t *m;
  med_item_t previous;
  int k;
  (void)e;
  if (!dm_wifi_clock_valid())
    {
      if (s_med_tip) lv_label_set_text(s_med_tip, "请先校准设备时间");
      return;
    }

  if (s_med_sel < 0 || s_med_sel >= s_med_n)
    {
      if (s_med_tip)
        {
          lv_label_set_text(s_med_tip, "先选中一种药");
        }
      return;
    }
  m = &s_med[s_med_sel];
  previous = *m;
  if (m->day != (uint16_t)med_today())
    {
      m->taken = 0;
      m->day = (uint16_t)med_today();
    }
  for (k = 0; k < m->times; k++)
    {
      if ((m->taken & (1u << k)) == 0)
        {
          m->taken |= (1u << k);
          m->day = (uint16_t)med_today();
          break;
        }
    }
  if (k >= m->times)
    {
      if (s_med_tip) lv_label_set_text(s_med_tip, "今天的次数已记录完成");
      return;
    }
  if (s_med_tip)
    {
      lv_label_set_text(s_med_tip, "已记录服药");
    }
  if (med_save() != 0)
    {
      *m = previous;
      if (s_med_tip) lv_label_set_text(s_med_tip, "保存失败，请重试");
    }
  med_paint();
}

static void med_del_cb(lv_event_t *e)
{
  med_item_t previous[MED_MAX];
  int old_count = s_med_n;
  int old_selected = s_med_sel;
  int i;
  memcpy(previous, s_med, sizeof(previous));
  (void)e;
  if (s_med_n <= 0)
    {
      return;
    }
  for (i = s_med_sel; i < s_med_n - 1; i++)
    {
      s_med[i] = s_med[i + 1];
    }
  s_med_n--;
  if (s_med_sel >= s_med_n)
    {
      s_med_sel = s_med_n > 0 ? s_med_n - 1 : 0;
    }
  if (s_med_tip)
    {
      lv_label_set_text(s_med_tip, "已删除");
    }
  if (med_save() != 0)
    {
      memcpy(s_med, previous, sizeof(previous));
      s_med_n = old_count;
      s_med_sel = old_selected;
      if (s_med_tip) lv_label_set_text(s_med_tip, "删除未保存，请重试");
    }
  med_paint();
}

int dm_med_ai_delete(const dm_ai_delete_request_t *request)
{
  med_item_t previous;
  int i, found = -1;
  if (!request || request->kind != DM_AI_DATA_MEDICATION) return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_MEDICATION)) return DM_AI_DATA_DENIED;
  if (!dm_wifi_clock_valid()) return DM_AI_DATA_CLOCK;
  if (request->selector != DM_AI_SELECT_TITLE &&
      request->selector != DM_AI_SELECT_INDEX) return DM_AI_DATA_UNSUPPORTED;
  for (i = 0; i < s_med_n; i++)
    if (dm_ai_data_match(request, s_med[i].name, i + 1))
      {
        if (found >= 0) return DM_AI_DATA_AMBIGUOUS;
        found = i;
      }
  if (found < 0 || s_med[found].day != (uint16_t)med_today() ||
      !s_med[found].taken) return DM_AI_DATA_NOT_FOUND;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_MEDICATION)) return DM_AI_DATA_DENIED;
  previous = s_med[found];
  /* Delete this drug's today's check-ins, preserving the drug and schedule. */
  s_med[found].taken = 0;
  if (med_save() != 0)
    { s_med[found] = previous; return DM_AI_DATA_SAVE; }
  med_paint();
  dm_health_refresh();
  return 0;
}

static void med_kb_back_cb(lv_event_t *e);
static void med_kb_confirm_cb(lv_event_t *e);

static void med_add_open_cb(lv_event_t *e)
{
  (void)e;
  s_add_name_len = 0;
  s_add_name[0] = 0;
  s_add_times = 1;
  s_add_preset = -1;
  s_add_h0 = 8;
  s_add_h1 = 12;
  s_add_h2 = 20;
  med_hours_from_state();
  s_kb_buf_len = 0;
  s_kb_buf[0] = 0;
  s_kb_pin_len = 0;
  s_kb_pin[0] = 0;
  s_kb_mode = 0;
  if (s_add_sc)
    {
      lv_obj_scroll_to_y(s_add_sc, 0, LV_ANIM_OFF);
    }
  med_paint();
  dm_show(PAGE_MED_ADD);
}

static void med_add_back_cb(lv_event_t *e)
{
  (void)e;
  med_paint();
  dm_show(PAGE_MED);
}

void dm_med_kb_open(void)
{
  s_kb_buf_len = 0;
  s_kb_buf[0] = 0;
  if (s_add_name_len > 0)
    {
      snprintf(s_kb_buf, sizeof(s_kb_buf), "%s", s_add_name);
      s_kb_buf_len = (int)strlen(s_kb_buf);
    }
  s_kb_pin_len = 0;
  s_kb_pin[0] = 0;
  med_slot_paint();
  dm_show(PAGE_MED_KB);
}

static void med_kb_open_cb(lv_event_t *e)
{
  (void)e;
  dm_med_kb_open();
}

static void med_preset_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  s_add_preset = i;
  if (i == 7)
    {
      dm_med_kb_open();
      return;
    }
  snprintf(s_add_name, MED_NAME_MAX, "%s", s_med_presets[i]);
  s_add_name_len = (int)strlen(s_add_name);
  med_paint();
}

static void med_times_seg_cb(lv_event_t *e)
{
  int n = (int)(intptr_t)lv_event_get_user_data(e);
  if (n < 1 || n > 3)
    {
      return;
    }
  s_add_times = n;
  med_unique_hours();
  med_hours_to_state();
  med_slot_paint();
}

static void med_slot_hour_cb(lv_event_t *e)
{
  const char *k = (const char *)lv_event_get_user_data(e);
  int idx;
  int d;

  if (!k || !k[0])
    {
      return;
    }
  idx = k[0] - '0';
  d = (k[1] == '+') ? 1 : -1;
  if (idx < 0 || idx >= s_add_times)
    {
      return;
    }
  add_hours[idx] = (add_hours[idx] + d + 24) % 24;
  med_unique_hours();
  med_hours_to_state();
  med_slot_paint();
}

static int med_py_cands(const char *pin, const char **out, int maxn)
{
  int n = 0;
  int i;

  if (!pin || !pin[0])
    {
      return 0;
    }
  for (i = 0; i < MED_PY_DICT_N && n < maxn; i++)
    {
      if (strcmp(s_med_py[i].py, pin) == 0 ||
          strncmp(s_med_py[i].py, pin, strlen(pin)) == 0)
        {
          int dup = 0;
          int j;
          for (j = 0; j < n; j++)
            {
              if (strcmp(out[j], s_med_py[i].word) == 0)
                {
                  dup = 1;
                  break;
                }
            }
          if (!dup)
            {
              out[n++] = s_med_py[i].word;
            }
        }
    }
  return n;
}

static void med_kb_paint_cands(void)
{
  const char *cands[MED_KB_CAND_MAX];
  int n;
  int i;

  if (!s_kb_cand_box)
    {
      return;
    }

  lv_obj_clean(s_kb_cand_box);
  if (s_kb_mode != 0 || s_kb_pin_len == 0)
    {
      return;
    }

  n = med_py_cands(s_kb_pin, cands, MED_KB_CAND_MAX);
  for (i = 0; i < n; i++)
    {
      lv_obj_t *b = dm_btn(s_kb_cand_box, cands[i], cands[i], 40, 24,
                           0x0d3a4a, C_ACCENT, med_kb_confirm_cb,
                           (void *)cands[i]);
      lv_obj_set_pos(b, i * 42, 0);
    }
}

/* Confirm with optional candidate word from user_data */
static void med_kb_confirm_cb(lv_event_t *e)
{
  const char *word = (const char *)lv_event_get_user_data(e);

  if (word && word[0])
    {
      snprintf(s_kb_buf, sizeof(s_kb_buf), "%s", word);
      s_kb_buf_len = (int)strlen(s_kb_buf);
      s_kb_pin_len = 0;
      s_kb_pin[0] = 0;
    }

  snprintf(s_add_name, MED_NAME_MAX, "%s", s_kb_buf);
  s_add_name_len = (int)strlen(s_add_name);
  s_add_preset = -1;
  med_slot_paint();
  med_paint();
  dm_show(PAGE_MED_ADD);
}

static void med_kb_key_cb(lv_event_t *e)
{
  const char *k = (const char *)lv_event_get_user_data(e);
  if (!k || !k[0])
    {
      return;
    }

  if (strcmp(k, "DEL") == 0)
    {
      if (s_kb_mode == 0 && s_kb_pin_len > 0)
        {
          s_kb_pin_len--;
          s_kb_pin[s_kb_pin_len] = 0;
        }
      else if (s_kb_buf_len > 0)
        {
          /* UTF-8: strip one character (1-3 bytes) */
          s_kb_buf_len--;
          while (s_kb_buf_len > 0 &&
                 (s_kb_buf[s_kb_buf_len] & 0xc0) == 0x80)
            {
              s_kb_buf_len--;
            }
          s_kb_buf[s_kb_buf_len] = 0;
        }
    }
  else if (strcmp(k, "SP") == 0)
    {
      if (s_kb_mode == 0 && s_kb_pin_len > 0)
        {
          const char *cands[MED_KB_CAND_MAX];
          int n = med_py_cands(s_kb_pin, cands, MED_KB_CAND_MAX);
          if (n > 0)
            {
              size_t cur = strlen(s_kb_buf);
              size_t add = strlen(cands[0]);
              if (cur + add < sizeof(s_kb_buf))
                {
                  memcpy(s_kb_buf + cur, cands[0], add + 1);
                  s_kb_buf_len = (int)(cur + add);
                }
              s_kb_pin_len = 0;
              s_kb_pin[0] = 0;
            }
        }
      else if (s_kb_buf_len < MED_NAME_MAX - 1)
        {
          s_kb_buf[s_kb_buf_len++] = ' ';
          s_kb_buf[s_kb_buf_len] = 0;
        }
    }
  else if (strcmp(k, "OK") == 0)
    {
      med_kb_confirm_cb(e);
      return;
    }
  else
    {
      size_t n = strlen(k);
      if (s_kb_mode == 0)
        {
          if (s_kb_pin_len + (int)n < MED_KB_PIN_MAX)
            {
              memcpy(s_kb_pin + s_kb_pin_len, k, n + 1);
              s_kb_pin_len += (int)n;
            }
        }
      else if (s_kb_buf_len + (int)n < MED_NAME_MAX)
        {
          memcpy(s_kb_buf + s_kb_buf_len, k, n + 1);
          s_kb_buf_len += (int)n;
        }
    }

  med_slot_paint();
  med_kb_paint_cands();
}

static void med_kb_mode_cb(lv_event_t *e)
{
  int m = (int)(intptr_t)lv_event_get_user_data(e);
  int i;
  s_kb_mode = m;
  s_kb_pin_len = 0;
  s_kb_pin[0] = 0;
  for (i = 0; i < 2; i++)
    {
      if (!s_kb_mode_btns[i])
        {
          continue;
        }
      lv_obj_set_style_bg_color(
          s_kb_mode_btns[i],
          lv_color_hex(i == s_kb_mode ? C_FACE : C_BTN),
          LV_PART_MAIN);
      {
        lv_obj_t *lab = lv_obj_get_child(s_kb_mode_btns[i], 0);
        if (lab)
          {
            lv_obj_set_style_text_color(
                lab, lv_color_hex(i == s_kb_mode ? C_EYE : C_MUTED),
                LV_PART_MAIN);
          }
      }
    }
  med_slot_paint();
  med_kb_paint_cands();
}

static void med_kb_back_cb(lv_event_t *e)
{
  (void)e;
  /* keep buffer as draft name when returning */
  if (s_kb_buf_len > 0)
    {
      snprintf(s_add_name, MED_NAME_MAX, "%s", s_kb_buf);
      s_add_name_len = (int)strlen(s_add_name);
    }
  med_paint();
  dm_show(PAGE_MED_ADD);
}

static void med_save_add_cb(lv_event_t *e)
{
  med_item_t *m;
  (void)e;

  med_hours_to_state();
  med_unique_hours();
  med_hours_to_state();

  if (s_add_name_len <= 0)
    {
      dm_show(PAGE_MED);
      if (s_med_tip)
        {
          lv_label_set_text(s_med_tip, "请先选药名");
        }
      return;
    }
  if (s_med_n >= MED_MAX)
    {
      dm_show(PAGE_MED);
      if (s_med_tip)
        {
          lv_label_set_text(s_med_tip, "最多 8 种");
        }
      return;
    }

  m = &s_med[s_med_n];
  memset(m, 0, sizeof(*m));
  snprintf(m->name, MED_NAME_MAX, "%s", s_add_name);
  m->times = (uint8_t)s_add_times;
  m->hours[0] = (uint8_t)s_add_h0;
  m->hours[1] = (uint8_t)s_add_h1;
  m->hours[2] = (uint8_t)s_add_h2;
  m->taken = 0;
  m->day = (uint16_t)med_today();
  s_med_sel = s_med_n;
  s_med_n++;
  if (med_save() != 0)
    {
      s_med_n--;
      memset(&s_med[s_med_n], 0, sizeof(s_med[s_med_n]));
      s_med_sel = s_med_n > 0 ? s_med_n - 1 : 0;
      if (s_med_tip) lv_label_set_text(s_med_tip, "添加未保存，请重试");
      med_paint();
      dm_show(PAGE_MED);
      return;
    }
  if (s_med_tip)
    {
      lv_label_set_text(s_med_tip, "已添加");
    }
  med_paint();
  dm_show(PAGE_MED);
}

static lv_obj_t *med_sec_card(lv_obj_t *parent, int h)
{
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, 300, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(C_BTN), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(c, 12, LV_PART_MAIN);
  lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 8, LV_PART_MAIN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static char s_kb_key_pool[40][4];
static int s_kb_key_n;

static void mk_kb_row(lv_obj_t *page, int y, const char *keys[], int n,
                      int kw, int x0)
{
  int i;
  for (i = 0; i < n && s_kb_key_n < 40; i++)
    {
      lv_obj_t *b;
      snprintf(s_kb_key_pool[s_kb_key_n], sizeof(s_kb_key_pool[0]), "%s",
               keys[i]);
      b = dm_btn(page, s_kb_key_pool[s_kb_key_n], s_kb_key_pool[s_kb_key_n],
                 kw, 26, C_BTN_HI, C_INK, med_kb_key_cb,
                 (void *)s_kb_key_pool[s_kb_key_n]);
      lv_obj_set_pos(b, x0 + i * (kw + 3), y);
      s_kb_key_n++;
    }
}


int dm_med_today_stats(int *sched_out, int *taken_out)
{
  int i;
  int sched = 0;
  int taken = 0;
  int today = med_today();

  for (i = 0; i < s_med_n; i++)
    {
      int k;
      int times = s_med[i].times;
      if (times < 1)
        {
          continue;
        }
      if (times > 3)
        {
          times = 3;
        }
      /* only today's schedule counts as valid factor data */
      if ((int)s_med[i].day != today)
        {
          continue;
        }
      sched += times;
      for (k = 0; k < times; k++)
        {
          if (s_med[i].taken & (1u << k))
            {
              taken++;
            }
        }
    }
  if (sched_out)
    {
      *sched_out = sched;
    }
  if (taken_out)
    {
      *taken_out = taken;
    }
  return sched;
}

static int med_voice_has_any(const char *text, const char *const *words,
                             size_t count)
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

static int med_voice_action_near_term(const char *text,
                                      const char *const *actions,
                                      size_t action_count,
                                      const char *medicine_word)
{
  const char *medicine;

  for (medicine = strstr(text, medicine_word); medicine;
       medicine = strstr(medicine + 1, medicine_word))
    {
      size_t i;
      for (i = 0; i < action_count; i++)
        {
          const char *action;
          for (action = strstr(text, actions[i]); action;
               action = strstr(action + 1, actions[i]))
            {
              const char *begin = action < medicine ? action : medicine;
              const char *end = action < medicine ? medicine : action;
              const char *q;
              int same_clause = 1;
              for (q = begin; q < end; q++)
                if (*q == ',' || *q == '.' || *q == ';' ||
                    strncmp(q, "，", 3) == 0 || strncmp(q, "。", 3) == 0 ||
                    strncmp(q, "；", 3) == 0)
                  {
                    same_clause = 0;
                    break;
                  }
              if (same_clause) return 1;
            }
        }
    }
  return 0;
}

int dm_med_voice_take(const char *utterance, char *reply, int reply_cap)
{
  static const char *const completed_words[] = {
    "吃了", "吃过", "吃完药", "吃药了", "服了", "服过", "服用过",
    "已服", "已吃", "已经吃", "已经服", "刚吃", "刚刚吃",
    "刚服", "刚刚服", "服药了", "服完药"
  };
  static const char *const negated_words[] = {
    "没吃", "没有吃", "未吃", "没服", "没有服", "未服", "还没吃",
    "还没有吃", "还没服", "还没有服", "不吃", "不服", "漏服",
    "忘记吃", "忘了吃", "忘记服", "忘了服", "准备吃", "准备服",
    "要吃药", "要服药", "该吃药", "该服药", "记得吃药", "记得服药"
  };
  static const char *const question_words[] = {
    "吗", "有没有", "是否", "是不是", "要不要", "该不该", "？", "?"
  };
  med_item_t old_item;
  int selected = -1;
  int ambiguous = 0;
  int today;
  int old_selected = s_med_sel;
  int i;
  int slot;

  if (!utterance || !utterance[0] || !reply || reply_cap < 8)
    {
      return -1;
    }
  reply[0] = '\0';

  if (med_voice_has_any(utterance, negated_words,
                        sizeof(negated_words) / sizeof(negated_words[0])) ||
      med_voice_has_any(utterance, question_words,
                        sizeof(question_words) / sizeof(question_words[0])) ||
      !med_voice_has_any(utterance, completed_words,
                         sizeof(completed_words) / sizeof(completed_words[0])))
    {
      snprintf(reply, (size_t)reply_cap,
               "我只会记录已经明确说出吃过的药；请说“我刚吃了药名”。");
      return -1;
    }

  if (!s_med_loaded)
    {
      med_load();
    }
  if (s_med_n <= 0)
    {
      snprintf(reply, (size_t)reply_cap,
               "吃药清单还是空的，请先在吃药页面添加药品，再让我记录。");
      return -2;
    }

  for (i = 0; i < s_med_n; i++)
    {
      const char *found = strstr(utterance, s_med[i].name);
      if (found && med_voice_action_near_term(
                       utterance, completed_words,
                       sizeof(completed_words) / sizeof(completed_words[0]),
                       s_med[i].name))
        {
          if (selected < 0)
            {
              selected = i;
            }
          else if (selected != i)
            {
              const char *old_name = s_med[selected].name;
              const char *new_name = s_med[i].name;
              size_t old_len = strlen(old_name);
              size_t new_len = strlen(new_name);
              if (new_len > old_len && strstr(new_name, old_name))
                {
                  selected = i;
                }
              else if (!(old_len > new_len && strstr(old_name, new_name)))
                {
                  ambiguous = 1;
                }
            }
        }
    }

  if (ambiguous)
    {
      snprintf(reply, (size_t)reply_cap,
               "我听到多个药名了，请只说一种药的完整名称；没有修改记录。");
      return -3;
    }
  if (selected < 0)
    {
      snprintf(reply, (size_t)reply_cap,
               "没有在吃药清单中找到这个药名，请说清已登记的药名；没有修改记录。");
      return -3;
    }

  today = med_today();
  old_item = s_med[selected];
  if ((int)s_med[selected].day != today)
    {
      s_med[selected].taken = 0;
      s_med[selected].day = (uint16_t)today;
    }
  {
    time_t now = time(NULL);
    struct tm current;
    int best_hour = -1;
    slot = -1;
    if (!dm_wifi_clock_valid() || !localtime_r(&now, &current))
      {
        s_med[selected] = old_item;
        snprintf(reply, (size_t)reply_cap, "%s", "请先校准设备时间，再记录服药。");
        return -4;
      }
    for (i = 0; i < s_med[selected].times && i < 3; i++)
      if (s_med[selected].hours[i] <= current.tm_hour &&
          s_med[selected].hours[i] > best_hour)
        {
          slot = i;
          best_hour = s_med[selected].hours[i];
        }
  }
  if (slot < 0 || (s_med[selected].taken & (1u << slot)))
    {
      s_med[selected] = old_item;
      snprintf(reply, (size_t)reply_cap, "%s",
               "当前时段已记录，或尚未到计划时段。请在吃药页核对，需要补记时手动确认。");
      return -4;
    }

  s_med[selected].taken |= (uint8_t)(1u << slot);
  s_med[selected].day = (uint16_t)today;
  s_med_sel = selected;
  if (med_save() != 0)
    {
      s_med[selected] = old_item;
      s_med_sel = old_selected;
      snprintf(reply, (size_t)reply_cap,
               "“%s”的服药记录没有保存成功，请稍后重试。",
               s_med[selected].name);
      med_paint();
      return -5;
    }

  if (s_med_tip)
    {
      lv_label_set_text(s_med_tip, "语音已记录服药");
    }
  med_paint();
  slot = 0;
  for (i = 0; i < s_med[selected].times; i++)
    if (s_med[selected].taken & (1u << i)) slot++;
  snprintf(reply, (size_t)reply_cap, "已记录“%s”今天服药 %d/%d。",
           s_med[selected].name, slot, s_med[selected].times);
  return 0;
}

void dm_create_med(void)
{
  lv_obj_t *page;
  lv_obj_t *b;
  lv_obj_t *card;
  lv_obj_t *lab;
  lv_obj_t *row;
  int i;

  mkdir("/data", 0755);

  med_hours_from_state();

  /* ========== LIST PAGE ========== */
  page = mk_med_page(PAGE_MED, "吃药", "Meds");

  s_med_list = lv_obj_create(page);
  lv_obj_set_size(s_med_list, 304, 160);
  lv_obj_set_pos(s_med_list, 8, 34);
  lv_obj_set_style_bg_opa(s_med_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_med_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_med_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_med_list, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_med_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_med_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(s_med_list, LV_FLEX_FLOW_COLUMN);

  for (i = 0; i < MED_MAX; i++)
    {
      row = lv_obj_create(s_med_list);
      lv_obj_set_size(row, 300, 44);
      lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 4, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, med_row_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      s_med_names[i] = dm_lbl(row, "", "", g_dm_font_s, C_INK);
      lv_obj_set_pos(s_med_names[i], 8, 6);
      s_med_stat[i] = dm_lbl(row, "", "", g_dm_font_s, C_MUTED);
      lv_obj_set_pos(s_med_stat[i], 8, 24);
      s_med_rows[i] = row;
    }

  s_med_tip = dm_lbl(page, " ", " ", g_dm_font_s, C_STAR);
  lv_obj_align(s_med_tip, LV_ALIGN_TOP_MID, 0, 196);

  b = dm_btn(page, "添加", "Add", 100, 32, 0x0d3a4a, C_ACCENT,
             med_add_open_cb, NULL);
  lv_obj_set_pos(b, 8, 204);
  b = dm_btn(page, "已服", "Taken", 100, 32, C_OK, C_EYE, med_take_cb,
             NULL);
  lv_obj_set_pos(b, 110, 204);
  b = dm_btn(page, "删除", "Del", 100, 32, C_BTN, C_INK, med_del_cb, NULL);
  lv_obj_set_pos(b, 212, 204);

  med_load();
  med_paint();

  /* ========== ADD PAGE ========== */
  page = mk_med_page(PAGE_MED_ADD, "添加药品", "Add Med");

  s_add_sc = lv_obj_create(page);
  lv_obj_set_size(s_add_sc, 304, 176);
  lv_obj_set_pos(s_add_sc, 8, 34);
  lv_obj_set_style_bg_opa(s_add_sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_add_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_add_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_add_sc, 8, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_add_sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_add_sc, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(s_add_sc, LV_FLEX_FLOW_COLUMN);

  card = med_sec_card(s_add_sc, 128);
  lab = dm_lbl(card, "药名", "Name", g_dm_font_s, C_MUTED);
  lv_obj_set_pos(lab, 4, 0);
  s_add_name_l = dm_lbl(card, "未选择", "None", g_dm_font_m, C_INK);
  lv_obj_set_pos(s_add_name_l, 4, 18);

  for (i = 0; i < 8; i++)
    {
      int px = 4 + (i % 4) * 72;
      int py = 48 + (i / 4) * 36;
      b = dm_btn(card, s_med_presets[i], s_med_presets[i], 68, 32, C_BTN_HI,
                 C_DIM, med_preset_cb, (void *)(intptr_t)i);
      lv_obj_set_pos(b, px, py);
      s_add_preset_btns[i] = b;
    }

  b = dm_btn(s_add_sc, "打开键盘 · 自定义中文名", "Open keyboard", 300, 32,
             C_BTN, C_ACCENT, med_kb_open_cb, NULL);
  lv_obj_set_width(b, 300);

  card = med_sec_card(s_add_sc, 120);
  lab = dm_lbl(card, "每天次数", "Times/day", g_dm_font_s, C_MUTED);
  lv_obj_set_pos(lab, 4, 0);
  s_add_times_l = dm_lbl(card, "1 次 / 天", "1x", g_dm_font_s, C_DIM);
  lv_obj_set_pos(s_add_times_l, 4, 16);

  for (i = 0; i < 3; i++)
    {
      static const char *zh3[] = { "1 次", "2 次", "3 次" };
      static const char *en3[] = { "1x", "2x", "3x" };
      b = dm_btn(card, zh3[i], en3[i], 88, 28, C_BTN_HI, C_DIM,
                 med_times_seg_cb, (void *)(intptr_t)(i + 1));
      lv_obj_set_pos(b, 8 + i * 94, 34);
      s_add_times_btns[i] = b;
    }

  s_add_hours_l = dm_lbl(card, "每天 08:00", "08:00", g_dm_font_s, C_STAR);
  lv_obj_set_pos(s_add_hours_l, 4, 66);

  s_add_slot_box = lv_obj_create(card);
  lv_obj_set_size(s_add_slot_box, 284, 40);
  lv_obj_set_pos(s_add_slot_box, 8, 82);
  lv_obj_set_style_bg_opa(s_add_slot_box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_add_slot_box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_add_slot_box, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_add_slot_box, LV_OBJ_FLAG_SCROLLABLE);

  for (i = 0; i < 3; i++)
    {
      static const char *minus_k[3] = { "0-", "1-", "2-" };
      static const char *plus_k[3] = { "0+", "1+", "2+" };
      int x = i * 94;
      b = dm_btn(s_add_slot_box, "-", "-", 24, 24, C_BTN_HI, C_INK,
                 med_slot_hour_cb, (void *)minus_k[i]);
      lv_obj_set_pos(b, x, 4);
      s_add_slot_val[i] = dm_lbl(s_add_slot_box, "08:00", "08:00",
                                 g_dm_font_s, C_STAR);
      lv_obj_set_width(s_add_slot_val[i], 44);
      lv_obj_set_style_text_align(s_add_slot_val[i], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_set_pos(s_add_slot_val[i], x + 26, 2);
      s_add_slot_tag[i] = dm_lbl(s_add_slot_box, "上午", "AM", g_dm_font_s,
                                 C_DIM);
      lv_obj_set_width(s_add_slot_tag[i], 44);
      lv_obj_set_style_text_align(s_add_slot_tag[i], LV_TEXT_ALIGN_CENTER,
                                  LV_PART_MAIN);
      lv_obj_set_pos(s_add_slot_tag[i], x + 26, 20);
      b = dm_btn(s_add_slot_box, "+", "+", 24, 24, C_BTN_HI, C_INK,
                 med_slot_hour_cb, (void *)plus_k[i]);
      lv_obj_set_pos(b, x + 70, 4);
    }

  b = dm_btn(page, "返回", "Back", 140, 32, C_BTN, C_MUTED, med_add_back_cb,
             NULL);
  lv_obj_set_pos(b, 12, 204);
  b = dm_btn(page, "保存", "Save", 140, 32, C_ACCENT, C_EYE, med_save_add_cb,
             NULL);
  lv_obj_set_pos(b, 168, 204);

  /* ========== KEYBOARD PAGE (independent + pinyin) ========== */
  page = mk_med_page(PAGE_MED_KB, "输入药名", "Med name");
  {
    lv_obj_t *okb = dm_btn(page, "确认", "OK", 48, 24, C_FACE, C_EYE,
                           med_kb_confirm_cb, NULL);
    lv_obj_set_pos(okb, 260, 6);
  }

  s_kb_name_l = dm_lbl(page, "未输入", "Empty", g_dm_font_m, C_INK);
  lv_obj_set_width(s_kb_name_l, 300);
  lv_obj_set_style_bg_color(s_kb_name_l, lv_color_hex(C_BTN), LV_PART_MAIN);
  lv_obj_set_pos(s_kb_name_l, 12, 36);

  lab = dm_lbl(page, "拼音", "PY", g_dm_font_s, C_DIM);
  lv_obj_set_pos(lab, 12, 62);
  s_kb_pin_l = dm_lbl(page, "…", "…", g_dm_font_s, C_ACCENT);
  lv_obj_set_pos(s_kb_pin_l, 48, 62);

  s_kb_cand_box = lv_obj_create(page);
  lv_obj_set_size(s_kb_cand_box, 304, 28);
  lv_obj_set_pos(s_kb_cand_box, 8, 84);
  lv_obj_set_style_bg_opa(s_kb_cand_box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_kb_cand_box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_kb_cand_box, 0, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_kb_cand_box, LV_DIR_HOR);
  lv_obj_clear_flag(s_kb_cand_box, LV_OBJ_FLAG_SCROLLABLE);

  s_kb_mode_btns[0] = dm_btn(page, "拼音", "PY", 150, 24, C_FACE, C_EYE,
                             med_kb_mode_cb, (void *)(intptr_t)0);
  lv_obj_set_pos(s_kb_mode_btns[0], 8, 116);
  s_kb_mode_btns[1] = dm_btn(page, "英文", "EN", 150, 24, C_BTN, C_MUTED,
                             med_kb_mode_cb, (void *)(intptr_t)1);
  lv_obj_set_pos(s_kb_mode_btns[1], 162, 116);

  {
    static const char *r0[] = { "q", "w", "e", "r", "t", "y", "u", "i", "o",
                                "p" };
    static const char *r1[] = { "a", "s", "d", "f", "g", "h", "j", "k", "l" };
    static const char *r2[] = { "z", "x", "c", "v", "b", "n", "m" };
    mk_kb_row(page, 146, r0, 10, 28, 8);
    mk_kb_row(page, 176, r1, 9, 28, 22);
    mk_kb_row(page, 206, r2, 7, 28, 36);
    b = dm_btn(page, "删", "Del", 40, 26, 0x333333, C_HEART, med_kb_key_cb,
               "DEL");
    lv_obj_set_pos(b, 232, 206);
  }

  b = dm_btn(page, "空格", "Space", 70, 24, C_BTN, C_DIM, med_kb_key_cb,
             "SP");
  lv_obj_set_pos(b, 8, 206);
  /* bottom back handled by page back button → keep draft */
  {
    lv_obj_t *bb = lv_obj_get_child(page, 0);
    if (bb)
      {
        lv_obj_remove_event_cb(bb, med_back);
        lv_obj_add_event_cb(bb, med_kb_back_cb, LV_EVENT_CLICKED, NULL);
      }
  }

  med_paint();
}

void dm_med_tick(void)
{
  static int acc;
  int today = med_today();
  int i;

  if (!dm_wifi_clock_valid()) return;
  acc++;
  if (acc < 8)
    {
      return;
    }
  acc = 0;
  for (i = 0; i < s_med_n; i++)
    {
      if (s_med[i].day != (uint16_t)today)
        {
          s_med[i].taken = 0;
          s_med[i].day = (uint16_t)today;
        }
    }
  if (g_dm.page == PAGE_MED || g_dm.page == PAGE_MED_ADD ||
      g_dm.page == PAGE_MED_KB)
    {
      med_paint();
    }
}

#endif /* CONFIG_DESKMATE_APP */
