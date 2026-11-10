#include "deskmate.h"
#include "dm_ai_permissions.h"

#ifdef CONFIG_DESKMATE_APP

#include <stdio.h>

static lv_obj_t *s_status;
static bool s_switch_sync;
static lv_obj_t *s_detail_name;
static lv_obj_t *s_detail_state;
static lv_obj_t *s_detail_body;
static lv_obj_t *s_detail_scroll;

static const char *const s_details[DM_AI_PERMISSION_COUNT] = {
  "总开关，控制下方所有 AI 本地操作",
  "允许 AI 跳转页面；关闭会拦截需跳转的操作",
  "允许 AI 启动专注倒计时",
  "允许 AI 添加定时待办",
  "允许 AI 创建单次或重复闹钟",
  "允许 AI 写入本地备忘",
  "允许 AI 添加指定日期的倒数日",
  "允许 AI 打开吃药页、记录已服药品",
  "允许 AI 保存心情、打开心情记录页",
  "允许 AI 打开 2048 游戏",
  "允许 AI 打开猜数字游戏",
  "允许 AI 打开 24 点游戏",
  "允许 AI 打开 MBTI 测试",
  "允许本地问候、情绪建议与游戏选择对话",
  "喝水杯数、最近心情与时间，以及健康统计",
  "备忘、待办、闹钟、倒数日（仅本地读取）",
  "删除后打开对应页，需允许生活数据读取和跳页",
  "删除后打开对应页，需允许健康读取和跳页"
};

static void back_to_permissions(lv_event_t *event)
{
  (void)event;
  dm_show(PAGE_PERMISSIONS);
}

static void open_permission_detail(lv_event_t *event)
{
  dm_ai_permission_t p = (dm_ai_permission_t)(intptr_t)lv_event_get_user_data(event);
  if (p < 0 || p >= DM_AI_PERMISSION_COUNT) return;
  lv_label_set_text(s_detail_name, dm_ai_permission_name(p));
  lv_label_set_text(s_detail_state, !dm_ai_permission_enabled(p) ? "当前开关：已关闭" :
                    p != DM_AI_LOCAL && !dm_ai_permission_enabled(DM_AI_LOCAL)
                      ? "开关已开启，但 AI 本地操作总开关关闭" :
                    p == DM_AI_DELETE_LOCAL_DATA && !dm_ai_permission_allowed(DM_AI_READ_LOCAL_DATA)
                      ? "开关已开启，但本地生活数据读取已关闭" :
                    p == DM_AI_DELETE_HEALTH && !dm_ai_permission_allowed(DM_AI_READ_HEALTH)
                      ? "开关已开启，但健康记录读取已关闭" :
                    (p == DM_AI_DELETE_LOCAL_DATA || p == DM_AI_DELETE_HEALTH) &&
                    !dm_ai_permission_allowed(DM_AI_OPEN_APPS)
                      ? "开关已开启，但打开应用/页面权限关闭" : "当前开关：已开启");
  lv_label_set_text(s_detail_body, dm_ai_permission_explanation(p));
  lv_obj_scroll_to_y(s_detail_scroll, 0, LV_ANIM_OFF);
  dm_show(PAGE_PERMISSION_DETAIL);
}

static void create_permission_detail(void)
{
  lv_obj_t *page = lv_obj_create(g_dm_root);
  lv_obj_t *back;
  g_dm_pages[PAGE_PERMISSION_DETAIL] = page;
  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  back = dm_btn(page, "←", "<", 34, 24, MD_SURFACE_HIGH, MD_ON_SURFACE,
                back_to_permissions, NULL);
  lv_obj_set_pos(back, 8, 8);
  s_detail_name = dm_lbl(page, "权限说明", "Permission details", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(s_detail_name, 48, 8);
  lv_obj_set_width(s_detail_name, 264);
  lv_label_set_long_mode(s_detail_name, LV_LABEL_LONG_DOT);
  s_detail_state = dm_lbl(page, "", "", g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_pos(s_detail_state, 10, 40);
  lv_obj_set_width(s_detail_state, 300);
  lv_label_set_long_mode(s_detail_state, LV_LABEL_LONG_DOT);
  s_detail_scroll = lv_obj_create(page);
  lv_obj_set_size(s_detail_scroll, 304, 164);
  lv_obj_set_pos(s_detail_scroll, 8, 68);
  lv_obj_set_style_bg_color(s_detail_scroll, lv_color_hex(MD_SURFACE_CONTAINER), LV_PART_MAIN);
  lv_obj_set_style_border_width(s_detail_scroll, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_detail_scroll, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_detail_scroll, 10, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_detail_scroll, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_detail_scroll, LV_SCROLLBAR_MODE_AUTO);
  s_detail_body = dm_lbl(s_detail_scroll, "", "", g_dm_font_s, MD_ON_SURFACE);
  lv_obj_set_width(s_detail_body, 274);
  lv_label_set_long_mode(s_detail_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(s_detail_body, 5, LV_PART_MAIN);
}

static void back_to_settings(lv_event_t *event)
{
  (void)event;
  dm_show(PAGE_SETTINGS);
}

static void permission_changed(lv_event_t *event)
{
  dm_ai_permission_t permission;
  lv_obj_t *toggle;
  bool enabled;

  if (s_switch_sync) return;
  permission = (dm_ai_permission_t)(intptr_t)lv_event_get_user_data(event);
  toggle = lv_event_get_target(event);
  enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);
  if (dm_ai_permission_set(permission, enabled) != 0)
    {
      s_switch_sync = true;
      if (dm_ai_permission_enabled(permission))
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
      else
        lv_obj_clear_state(toggle, LV_STATE_CHECKED);
      s_switch_sync = false;
      lv_label_set_text(s_status, "保存失败，开关已恢复，请稍后重试");
      return;
    }
  lv_label_set_text(s_status,
                    dm_ai_permission_enabled(DM_AI_LOCAL)
                      ? "已保存，关闭的项目会在执行前被拦截"
                      : "总开关已关闭，下方各项暂不生效");
}

void dm_create_permissions(void)
{
  lv_obj_t *page;
  lv_obj_t *back;
  lv_obj_t *title;
  lv_obj_t *list;
  int i;

  if (g_dm_pages[PAGE_PERMISSIONS]) return;
  page = lv_obj_create(g_dm_root);
  g_dm_pages[PAGE_PERMISSIONS] = page;
  lv_obj_set_size(page, DM_SCR_W, DM_SCR_H);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(MD_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(page, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page, 0, LV_PART_MAIN);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

  back = dm_btn(page, "←", "<", 34, 24, MD_SURFACE_HIGH, MD_ON_SURFACE,
                back_to_settings, NULL);
  lv_obj_set_pos(back, 8, 8);
  title = dm_lbl(page, "权限管理", "AI permissions", g_dm_font_m, MD_ON_SURFACE);
  lv_obj_set_pos(title, 48, 8);
  s_status = dm_lbl(page, "默认开启；点项目查看权限详细说明",
                    "Enabled by default; changes are saved",
                    g_dm_font_s, MD_ON_SURFACE_VARIANT);
  lv_obj_set_width(s_status, 300);
  lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(s_status, 10, 38);
  if (!dm_ai_permission_enabled(DM_AI_LOCAL))
    lv_label_set_text(s_status, "总开关已关闭，下方各项暂不生效");

  list = lv_obj_create(page);
  lv_obj_set_size(list, 304, 158);
  lv_obj_set_pos(list, 8, 74);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

  for (i = 0; i < DM_AI_PERMISSION_COUNT; i++)
    {
      lv_obj_t *row = lv_obj_create(list);
      lv_obj_t *name;
      lv_obj_t *detail;
      lv_obj_t *toggle;
      lv_obj_set_size(row, 288, 76);
      lv_obj_set_style_bg_color(row, lv_color_hex(MD_SURFACE_CONTAINER), LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_radius(row, 14, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 10, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(row, open_permission_detail, LV_EVENT_CLICKED,
                          (void *)(intptr_t)i);
      name = dm_lbl(row, dm_ai_permission_name(i), dm_ai_permission_name(i),
                    g_dm_font_s, MD_ON_SURFACE);
      lv_obj_set_width(name, 200);
      lv_obj_set_pos(name, 0, 0);
      detail = dm_lbl(row, s_details[i], s_details[i], g_dm_font_s,
                      MD_ON_SURFACE_VARIANT);
      lv_obj_set_width(detail, 208);
      lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
      lv_obj_set_pos(detail, 0, 22);
      toggle = lv_switch_create(row);
      lv_obj_set_size(toggle, 44, 24);
      lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, 0, 0);
      lv_obj_set_style_bg_color(toggle, lv_color_hex(MD_SURFACE_HIGHEST), LV_PART_MAIN);
      lv_obj_set_style_bg_color(toggle, lv_color_hex(MD_PRIMARY),
                                LV_PART_INDICATOR | LV_STATE_CHECKED);
      lv_obj_set_style_bg_color(toggle, lv_color_hex(MD_ON_SURFACE), LV_PART_KNOB);
      lv_obj_set_style_bg_color(toggle, lv_color_hex(MD_ON_PRIMARY),
                                LV_PART_KNOB | LV_STATE_CHECKED);
      if (dm_ai_permission_enabled(i)) lv_obj_add_state(toggle, LV_STATE_CHECKED);
      lv_obj_add_event_cb(toggle, permission_changed, LV_EVENT_VALUE_CHANGED,
                          (void *)(intptr_t)i);
    }
  create_permission_detail();
}

#endif
