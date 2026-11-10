#include <nuttx/config.h>
#include "dm_ai_log.h"

#ifdef CONFIG_DESKMATE_APP
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_PATH "/data/flowdesk_ai_activity.txt"
#define LOG_VERSION "FLOWDESK_AI_ACTIVITY_V1\n"

extern int dm_wifi_clock_valid(void);

static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static dm_ai_log_entry_t s_entries[DM_AI_LOG_MAX];
static size_t s_count;
static uint32_t s_sequence;
static uint32_t s_revision;
static bool s_persisted = true;

static const struct {
  const char *name;
  const char *target;
  const char *method;
} s_actions[DM_AI_LOG_ACTION_COUNT] = {
  {"启动专注", "专注计时器（设置）", "设置时长并打开专注倒计时"},
  {"添加待办", "待办列表与提醒时间（写入）", "保存待办、登记定时提醒并打开待办页"},
  {"设置闹钟", "闹钟列表与响铃时间（写入）", "保存闹钟计划并打开闹钟页"},
  {"添加备忘", "备忘列表（写入；正文不记录）", "保存一条本地备忘并打开备忘页"},
  {"添加倒数日", "倒数日列表与目标日期（写入）", "保存事件日期并打开倒数日页"},
  {"打开吃药页", "吃药页面（打开）", "切换到吃药页供用户查看"},
  {"记录服药", "已登记药品与时段（本地匹配／写入）", "本地匹配药名及服用时段，保存打卡；药名与记录正文不记入日志"},
  {"记录心情", "心情记录（写入）或心情选择页", "识别用户主动描述并保存心情，或打开手动记录页；情绪正文不记录"},
  {"本地陪伴建议", "用户主动表达（本地处理）", "生成本地建议；健康统计读取另列记录"},
  {"选择小游戏", "已允许的游戏入口", "检查游戏权限，显示可选择的小游戏"},
  {"打开2048", "2048 游戏页面", "检查权限后切换到2048"},
  {"打开猜数字", "猜数字游戏页面", "检查权限后切换到猜数字"},
  {"打开24点", "24点游戏页面", "检查权限后切换到24点"},
  {"退出游戏选择", "本地游戏选择状态", "取消等待选择；允许页面访问时返回聊天页"},
  {"打开MBTI", "MBTI 测试页面", "检查权限后打开测试入口"},
  {"本地问候", "预置问候与音色", "使用本地问候及预置音频"},
  {"取消本次操作", "本次语音操作", "停止本次新建或修改，不删除已有记录"},
  {"请求删除记录", "用户指定的数据类别（内容隐藏）", "检查删除类别和定位条件；目标不明确或不支持的范围不执行"},
  {"提醒时间检查", "用户提供的提醒时间", "检查设备时间与提醒时间有效性"},
  {"读取健康统计", "已有心情汇总、专注分钟、喝水杯数、服药统计（读取）", "在设备本地读取统计用于建议；不记录数值、药名或健康正文"},
  {"打开心情记录页", "手动心情选择页面（打开）", "打开选择页面等待用户保存；本次尚未写入心情记录"},
  {"读取备忘", "已有备忘正文（读取；正文不记入日志）", "在设备本地查询并显示，不加入云端对话历史或云端播报"},
  {"读取待办", "当前待办内容、到期时间和状态（读取）", "在设备本地查询并显示，不加入云端对话历史或云端播报"},
  {"读取闹钟", "闹钟名称、时间和重复设置（读取）", "在设备本地查询并显示，不加入云端对话历史或云端播报"},
  {"读取倒数日", "事件名称和目标日期（读取）", "在设备本地查询并显示，不加入云端对话历史或云端播报"},
  {"查询今日喝水", "今日已保存的喝水杯数（读取）", "检查健康读取权限后查询本机累计；不调用聊天模型，不记入对话历史；允许时语音播报"},
  {"查询最近心情", "今日保存的最近一条心情及时间（读取）", "检查健康读取权限后查询本机记录；无记录时明确返回，不编造时间；允许时语音播报"},
  {"删除指定备忘", "用户指定的一条备忘（删除；正文隐藏）", "先检查读取、删除及页面权限；精确定位并原子保存，失败回滚，成功后打开备忘页"},
  {"删除指定待办", "用户指定的一条待办（删除；正文隐藏）", "先检查读取、删除及页面权限；精确定位，响铃中拒绝，保存失败回滚，成功后打开待办页"},
  {"删除指定闹钟", "用户指定的一条闹钟（删除；正文隐藏）", "先检查读取、删除及页面权限；精确定位，响铃中拒绝，保存失败回滚，成功后打开闹钟页"},
  {"删除指定倒数日", "用户指定的一条倒数日（删除；正文隐藏）", "先检查读取、删除及页面权限；精确定位，提醒中拒绝，保存失败回滚，成功后打开倒数日页"},
  {"删除指定心情", "今日指定编号或最近一条心情（删除；正文隐藏）", "先检查健康读取、删除、页面权限及时间；精确删除一条，失败回滚，成功后打开健康页"},
  {"删除喝水记录", "今日指定杯数或今日累计（删除；数值隐藏）", "先检查健康读取、删除、页面权限及日期；指定减量或清零，失败回滚，成功后打开喝水页"},
  {"删除服药打卡", "指定药品今天的服药打卡（删除；药名隐藏）", "先检查健康读取、删除、页面权限及日期；清除今日打卡并保留药品计划，失败回滚，成功后打开吃药页"}
};

static bool valid_entry(const dm_ai_log_entry_t *e)
{
  return e->sequence > 0 && e->action >= 0 && e->action < DM_AI_LOG_ACTION_COUNT &&
         e->result >= 0 && e->result < DM_AI_LOG_RESULT_COUNT &&
         e->reason >= 0 && e->reason < DM_AI_LOG_REASON_COUNT &&
         e->permission >= 0 && e->permission <= DM_AI_PERMISSION_COUNT &&
         e->seconds >= 0 && e->seconds <= 7 * 24 * 60 * 60;
}

static void append_memory(const dm_ai_log_entry_t *entry)
{
  if (s_count == DM_AI_LOG_MAX)
    {
      memmove(s_entries, s_entries + 1, (DM_AI_LOG_MAX - 1) * sizeof(*s_entries));
      s_count--;
    }
  s_entries[s_count++] = *entry;
}

static int save_locked(void)
{
  FILE *file;
  int fd;
  int ok;
  size_t i;

  if (mkdir("/data", 0755) != 0 && errno != EEXIST) return -1;
  fd = open(LOG_PATH ".tmp", O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  file = fdopen(fd, "w");
  if (!file) { close(fd); unlink(LOG_PATH ".tmp"); return -1; }
  ok = fputs(LOG_VERSION, file) >= 0;
  for (i = 0; i < s_count && ok; i++)
    {
      const dm_ai_log_entry_t *e = &s_entries[i];
      ok = fprintf(file, "%lu %lld %u %u %u %u %u %d %d\n",
                   (unsigned long)e->sequence, (long long)e->timestamp,
                   (unsigned)e->clock_valid, (unsigned)e->action,
                   (unsigned)e->result, (unsigned)e->reason,
                   e->permission == DM_AI_PERMISSION_COUNT ? UINT32_MAX :
                     (unsigned)e->permission, e->seconds, e->operation_rc) > 0;
    }
  if (fclose(file) != 0) ok = 0;
  if (ok && rename(LOG_PATH ".tmp", LOG_PATH) == 0) return 0;
  unlink(LOG_PATH ".tmp");
  return -1;
}

void dm_ai_log_init(void)
{
  FILE *file;
  char line[192];
  pthread_mutex_lock(&s_log_lock);
  s_count = 0;
  s_sequence = 0;
  s_persisted = true;
  file = fopen(LOG_PATH, "r");
  if (file)
    {
      if (!fgets(line, sizeof(line), file) || strcmp(line, LOG_VERSION) != 0)
        s_persisted = false;
      else while (fgets(line, sizeof(line), file))
        {
          dm_ai_log_entry_t e;
          unsigned long sequence;
          long long stamp;
          unsigned clock_valid, action, result, reason, permission;
          char extra;
          memset(&e, 0, sizeof(e));
          if (!strchr(line, '\n') ||
              sscanf(line, "%lu %lld %u %u %u %u %u %d %d %c",
                     &sequence, &stamp, &clock_valid, &action, &result,
                     &reason, &permission, &e.seconds, &e.operation_rc, &extra) != 9 ||
              sequence > UINT32_MAX || clock_valid > 1 || stamp < 0 ||
              (long long)(time_t)stamp != stamp)
            { s_persisted = false; break; }
          e.sequence = (uint32_t)sequence;
          e.timestamp = (time_t)stamp;
          e.clock_valid = clock_valid != 0;
          e.action = (dm_ai_log_action_t)action;
          e.result = (dm_ai_log_result_t)result;
          e.reason = (dm_ai_log_reason_t)reason;
          /* Old files stored the then-current permission COUNT as a sentinel.
           * Only a real permission denial may contain a permission ID. */
          e.permission = permission == UINT32_MAX || result != DM_AI_LOG_DENIED ||
                         reason != DM_AI_LOG_REASON_PERMISSION
                           ? DM_AI_PERMISSION_COUNT : (dm_ai_permission_t)permission;
          if (!valid_entry(&e) || e.sequence <= s_sequence)
            { s_persisted = false; break; }
          append_memory(&e);
          s_sequence = e.sequence;
        }
      if (ferror(file)) s_persisted = false;
      if (fclose(file) != 0) s_persisted = false;
    }
  else if (errno != ENOENT) s_persisted = false;
  s_revision++;
  pthread_mutex_unlock(&s_log_lock);
}

void dm_ai_log_add(dm_ai_log_action_t action, dm_ai_log_result_t result,
                   dm_ai_log_reason_t reason, dm_ai_permission_t permission,
                   int seconds, int operation_rc)
{
  dm_ai_log_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.action = action;
  entry.result = result;
  entry.reason = reason;
  entry.permission = permission;
  entry.seconds = seconds;
  entry.operation_rc = operation_rc;
  entry.clock_valid = dm_wifi_clock_valid() != 0;
  entry.timestamp = entry.clock_valid ? time(NULL) : 0;
  if (entry.timestamp < 0) { entry.timestamp = 0; entry.clock_valid = false; }
  pthread_mutex_lock(&s_log_lock);
  entry.sequence = s_sequence == UINT32_MAX ? 1 : s_sequence + 1;
  if (!valid_entry(&entry)) { pthread_mutex_unlock(&s_log_lock); return; }
  if (s_sequence == UINT32_MAX) s_count = 0;
  s_sequence = entry.sequence;
  append_memory(&entry);
  /* An executed action must remain visible even if disk persistence fails. */
  s_persisted = save_locked() == 0;
  s_revision++;
  pthread_mutex_unlock(&s_log_lock);
}

size_t dm_ai_log_snapshot(dm_ai_log_entry_t *out, size_t cap,
                           uint32_t *revision, bool *persisted)
{
  size_t n, i;
  pthread_mutex_lock(&s_log_lock);
  n = out ? (cap < s_count ? cap : s_count) : 0;
  for (i = 0; i < n; i++) out[i] = s_entries[s_count - 1 - i];
  if (revision) *revision = s_revision;
  if (persisted) *persisted = s_persisted;
  pthread_mutex_unlock(&s_log_lock);
  return n;
}

uint32_t dm_ai_log_revision(void)
{
  uint32_t value;
  pthread_mutex_lock(&s_log_lock);
  value = s_revision;
  pthread_mutex_unlock(&s_log_lock);
  return value;
}

const char *dm_ai_log_action_name(dm_ai_log_action_t action)
{
  return action >= 0 && action < DM_AI_LOG_ACTION_COUNT ? s_actions[action].name : "未知操作";
}

const char *dm_ai_log_result_name(dm_ai_log_result_t result)
{
  static const char *const names[] = {"成功", "失败", "被拦截", "未执行"};
  return result >= 0 && result < DM_AI_LOG_RESULT_COUNT ? names[result] : "未知";
}

void dm_ai_log_time(const dm_ai_log_entry_t *entry, char *out, size_t cap)
{
  struct tm tm;
  if (!out || !cap) return;
  if (!entry->clock_valid || !localtime_r(&entry->timestamp, &tm))
    snprintf(out, cap, "%s", "时间未校准");
  else
    snprintf(out, cap, "%02d-%02d %02d:%02d:%02d", tm.tm_mon + 1,
             tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void dm_ai_log_detail(const dm_ai_log_entry_t *entry, char *out, size_t cap)
{
  static const char *const reasons[DM_AI_LOG_REASON_COUNT] = {
    "本地功能返回成功", "权限关闭，检查阶段已拦截，未调用该操作",
    "列表容量已满", "记录保存失败", "设备时间尚未校准",
    "时间、输入或设备状态无效", "内容超过存储长度限制",
    "已有任务正在运行或设备忙", "没有登记药品或药名不明确",
    "没有可打卡时段或该时段已记录", "请求的目标、范围或操作方式不支持，未执行",
    "用户取消本次操作，未修改已有记录", "本地模块返回失败，请结合返回码核对",
    "2048、猜数字与24点权限均关闭，没有可用游戏入口",
    "没有找到指定记录或指定范围无可删除数据", "存在同名记录，目标不唯一，未执行删除"
  };
  char when[48], permission[96], duration[64];
  const char *execution;
  if (!out || !cap) return;
  if (!valid_entry(entry)) { snprintf(out, cap, "%s", "无效记录"); return; }
  dm_ai_log_time(entry, when, sizeof(when));
  permission[0] = duration[0] = '\0';
  if (entry->permission < DM_AI_PERMISSION_COUNT)
    snprintf(permission, sizeof(permission), "\n被关闭权限：%s", dm_ai_permission_name(entry->permission));
  if (entry->seconds > 0)
    snprintf(duration, sizeof(duration), "\n请求时长：%d分%02d秒", entry->seconds / 60, entry->seconds % 60);
  execution = entry->result == DM_AI_LOG_DENIED ? "权限检查拦截，未调用该操作" :
              entry->result == DM_AI_LOG_SKIPPED ? "未执行数据更改" :
              entry->result == DM_AI_LOG_FAILED ? "本地功能返回失败" : "本地功能已完成";
  snprintf(out, cap, "记录 #%lu\n时间：%s\n来源：AI 本地助手\n操作：%s\n访问对象：%s\n操作方式：%s%s\n执行情况：%s\n结果：%s\n原因：%s%s\n模块返回码：%d\n\n仅保存操作元数据，不含聊天、密码或健康记录正文。",
           (unsigned long)entry->sequence, when, s_actions[entry->action].name,
           s_actions[entry->action].target, s_actions[entry->action].method,
           duration, execution, dm_ai_log_result_name(entry->result),
           reasons[entry->reason], permission, entry->operation_rc);
}
#endif
