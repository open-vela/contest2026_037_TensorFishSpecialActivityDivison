#include "deskmate.h"
#include "dm_ai_log.h"

#ifdef CONFIG_DESKMATE_APP
#include <stdio.h>

static lv_obj_t *s_list;
static lv_obj_t *s_detail;
static lv_obj_t *s_detail_text;
static lv_obj_t *s_summary;
static dm_ai_log_entry_t s_visible[DM_AI_LOG_MAX];
static size_t s_visible_count;
static uint32_t s_seen_revision;
static bool s_in_detail;
static void log_refresh(void);

static void log_back(lv_event_t *event)
{
  (void)event;
  if (s_in_detail)
    {
      s_in_detail = false;
      lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
      log_refresh();
    }
  else dm_show(PAGE_SETTINGS);
}

static void log_detail(lv_event_t *event)
{
  size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
  char text[1536];
  if (index >= s_visible_count) return;
  dm_ai_log_detail(&s_visible[index], text, sizeof(text));
  lv_label_set_text(s_detail_text, text);
  lv_obj_scroll_to_y(s_detail, 0, LV_ANIM_OFF);
  lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(s_summary, "访问对象、执行方式、结果与拦截原因");
  s_in_detail = true;
}

static void log_refresh(void)
{
  bool persisted;
  char text[96];
  size_t i;
  s_visible_count = dm_ai_log_snapshot(s_visible, DM_AI_LOG_MAX,
                                       &s_seen_revision, &persisted);
  lv_obj_clean(s_list);
  if (!persisted)
    lv_label_set_text(s_summary, "记录暂未完整保存，重启后可能缺失");
  else
    {
      snprintf(text, sizeof(text), "最近%u条 · 点击查看详情 · 仅保存在本机",
               (unsigned)s_visible_count);
      lv_label_set_text(s_summary, text);
    }
  if (!s_visible_count)
    {
      lv_obj_t *empty = dm_lbl(s_list, "暂无 AI 操作记录\n使用 AI 本地功能后将在这里显示",
                               "No AI activity yet", g_dm_font_s, MD_ON_SURFACE_VARIANT);
      lv_obj_set_width(empty, 278);
      lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      return;
    }
  for (i = 0; i < s_visible_count; i++)
    {
      const dm_ai_log_entry_t *entry = &s_visible[i];
      uint32_t color = entry->result == DM_AI_LOG_SUCCESS ? C_OK :
                       entry->result == DM_AI_LOG_SKIPPED ? C_MUTED : C_HEART;
      lv_obj_t *row = lv_obj_create(s_list);
      lv_obj_t *name;
      lv_obj_t *result;
      lv_obj_t *when;
      char stamp[48];
      lv_obj_set_size(row, 288, 68);
      lv_obj_set_style_bg_color(row, lv_color_hex(MD_SURFACE_CONTAINER), LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 14, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, log_detail, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
      name = dm_lbl(row, dm_ai_log_action_name(entry->action),
                     dm_ai_log_action_name(entry->action), g_dm_font_s, MD_ON_SURFACE);
      lv_obj_set_width(name, 192);
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      lv_obj_set_pos(name, 0, 0);
      result = dm_lbl(row, dm_ai_log_result_name(entry->result),
                       dm_ai_log_result_name(entry->result), g_dm_font_s, color);
      lv_obj_align(result, LV_ALIGN_TOP_RIGHT, 0, 0);
      dm_ai_log_time(entry, stamp, sizeof(stamp));
      snprintf(text, sizeof(text), "%s  #%lu  ›", stamp, (unsigned long)entry->sequence);
      when = dm_lbl(row, text, text, g_dm_font_s, MD_ON_SURFACE_VARIANT);
      lv_obj_set_pos(when, 0, 26);
      lv_obj_set_width(when, 264);
      lv_label_set_long_mode(when, LV_LABEL_LONG_DOT);
    }
}

void dm_create_ai_log(void)
{
  lv_obj_t *page;
  lv_obj_t *title;
  lv_obj_t *back;
  if (g_dm_pages[PAGE_AI_LOG]) return;
  page = lv_obj_create(g_dm_root);
  g_dm_pages[PAGE_AI_LOG] = page;
  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  back = dm_btn(page, "←", "<", 34, 24, MD_SURFACE_HIGH, MD_ON_SURFACE, log_back, NULL);
  lv_obj_set_pos(back, 8, 8);
  title = dm_lbl(page, "AI操作记录", "AI activity", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(title, 48, 8);
  s_summary = dm_lbl(page, "最近50条 · 点击查看详情", "Latest 50 actions",
                      g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_summary, 10, 40);
  lv_obj_set_width(s_summary, 300);
  lv_label_set_long_mode(s_summary, LV_LABEL_LONG_DOT);
  s_list = lv_obj_create(page);
  lv_obj_set_size(s_list, 304, 164);
  lv_obj_set_pos(s_list, 8, 68);
  lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_list, 8, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  s_detail = lv_obj_create(page);
  lv_obj_set_size(s_detail, 304, 164);
  lv_obj_set_pos(s_detail, 8, 68);
  lv_obj_set_style_bg_color(s_detail, lv_color_hex(MD_SURFACE_CONTAINER), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_detail, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_detail, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_detail, 10, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_detail, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_detail, LV_SCROLLBAR_MODE_AUTO);
  s_detail_text = dm_lbl(s_detail, "", "", g_dm_font_s, MD_ON_SURFACE);
  lv_obj_set_width(s_detail_text, 274);
  lv_label_set_long_mode(s_detail_text, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(s_detail_text, 5, LV_PART_MAIN);
  lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
  log_refresh();
}

void dm_ai_log_tick(void)
{
  if (!s_list || g_dm.page != PAGE_AI_LOG) return;
  if (!s_in_detail && s_seen_revision != dm_ai_log_revision()) log_refresh();
}
#endif
