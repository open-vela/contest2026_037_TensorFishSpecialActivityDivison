/****************************************************************************
 * dm_ui_health.c — health page + full-page mood log + file storage
 *
 * Flow: Health → 「记录心情」全页 → 保存写入 /data → 返回健康页刷新
 ****************************************************************************/

#include "deskmate.h"
#include "dm_ai_permissions.h"
#include "dm_ai_log.h"
#include "dm_ai_data.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <math.h>
#include <unistd.h>

#ifdef CONFIG_DESKMATE_APP

#define DM_MOOD_PATH "/data/deskmate_mood.bin"
#define DM_MOOD_TMP_PATH "/data/deskmate_mood.bin.tmp"
#define DM_MOOD_MAGIC 0x4d4f4f45u /* 'MOOD' v2: + yday for daily reset */

#define M_NEG (DM_M_SAD | DM_M_ANGRY | DM_M_TIRED | DM_M_ANXIOUS | \
               DM_M_LONELY | DM_M_STRESS)

typedef struct
{
  const char *zh;
  const char *en;
  uint16_t bit;
  int score;
} mood_item_t;

static const mood_item_t s_moods[] = {
  { "开心", "Joy", DM_M_JOY, 85 },
  { "喜爱", "Love", DM_M_LOVE, 82 },
  { "兴奋", "Excite", DM_M_EXCITE, 90 },
  { "自信", "Confident", DM_M_CONFIDENT, 88 },
  { "期待", "Expect", DM_M_EXPECT, 80 },
  { "满足", "Satisfy", DM_M_SATISFY, 75 },
  { "感动", "Touched", DM_M_TOUCHED, 80 },
  { "平静", "Calm", DM_M_CALM, 70 },
  { "思考", "Think", DM_M_THINK, 55 },
  { "惊讶", "Surprise", DM_M_SURPRISE, 60 },
  { "难过", "Sad", DM_M_SAD, 25 },
  { "生气", "Angry", DM_M_ANGRY, 15 },
  { "疲惫", "Tired", DM_M_TIRED, 35 },
  { "焦虑", "Anxious", DM_M_ANXIOUS, 20 },
  { "孤独", "Lonely", DM_M_LONELY, 30 },
  { "压力", "Stress", DM_M_STRESS, 20 },
};

static const char *s_quick_zh[DM_QUICK_MAX] = {
  "今天心情不错", "感觉一般般", "有点不开心", "很累想休息",
  "充满活力", "心平气和", "坐立不安", "若有所思",
};
static const char *s_quick_en[DM_QUICK_MAX] = {
  "Pretty good", "Just okay", "A bit down", "Tired",
  "Energetic", "Calm", "Restless", "Pensive",
};
static const int s_quick_score[DM_QUICK_MAX] = {
  78, 50, 32, 38, 88, 75, 35, 65,
};

static dm_mood_rec_t s_recs[DM_MOOD_REC_MAX];
static int s_rec_n;

static lv_obj_t *s_page;
static lv_obj_t *s_sc;
static lv_obj_t *s_score_lbl;
static lv_obj_t *s_tag_lbl;
static lv_obj_t *s_msg_lbl;
static lv_obj_t *s_focus_lbl;
static lv_obj_t *s_list_box;
static lv_obj_t *s_fab;

static lv_obj_t *s_log_page;
static lv_obj_t *s_mood_btns[16];
static lv_obj_t *s_quick_btns[DM_QUICK_MAX];
static lv_obj_t *s_seg_cur;
static lv_obj_t *s_seg_day;
static uint16_t s_pick_mask;
static uint8_t s_pick_quick;
static uint8_t s_pick_type;

static void paint_log_picks(void);
static int voice_mood_term_negated(const char *text, const char *found);

/* ---------- storage ---------- */

typedef struct
{
  uint32_t magic;
  uint32_t count;
  int32_t focus_done_min;
  int32_t yday; /* tm_yday + tm_year*1000 — day of this snapshot */
  int32_t game_ms; /* time on 2048 today */
  dm_mood_rec_t recs[DM_MOOD_REC_MAX];
} dm_mood_file_t;

static int32_t s_game_ms;
static int32_t s_game_saved_ms;

static int health_today_key(void)
{
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  return tmv.tm_yday + tmv.tm_year * 1000;
}

static int s_health_day_key = -1;

static int store_save(void)
{
  dm_mood_file_t f;
  size_t written = 0;
  int fd;

  if (!dm_wifi_clock_valid()) return -1;
  memset(&f, 0, sizeof(f));
  f.magic = DM_MOOD_MAGIC;
  f.count = (uint32_t)s_rec_n;
  f.focus_done_min = g_dm.focus_done_min;
  f.yday = health_today_key();
  f.game_ms = s_game_ms;
  if (s_rec_n > 0)
    {
      memcpy(f.recs, s_recs, sizeof(dm_mood_rec_t) * (size_t)s_rec_n);
    }

  fd = open(DM_MOOD_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      return -1;
    }
  while (written < sizeof(f))
    {
      ssize_t n = write(fd, (const char *)&f + written, sizeof(f) - written);
      if (n <= 0)
        {
          close(fd);
          unlink(DM_MOOD_TMP_PATH);
          return -1;
        }
      written += (size_t)n;
    }
  if (close(fd) != 0 || rename(DM_MOOD_TMP_PATH, DM_MOOD_PATH) != 0)
    {
      unlink(DM_MOOD_TMP_PATH);
      return -1;
    }
  s_game_saved_ms = s_game_ms;
  return 0;
}

static void health_rollover_if_new_day(void)
{
  int key = health_today_key();
  if (!dm_wifi_clock_valid()) return;
  if (s_health_day_key < 0)
    {
      s_health_day_key = key;
      return;
    }
  if (s_health_day_key == key)
    {
      return;
    }
  /* new calendar day: clear today-scoped stats */
  s_rec_n = 0;
  s_game_saved_ms = 0;
  g_dm.focus_done_min = 0;
  s_game_ms = 0;
  s_health_day_key = key;
  store_save();
}

static void store_load(void)
{
  dm_mood_file_t f;
  int fd;
  ssize_t n;
  int key = health_today_key();

  fd = open(DM_MOOD_PATH, O_RDONLY);
  if (fd < 0)
    {
      s_health_day_key = key;
      return;
    }
  n = read(fd, &f, sizeof(f));
  close(fd);

  if (n != (ssize_t)sizeof(f) || f.magic != DM_MOOD_MAGIC)
    {
      /* old/invalid file: start clean today */
      s_rec_n = 0;
      g_dm.focus_done_min = 0;
      s_health_day_key = key;
      return;
    }

  if (dm_wifi_clock_valid() && f.yday != key)
    {
      /* data from previous day — do not leak into today's score */
      s_rec_n = 0;
      g_dm.focus_done_min = 0;
      s_health_day_key = key;
      store_save();
      return;
    }

  if (f.count > DM_MOOD_REC_MAX)
    {
      f.count = DM_MOOD_REC_MAX;
    }
  s_rec_n = (int)f.count;
  if (s_rec_n > 0)
    {
      memcpy(s_recs, f.recs, sizeof(dm_mood_rec_t) * (size_t)s_rec_n);
    }
  g_dm.focus_done_min = f.focus_done_min;
  s_game_ms = f.game_ms;
  s_game_saved_ms = s_game_ms;
  s_health_day_key = f.yday;
}

/* ---------- score ---------- */

static int mood_mask_score(uint16_t mask)
{
  int sum = 0;
  int n = 0;
  int i;

  if (mask == 0)
    {
      return -1;
    }
  for (i = 0; i < (int)(sizeof(s_moods) / sizeof(s_moods[0])); i++)
    {
      if (mask & s_moods[i].bit)
        {
          sum += s_moods[i].score;
          n++;
        }
    }
  return n ? (sum / n) : -1;
}

/* Daily focus → 0-100 subscore. 0 min should not look "wavy-good".
 * 0→20, 15→~36, 30→~52, 45→~68, 60+→95 */
/* ===== multi-factor mood score (v1 board model) ===== */

#define MF_WORDS_TARGET 25.0f
#define MF_FOCUS_TARGET 150.0f
#define MF_WATER_TARGET 8.0f

typedef struct
{
  const char *key;
  float w;
  float s;      /* 0..1 */
  int valid;    /* 1 if present */
} mf_item_t;

static float clampf(float v, float lo, float hi)
{
  if (v < lo)
    {
      return lo;
    }
  if (v > hi)
    {
      return hi;
    }
  return v;
}

static float mf_trapezoid(float x, float lo, float opt_lo, float opt_hi,
                          float hi)
{
  if (x < lo || x > hi)
    {
      return 0.0f;
    }
  if (x < opt_lo)
    {
      return (x - lo) / (opt_lo - lo);
    }
  if (x <= opt_hi)
    {
      return 1.0f;
    }
  return (hi - x) / (hi - opt_hi);
}

static float mf_env_score(float t, float h, int has_t, int has_h)
{
  float st = 0.0f;
  float sh = 0.0f;
  int nt = 0;
  int nh = 0;

  if (has_t)
    {
      st = mf_trapezoid(t, 10.0f, 20.0f, 26.0f, 35.0f);
      nt = 1;
    }
  if (has_h)
    {
      sh = mf_trapezoid(h, 20.0f, 40.0f, 60.0f, 85.0f);
      nh = 1;
    }
  if (nt && nh)
    {
      return 0.6f * st + 0.4f * sh;
    }
  if (nt)
    {
      return st;
    }
  if (nh)
    {
      return sh;
    }
  return -1.0f;
}

static float mf_game_score(int game_ms)
{
  float g = (float)game_ms / 1000.0f / 60.0f; /* minutes */

  if (game_ms < 0)
    {
      return -1.0f; /* missing */
    }
  if (g <= 0.0f)
    {
      return 0.55f; /* neutral if tracked as 0 */
    }
  if (g <= 30.0f)
    {
      return 0.55f + 0.45f * (g / 30.0f);
    }
  if (g <= 60.0f)
    {
      return 1.0f;
    }
  if (g <= 120.0f)
    {
      return 1.0f - 0.45f * ((g - 60.0f) / 60.0f);
    }
  if (g <= 180.0f)
    {
      return 0.55f - 0.35f * ((g - 120.0f) / 60.0f);
    }
  {
    float v = 0.20f - 0.15f * ((g - 180.0f) / 60.0f);
    return v < 0.05f ? 0.05f : v;
  }
}

static int health_file_water_cups(void)
{
  /* prefer live UI state via getter when linked */
  return dm_water_today_cups();
}

static void health_collect_mf(mf_item_t *out, int *n_out, int *cov100,
                              int *n_factors)
{
  mf_item_t items[8];
  int n = 0;
  float t = 0.0f;
  float h = 0.0f;
  int has_env;
  int words_n;
  int sched = 0;
  int taken = 0;
  int water;
  float ms = -1.0f;
  float fscore;
  float es;
  int i;
  float wsum = 0.0f;
  float acc = 0.0f;
  int nf = 0;

  health_rollover_if_new_day();

  has_env = (dm_sensor_last_th(&t, &h) == 0) ? 1 : 0;
  es = mf_env_score(t, h, has_env, has_env);
  words_n = dm_word_today_n();
  water = health_file_water_cups();
  (void)dm_med_today_stats(&sched, &taken);

  /* env */
  items[n].key = "env";
  items[n].w = 0.15f;
  items[n].valid = (es >= 0.0f) ? 1 : 0;
  items[n].s = (es >= 0.0f) ? es : 0.0f;
  n++;

  /* words — if module used today (today_learn may be 0 after day rollover) */
  items[n].key = "words";
  items[n].w = 0.15f;
  items[n].valid = 1; /* 0 words is valid low score once word app opened? treat 0 as valid neutral-low */
  items[n].s = clampf((float)words_n / MF_WORDS_TARGET, 0.0f, 1.0f);
  /* if never opened word store today_learn=0 — still valid 0 contribution after renormalize only if we mark valid.
     Spec: 0 valid vs null missing. We treat unopened as 0 valid so learning is rewarded when used. */
  n++;

  /* focus */
  fscore = (g_dm.focus_done_min <= 0)
               ? 0.0f
               : clampf((float)g_dm.focus_done_min / MF_FOCUS_TARGET, 0.0f, 1.0f);
  items[n].key = "focus";
  items[n].w = 0.25f;
  items[n].valid = 1;
  items[n].s = fscore;
  n++;

  /* med — missing if no schedule */
  items[n].key = "med";
  items[n].w = 0.20f;
  if (sched > 0)
    {
      items[n].valid = 1;
      items[n].s = clampf((float)taken / (float)sched, 0.0f, 1.0f);
    }
  else
    {
      items[n].valid = 0;
      items[n].s = 0.0f;
    }
  n++;

  /* water */
  items[n].key = "water";
  items[n].w = 0.15f;
  items[n].valid = 1;
  items[n].s = clampf((float)water / MF_WATER_TARGET, 0.0f, 1.0f);
  n++;

  /* game — track when 2048 used; 0ms still neutral if health engine always runs.
     If s_game_ms==0 and never visited 2048, score as neutral 0.55 valid. */
  items[n].key = "game";
  items[n].w = 0.10f;
  items[n].valid = 1;
  items[n].s = mf_game_score(s_game_ms);
  n++;

  /* mood (tags/quick) — optional factor */
  {
    int mood_sum = 0;
    int mood_n = 0;
    for (i = 0; i < s_rec_n; i++)
      {
        int one = mood_mask_score(s_recs[i].mood_mask);
        if (one < 0 && s_recs[i].quick > 0 && s_recs[i].quick <= DM_QUICK_MAX)
          {
            one = s_quick_score[s_recs[i].quick - 1];
          }
        else if (one >= 0 && s_recs[i].quick > 0 &&
                 s_recs[i].quick <= DM_QUICK_MAX)
          {
            one = (one + s_quick_score[s_recs[i].quick - 1] + 1) / 2;
          }
        if (one >= 0)
          {
            mood_sum += one;
            mood_n++;
          }
      }
    items[n].key = "mood";
    items[n].w = 0.14f;
    if (mood_n > 0)
      {
        float mv = (float)((mood_sum + mood_n / 2) / mood_n) / 100.0f;
        items[n].valid = 1;
        items[n].s = clampf(mv, 0.0f, 1.0f);
        ms = items[n].s;
      }
    else
      {
        items[n].valid = 0;
        items[n].s = 0.0f;
      }
    n++;
  }
  (void)ms;

  /* renormalize */
  for (i = 0; i < n; i++)
    {
      if (!items[i].valid)
        {
          continue;
        }
      acc += items[i].w * items[i].s;
      wsum += items[i].w;
      nf++;
    }

  *n_out = n;
  *n_factors = nf;
  *cov100 = (int)(wsum * 100.0f + 0.5f);
  if (wsum <= 0.0001f)
    {
      out[0].s = 0.5f; /* unused */
      out[0].valid = 0;
      out[0].key = "none";
      *n_out = 0;
      return;
    }
  /* store final normalized 0..1 in out[0].s via dedicated fields — use out array copy */
  for (i = 0; i < n; i++)
    {
      out[i] = items[i];
    }
  out[0].valid = 2; /* sentinel: out[0].s not used for total */
  /* encode total in cov unused — return via static */
  {
    /* place total in items — health_score will recompute using same logic for simplicity */
  }
  (void)acc;
}

/* ===== mood score ported from 静心轨迹 / score-detail.ux ===== */

#define M_NEG_MASK (DM_M_SAD | DM_M_ANGRY | DM_M_TIRED | DM_M_ANXIOUS | \
                    DM_M_LONELY | DM_M_STRESS)
#define M_POS_MASK (DM_M_JOY | DM_M_LOVE | DM_M_EXCITE | DM_M_CONFIDENT | \
                    DM_M_EXPECT | DM_M_SATISFY | DM_M_TOUCHED | DM_M_CALM)

static int s_last_cov;
static int s_last_nf;
static int s_last_mood_n;
static int s_last_mood_score;
static int s_last_behavior;
static int s_last_has_mood;
static int s_last_has_beh;

/* per-record mood score — same as calculateScore in score-detail.ux */
static float mood_record_score(const dm_mood_rec_t *rec)
{
  float base = 50.0f;
  int pos = 0;
  int neg = 0;
  int i;
  int ntag = 0;
  float tsum = 0.0f;

  /* type average */
  for (i = 0; i < (int)(sizeof(s_moods) / sizeof(s_moods[0])); i++)
    {
      if (rec->mood_mask & s_moods[i].bit)
        {
          tsum += (float)s_moods[i].score;
          ntag++;
          if (s_moods[i].bit & M_POS_MASK)
            {
              pos++;
            }
          if (s_moods[i].bit & M_NEG_MASK)
            {
              neg++;
            }
        }
    }
  if (ntag > 0)
    {
      base = tsum / (float)ntag;
      /* mixed positive+negative: down up to 30% */
      if (pos > 0 && neg > 0)
        {
          float ratio = (float)neg / (float)(pos + neg);
          base = base * (1.0f - ratio * 0.3f);
        }
    }

  /* quick text: 30% blend (same as app) */
  if (rec->quick > 0 && rec->quick <= DM_QUICK_MAX)
    {
      float qs = (float)s_quick_score[rec->quick - 1];
      base = base * 0.7f + qs * 0.3f;
    }

  return base;
}

/* weighted avg + stability bonus — same as score-detail.ux */
static int mood_score_from_recs(const dm_mood_rec_t *recs, int n)
{
  float wsum = 0.0f;
  float acc = 0.0f;
  float list[DM_MOOD_REC_MAX];
  float final;
  int i;
  float bonus = 0.0f;

  if (n <= 0 || !recs)
    {
      return -1;
    }
  if (n > DM_MOOD_REC_MAX)
    {
      n = DM_MOOD_REC_MAX;
    }

  for (i = 0; i < n; i++)
    {
      list[i] = mood_record_score(&recs[i]);
      /* recs[0] is newest → weight = n - i */
      {
        float w = (float)(n - i);
        acc += list[i] * w;
        wsum += w;
      }
    }

  final = (wsum > 0.0f) ? (acc / wsum) : 50.0f;

  /* stability: last up to 5 records in list order (index 0..min(n,5)-1 = newest first in FlowDesk)
   * App used slice(-5) oldest-of-recent on their list order.
   * FlowDesk stores newest at [0]; use up to 5 newest = list[0..m-1]. */
  if (n >= 3)
    {
      int m = n < 5 ? n : 5;
      float avg = 0.0f;
      float var = 0.0f;
      float sd;
      for (i = 0; i < m; i++)
        {
          avg += list[i];
        }
      avg /= (float)m;
      for (i = 0; i < m; i++)
        {
          float d = list[i] - avg;
          var += d * d;
        }
      var /= (float)m;
      sd = (float)sqrt((double)var);
      if (sd < 10.0f)
        {
          bonus = 5.0f;
        }
      else if (sd < 20.0f)
        {
          bonus = 3.0f;
        }
      else if (sd < 30.0f)
        {
          bonus = 1.0f;
        }
    }

  final = final + bonus;
  if (final < 0.0f)
    {
      final = 0.0f;
    }
  if (final > 100.0f)
    {
      final = 100.0f;
    }
  return (int)(final + 0.5f);
}

/* behavior multi-factor 0..100 — env/words/focus/med/water/game */
static int behavior_score_from_board(int *cov100, int *nf)
{
  float t = 0.0f;
  float h = 0.0f;
  int has_env;
  float es;
  int words_n;
  int water;
  int sched = 0;
  int taken = 0;
  float items_s[6];
  float items_w[6];
  int items_ok[6];
  int n = 6;
  int i;
  float acc = 0.0f;
  float wsum = 0.0f;
  int cnt = 0;

  has_env = (dm_sensor_last_th(&t, &h) == 0) ? 1 : 0;
  es = mf_env_score(t, h, has_env, has_env);
  words_n = dm_word_today_n();
  water = dm_water_today_cups();
  (void)dm_med_today_stats(&sched, &taken);

  items_w[0] = 0.15f;
  items_s[0] = es;
  items_ok[0] = (es >= 0.0f) ? 1 : 0;

  items_w[1] = 0.15f;
  items_s[1] = clampf((float)words_n / MF_WORDS_TARGET, 0.0f, 1.0f);
  items_ok[1] = 1;

  items_w[2] = 0.25f;
  items_s[2] = (g_dm.focus_done_min <= 0)
                   ? 0.0f
                   : clampf((float)g_dm.focus_done_min / MF_FOCUS_TARGET,
                            0.0f, 1.0f);
  items_ok[2] = 1;

  items_w[3] = 0.20f;
  if (sched > 0)
    {
      items_s[3] = clampf((float)taken / (float)sched, 0.0f, 1.0f);
      items_ok[3] = 1;
    }
  else
    {
      items_s[3] = 0.0f;
      items_ok[3] = 0;
    }

  items_w[4] = 0.15f;
  items_s[4] = clampf((float)water / MF_WATER_TARGET, 0.0f, 1.0f);
  items_ok[4] = 1;

  items_w[5] = 0.10f;
  items_s[5] = mf_game_score(s_game_ms);
  items_ok[5] = 1;

  for (i = 0; i < n; i++)
    {
      if (!items_ok[i])
        {
          continue;
        }
      acc += items_w[i] * items_s[i];
      wsum += items_w[i];
      cnt++;
    }
  if (nf)
    {
      *nf = cnt;
    }
  if (cov100)
    {
      *cov100 = (int)(wsum * 100.0f + 0.5f);
    }
  if (wsum <= 0.0001f || cnt <= 0)
    {
      return -1;
    }
  return (int)((acc / wsum) * 100.0f + 0.5f);
}

int dm_health_score(void)
{
  int mood_v;
  int beh_v;
  int cov = 0;
  int nf = 0;

  health_rollover_if_new_day();

  mood_v = mood_score_from_recs(s_recs, s_rec_n);
  beh_v = behavior_score_from_board(&cov, &nf);

  s_last_mood_n = s_rec_n;
  s_last_mood_score = mood_v;
  s_last_behavior = beh_v;
  s_last_cov = cov;
  s_last_nf = nf;
  s_last_has_mood = (mood_v >= 0) ? 1 : 0;
  s_last_has_beh = (beh_v >= 0) ? 1 : 0;

  /* blend: mood algorithm (静心轨迹) + board behavior factors */
  if (mood_v >= 0 && beh_v >= 0)
    {
      return (mood_v * 6 + beh_v * 4) / 10;
    }
  if (mood_v >= 0)
    {
      return mood_v;
    }
  if (beh_v >= 0)
    {
      return beh_v;
    }
  return 50;
}

int dm_health_score_detail(char *buf, int cap)
{
  int v = dm_health_score();
  if (!buf || cap < 8)
    {
      return v;
    }
  if (!s_last_has_mood && !s_last_has_beh)
    {
      snprintf(buf, (size_t)cap, "暂无数据");
      return v;
    }
  if (s_last_has_mood && s_last_has_beh)
    {
      snprintf(buf, (size_t)cap, "心情%d + 行为%d", s_last_mood_score,
               s_last_behavior);
      return v;
    }
  if (s_last_has_mood)
    {
      snprintf(buf, (size_t)cap, "仅心情 %d 分 · %d 条", s_last_mood_score,
               s_last_mood_n);
      return v;
    }
  snprintf(buf, (size_t)cap, "仅行为分 %d · 覆盖 %d%%", s_last_behavior,
           s_last_cov);
  return v;
}

static int health_voice_is_down(const char *text)
{
  static const char *const terms[] = {
    "不开心", "不太开心", "不高兴", "不太高兴", "心情不好",
    "心情不太好", "心情差", "状态不好", "状态不太好", "情绪不好",
    "情绪不太好", "情绪低落", "不太好", "不好受", "低落", "提不起劲",
    "心累", "心烦",
    "难受", "难过", "伤心", "焦虑", "压力大", "压力很大", "有压力",
    "很累", "累", "疲惫",
    "孤独", "孤单", "烦躁", "沮丧"
  };
  size_t i;

  if (!text)
    {
      return 0;
    }
  for (i = 0; i < sizeof(terms) / sizeof(terms[0]); i++)
    {
      const char *found = strstr(text, terms[i]);
      if (found && !voice_mood_term_negated(text, found))
        {
          return 1;
        }
    }
  return 0;
}

int dm_health_ai_read(dm_ai_health_query_t query, char *out, size_t cap)
{
  int cups, rc;
  struct tm now;
  time_t current;
  char mood[72] = "";
  char water_prefix[48] = "";
  int combined = query == DM_AI_HEALTH_WATER_AND_MOOD;
  size_t i;
  if (!dm_ai_permission_allowed(DM_AI_READ_HEALTH)) return DM_AI_DATA_DENIED;
  if (!out || cap < 80) return DM_AI_DATA_INVALID;
  if (!dm_wifi_clock_valid()) return DM_AI_DATA_CLOCK;
  if (query == DM_AI_HEALTH_WATER || combined)
    {
      rc = dm_water_ai_cups(&cups);
      if (rc) return rc;
      if (!combined)
        {
          snprintf(out, cap, "设备今日已记录喝水%d杯；仅统计在设备上保存的打卡。", cups);
          return 0;
        }
      snprintf(water_prefix, sizeof(water_prefix), "今日已记录喝水%d杯；", cups);
    }
  if (query != DM_AI_HEALTH_LAST_MOOD && !combined) return DM_AI_DATA_INVALID;
  if (s_health_day_key != health_today_key() || s_rec_n == 0)
    {
      snprintf(out, cap, "%s设备没有保存今天的心情记录，无法给出最近一次的时间。", water_prefix);
      return 0;
    }
  if (s_rec_n < 0 || s_rec_n > DM_MOOD_REC_MAX ||
      s_recs[0].hour > 23 || s_recs[0].min > 59 ||
      s_recs[0].quick > DM_QUICK_MAX) return DM_AI_DATA_INVALID;
  if (s_recs[0].quick)
    snprintf(mood, sizeof(mood), "%s", s_quick_zh[s_recs[0].quick - 1]);
  else for (i = 0; i < sizeof(s_moods) / sizeof(s_moods[0]); i++)
    if (s_recs[0].mood_mask & s_moods[i].bit)
      {
        size_t used = strlen(mood), len = strlen(s_moods[i].zh);
        if (used + len + 4 >= sizeof(mood)) break;
        snprintf(mood + used, sizeof(mood) - used, "%s%s", used ? "、" : "", s_moods[i].zh);
      }
  current = time(NULL);
  if (!localtime_r(&current, &now)) return DM_AI_DATA_CLOCK;
  snprintf(out, cap, "%s设备保存的最近一条心情：%04d-%02d-%02d %02u:%02u，%s。",
           water_prefix, now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
           (unsigned)s_recs[0].hour, (unsigned)s_recs[0].min,
           mood[0] ? mood : "未选择具体情绪");
  return 0;
}

int dm_health_ai_delete(const dm_ai_delete_request_t *request)
{
  dm_mood_rec_t previous[DM_MOOD_REC_MAX];
  int i, found = -1, old_n;
  if (!request || request->kind != DM_AI_DATA_MOOD) return DM_AI_DATA_INVALID;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_MOOD)) return DM_AI_DATA_DENIED;
  if (!dm_wifi_clock_valid()) return DM_AI_DATA_CLOCK;
  if (request->selector != DM_AI_SELECT_INDEX &&
      request->selector != DM_AI_SELECT_LATEST) return DM_AI_DATA_UNSUPPORTED;
  if (s_health_day_key != health_today_key()) return DM_AI_DATA_NOT_FOUND;
  for (i = 0; i < s_rec_n; i++)
    if (dm_ai_data_match(request, NULL, i + 1)) found = i;
  if (found < 0) return DM_AI_DATA_NOT_FOUND;
  if (!dm_ai_data_delete_allowed(DM_AI_DATA_MOOD)) return DM_AI_DATA_DENIED;
  old_n = s_rec_n;
  memcpy(previous, s_recs, sizeof(previous));
  for (i = found; i + 1 < s_rec_n; i++) s_recs[i] = s_recs[i + 1];
  s_rec_n--;
  memset(&s_recs[s_rec_n], 0, sizeof(s_recs[s_rec_n]));
  if (store_save() != 0)
    {
      memcpy(s_recs, previous, sizeof(previous));
      s_rec_n = old_n;
      return DM_AI_DATA_SAVE;
    }
  dm_health_refresh();
  return 0;
}

void dm_health_voice_advice(const char *user_text, char *out, int cap)
{
  int score;
  int water;
  int sched = 0;
  int taken = 0;
  int focus;
  int is_down;
  int has_data;

  if (!out || cap < 8)
    {
      return;
    }
  if (!dm_ai_permission_allowed(DM_AI_COMPANION))
    {
      dm_ai_log_add(DM_AI_LOG_ADVICE, DM_AI_LOG_DENIED,
                     DM_AI_LOG_REASON_PERMISSION, DM_AI_COMPANION, 0, -2);
      out[0] = '\0';
      return;
    }
  if (!dm_ai_permission_allowed(DM_AI_READ_HEALTH))
    {
      dm_ai_log_add(DM_AI_LOG_READ_HEALTH, DM_AI_LOG_DENIED,
                     DM_AI_LOG_REASON_PERMISSION, DM_AI_READ_HEALTH, 0, -2);
      snprintf(out, (size_t)cap, "%s",
               "我在这里陪你。可以先深呼吸、喝点水，或者休息一下。");
      return;
    }
  health_rollover_if_new_day();
  score = dm_health_score();
  has_data = s_last_has_mood || s_last_has_beh;
  water = dm_water_today_cups();
  focus = g_dm.focus_done_min;
  (void)dm_med_today_stats(&sched, &taken);
  dm_ai_log_add(DM_AI_LOG_READ_HEALTH, DM_AI_LOG_SUCCESS,
                 DM_AI_LOG_REASON_NONE, DM_AI_PERMISSION_COUNT, 0, 0);
  is_down = health_voice_is_down(user_text);

  if (is_down)
    {
      snprintf(out, (size_t)cap,
               "听起来你现在不太好受，谢谢你告诉我。我在这里陪你。愿意说说发生了什么吗？可以先深呼吸、喝点水或休息一下。若持续影响生活，找信任的人聊聊。");
    }
  else if (has_data)
    {
      snprintf(out, (size_t)cap,
               "根据健康页今日记录：综合分%d，专注%d分钟、喝水%d杯。可以先做一件轻松的小事，再看看状态。",
               score, focus, water);
    }
  else
    {
      snprintf(out, (size_t)cap,
               "我还没看到今天的健康记录。可以先做几次慢呼吸、喝点水，或者记录一下此刻的心情。");
    }
  if (!is_down && sched > 0 && taken < sched &&
      strlen(out) + 64 < (size_t)cap)
    {
      size_t used = strlen(out);
      snprintf(out + used, (size_t)cap - used,
               "另外，健康页显示今天有%d项用药记录尚未打卡。", sched - taken);
    }
}

static uint32_t score_color(int v)
{
  if (v >= 70)
    {
      return C_OK; /* green */
    }
  if (v >= 55)
    {
      return C_STAR; /* yellow */
    }
  if (v >= 40)
    {
      return 0xFF9500; /* orange */
    }
  return C_HEART; /* red */
}

static const char *score_tag_zh(int v)
{
  if (!s_last_has_mood && !s_last_has_beh)
    {
      return "暂无记录";
    }
  if (v >= 85)
    {
      return "非常好";
    }
  if (v >= 70)
    {
      return "良好";
    }
  if (v >= 55)
    {
      return "一般";
    }
  if (v >= 40)
    {
      return "偏低";
    }
  return "需要注意";
}

static const char *score_tag_en(int v)
{
  if (!s_last_has_mood && !s_last_has_beh)
    {
      return "No data yet";
    }
  if (v >= 85)
    {
      return "Excellent";
    }
  if (v >= 70)
    {
      return "Good";
    }
  if (v >= 55)
    {
      return "Fair";
    }
  if (v >= 40)
    {
      return "Low";
    }
  return "Take care";
}

static const char *score_msg_zh(int v)
{
  if (!s_last_has_mood && !s_last_has_beh)
    {
      return "记录心情或开始专注/喝水后计分";
    }
  if (v >= 85)
    {
      return "精神状态非常好，继续保持！";
    }
  if (v >= 70)
    {
      return "状态不错，继续加油！";
    }
  if (v >= 55)
    {
      return "注意适当休息，保持好心情！";
    }
  if (v >= 40)
    {
      return "建议适当放松，调整状态！";
    }
  return "建议充分休息，必要时寻求帮助。";
}

static const char *score_msg_en(int v)
{
  if (!s_last_has_mood && !s_last_has_beh)
    {
      return "Log mood or focus/water to score";
    }
  if (v >= 85)
    {
      return "You're in great shape — keep it up!";
    }
  if (v >= 70)
    {
      return "Doing well, keep going!";
    }
  if (v >= 55)
    {
      return "Take some rest, stay easy.";
    }
  if (v >= 40)
    {
      return "Try to relax and reset.";
    }
  return "Rest well; seek help if needed.";
}

/* ---------- helpers ---------- */

static lv_obj_t *mk_page(lv_obj_t *parent, dm_page_t id)
{
  lv_obj_t *page = lv_obj_create(parent);
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

static lv_obj_t *mk_card(lv_obj_t *parent, int h)
{
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_width(c, 292);
  lv_obj_set_height(c, h);
  lv_obj_set_style_bg_color(c, lv_color_hex(0x111111), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(c, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 10, LV_PART_MAIN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void now_hm(uint8_t *h, uint8_t *m)
{
  time_t t = time(NULL);
  struct tm tmv;
  localtime_r(&t, &tmv);
  *h = (uint8_t)tmv.tm_hour;
  *m = (uint8_t)tmv.tm_min;
}

static int voice_mood_term_negated(const char *text, const char *found)
{
  static const char *const negations[] = {
    "没有觉得", "并没有", "不认为", "不觉得", "没什么", "没那么",
    "并不是", "不是", "没有", "并不", "不再", "不太", "不怎么",
    "不", "没", "未"
  };
  size_t i;

  for (i = 0; i < sizeof(negations) / sizeof(negations[0]); i++)
    {
      size_t n = strlen(negations[i]);
      if (found >= text + n && memcmp(found - n, negations[i], n) == 0)
        {
          return 1;
        }
    }
  return 0;
}

int dm_health_record_mood_voice(const char *text)
{
  static const struct
  {
    const char *word;
    uint16_t bit;
  } terms[] = {
    { "心情不好", DM_M_SAD }, { "心情不太好", DM_M_SAD },
    { "心情差", DM_M_SAD }, { "状态不好", DM_M_SAD },
    { "状态不太好", DM_M_SAD }, { "情绪不好", DM_M_SAD },
    { "情绪不太好", DM_M_SAD }, { "情绪低落", DM_M_SAD },
    { "不开心", DM_M_SAD }, { "不太开心", DM_M_SAD },
    { "不高兴", DM_M_SAD }, { "不太高兴", DM_M_SAD },
    { "不愉快", DM_M_SAD }, { "提不起劲", DM_M_TIRED },
    { "心累", DM_M_TIRED }, { "心烦", DM_M_STRESS },
    { "低落", DM_M_SAD }, { "难过", DM_M_SAD },
    { "伤心", DM_M_SAD }, { "悲伤", DM_M_SAD },
    { "沮丧", DM_M_SAD }, { "难受", DM_M_SAD },
    { "开心", DM_M_JOY },
    { "高兴", DM_M_JOY }, { "愉快", DM_M_JOY },
    { "喜爱", DM_M_LOVE }, { "幸福", DM_M_LOVE },
    { "兴奋", DM_M_EXCITE }, { "激动", DM_M_EXCITE },
    { "有活力", DM_M_EXCITE }, { "自信", DM_M_CONFIDENT },
    { "有信心", DM_M_CONFIDENT }, { "期待", DM_M_EXPECT },
    { "满足", DM_M_SATISFY }, { "满意", DM_M_SATISFY },
    { "感动", DM_M_TOUCHED }, { "平静", DM_M_CALM },
    { "平和", DM_M_CALM }, { "放松", DM_M_CALM },
    { "安心", DM_M_CALM }, { "若有所思", DM_M_THINK },
    { "思考", DM_M_THINK }, { "惊讶", DM_M_SURPRISE },
    { "生气", DM_M_ANGRY }, { "愤怒", DM_M_ANGRY },
    { "烦躁", DM_M_ANGRY }, { "恼火", DM_M_ANGRY },
    { "疲惫", DM_M_TIRED }, { "很累", DM_M_TIRED },
    { "累", DM_M_TIRED }, { "困", DM_M_TIRED },
    { "焦虑", DM_M_ANXIOUS }, { "紧张", DM_M_ANXIOUS },
    { "担心", DM_M_ANXIOUS }, { "不安", DM_M_ANXIOUS },
    { "孤独", DM_M_LONELY }, { "孤单", DM_M_LONELY },
    { "压力很大", DM_M_STRESS }, { "压力大", DM_M_STRESS },
    { "有压力", DM_M_STRESS }, { "压力", DM_M_STRESS }
  };
  dm_mood_rec_t rec;
  dm_mood_rec_t old_recs[DM_MOOD_REC_MAX];
  uint16_t mask = 0;
  uint8_t quick = 0;
  int old_n;
  size_t i;

  if (!text || !text[0])
    {
      return -1;
    }

  for (i = 0; i < sizeof(terms) / sizeof(terms[0]); i++)
    {
      const char *found = strstr(text, terms[i].word);
      if (!found)
        {
          continue;
        }
      /* "不开心/不高兴" are sadness labels, but do not also score them as
       * positive happiness. Other negated states are not logged as facts. */
      if ((terms[i].bit == DM_M_JOY &&
           (strstr(text, "不开心") || strstr(text, "不太开心") ||
            strstr(text, "不高兴") || strstr(text, "不太高兴") ||
            strstr(text, "不愉快") || strstr(text, "心情不好") ||
            strstr(text, "心情差") || strstr(text, "状态不好") ||
            strstr(text, "状态不太好") || strstr(text, "情绪不好") ||
            strstr(text, "情绪不太好"))) ||
          (terms[i].bit == DM_M_STRESS &&
           (strstr(text, "压力不大") || strstr(text, "压力不高") ||
            strstr(text, "没有压力") || strstr(text, "没压力"))) ||
          voice_mood_term_negated(text, found))
        {
          continue;
        }
      mask |= terms[i].bit;
    }

  if (strstr(text, "心情不错") || strstr(text, "心情很好") ||
      strstr(text, "心情挺好")) quick = 1;
  else if (strstr(text, "感觉一般") || strstr(text, "心情一般") ||
           strstr(text, "一般般")) quick = 2;
  else if (strstr(text, "有点不开心") || strstr(text, "有点不高兴")) quick = 3;
  else if (strstr(text, "很累想休息") || strstr(text, "累想休息")) quick = 4;
  else if (strstr(text, "充满活力")) quick = 5;
  else if (strstr(text, "心平气和")) quick = 6;
  else if (strstr(text, "坐立不安")) quick = 7;
  else if (strstr(text, "若有所思")) quick = 8;

  if (mask == 0 && quick == 0)
    {
      s_pick_mask = 0;
      s_pick_quick = 0;
      s_pick_type = 0;
      paint_log_picks();
      return -1;
    }

  health_rollover_if_new_day();
  memset(&rec, 0, sizeof(rec));
  rec.mood_mask = mask;
  rec.quick = quick;
  rec.rec_type = (strstr(text, "今天整体") || strstr(text, "今日整体")) ? 1 : 0;
  now_hm(&rec.hour, &rec.min);

  old_n = s_rec_n;
  memcpy(old_recs, s_recs, sizeof(old_recs));
  if (s_rec_n < DM_MOOD_REC_MAX)
    {
      memmove(&s_recs[1], &s_recs[0], sizeof(dm_mood_rec_t) * (size_t)s_rec_n);
      s_rec_n++;
    }
  else
    {
      memmove(&s_recs[1], &s_recs[0],
              sizeof(dm_mood_rec_t) * (DM_MOOD_REC_MAX - 1));
    }
  s_recs[0] = rec;

  if (store_save() != 0)
    {
      memcpy(s_recs, old_recs, sizeof(old_recs));
      s_rec_n = old_n;
      return -2;
    }

  dm_health_refresh();
  return 0;
}

void dm_health_add_focus_min(int32_t min)
{
  if (min > 0)
    {
      g_dm.focus_done_min += min;
      if (g_dm.focus_done_min > 600)
        {
          g_dm.focus_done_min = 600;
        }
      store_save();
    }
}

void dm_health_add_game_ms(int ms)
{
  health_rollover_if_new_day();
  if (ms <= 0)
    {
      return;
    }
  s_game_ms += ms;
  if (s_game_ms > 4 * 3600 * 1000)
    {
      s_game_ms = 4 * 3600 * 1000;
    }
  if (s_game_ms - s_game_saved_ms >= 60000)
    {
      if (store_save() == 0) s_game_saved_ms = s_game_ms;
    }
}

void dm_health_flush_game(void)
{
  if (s_game_ms != s_game_saved_ms && store_save() == 0)
    s_game_saved_ms = s_game_ms;
}

static void rebuild_list(void)
{
  int i;
  char line[64];

  if (!s_list_box)
    {
      return;
    }
  lv_obj_clean(s_list_box);

  if (s_rec_n == 0)
    {
      lv_obj_t *e = dm_lbl(s_list_box, "还没有记录，点「记录心情」添加",
                           "No records yet", g_dm_font_s, C_MUTED);
      lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
      lv_obj_set_width(e, 260);
      return;
    }

  for (i = 0; i < s_rec_n; i++)
    {
      lv_obj_t *row = lv_obj_create(s_list_box);
      lv_obj_set_width(row, 270);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

      char tags[48];
      int pos = 0;
      int ti;

      tags[0] = 0;
      pos = snprintf(tags, sizeof(tags), "%s",
                     s_recs[i].rec_type ? dm_t("今日整体", "Daily")
                                        : dm_t("当前心情", "Now"));
      for (ti = 0; ti < (int)(sizeof(s_moods) / sizeof(s_moods[0])); ti++)
        {
          if (s_recs[i].mood_mask & s_moods[ti].bit)
            {
              pos += snprintf(tags + pos, sizeof(tags) - pos, " · %s",
                              dm_t(s_moods[ti].zh, s_moods[ti].en));
              if (pos >= (int)sizeof(tags) - 6)
                {
                  break;
                }
            }
        }

      lv_obj_t *head = dm_lbl(row, tags, tags, g_dm_font_s, C_INK);
      lv_label_set_long_mode(head, LV_LABEL_LONG_DOT);
      lv_obj_set_width(head, 200);

      if (s_recs[i].quick > 0 && s_recs[i].quick <= DM_QUICK_MAX)
        {
          lv_obj_t *q = dm_lbl(row, s_quick_zh[s_recs[i].quick - 1],
                               s_quick_en[s_recs[i].quick - 1],
                               g_dm_font_s, C_DIM);
          lv_obj_set_pos(q, 0, 16);
        }

      lv_snprintf(line, sizeof(line), "%02u:%02u", s_recs[i].hour,
                  s_recs[i].min);
      lv_obj_t *time_l = lv_label_create(row);
      lv_label_set_text(time_l, line);
      dm_style(time_l, g_dm_font_s, C_MUTED);
      lv_obj_align(time_l, LV_ALIGN_TOP_RIGHT, 0, 0);

      lv_obj_set_height(row, s_recs[i].quick ? 34 : 20);
    }
}

void dm_health_refresh(void)
{
  int v;
  char b[48];

  health_rollover_if_new_day();
  v = dm_health_score();

  if (s_score_lbl)
    {
      lv_snprintf(b, sizeof(b), "%d", v);
      lv_label_set_text(s_score_lbl, b);
      lv_obj_set_style_text_color(s_score_lbl, lv_color_hex(score_color(v)),
                                  LV_PART_MAIN);
    }
  if (s_tag_lbl)
    {
      lv_label_set_text(s_tag_lbl, dm_t(score_tag_zh(v), score_tag_en(v)));
      lv_obj_set_style_text_color(s_tag_lbl, lv_color_hex(score_color(v)),
                                  LV_PART_MAIN);
    }
  if (s_msg_lbl)
    {
      lv_label_set_text(s_msg_lbl, dm_t(score_msg_zh(v), score_msg_en(v)));
    }
  if (s_focus_lbl)
    {
      lv_snprintf(b, sizeof(b), "%s %ld′ · %s %d · %s %d",
                  dm_t("专注", "Focus"), (long)g_dm.focus_done_min,
                  dm_t("心情", "Mood"), s_rec_n,
                  dm_t("水", "H2O"), dm_water_today_cups());
      lv_label_set_text(s_focus_lbl, b);
    }
  rebuild_list();
}

/* ---------- mood log page ---------- */

static lv_obj_t *first_label(lv_obj_t *btn)
{
  uint32_t n;
  uint32_t i;

  if (!btn || !lv_obj_is_valid(btn))
    {
      return NULL;
    }
  n = lv_obj_get_child_count(btn);
  for (i = 0; i < n; i++)
    {
      lv_obj_t *c = lv_obj_get_child(btn, i);
      if (c && lv_obj_check_type(c, &lv_label_class))
        {
          return c;
        }
    }
  return NULL;
}

static void style_chip(lv_obj_t *btn, uint32_t bg, uint32_t fg)
{
  lv_obj_t *lab;

  if (!btn || !lv_obj_is_valid(btn))
    {
      return;
    }
  lv_obj_set_style_bg_color(btn, lv_color_hex(bg), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
  lab = first_label(btn);
  if (lab)
    {
      lv_obj_set_style_text_color(lab, lv_color_hex(fg), LV_PART_MAIN);
    }
}

static void paint_log_picks(void)
{
  int i;

  for (i = 0; i < 16; i++)
    {
      if (!s_mood_btns[i])
        {
          continue;
        }
      if (s_pick_mask & s_moods[i].bit)
        {
          style_chip(s_mood_btns[i],
                     (s_moods[i].bit & M_NEG) ? C_HEART : C_FACE, C_EYE);
        }
      else
        {
          style_chip(s_mood_btns[i], C_BTN_HI, C_DIM);
        }
    }

  for (i = 0; i < DM_QUICK_MAX; i++)
    {
      if (!s_quick_btns[i])
        {
          continue;
        }
      if (s_pick_quick == i + 1)
        {
          style_chip(s_quick_btns[i], C_ACCENT, C_EYE);
        }
      else
        {
          style_chip(s_quick_btns[i], C_BTN, C_MUTED);
        }
    }

  if (s_seg_cur && s_seg_day)
    {
      style_chip(s_seg_cur, s_pick_type == 0 ? C_FACE : C_BTN,
                 s_pick_type == 0 ? C_EYE : C_MUTED);
      style_chip(s_seg_day, s_pick_type ? C_FACE : C_BTN,
                 s_pick_type ? C_EYE : C_MUTED);
    }
}

static void open_log_page(lv_event_t *e)
{
  (void)e;
  s_pick_mask = 0;
  s_pick_quick = 0;
  s_pick_type = 0;
  paint_log_picks();
  dm_show(PAGE_MOOD_LOG);
}

static void back_health(lv_event_t *e)
{
  (void)e;
  dm_health_refresh();
  dm_show(PAGE_HEALTH);
}

static void mood_btn_cb(lv_event_t *e)
{
  int idx = (int)(uintptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= 16)
    {
      return;
    }
  s_pick_mask ^= s_moods[idx].bit;
  paint_log_picks();
}

static void quick_btn_cb(lv_event_t *e)
{
  int idx = (int)(uintptr_t)lv_event_get_user_data(e);
  if (idx < 0 || idx >= DM_QUICK_MAX)
    {
      return;
    }
  if (s_pick_quick == idx + 1)
    {
      s_pick_quick = 0;
    }
  else
    {
      s_pick_quick = (uint8_t)(idx + 1);
    }
  paint_log_picks();
}

static void seg_cb(lv_event_t *e)
{
  s_pick_type = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
  paint_log_picks();
}

static void save_cb(lv_event_t *e)
{
  dm_mood_rec_t rec;
  dm_mood_rec_t previous[DM_MOOD_REC_MAX];
  int old_count = s_rec_n;
  memcpy(previous, s_recs, sizeof(previous));
  (void)e;

  if (s_pick_mask == 0 && s_pick_quick == 0)
    {
      /* stay on page; could flash hint */
      return;
    }

  memset(&rec, 0, sizeof(rec));
  rec.mood_mask = s_pick_mask;
  rec.quick = s_pick_quick;
  rec.rec_type = s_pick_type;
  now_hm(&rec.hour, &rec.min);

  if (s_rec_n < DM_MOOD_REC_MAX)
    {
      memmove(&s_recs[1], &s_recs[0], sizeof(dm_mood_rec_t) * (size_t)s_rec_n);
      s_recs[0] = rec;
      s_rec_n++;
    }
  else
    {
      memmove(&s_recs[1], &s_recs[0],
              sizeof(dm_mood_rec_t) * (DM_MOOD_REC_MAX - 1));
      s_recs[0] = rec;
    }

  if (store_save() != 0)
    {
      memcpy(s_recs, previous, sizeof(previous));
      s_rec_n = old_count;
      dm_say("保存失败，请稍后重试", "Save failed; retry");
      return;
    }
  dm_health_refresh();
  dm_show(PAGE_HEALTH);
}

/* ---------- build ---------- */

static lv_obj_t *chip_btn(lv_obj_t *p, const char *zh, const char *en, int w,
                          int h, lv_event_cb_t cb, void *ud)
{
  return dm_btn(p, zh, en, w, h, C_BTN_HI, C_DIM, cb, ud);
}

void dm_create_health(void)
{
  lv_obj_t *title;
  lv_obj_t *score_card;
  lv_obj_t *num;
  lv_obj_t *unit;
  lv_obj_t *focus_card;
  lv_obj_t *list_card;
  lv_obj_t *list_t;
  int i;
  int x;
  int y;

  store_load();

  s_page = mk_page(g_dm_root, PAGE_HEALTH);

  title = dm_lbl(s_page, "健康", "Health", g_dm_font_m, C_INK);
  lv_obj_set_pos(title, 12, 8);

  s_sc = lv_obj_create(s_page);
  lv_obj_set_size(s_sc, DM_SCR_W, 160);
  lv_obj_set_pos(s_sc, 0, 28);
  lv_obj_set_style_bg_opa(s_sc, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_sc, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_sc, 6, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_sc, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(s_sc, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_flex_flow(s_sc, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_sc, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_sc, 8, LV_PART_MAIN);

  score_card = mk_card(s_sc, 72);
  {
    lv_obj_t *cap = dm_lbl(score_card, "情绪综合分", "Mood score",
                           g_dm_font_s, C_MUTED);
    lv_obj_set_pos(cap, 0, 0);
    num = lv_label_create(score_card);
    lv_label_set_text(num, "60");
    dm_style(num, g_dm_font_l, C_OK);
    lv_obj_set_pos(num, 0, 22);
    s_score_lbl = num;
    unit = dm_lbl(score_card, "/100", "/100", g_dm_font_s, C_MUTED);
    lv_obj_set_pos(unit, 40, 30);
    s_tag_lbl = dm_lbl(score_card, "状态良好", "Good", g_dm_font_s, C_OK);
    lv_obj_set_pos(s_tag_lbl, 90, 22);
    s_msg_lbl = dm_lbl(score_card, "综合心情与专注", "Mood + focus",
                       g_dm_font_s, C_DIM);
    lv_label_set_long_mode(s_msg_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_msg_lbl, 180);
    lv_obj_set_pos(s_msg_lbl, 90, 40);
  }

  focus_card = mk_card(s_sc, 40);
  s_focus_lbl = dm_lbl(focus_card, "专注 0 min · 心情 0",
                       "Focus 0 · Mood 0", g_dm_font_s, C_INK);
  lv_obj_center(s_focus_lbl);

  list_card = mk_card(s_sc, 120);
  list_t = dm_lbl(list_card, "最近心情", "Recent moods", g_dm_font_s,
                  C_MUTED);
  lv_obj_set_pos(list_t, 0, 0);
  s_list_box = lv_obj_create(list_card);
  lv_obj_set_size(s_list_box, 260, 90);
  lv_obj_set_pos(s_list_box, 0, 18);
  lv_obj_set_style_bg_opa(s_list_box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_list_box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_list_box, 0, LV_PART_MAIN);
  lv_obj_set_scroll_dir(s_list_box, LV_DIR_VER);
  lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_list_box, 2, LV_PART_MAIN);

  {
    lv_obj_t *tipc = mk_card(s_sc, 56);
    lv_obj_t *tt = dm_lbl(tipc, "说明", "Note", g_dm_font_s, C_MUTED);
    lv_obj_t *tb = dm_lbl(tipc,
                          "评分=心情/专注/喝水/吃药/单词/环境/游戏\n缺失因子会重归一，每日0点清零",
                          "Mood+focus+water+med+words+env+game\nRenorm; resets daily",
                          g_dm_font_s, C_DIM);
    lv_obj_set_pos(tt, 0, 0);
    lv_label_set_long_mode(tb, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(tb, 250);
    lv_obj_set_pos(tb, 0, 16);
  }

  s_fab = dm_btn(s_page, "记录心情", "Log mood", 120, 30, C_FACE, C_EYE,
                 open_log_page, NULL);
  lv_obj_align(s_fab, LV_ALIGN_TOP_MID, 0, 160);

  /* ---- full-page mood log ---- */
  s_log_page = mk_page(g_dm_root, PAGE_MOOD_LOG);

  {
    lv_obj_t *ht = dm_lbl(s_log_page, "记录心情", "Log mood", g_dm_font_m,
                          C_INK);
    lv_obj_set_pos(ht, 12, 8);

    lv_obj_t *back = dm_btn(s_log_page, "返回", "Back", 56, 26, C_BTN,
                            C_MUTED, back_health, NULL);
    lv_obj_set_pos(back, 252, 6);

    s_seg_cur = chip_btn(s_log_page, "当前", "Now", 90, 26, seg_cb,
                         (void *)(uintptr_t)0);
    lv_obj_set_pos(s_seg_cur, 12, 40);
    s_seg_day = chip_btn(s_log_page, "今日整体", "Daily", 100, 26, seg_cb,
                         (void *)(uintptr_t)1);
    lv_obj_set_pos(s_seg_day, 110, 40);

    x = 12;
    y = 72;
    for (i = 0; i < 16; i++)
      {
        int w = 72;
        s_mood_btns[i] = chip_btn(s_log_page, s_moods[i].zh, s_moods[i].en,
                                  w, 24, mood_btn_cb, (void *)(uintptr_t)i);
        lv_obj_set_pos(s_mood_btns[i], x, y);
        x += w + 4;
        if (x > 240)
          {
            x = 12;
            y += 26;
          }
      }

    x = 12;
    y = 180;
    for (i = 0; i < DM_QUICK_MAX; i++)
      {
        int w = 72;
        s_quick_btns[i] = chip_btn(s_log_page, s_quick_zh[i], s_quick_en[i],
                                   w, 22, quick_btn_cb,
                                   (void *)(uintptr_t)i);
        lv_obj_set_pos(s_quick_btns[i], x, y);
        x += w + 4;
        if (x > 240)
          {
            x = 12;
            y += 24;
          }
      }
  }

  {
    lv_obj_t *save = dm_btn(s_log_page, "保存", "Save", 70, 26, C_FACE,
                            C_EYE, save_cb, NULL);
    lv_obj_set_pos(save, 174, 6);
  }

  paint_log_picks();
  dm_health_refresh();
}

#endif /* CONFIG_DESKMATE_APP */
