/****************************************************************************
 * deskmate.h — FlowDesk shared types & API
 ****************************************************************************/

#ifndef DESKMATE_H
#define DESKMATE_H

#include <nuttx/config.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <lvgl/lvgl.h>

#ifdef CONFIG_DESKMATE_APP

#define DM_VER "0.20.1"

#define C_BG     0x000000
#define C_FACE   0xFFFFFF
#define C_EYE    0x000000
#define C_INK    0xFFFFFF
#define C_MUTED  0x666666
#define C_DIM    0x888888
#define C_BTN    0x111111
#define C_BTN_HI 0x222222
#define C_STAR   0xFFD54F
#define C_HEART  0xFF6B8A
#define C_OK     0x6BCB77
#define C_ACCENT 0x7DD3FC
#define C_BLUSH  0xFF8A9A
#define C_TEAR   0x7DD3FC

/* Material Design 3 (dark) palette for Wi-Fi pages */
#define MD_PRIMARY              0xD0BCFF
#define MD_ON_PRIMARY           0x381E72
#define MD_PRIMARY_CONTAINER    0x4F378B
#define MD_ON_PRIMARY_CONTAINER 0xEADDFF
#define MD_SECONDARY_CONTAINER  0x4A4458
#define MD_ON_SECONDARY_CONT    0xE8DEF8
#define MD_SURFACE              0x141218
#define MD_SURFACE_CONTAINER    0x211F26
#define MD_SURFACE_HIGH         0x2B2930
#define MD_SURFACE_HIGHEST      0x36343B
#define MD_ON_SURFACE           0xE6E0E9
#define MD_ON_SURFACE_VARIANT   0xCAC4D0
#define MD_OUTLINE              0x938F99
#define MD_ERROR                0xF2B8B5

#define DM_SCR_W 320
#define DM_SCR_H 240
#define DM_TICK_MS 250
#define DM_TICKS_PER_SEC (1000 / DM_TICK_MS)

typedef enum {
  PAGE_FOCUS_HOME = 0,
  PAGE_FOCUS_RUN,
  PAGE_FOCUS_DONE,
  PAGE_CHAT,
  PAGE_HEALTH,
  PAGE_MOOD_LOG,
  PAGE_FEATURES,
  PAGE_SUPERVISE,
  PAGE_NOTE,
  PAGE_NOTE_ADD,
  PAGE_NOTE_KB,
  PAGE_SETTINGS,
  PAGE_TIME_SET,
  PAGE_WIFI,
  PAGE_APP_CONNECT,
  PAGE_WIFI_PW,
  PAGE_WIFI_KB,
  PAGE_WIFI_CONN,
  PAGE_WIFI_DONE,
  PAGE_WORD_HOME,
  PAGE_WORD_STUDY,
  PAGE_WORD_RES,
  PAGE_WORD_QMODE,
  PAGE_WORD_QUIZ,
  PAGE_WORD_WRONG,
  PAGE_WORD_LIST,
  PAGE_WORD_WSET,
  PAGE_CALC,
  PAGE_SENSORS,
  PAGE_GUESS,
  PAGE_CONVERT,
  PAGE_WATER,
  PAGE_COUNTDOWN,
  PAGE_EAT,
  PAGE_24,
  PAGE_DRAW,
  PAGE_BMI,
  PAGE_MED,
  PAGE_MED_ADD,
  PAGE_MED_KB,
  PAGE_2048,
  PAGE_MBTI,
  PAGE_MBTI_Q,
  PAGE_MBTI_RESULT,
  PAGE_DICT,
  PAGE_DICT_SEARCH,
  PAGE_DICT_DETAIL,
  PAGE_VOLUME,
  PAGE_VOICE_SEL,
  PAGE_ABOUT,
  PAGE_RECORDER,
  PAGE_TODO,
  PAGE_ALARM,
  PAGE_ALARM_ADD,
  PAGE_MEETING,
  PAGE_MEETING_DETAIL,
  PAGE_PERMISSIONS,
  PAGE_AI_LOG,
  PAGE_PERMISSION_DETAIL,
  PAGE_COUNT
} dm_page_t;

typedef enum {
  EYE_DECOR_NONE = 0,
  EYE_DECOR_STAR,
  EYE_DECOR_HEART,
  EYE_DECOR_WIDE,
} dm_eye_decor_t;

typedef enum {
  FACE_IDLE = 0,
  FACE_FOCUS,
  FACE_HAPPY,
  FACE_LOVE,
  FACE_SURPRISE,
  FACE_DONE,
  FACE_WINK,
  FACE_SHY,
  FACE_SLEEPY,
  FACE_THINK,
  FACE_CRY,
  FACE_LAUGH,
  FACE_ANGRY,
} dm_face_t;

typedef struct {
  dm_page_t page;
  dm_face_t face;
  dm_eye_decor_t decor;
  int32_t focus_left;
  int32_t focus_total;
  int32_t focus_sel_min;
  int32_t focus_custom_min;
  bool focus_run;
  bool focus_finished;
  bool zh;
  int decor_hold;
  int look_hold;
  int look_x;
  int look_y;
  int wide_hold;
  int32_t focus_done_min; /* today completed focus minutes */
} dm_ctx_t;

/* mood bits */
#define DM_M_JOY      (1u << 0)
#define DM_M_LOVE     (1u << 1)
#define DM_M_EXCITE   (1u << 2)
#define DM_M_CONFIDENT (1u << 3)
#define DM_M_EXPECT   (1u << 4)
#define DM_M_SATISFY  (1u << 5)
#define DM_M_TOUCHED  (1u << 6)
#define DM_M_CALM     (1u << 7)
#define DM_M_THINK    (1u << 8)
#define DM_M_SURPRISE (1u << 9)
#define DM_M_SAD      (1u << 10)
#define DM_M_ANGRY    (1u << 11)
#define DM_M_TIRED    (1u << 12)
#define DM_M_ANXIOUS  (1u << 13)
#define DM_M_LONELY   (1u << 14)
#define DM_M_STRESS   (1u << 15)

#define DM_MOOD_REC_MAX 8
#define DM_QUICK_MAX 8

typedef struct {
  uint16_t mood_mask;
  uint8_t quick;    /* 0 none, 1..DM_QUICK_MAX */
  uint8_t rec_type; /* 0 current, 1 daily */
  uint8_t hour;
  uint8_t min;
} dm_mood_rec_t;

extern dm_ctx_t g_dm;
extern const lv_font_t *g_dm_font_s;
extern const lv_font_t *g_dm_font_m;
extern const lv_font_t *g_dm_font_l;
extern const lv_font_t *g_dm_font_xl;

extern lv_obj_t *g_dm_root;
extern lv_obj_t *g_dm_pages[PAGE_COUNT];
extern lv_obj_t *g_dm_dock;
extern lv_obj_t *g_dm_face;
extern lv_obj_t *g_dm_bubble;
extern lv_obj_t *g_dm_eye_l;
extern lv_obj_t *g_dm_eye_r;
extern lv_obj_t *g_dm_mouth;
extern lv_obj_t *g_dm_decor_l;
extern lv_obj_t *g_dm_decor_r;
extern lv_obj_t *g_dm_blush_l;
extern lv_obj_t *g_dm_blush_r;

/* roll clock */
typedef struct {
  lv_obj_t *clip;
  lv_obj_t *cur;
  int digit;
} dm_digit_t;

typedef struct {
  lv_obj_t *box;
  lv_obj_t *colon;
  lv_obj_t *wide;
  dm_digit_t d[4];
} dm_clock_t;

const char *dm_t(const char *zh, const char *en);
void dm_style(lv_obj_t *o, const lv_font_t *f, uint32_t c);
lv_obj_t *dm_lbl(lv_obj_t *p, const char *zh, const char *en,
                 const lv_font_t *f, uint32_t c);
lv_obj_t *dm_btn(lv_obj_t *p, const char *zh, const char *en, int w, int h,
                 uint32_t bg, uint32_t fg, lv_event_cb_t cb, void *ud);
void dm_say(const char *zh, const char *en);
void dm_init_fonts(void);
void dm_show(dm_page_t p);
void dm_tick(void);

/* face */
void dm_face_build(lv_obj_t *parent, int x, int y, int size);
void dm_face_attach(lv_obj_t *page, int x, int y);
void dm_face_set(dm_face_t f, dm_eye_decor_t decor, int decor_hold);
void dm_face_tick(void);
void dm_face_on_click(lv_event_t *e);
void dm_face_repaint(void);
void dm_word_face_click(void);

/* pages */
void dm_create_focus_home(void);
void dm_create_focus_run(void);
void dm_create_focus_done(void);
void dm_focus_show_done(int32_t minutes);
void dm_create_chat(void);
void dm_create_health(void);
void dm_create_2048(void);
void dm_create_mbti(void);
void dm_create_dict(void);
void dm_create_volume(void);
void dm_create_voice_sel(void);
void dm_voice_sel_tick(void);
void dm_create_about(void);
void dm_create_recorder(void);
void dm_recorder_tick(void);
void dm_create_reminders(void);
void dm_reminder_tick(void);
void dm_create_meeting(void);
void dm_meeting_tick(void);
int dm_todo_add_voice(const char *name, int delay_seconds);
int dm_todo_add_at_voice(const char *name, int year, int month, int day,
                         int hour, int minute);
int dm_alarm_add_voice(const char *name, int year, int month, int day,
                       int hour, int minute, int repeat_daily);
int dm_alarm_add_after_voice(const char *name, int delay_seconds);
void dm_voice_profile_init(void);
void dm_proactive_tick(void);
void dm_proactive_focus_done(int32_t minutes);
int dm_dict_ai_define(const char *word, char *out, int cap);
void dm_chat_tick(void);
void dm_assistant_intent_tick(void);
/* Positive: 1=local, 2=wake, 3=health, 4=life records (no history/TTS),
 * 5=deterministic health query (no history; TTS requires health read access). */
int dm_assistant_intent_try(const char *text, char *reply, int reply_cap);
void dm_volume_tick(void);
int dm_chat_ai_available(void);
const char *dm_chat_ai_ask(const char *user_text);
int dm_chat_ai_remote_ok(void);
int dm_chat_ai_intent_ok(void);
const char *dm_chat_local_reply(const char *user_text);
void dm_chat_hist_reset(void);
void dm_chat_hist_push(const char *user, const char *assistant);
void dm_chat_hist_push_health(const char *user, const char *assistant);
int dm_chat_ai_health_reply(void);
int dm_chat_ai_local_reply(void);
int dm_chat_hist_json(char *out, int cap);
int dm_chat_hist_json_for_cloud(char *out, int cap, bool allow_health);
void dm_create_supervise(void);
void dm_create_dock(void);
void dm_dock_highlight(dm_page_t p);
void dm_focus_home_tick(void);
void dm_focus_run_tick(void);
void dm_focus_start(void);
int dm_focus_start_minutes(int32_t minutes);
int dm_focus_start_seconds(int32_t seconds);
void dm_focus_end(void);
void dm_focus_toggle_pause(void);
void dm_update_run_clock(void);

/* health */
void dm_health_add_focus_min(int32_t min);
void dm_health_flush_game(void);
void dm_health_add_game_ms(int ms);
int dm_water_today_cups(void);
int dm_med_today_stats(int *sched_out, int *taken_out);
int dm_word_today_n(void);
int dm_sensor_last_th(float *t_c, float *h_pct);
void dm_health_refresh(void);
int dm_health_record_mood_voice(const char *text);
int dm_health_score(void);
void dm_health_voice_advice(const char *user_text, char *out, int cap);
int dm_countdown_add_voice(const char *name, int year, int month, int day,
                           int hour, int minute);
int dm_note_add_voice(const char *text);

/* settings / wifi */
void dm_create_settings(void);
void dm_create_permissions(void);
void dm_create_ai_log(void);
void dm_ai_log_tick(void);
void dm_create_features(void);
void dm_create_wifi(void);
void dm_create_app_connect(void);
void dm_create_word(void);
void dm_create_tools(void);
void dm_create_life(void);
void dm_create_fun(void);
void dm_create_med(void);
void dm_create_note(void);
void dm_med_kb_open(void);
void dm_tools_tick(void);
void dm_life_tick(void);
void dm_fun_tick(void);
void dm_med_tick(void);
int dm_med_voice_has_name(const char *text);
int dm_med_voice_take(const char *utterance, char *reply, int reply_cap);
void dm_wifi_tick(void);
void dm_appconn_tick(void);
int dm_wifi_start_scan(void);
int dm_wifi_connect(const char *ssid, const char *pass);
void dm_wifi_auto_start(void);
const char *dm_wifi_cur_ssid(void);
const char *dm_wifi_cur_ip(void);
int dm_wifi_connected(void);
int dm_wifi_wait_time_sync(int timeout_sec);
int dm_wifi_enabled(void);
int dm_wifi_clock_valid(void);
int dm_wifi_set_manual_time(time_t epoch);
void dm_wifi_manual_time_restore(void);
int dm_wifi_set_enabled(int on);
void dm_wifi_poll(void);
int dm_wifi_link_lost(void);
void dm_wifi_clear_link_lost(void);
int dm_wifi_ap_count(void);
int dm_wifi_ap_get(int i, const char **ssid, int *rssi, bool *open);
int dm_wifi_sel(void);
void dm_wifi_set_sel(int i);
int dm_wifi_scan_busy(void);
int dm_wifi_scan_ready(void);
void dm_wifi_clear_scan_ready(void);
int dm_wifi_conn_busy(void);
int dm_wifi_conn_ok(void);
int dm_wifi_conn_fail(void);
void dm_wifi_clear_conn_flags(void);

/* roll */
void dm_clock_create(dm_clock_t *c, lv_obj_t *parent, int x, int y, int w,
                     int h, const lv_font_t *font);
void dm_clock_set(dm_clock_t *c, int32_t seconds_left);
void dm_clock_refresh_now(dm_clock_t *c);

#endif
#endif /* DESKMATE_H */
