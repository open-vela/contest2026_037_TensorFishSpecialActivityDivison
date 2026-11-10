/****************************************************************************
 * dm_chat_ai.c — MiMo cloud chat + local offline fallback
 ****************************************************************************/

#include "deskmate.h"
#include "dm_ai_permissions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <syslog.h>
#include <netutils/cJSON.h>

#ifdef CONFIG_DESKMATE_APP

/* MiMo OpenAI-compatible endpoint. Store the rotated key only on-device. */
#define MIMO_BASE "https://token-plan-sgp.xiaomimimo.com/v1"
#define MIMO_KEY_PATH "/data/deskmate_mimo.key"
#define MIMO_CURL_CONFIG "/tmp/dm_chat_mimo.conf"
#define MIMO_MODEL "mimo-v2.6-flash"
#define CHAT_REQ_PATH "/tmp/dm_chat_req.json"
#define CHAT_RSP_PATH "/tmp/dm_chat_rsp.json"
#define CHAT_MAX_REPLY 240

static int chat_key_load(char *key, size_t key_cap)
{
  FILE *f;
  size_t i;
  size_t n;
  int valid = 0;

  if (!key || key_cap < 16)
    {
      return 0;
    }
  key[0] = '\0';
  f = fopen(MIMO_KEY_PATH, "rb");
  if (!f)
    {
      return 0;
    }
  if (fgets(key, (int)key_cap, f))
    {
      n = strlen(key);
      while (n > 0 && (key[n - 1] == '\r' || key[n - 1] == '\n' ||
                       key[n - 1] == ' ' || key[n - 1] == '\t'))
        {
          key[--n] = '\0';
        }
      valid = n > 8 && n < key_cap - 1;
      for (i = 0; valid && i < n; i++)
        {
          unsigned char ch = (unsigned char)key[i];
          if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                ch == '.'))
            {
              valid = 0;
            }
        }
    }
  fclose(f);
  if (!valid)
    {
      memset(key, 0, key_cap);
    }
  return valid;
}

/* Store every curl option in its config file. The NSH command is kept to two
 * arguments (curl and -K config) because this image's argument limit is low. */
static int run_curl_command(const char *url, const char *request_path,
                            const char *out_path, int timeout_sec)
{
  char key[128];
  FILE *kf;
  int config_ok;
  int close_rc;
  int rc;
  int saved_errno;

  if (!url || !request_path || !out_path || timeout_sec < 1 ||
      timeout_sec > 120 || strchr(url, '"') || strchr(url, '\\') ||
      strchr(url, '\r') || strchr(url, '\n') || strchr(request_path, '"') ||
      strchr(request_path, '\\') || strchr(request_path, '\r') ||
      strchr(request_path, '\n') || strchr(out_path, '"') ||
      strchr(out_path, '\\') || strchr(out_path, '\r') ||
      strchr(out_path, '\n') || !chat_key_load(key, sizeof(key)))
    {
      syslog(LOG_ERR, "[dm-chat] invalid curl request configuration\n");
      return -1;
    }

  kf = fopen(MIMO_CURL_CONFIG, "w");
  if (!kf)
    {
      syslog(LOG_ERR, "[dm-chat] cannot create curl config errno=%d\n", errno);
      memset(key, 0, sizeof(key));
      return -1;
    }
  if (chmod(MIMO_CURL_CONFIG, S_IRUSR | S_IWUSR) != 0)
    {
      /* chmod is unsupported on this image; that must not block requests. */
      syslog(LOG_WARNING,
             "[dm-chat] chmod unavailable for curl config errno=%d; continuing\n",
             errno);
    }

  config_ok = fputs("silent\nshow-error\nfail\n",
                    kf) != EOF &&
              fprintf(kf, "max-time = %d\n", timeout_sec) > 0 &&
              fprintf(kf, "url = \"%s\"\n", url) > 0 &&
              fputs("cacert = \"/resource/etc/ssl/curl/ca-certificates.crt\"\n"
                    "request = \"POST\"\n"
                    "header = \"Content-Type: application/json\"\n",
                    kf) != EOF &&
              fprintf(kf, "header = \"Authorization: Bearer %s\"\n",
                      key) > 0 &&
              fprintf(kf, "data = \"@%s\"\noutput = \"%s\"\n",
                      request_path, out_path) > 0 &&
              fputs("write-out = \"HTTP_STATUS:%{http_code}\\n\"\n",
                    kf) != EOF;
  close_rc = fclose(kf);
  memset(key, 0, sizeof(key));
  if (!config_ok || close_rc != 0)
    {
      syslog(LOG_ERR, "[dm-chat] cannot write curl config\n");
      unlink(MIMO_CURL_CONFIG);
      return -1;
    }

  errno = 0;
  rc = system("curl -K " MIMO_CURL_CONFIG);
  saved_errno = errno;
  unlink(MIMO_CURL_CONFIG);
  if (rc != 0)
    {
      syslog(LOG_ERR,
             "[dm-chat] curl command failed system_status=%d errno=%d\n",
             rc, saved_errno);
      return -1;
    }
  return 0;
}

static char s_last_reply[CHAT_MAX_REPLY];
static int s_last_request_remote_ok;
static int s_last_request_intent_ok;
static int s_last_reply_health;
static int s_last_reply_local;

int dm_chat_ai_local_reply(void)
{
  return s_last_reply_local;
}

int dm_chat_ai_health_reply(void)
{
  return s_last_reply_health;
}
static char s_persona_ctx[512];

static const char *const s_local_lines[] = {
  "我在呢。想聊什么，或者要不要先专注一会儿？",
  "今天也要好好照顾自己呀。",
  "记住：完成比完美更重要。",
  "累了就歇一下，我陪你。",
  "要不要喝口水？身体是本钱。",
  "你已经很棒了，继续加油！",
  "需要我帮你记点什么吗？去备忘看看。",
  "玩 2048 放松一下也可以哦。",
};

static const char *local_reply(const char *text)
{
  if (!text || !text[0])
    {
      return s_local_lines[0];
    }

  if (strstr(text, "你好") || strstr(text, "hi") || strstr(text, "hello") ||
      strstr(text, "嗨"))
    {
      return "你好呀！我是 openvela小助手，桌边的小搭档。";
    }
  if (strstr(text, "你是谁") || strstr(text, "介绍"))
    {
      return "我是 openvela小助手：陪你专注、记事、聊天的桌面小伙伴。";
    }
  if (strstr(text, "笑话") || strstr(text, "无聊"))
    {
      return "为什么程序员总分不清万圣节和圣诞节？因为 Oct 31 == Dec 25。";
    }
  if (strstr(text, "加油") || strstr(text, "鼓励") || strstr(text, "累"))
    {
      return "深呼吸。你不需要一次做完所有事，先做下一件就好。";
    }
  if (strstr(text, "专注") || strstr(text, "工作") || strstr(text, "学习"))
    {
      return "要不来一段专注？1 分钟也可以，开始了就不算晚。";
    }
  if (strstr(text, "天气"))
    {
      return "我这边暂时看不了天气，不过记得出门看一眼天空呀。";
    }
  if (strstr(text, "喝水"))
    {
      return "好主意，现在就去喝一口水吧。";
    }
  if (strstr(text, "吃药") || strstr(text, "药"))
    {
      {
        const char *cur = dm_t("去「功能 → 吃药」看看今天任务。",
                               "Check Features → Meds for today.");
        return cur;
      }
    }
  if (strstr(text, "2048") || strstr(text, "游戏") || strstr(text, "玩"))
    {
      return "功能里有 2048，滑一滑很解压。玩完记得回来专注哦。";
    }

  return s_local_lines[rand() % (int)(sizeof(s_local_lines) /
                                      sizeof(s_local_lines[0]))];
}

static int json_escape(const char *in, char *out, size_t cap)
{
  size_t o = 0;
  size_t i;

  if (!in || !out || cap < 2)
    {
      return -1;
    }
  for (i = 0; in[i] && o + 8 < cap; i++)
    {
      unsigned char ch = (unsigned char)in[i];
      if (ch == '"' || ch == '\\')
        {
          out[o++] = '\\';
          out[o++] = (char)ch;
        }
      else if (ch == '\n')
        {
          out[o++] = '\\';
          out[o++] = 'n';
        }
      else if (ch < 0x20)
        {
          /* skip control chars */
        }
      else
        {
          out[o++] = (char)ch;
        }
    }
  out[o] = '\0';
  return (int)o;
}

/* ---- multi-turn conversation memory (shared with voice chat) ---- */
#define HIST_TURN_MAX 3
#define HIST_LINE     128

static struct
{
  char user[HIST_LINE];
  char bot[HIST_LINE];
  bool private_context;
} s_hist[HIST_TURN_MAX];
static int s_hist_n;

void dm_chat_hist_reset(void)
{
  s_hist_n = 0;
}

static void chat_hist_push(const char *user, const char *assistant,
                           bool private_context)
{
  int i;

  if (!user || !user[0])
    {
      return;
    }
  if (s_hist_n == HIST_TURN_MAX)
    {
      for (i = 1; i < HIST_TURN_MAX; i++)
        {
          s_hist[i - 1] = s_hist[i];
        }
      s_hist_n = HIST_TURN_MAX - 1;
    }
  strncpy(s_hist[s_hist_n].user, user, HIST_LINE - 1);
  s_hist[s_hist_n].user[HIST_LINE - 1] = '\0';
  if (assistant)
    {
      strncpy(s_hist[s_hist_n].bot, assistant, HIST_LINE - 1);
      s_hist[s_hist_n].bot[HIST_LINE - 1] = '\0';
    }
  else
    {
      s_hist[s_hist_n].bot[0] = '\0';
    }
  s_hist[s_hist_n].private_context = private_context;
  s_hist_n++;
}

void dm_chat_hist_push(const char *user, const char *assistant)
{
  /* A turn generated with record access may repeat data from its context. */
  chat_hist_push(user, assistant, dm_ai_permission_allowed(DM_AI_READ_HEALTH));
}

void dm_chat_hist_push_health(const char *user, const char *assistant)
{
  chat_hist_push(user, assistant, true);
}

/* emit {"role":..,"content":..}, entries (each with trailing comma) */
int dm_chat_hist_json_for_cloud(char *out, int cap, bool allow_health)
{
  char esc[HIST_LINE * 2];
  int o = 0;
  int i;

  if (!out || cap < 2)
    {
      return -1;
    }
  allow_health = allow_health && dm_ai_permission_allowed(DM_AI_READ_HEALTH);
  out[0] = '\0';
  for (i = 0; i < s_hist_n; i++)
    {
      int n;

      if (s_hist[i].private_context && !allow_health) continue;
      if (s_hist[i].user[0])
        {
          json_escape(s_hist[i].user, esc, sizeof(esc));
          n = snprintf(out + o, (size_t)(cap - o),
                       "{\"role\":\"user\",\"content\":\"%s\"},", esc);
          if (n < 0 || n >= cap - o)
            {
              out[o] = '\0';
              return -1;
            }
          o += n;
        }
      if (s_hist[i].bot[0])
        {
          json_escape(s_hist[i].bot, esc, sizeof(esc));
          n = snprintf(out + o, (size_t)(cap - o),
                       "{\"role\":\"assistant\",\"content\":\"%s\"},", esc);
          if (n < 0 || n >= cap - o)
            {
              out[o] = '\0';
              return -1;
            }
          o += n;
        }
    }
  return o;
}

int dm_chat_hist_json(char *out, int cap)
{
  return dm_chat_hist_json_for_cloud(out, cap, true);
}

static void extract_content(const char *path, char *out, size_t cap)
{
  FILE *f;
  char *buf;
  long sz;
  cJSON *root;
  cJSON *choices;
  cJSON *choice;
  cJSON *message;
  cJSON *content;

  if (!out || cap < 2)
    {
      return;
    }
  out[0] = '\0';
  f = fopen(path, "rb");
  if (!f)
    {
      return;
    }
  if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) <= 0 ||
      sz > 64 * 1024 || fseek(f, 0, SEEK_SET) != 0)
    {
      fclose(f);
      return;
    }
  buf = malloc((size_t)sz + 1);
  if (!buf)
    {
      fclose(f);
      return;
    }
  if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
    {
      free(buf);
      fclose(f);
      return;
    }
  buf[sz] = '\0';
  fclose(f);

  root = cJSON_Parse(buf);
  free(buf);
  if (!root)
    {
      return;
    }
  choices = cJSON_GetObjectItem(root, "choices");
  choice = choices && cJSON_IsArray(choices)
             ? cJSON_GetArrayItem(choices, 0) : NULL;
  message = choice ? cJSON_GetObjectItem(choice, "message") : NULL;
  content = message ? cJSON_GetObjectItem(message, "content") : NULL;
  if (content && cJSON_IsString(content) && content->valuestring)
    {
      snprintf(out, cap, "%s", content->valuestring);
    }
  cJSON_Delete(root);
}

int dm_chat_ai_available(void)
{
  return dm_wifi_connected() ? 1 : 0;
}

/* True only when the most recent chat answer came from the cloud API. */
int dm_chat_ai_remote_ok(void)
{
  return s_last_request_remote_ok;
}

int dm_chat_ai_intent_ok(void)
{
  return s_last_request_intent_ok;
}

void dm_chat_ai_set_persona(const char *persona)
{
  if (!persona)
    {
      s_persona_ctx[0] = '\0';
      return;
    }
  strncpy(s_persona_ctx, persona, sizeof(s_persona_ctx) - 1);
    s_persona_ctx[sizeof(s_persona_ctx) - 1] = '\0';
}

const char *dm_chat_ai_ask(const char *user_text)
{
  char esc[400];
  char persona[1024];
  char hist[1400];
  char body[2400];
  const char *persona_src;
  FILE *f;
  int n;
  int write_ok;
  int close_rc;
  int curl_rc;
  int intent_result;
  bool private_context;

  s_last_reply[0] = '\0';
  s_last_request_remote_ok = 0;
  s_last_request_intent_ok = 0;
  s_last_reply_health = 0;
  s_last_reply_local = 0;
  if (!user_text || !user_text[0])
    {
      return local_reply("");
    }

  intent_result = dm_assistant_intent_try(user_text, s_last_reply,
                                           sizeof(s_last_reply));
  if (intent_result > 0)
    {
      s_last_request_intent_ok = 1;
      s_last_reply_local = intent_result == 4;
      if (s_last_reply_local) return s_last_reply;
      s_last_reply_health = intent_result == 3 || intent_result == 5;
      /* Deterministic health queries never become model conversation facts. */
      if (intent_result == 5) return s_last_reply;
      if (s_last_reply_health) dm_chat_hist_push_health(user_text, s_last_reply);
      else dm_chat_hist_push(user_text, s_last_reply);
      return s_last_reply;
    }

  if (!dm_wifi_connected())
    {
      strncpy(s_last_reply, local_reply(user_text), sizeof(s_last_reply) - 1);
      s_last_reply[sizeof(s_last_reply) - 1] = '\0';
      return s_last_reply;
    }

  /* Do not send HTTPS while the board clock is still at its boot epoch;
   * this causes mbedTLS to reject valid certificates as not-yet-valid. */
  if (dm_wifi_wait_time_sync(40) != 0)
    {
      syslog(LOG_WARNING, "[dm-chat] system clock not synchronized\n");
      strncpy(s_last_reply, local_reply(user_text), sizeof(s_last_reply) - 1);
      s_last_reply[sizeof(s_last_reply) - 1] = '\0';
      return s_last_reply;
    }

  if (json_escape(user_text, esc, sizeof(esc)) < 0)
    {
      return local_reply(user_text);
    }

  private_context = dm_ai_permission_allowed(DM_AI_READ_HEALTH);
  persona_src = private_context &&
                s_persona_ctx[0] ? s_persona_ctx
                                 : "你是openvela小助手，桌边陪伴助手，回答简洁温暖，不超过80字，用中文。只提供建议；不要声称已启动计时、保存备忘或创建提醒，除非应用明确反馈执行成功。遇到情绪困扰时给予温和、非诊断性的支持。";
  if (json_escape(persona_src, persona, sizeof(persona)) < 0)
    {
      return local_reply(user_text);
    }

  dm_chat_hist_json_for_cloud(hist, sizeof(hist), private_context);
  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"system\",\"content\":\"%s\"},"
               "%s"
               "{\"role\":\"user\",\"content\":\"%s\"}],"
               "\"max_completion_tokens\":400,\"thinking\":{\"type\":\"disabled\"}}",
               MIMO_MODEL, persona, hist, esc);
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return local_reply(user_text);
    }

  f = fopen(CHAT_REQ_PATH, "w");
  if (!f)
    {
      syslog(LOG_ERR, "[dm-chat] cannot create request body errno=%d\n", errno);
      return local_reply(user_text);
    }
  write_ok = fwrite(body, 1, (size_t)n, f) == (size_t)n;
  close_rc = fclose(f);
  if (!write_ok || close_rc != 0)
    {
      syslog(LOG_ERR, "[dm-chat] cannot write request body\n");
      return local_reply(user_text);
    }

  unlink(CHAT_RSP_PATH);
  if (private_context && !dm_ai_permission_allowed(DM_AI_READ_HEALTH))
    return local_reply("权限已更新");
  curl_rc = run_curl_command(MIMO_BASE "/chat/completions",
                             CHAT_REQ_PATH, CHAT_RSP_PATH, 20);
  if (curl_rc != 0)
    {
      syslog(LOG_WARNING,
             "[dm-chat] HTTP request failed; inspect curl output/status above\n");
      strncpy(s_last_reply, local_reply(user_text),
              sizeof(s_last_reply) - 1);
      s_last_reply[sizeof(s_last_reply) - 1] = '\0';
      return s_last_reply;
    }
  syslog(LOG_INFO, "[dm-chat] HTTP request completed\n");

  extract_content(CHAT_RSP_PATH, s_last_reply, sizeof(s_last_reply));
  if (s_last_reply[0])
    {
      s_last_request_remote_ok = 1;
      s_last_reply_health = private_context;
      chat_hist_push(user_text, s_last_reply, private_context);
    }
  else
    {
      strncpy(s_last_reply, local_reply(user_text),
              sizeof(s_last_reply) - 1);
      s_last_reply[sizeof(s_last_reply) - 1] = '\0';
    }
  return s_last_reply;
}

const char *dm_chat_local_reply(const char *user_text)
{
  return local_reply(user_text);
}


/* ---- dictionary AI definition ---- */
int dm_dict_ai_define(const char *word, char *out, int cap)
{
  char esc[96];
  char body[700];
  FILE *f;
  int n;

  if (!word || !word[0] || !out || cap < 8)
    {
      return -1;
    }
  out[0] = '\0';
  if (!dm_wifi_connected() || dm_wifi_wait_time_sync(40) != 0)
    {
      return -1;
    }
  if (json_escape(word, esc, sizeof(esc)) < 0)
    {
      return -1;
    }
  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"system\",\"content\":\"你是专业词典助手，用中文简洁解释单词，不超过100字。\"},"
               "{\"role\":\"user\",\"content\":\"解释单词 %s：词性、常见释义、一个例句。\"}],"
               "\"max_completion_tokens\":180,\"thinking\":{\"type\":\"disabled\"}}",
               MIMO_MODEL, esc);
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return -1;
    }
  f = fopen(CHAT_REQ_PATH, "w");
  if (!f)
    {
      return -1;
    }
  fwrite(body, 1, (size_t)n, f);
  fclose(f);
  unlink(CHAT_RSP_PATH);
  if (run_curl_command(MIMO_BASE "/chat/completions", CHAT_REQ_PATH,
                       CHAT_RSP_PATH, 18) != 0)
    {
      return -1;
    }
  extract_content(CHAT_RSP_PATH, out, (size_t)cap);
  return out[0] ? 0 : -1;
}

#endif /* CONFIG_DESKMATE_APP */
