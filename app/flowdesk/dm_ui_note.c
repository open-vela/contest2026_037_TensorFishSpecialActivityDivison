/****************************************************************************
 * dm_ui_note.c — 备忘 Notes
 *
 * List + Add + independent keyboard (pinyin/English).
 * Persist to /data/deskmate_note.txt (one note per line).
 ****************************************************************************/

#include "deskmate.h"
#include "dm_pinyin.h"
#include "dm_ai_local_read.h"
#include "dm_ai_data.h"
#include "dm_ai_permissions.h"
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#ifdef CONFIG_DESKMATE_APP

#define NOTE_PATH "/data/deskmate_note.txt"
#define NOTE_MAX 8
#define NOTE_LEN_MAX 32
#define NOTE_PY_MAX 24
#define NOTE_CAND_MAX 8

static char s_notes[NOTE_MAX][NOTE_LEN_MAX];
static int s_note_n;
static int s_note_sel;

static char s_draft[NOTE_LEN_MAX];
static int s_draft_len;
static char s_pin[PY_INPUT_MAX];
static int s_pin_len;
static int s_kb_mode; /* 0 py 1 en */

static lv_obj_t *s_rows[NOTE_MAX];
static lv_obj_t *s_row_txt[NOTE_MAX];
static lv_obj_t *s_tip;
static lv_obj_t *s_list;
static lv_obj_t *s_add_name;
static lv_obj_t *s_preset_btn[8];
static lv_obj_t *s_kb_name;
static lv_obj_t *s_kb_pin;
static lv_obj_t *s_kb_cand;
static lv_obj_t *s_kb_mode_btn[2];

static char s_kb_key_pool[40][4];
static int s_kb_key_n;

static const char *const s_presets[8] = {
  "买菜", "吃药", "喝水", "开会",
  "交作业", "打电话", "倒垃圾", "自定义",
};

static void note_kb_confirm_cb(lv_event_t *e);
static void note_paint(void);

static void note_back_features(lv_event_t *e)
{
  (void)e;
  dm_show(PAGE_FEATURES);
}

static void note_paint(void)
{
  int i;

  for (i = 0; i < NOTE_MAX; i++)
    {
      if (!s_rows[i])
        {
          continue;
        }
      if (i < s_note_n)
        {
          lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
          lv_obj_set_style_bg_color(
              s_rows[i],
              lv_color_hex(i == s_note_sel ? 0x0d3a4a : C_BTN),
              LV_PART_MAIN);
          if (s_row_txt[i])
            {
              char line[40];
              snprintf(line, sizeof(line), "%d. %s", i + 1, s_notes[i]);
              lv_label_set_text(s_row_txt[i], line);
            }
        }
      else
        {
          lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

  if (s_add_name)
    {
      if (s_draft_len > 0)
        {
          lv_label_set_text(s_add_name, s_draft);
        }
      else
        {
          lv_label_set_text(s_add_name, dm_t("未输入", "Empty"));
        }
    }
  if (s_kb_name)
    {
      if (s_draft_len > 0)
        {
          lv_label_set_text(s_kb_name, s_draft);
        }
      else
        {
          lv_label_set_text(s_kb_name, dm_t("未输入", "Empty"));
        }
    }
  if (s_kb_pin)
    {
      if (s_pin_len > 0)
        {
          char display[PY_INPUT_MAX + 8];
          dm_pinyin_get_display(s_pin, display, sizeof(display));
          lv_label_set_text(s_kb_pin, display);
        }
      else
        {
          lv_label_set_text(s_kb_pin, "…");
        }
    }
  for (i = 0; i < 8; i++)
    {
      if (!s_preset_btn[i])
        {
          continue;
        }
      /* highlight not tracked — visual only via draft */
      (void)i;
    }
}

static int note_save(void)
{
  FILE *f = fopen(NOTE_PATH ".tmp", "w");
  int i;
  int ok = 1;

  if (!f)
    {
      return -1;
    }
  for (i = 0; i < s_note_n; i++)
    {
      if (fprintf(f, "%s\n", s_notes[i]) < 0)
        {
          ok = 0;
          break;
        }
    }
  if (fclose(f) != 0)
    {
      ok = 0;
    }
  if (ok && rename(NOTE_PATH ".tmp", NOTE_PATH) != 0) ok = 0;
  if (!ok) unlink(NOTE_PATH ".tmp");
  return ok ? 0 : -1;
}

int dm_note_add_voice(const char *text)
{
  size_t len;
  size_t i;
  size_t copy_len;
  int old_selected = s_note_sel;

  if (!text || !text[0])
    {
      return -1;
    }
  if (s_note_n >= NOTE_MAX)
    {
      return -2;
    }

  len = strlen(text);
  if (len >= NOTE_LEN_MAX) return -4;
  copy_len = len < NOTE_LEN_MAX - 1 ? len : NOTE_LEN_MAX - 1;
  if (copy_len < len)
    {
      while (copy_len > 0 &&
             (((unsigned char)text[copy_len] & 0xc0u) == 0x80u))
        {
          copy_len--;
        }
    }
  for (i = 0; i < copy_len; i++)
    {
      s_notes[s_note_n][i] = (text[i] == '\n' || text[i] == '\r')
                               ? ' ' : text[i];
    }
  s_notes[s_note_n][copy_len] = '\0';
  if (!s_notes[s_note_n][0])
    {
      return -1;
    }

  s_note_n++;
  s_note_sel = s_note_n - 1;
  if (note_save() != 0)
    {
      s_note_n--;
      s_note_sel = old_selected;
      s_notes[s_note_n][0] = '\0';
      return -3;
    }
  note_paint();
  if (s_tip)
    {
      lv_label_set_text(s_tip, dm_t("语音已添加到备忘", "Voice note saved"));
    }
  dm_show(PAGE_NOTE);
  return 0;
}

int dm_note_ai_snapshot(dm_ai_local_record_t *out, int cap)
{
  int i, n;
  if (!dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA)) return -2;
  if (!out || cap < 1) return -1;
  n = s_note_n < cap ? s_note_n : cap;
  for (i = 0; i < n; i++)
    {
      memset(&out[i], 0, sizeof(out[i]));
      snprintf(out[i].title, sizeof(out[i].title), "%s", s_notes[i]);
    }
  return n;
}

int dm_note_ai_delete(const dm_ai_delete_request_t *request)
{
  char previous[NOTE_MAX][NOTE_LEN_MAX];
  int i, found = -1, old_n, old_sel;
  if (!request || request->kind != DM_AI_DATA_NOTE) return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_NOTE)) return DM_AI_DATA_DENIED;
  for (i = 0; i < s_note_n; i++)
    if (dm_ai_data_match(request, s_notes[i], i + 1))
      {
        if (found >= 0) return DM_AI_DATA_AMBIGUOUS;
        found = i;
      }
  if (found < 0) return DM_AI_DATA_NOT_FOUND;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_NOTE)) return DM_AI_DATA_DENIED;
  memcpy(previous, s_notes, sizeof(previous));
  old_n = s_note_n;
  old_sel = s_note_sel;
  for (i = found; i + 1 < s_note_n; i++)
    memcpy(s_notes[i], s_notes[i + 1], NOTE_LEN_MAX);
  s_note_n--;
  memset(s_notes[s_note_n], 0, NOTE_LEN_MAX);
  s_note_sel = s_note_n ? (found < s_note_n ? found : s_note_n - 1) : 0;
  if (note_save() != 0)
    {
      memcpy(s_notes, previous, sizeof(previous));
      s_note_n = old_n;
      s_note_sel = old_sel;
      return DM_AI_DATA_SAVE;
    }
  note_paint();
  return 0;
}

static void note_load(void)
{
  FILE *f = fopen(NOTE_PATH, "r");
  char line[NOTE_LEN_MAX + 8];

  s_note_n = 0;
  s_note_sel = 0;
  if (!f)
    {
      return;
    }
  while (s_note_n < NOTE_MAX && fgets(line, sizeof(line), f))
    {
      line[strcspn(line, "\r\n")] = 0;
      if (!line[0])
        {
          continue;
        }
      snprintf(s_notes[s_note_n], NOTE_LEN_MAX, "%s", line);
      s_note_n++;
    }
  fclose(f);
}

static void note_row_cb(lv_event_t *e)
{
  s_note_sel = (int)(intptr_t)lv_event_get_user_data(e);
  note_paint();
}

static void note_add_open_cb(lv_event_t *e)
{
  (void)e;
  s_draft_len = 0;
  s_draft[0] = 0;
  s_pin_len = 0;
  s_pin[0] = 0;
  s_kb_mode = 0;
  note_paint();
  dm_show(PAGE_NOTE_ADD);
}

static void note_del_cb(lv_event_t *e)
{
  int i;
  char previous[NOTE_MAX][NOTE_LEN_MAX];
  int old_count = s_note_n;
  int old_selected = s_note_sel;
  (void)e;
  memcpy(previous, s_notes, sizeof(previous));

  if (s_note_n <= 0)
    {
      if (s_tip)
        {
          lv_label_set_text(s_tip, dm_t("没有备忘", "No notes"));
        }
      return;
    }
  for (i = s_note_sel; i < s_note_n - 1; i++)
    {
      memcpy(s_notes[i], s_notes[i + 1], NOTE_LEN_MAX);
    }
  s_note_n--;
  if (s_note_sel >= s_note_n)
    {
      s_note_sel = s_note_n > 0 ? s_note_n - 1 : 0;
    }
  if (note_save() != 0)
    {
      memcpy(s_notes, previous, sizeof(previous));
      s_note_n = old_count;
      s_note_sel = old_selected;
      if (s_tip) lv_label_set_text(s_tip, dm_t("删除未保存，请重试", "Delete failed"));
      note_paint();
      return;
    }
  note_paint();
  if (s_tip)
    {
      lv_label_set_text(s_tip, dm_t("已删除", "Deleted"));
    }
}

static void note_save_draft(lv_event_t *e)
{
  (void)e;
  if (s_draft_len <= 0)
    {
      if (s_tip)
        {
          lv_label_set_text(s_tip, dm_t("请先输入", "Type first"));
        }
      dm_show(PAGE_NOTE);
      return;
    }
  if (s_note_n >= NOTE_MAX)
    {
      if (s_tip)
        {
          lv_label_set_text(s_tip, dm_t("最多 8 条", "Max 8"));
        }
      dm_show(PAGE_NOTE);
      return;
    }
  snprintf(s_notes[s_note_n], NOTE_LEN_MAX, "%s", s_draft);
  s_note_n++;
  s_note_sel = s_note_n - 1;
  if (note_save() != 0)
    {
      s_note_n--;
      s_notes[s_note_n][0] = 0;
      s_note_sel = s_note_n > 0 ? s_note_n - 1 : 0;
      if (s_tip) lv_label_set_text(s_tip, dm_t("保存失败，请重试", "Save failed"));
      dm_show(PAGE_NOTE);
      note_paint();
      return;
    }
  note_paint();
  if (s_tip)
    {
      lv_label_set_text(s_tip, dm_t("已保存", "Saved"));
    }
  dm_show(PAGE_NOTE);
}

static void note_preset_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);

  if (i == 7)
    {
      s_draft_len = 0;
      s_draft[0] = 0;
      s_pin_len = 0;
      s_pin[0] = 0;
      note_paint();
      dm_show(PAGE_NOTE_KB);
      return;
    }
  snprintf(s_draft, NOTE_LEN_MAX, "%s", s_presets[i]);
  s_draft_len = (int)strlen(s_draft);
  note_paint();
}

static void note_kb_open_cb(lv_event_t *e)
{
  (void)e;
  s_pin_len = 0;
  s_pin[0] = 0;
  note_paint();
  dm_show(PAGE_NOTE_KB);
}

static void note_add_back_cb(lv_event_t *e)
{
  (void)e;
  note_paint();
  dm_show(PAGE_NOTE);
}

static void note_paint_cands(void)
{
  py_cand_t c[NOTE_CAND_MAX];
  int n;
  int i, j;
  int x = 0;

  if (!s_kb_cand)
    {
      return;
    }
  lv_obj_clean(s_kb_cand);
  if (s_kb_mode != 0 || s_pin_len == 0)
    {
      return;
    }
  n = dm_pinyin_get_cands(s_pin, c, NOTE_CAND_MAX);

  /* Show individual characters from each candidate group */
  for (i = 0; i < n && x < 280; i++)
    {
      const char *chars = c[i].chars;
      int chars_len = c[i].chars_len;
      for (j = 0; j < chars_len && x < 280; j++)
        {
          char buf[8];
          memcpy(buf, chars + j * 3, 3);
          buf[3] = 0;
          lv_obj_t *b = dm_btn(s_kb_cand, buf, buf, 32, 24, 0x0d3a4a,
                               C_ACCENT, note_kb_confirm_cb, (void *)(chars + j * 3));
          lv_obj_set_pos(b, x, 0);
          x += 34;
        }
    }
}

static void note_kb_confirm_cb(lv_event_t *e)
{
  const char *w = (const char *)lv_event_get_user_data(e);

  if (w && w[0])
    {
      /* Append one CJK character (3 bytes UTF-8) to draft */
      size_t cur = s_draft_len;
      if (cur + 3 < NOTE_LEN_MAX - 1)
        {
          memcpy(s_draft + cur, w, 3);
          s_draft_len = (int)(cur + 3);
          s_draft[s_draft_len] = 0;
        }
      /* Clear pinyin input after selecting a character */
      s_pin_len = 0;
      s_pin[0] = 0;
    }
  note_paint();
  note_paint_cands();
}

static void note_kb_key_cb(lv_event_t *e)
{
  const char *k = (const char *)lv_event_get_user_data(e);

  if (!k || !k[0])
    {
      return;
    }
  if (strcmp(k, "DEL") == 0)
    {
      if (s_kb_mode == 0 && s_pin_len > 0)
        {
          s_pin[--s_pin_len] = 0;
        }
      else if (s_draft_len > 0)
        {
          s_draft_len--;
          while (s_draft_len > 0 &&
                 ((unsigned char)s_draft[s_draft_len] & 0xc0) == 0x80)
            {
              s_draft_len--;
            }
          s_draft[s_draft_len] = 0;
        }
    }
  else if (strcmp(k, "SP") == 0)
    {
      if (s_kb_mode == 0 && s_pin_len > 0)
        {
          py_cand_t c[NOTE_CAND_MAX];
          int n = dm_pinyin_get_cands(s_pin, c, NOTE_CAND_MAX);
          if (n > 0 && c[0].chars_len > 0)
            {
              /* Append the first character of the first candidate */
              size_t cur = s_draft_len;
              if (cur + 3 < NOTE_LEN_MAX - 1)
                {
                  memcpy(s_draft + cur, c[0].chars, 3);
                  s_draft_len = (int)(cur + 3);
                  s_draft[s_draft_len] = 0;
                }
              s_pin_len = 0;
              s_pin[0] = 0;
            }
          else
            {
              /* No candidates, add space */
              if (s_draft_len + 1 < NOTE_LEN_MAX - 1)
                {
                  s_draft[s_draft_len++] = ' ';
                  s_draft[s_draft_len] = 0;
                }
            }
        }
      else if (s_kb_mode == 1)
        {
          /* English mode: add space */
          if (s_draft_len + 1 < NOTE_LEN_MAX - 1)
            {
              s_draft[s_draft_len++] = ' ';
              s_draft[s_draft_len] = 0;
            }
        }
    }
  else if (strcmp(k, "OK") == 0)
    {
      note_kb_confirm_cb(e);
      return;
    }
  else
    {
      size_t n = strlen(k);
      if (s_kb_mode == 0)
        {
          if (s_pin_len + (int)n < (int)sizeof(s_pin) - 1)
            {
              memcpy(s_pin + s_pin_len, k, n + 1);
              s_pin_len += (int)n;
            }
        }
      else if (s_draft_len + (int)n < NOTE_LEN_MAX - 1)
        {
          memcpy(s_draft + s_draft_len, k, n + 1);
          s_draft_len += (int)n;
        }
    }
  note_paint();
  note_paint_cands();
}

static void note_kb_mode_cb(lv_event_t *e)
{
  int m = (int)(intptr_t)lv_event_get_user_data(e);
  int i;

  s_kb_mode = m;
  s_pin_len = 0;
  s_pin[0] = 0;
  for (i = 0; i < 2; i++)
    {
      if (!s_kb_mode_btn[i])
        {
          continue;
        }
      lv_obj_set_style_bg_color(
          s_kb_mode_btn[i],
          lv_color_hex(i == s_kb_mode ? C_FACE : C_BTN), LV_PART_MAIN);
      {
        lv_obj_t *lab = lv_obj_get_child(s_kb_mode_btn[i], 0);
        if (lab)
          {
            lv_obj_set_style_text_color(
                lab, lv_color_hex(i == s_kb_mode ? C_EYE : C_MUTED),
                LV_PART_MAIN);
          }
      }
    }
  note_paint();
  note_paint_cands();
}

static void note_kb_back_cb(lv_event_t *e)
{
  (void)e;
  note_paint();
  dm_show(PAGE_NOTE_ADD);
}

static lv_obj_t *mk_page(dm_page_t id, const char *zh, const char *en)
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

  back = dm_btn(page, "←", "<", 36, 24, C_BTN, C_MUTED, note_back_features,
                NULL);
  lv_obj_set_pos(back, 8, 6);

  title = dm_lbl(page, zh, en, g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 50, 8);

  g_dm_pages[id] = page;
  return page;
}

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
                 kw, 26, C_BTN_HI, C_INK, note_kb_key_cb,
                 (void *)s_kb_key_pool[s_kb_key_n]);
      lv_obj_set_pos(b, x0 + i * (kw + 3), y);
      s_kb_key_n++;
    }
}

void dm_create_note(void)
{
  lv_obj_t *page;
  lv_obj_t *b;
  lv_obj_t *lab;
  int i;

  /* Guard: only create once — duplicate create leaves orphan pages on top */
  if (g_dm_pages[PAGE_NOTE])
    {
      return;
    }

  mkdir("/data", 0755);
  dm_pinyin_init();

  /* LIST */
  page = mk_page(PAGE_NOTE, "备忘", "Notes");

  s_list = lv_obj_create(page);
  lv_obj_set_size(s_list, 304, 160);
  lv_obj_set_pos(s_list, 8, 34);
  lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

  for (i = 0; i < NOTE_MAX; i++)
    {
      lv_obj_t *row = lv_obj_create(s_list);
      lv_obj_set_size(row, 300, 40);
      lv_obj_set_style_bg_color(row, lv_color_hex(C_BTN), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, note_row_cb, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      s_row_txt[i] = dm_lbl(row, "", "", g_dm_font_s, C_INK);
      lv_obj_set_pos(s_row_txt[i], 10, 10);
      s_rows[i] = row;
    }

  s_tip = dm_lbl(page, " ", " ", g_dm_font_s, C_STAR);
  lv_obj_align(s_tip, LV_ALIGN_TOP_MID, 0, 196);

  b = dm_btn(page, "添加", "Add", 140, 32, 0x0d3a4a, C_ACCENT,
             note_add_open_cb, NULL);
  lv_obj_set_pos(b, 20, 204);
  b = dm_btn(page, "删除", "Del", 140, 32, C_BTN, C_INK, note_del_cb, NULL);
  lv_obj_set_pos(b, 160, 204);

  note_load();
  note_paint();

  /* ADD */
  page = mk_page(PAGE_NOTE_ADD, "新建备忘", "New note");

  lab = dm_lbl(page, "内容", "Text", g_dm_font_s, C_MUTED);
  lv_obj_set_pos(lab, 12, 36);
  s_add_name = dm_lbl(page, "未输入", "Empty", g_dm_font_m, C_INK);
  lv_obj_set_pos(s_add_name, 12, 54);

  for (i = 0; i < 8; i++)
    {
      int px = 12 + (i % 4) * 76;
      int py = 88 + (i / 4) * 36;
      b = dm_btn(page, s_presets[i], s_presets[i], 72, 32, C_BTN_HI, C_DIM,
                 note_preset_cb, (void *)(intptr_t)i);
      lv_obj_set_pos(b, px, py);
      s_preset_btn[i] = b;
    }

  b = dm_btn(page, "打开键盘 · 自定义", "Open keyboard", 300, 32, C_BTN,
             C_ACCENT, note_kb_open_cb, NULL);
  lv_obj_set_pos(b, 10, 164);

  b = dm_btn(page, "保存", "Save", 140, 32, C_ACCENT, C_EYE,
             note_save_draft, NULL);
  lv_obj_set_pos(b, 90, 204);
  /* override back for add page */
  {
    lv_obj_t *bk = lv_obj_get_child(page, 0);
    if (bk)
      {
        lv_obj_remove_event_cb(bk, note_back_features);
        lv_obj_add_event_cb(bk, note_add_back_cb, LV_EVENT_CLICKED, NULL);
      }
  }

  /* KEYBOARD */
  page = mk_page(PAGE_NOTE_KB, "输入备忘", "Type note");
  {
    lv_obj_t *ok = dm_btn(page, "确认", "OK", 48, 24, C_FACE, C_EYE,
                          note_kb_confirm_cb, NULL);
    lv_obj_set_pos(ok, 260, 6);
    lv_obj_t *bk = lv_obj_get_child(page, 0);
    if (bk)
      {
        lv_obj_remove_event_cb(bk, note_back_features);
        lv_obj_add_event_cb(bk, note_kb_back_cb, LV_EVENT_CLICKED, NULL);
      }
  }

  s_kb_name = dm_lbl(page, "未输入", "Empty", g_dm_font_m, C_INK);
  lv_obj_set_width(s_kb_name, 300);
  lv_obj_set_pos(s_kb_name, 12, 36);

  lab = dm_lbl(page, "拼音", "PY", g_dm_font_s, C_DIM);
  lv_obj_set_pos(lab, 12, 62);
  s_kb_pin = dm_lbl(page, "…", "…", g_dm_font_s, C_ACCENT);
  lv_obj_set_pos(s_kb_pin, 48, 62);

  s_kb_cand = lv_obj_create(page);
  lv_obj_set_size(s_kb_cand, 304, 28);
  lv_obj_set_pos(s_kb_cand, 8, 84);
  lv_obj_set_style_bg_opa(s_kb_cand, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_kb_cand, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_kb_cand, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_kb_cand, LV_OBJ_FLAG_SCROLLABLE);

  s_kb_mode_btn[0] = dm_btn(page, "拼音", "PY", 150, 24, C_FACE, C_EYE,
                            note_kb_mode_cb, (void *)(intptr_t)0);
  lv_obj_set_pos(s_kb_mode_btn[0], 8, 116);
  s_kb_mode_btn[1] = dm_btn(page, "英文", "EN", 150, 24, C_BTN, C_MUTED,
                            note_kb_mode_cb, (void *)(intptr_t)1);
  lv_obj_set_pos(s_kb_mode_btn[1], 162, 116);

  {
    static const char *r0[] = { "q", "w", "e", "r", "t", "y", "u", "i", "o",
                                "p" };
    static const char *r1[] = { "a", "s", "d", "f", "g", "h", "j", "k", "l" };
    static const char *r2[] = { "z", "x", "c", "v", "b", "n", "m" };
    mk_kb_row(page, 146, r0, 10, 28, 8);
    mk_kb_row(page, 176, r1, 9, 28, 22);
    mk_kb_row(page, 206, r2, 7, 28, 36);
  }
  b = dm_btn(page, "删", "Del", 40, 26, 0x333333, C_HEART, note_kb_key_cb,
             "DEL");
  lv_obj_set_pos(b, 232, 206);
  b = dm_btn(page, "空格", "Space", 70, 24, C_BTN, C_DIM, note_kb_key_cb,
             "SP");
  lv_obj_set_pos(b, 8, 206);

  /* Ensure all note pages start hidden — dm_show will reveal one at a time */
  if (g_dm_pages[PAGE_NOTE])
    {
      lv_obj_add_flag(g_dm_pages[PAGE_NOTE], LV_OBJ_FLAG_HIDDEN);
    }
  if (g_dm_pages[PAGE_NOTE_ADD])
    {
      lv_obj_add_flag(g_dm_pages[PAGE_NOTE_ADD], LV_OBJ_FLAG_HIDDEN);
    }
  if (g_dm_pages[PAGE_NOTE_KB])
    {
      lv_obj_add_flag(g_dm_pages[PAGE_NOTE_KB], LV_OBJ_FLAG_HIDDEN);
    }
}

#endif /* CONFIG_DESKMATE_APP */
