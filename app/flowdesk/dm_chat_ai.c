/****************************************************************************
 * dm_chat_ai.c — MiMo cloud chat + local offline fallback
 ****************************************************************************/

#include "deskmate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#ifdef CONFIG_DESKMATE_APP

/* MiMo OpenAI-compatible endpoint (user-provided key for this device). */
#define MIMO_BASE "https://token-plan-sgp.xiaomimimo.com/v1"
#define MIMO_KEY  "tp-s8f46wpkkkdpqika5xchqalgg7m632kp0n9wdd9omjlnn64c"
#define MIMO_MODEL "mimo-v2.6-flash"
#define CHAT_REQ_PATH "/tmp/dm_chat_req.json"
#define CHAT_RSP_PATH "/tmp/dm_chat_rsp.json"
#define CHAT_MAX_REPLY 240

static int run_curl_script(const char *line)
{
  FILE *f = fopen("/tmp/dm_curl.sh", "w");
  int rc;
  if (!f)
    {
      return -1;
    }
  fputs("#!/bin/sh\n", f);
  fputs(line, f);
  fputs("\n", f);
  fclose(f);
  rc = system("sh /tmp/dm_curl.sh");
  return rc == 0 ? 0 : -1;
}

static char s_last_reply[CHAT_MAX_REPLY];
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

static void extract_content(const char *path, char *out, size_t cap)
{
  FILE *f;
  char *buf;
  long sz;
  char *p;
  char *q;

  out[0] = '\0';
  f = fopen(path, "rb");
  if (!f)
    {
      return;
    }
  fseek(f, 0, SEEK_END);
  sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 64 * 1024)
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

  p = strstr(buf, "\"content\"");
  if (!p)
    {
      free(buf);
      return;
    }
  p = strchr(p + 9, '"');
  if (!p)
    {
      free(buf);
      return;
    }
  q = strchr(p + 1, '"');
  if (!q)
    {
      free(buf);
      return;
    }
  {
    size_t n = (size_t)(q - (p + 1));
    if (n >= cap)
      {
        n = cap - 1;
      }
    memcpy(out, p + 1, n);
    out[n] = '\0';
    /* unescape \" and \\ and \n lightly */
    {
      char *r = out;
      char *w = out;
      while (*r)
        {
          if (r[0] == '\\' && r[1] == 'n')
            {
              *w++ = ' ';
              r += 2;
            }
          else if (r[0] == '\\' && r[1] == '"')
            {
              *w++ = '"';
              r += 2;
            }
          else if (r[0] == '\\' && r[1] == '\\')
            {
              *w++ = '\\';
              r += 2;
            }
          else
            {
              *w++ = *r++;
            }
        }
      *w = '\0';
    }
  }
  free(buf);
}

int dm_chat_ai_available(void)
{
  return dm_wifi_connected() ? 1 : 0;
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
  char body[900];
  char cmd[1200];
  FILE *f;
  int n;

  s_last_reply[0] = '\0';
  if (!user_text || !user_text[0])
    {
      return local_reply("");
    }

  if (!dm_wifi_connected())
    {
      strncpy(s_last_reply, local_reply(user_text), sizeof(s_last_reply) - 1);
      s_last_reply[sizeof(s_last_reply) - 1] = '\0';
      return s_last_reply;
    }

  if (json_escape(user_text, esc, sizeof(esc)) < 0)
    {
      return local_reply(user_text);
    }

  n = snprintf(body, sizeof(body),
               "{\"model\":\"%s\",\"messages\":["
               "{\"role\":\"system\",\"content\":\"%s\"},"
               "{\"role\":\"user\",\"content\":\"%s\"}],"
               "\"temperature\":0.7,\"max_tokens\":200}",
               MIMO_MODEL,
               s_persona_ctx[0] ? s_persona_ctx
                                 : "你是openvela小助手，桌边陪伴助手，回答简洁温暖，"
                                   "不超过80字，用中文。",
               esc);
  if (n <= 0 || n >= (int)sizeof(body))
    {
      return local_reply(user_text);
    }

  f = fopen(CHAT_REQ_PATH, "w");
  if (!f)
    {
      return local_reply(user_text);
    }
  fwrite(body, 1, (size_t)n, f);
  fclose(f);

  unlink(CHAT_RSP_PATH);
  snprintf(cmd, sizeof(cmd),
           "curl -sS --cacert /resource/etc/ssl/curl/ca-certificates.crt -m 20 -X POST %s/chat/completions "
           "-H 'Authorization: Bearer %s' "
           "-H 'Content-Type: application/json' "
           "-d @%s -o %s",
           MIMO_BASE, MIMO_KEY, CHAT_REQ_PATH, CHAT_RSP_PATH);
  if (run_curl_script(cmd) != 0)
    {
      strncpy(s_last_reply, local_reply(user_text),
              sizeof(s_last_reply) - 1);
      s_last_reply[sizeof(s_last_reply) - 1] = '\0';
      return s_last_reply;
    }

  extract_content(CHAT_RSP_PATH, s_last_reply, sizeof(s_last_reply));
  if (!s_last_reply[0])
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
  char cmd[1100];
  FILE *f;
  int n;

  if (!word || !word[0] || !out || cap < 8)
    {
      return -1;
    }
  out[0] = '\0';
  if (!dm_wifi_connected())
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
               "\"temperature\":0.3,\"max_tokens\":180}",
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
  snprintf(cmd, sizeof(cmd),
           "curl -sS --cacert /resource/etc/ssl/curl/ca-certificates.crt -m 18 -X POST %s/chat/completions "
           "-H 'Authorization: Bearer %s' "
           "-H 'Content-Type: application/json' "
           "-d @%s -o %s",
           MIMO_BASE, MIMO_KEY, CHAT_REQ_PATH, CHAT_RSP_PATH);
  if (run_curl_script(cmd) != 0)
    {
      return -1;
    }
  extract_content(CHAT_RSP_PATH, out, (size_t)cap);
  return out[0] ? 0 : -1;
}

#endif /* CONFIG_DESKMATE_APP */
