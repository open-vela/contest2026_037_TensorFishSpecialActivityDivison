#include <nuttx/config.h>
#include "dm_ai_permissions.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CONFIG_DESKMATE_APP

#define PERMISSIONS_PATH "/data/flowdesk_ai_permissions.txt"
#define PERMISSIONS_TMP_PATH PERMISSIONS_PATH ".tmp"
#define PERMISSIONS_VERSION "FLOWDESK_AI_PERMISSIONS_V1"
#define PERMISSIONS_MASK ((UINT32_C(1) << DM_AI_PERMISSION_COUNT) - 1)

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
/* Store denied bits, so newly introduced capabilities also default to on. */
static uint32_t s_denied;

static const char *const s_names[DM_AI_PERMISSION_COUNT] = {
  "AI 本地操作", "打开应用/页面", "专注计时", "待办事项", "闹钟提醒",
  "备忘记录", "倒数日", "服药管理", "心情记录", "2048",
  "猜数字", "24 点", "MBTI 测试", "本地陪伴建议", "读取健康记录",
  "读取本地生活数据", "删除本地生活数据", "删除健康记录"
};

static const char *const s_explanations[DM_AI_PERMISSION_COUNT] = {
  [DM_AI_LOCAL] = "作用范围：所有 AI 本地功能、健康统计读取和本地生活数据读取的总开关。\n\n关闭后：AI 发起的本地操作会被拦截；普通语音识别和云端聊天仍可使用。\n\n不影响：用户手动操作、已经保存的提醒和查看 AI操作记录。下方开关会保留原设置，重新开启总开关后恢复生效。",
  [DM_AI_OPEN_APPS] = "允许：AI 打开功能页面。创建待办、闹钟、备忘等操作会跳转页面，因此也需要本权限。\n\n关闭后：这些需要页面跳转的 AI 操作会在执行前被拦截，避免只保存一半。\n\n不影响：用户手动切换页面；不跳页的本地生活数据查询仍由读取权限单独控制。",
  [DM_AI_FOCUS] = "允许：AI 根据用户说出的时长启动专注倒计时。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n关闭后：不能通过 AI 启动专注；已有计时和手动计时不受影响。本开关不授予读取历史健康统计的权限。",
  [DM_AI_TODO] = "允许：AI 添加带时间的待办，例如“30分钟之后有个会，帮我记一下”。\n\n关联权限：AI 本地操作、打开应用/页面；设备时间需要已校准。\n\n本权限只控制添加。读取由“读取本地生活数据”控制，删除还需“删除本地生活数据”，可以分别开关。",
  [DM_AI_ALARM] = "允许：AI 创建相对时间、单次或每天重复的闹钟。\n\n关联权限：AI 本地操作、打开应用/页面；设备时间需要已校准。\n\n已有闹钟的名称、时间和重复设置由“读取本地生活数据”控制。关闭添加权限不会停止已有闹钟，也不影响手动设置。",
  [DM_AI_NOTE] = "允许：AI 将用户主动说出的内容保存到备忘。\n\n关联权限：AI 本地操作、打开应用/页面。当前最多8条，每条最多31字节；过长时会拒绝保存。\n\n不授予读取已有备忘的权限。读取由“读取本地生活数据”控制，添加与读取可分别开关。",
  [DM_AI_COUNTDOWN] = "允许：AI 保存事件名称、目标日期及可用提醒时刻到倒数日。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n关闭后：AI 不能新增倒数日，手动操作及已有记录不受影响。已有名称和日期的读取由“读取本地生活数据”控制。",
  [DM_AI_MEDICATION] = "允许：AI 打开吃药页，或按用户主动说出的已登记药名进行本地匹配和服药打卡。\n\n关联权限：AI 本地操作、打开应用/页面。药名不明确、尚未到时段或重复打卡时会拒绝修改。\n\n本开关不授予新增药品或删除能力。删除指定药品今天的打卡由独立的“删除健康记录”和“读取健康记录”共同控制；药品及用药计划保留。已有统计的读取仍由健康读取权限控制。",
  [DM_AI_MOOD_RECORD] = "允许：AI 保存用户主动描述的心情，或打开心情选择页让用户手动保存。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n不等于允许读取已有健康记录。关闭健康读取后，仍可保存用户主动表达的心情，已有记录详情不会返回给 AI。",
  [DM_AI_GAME_2048] = "允许：AI 打开2048游戏页面。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n关闭后：AI 不能打开2048，游戏选择对话也不会提供此入口。用户手动进入游戏和本地分数保存不受影响。",
  [DM_AI_GAME_GUESS] = "允许：AI 打开猜数字游戏页面。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n关闭后：AI 不能打开猜数字，游戏选择对话也不会提供此入口。用户手动进入游戏不受影响，不会上传游戏过程。",
  [DM_AI_GAME_24] = "允许：AI 打开24点游戏页面。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n关闭后：AI 不能打开24点，游戏选择对话也不会提供此入口。手动进入游戏及本地计算提示不受影响。",
  [DM_AI_MBTI] = "允许：AI 打开MBTI测试入口。\n\n关联权限：AI 本地操作、打开应用/页面。\n\n关闭后：不能通过 AI 打开测试。手动测试不受影响；本权限不表示允许上传测试答案或结果。测试仅供娱乐性自测。",
  [DM_AI_COMPANION] = "允许：本地问候、情绪支持建议和小游戏选择对话。\n\n关联权限：AI 本地操作；跳页时另需打开应用/页面。打开具体游戏还要对应游戏权限。\n\n建议如需读取当日心情、专注、喝水或服药统计，还必须允许“读取健康记录”。关闭本开关不关闭普通云端聊天。",
  [DM_AI_READ_HEALTH] = "可读取：今日喝水杯数、今日保存的最近一条心情及其记录时间，以及心情汇总、健康分、专注分钟、服药统计。\n\n“喝了多少杯水”“最近一条心情是什么时候”直接查询本机数据，不让云端模型编造；没有保存的数据或历史日期会明确说明。健康查询不写入云端对话历史；开启本权限时可用云端语音播报结果。\n\n关闭后：访问数据前拦截，禁止新的健康数据上传和播报，并隐藏聊天页健康结果；删除健康记录也会拦截。保留用户主动聊天、录音、保存心情和手动查看。已经发送的请求无法撤回。",
  [DM_AI_READ_LOCAL_DATA] = "可读取：\n1. 已有备忘正文。\n2. 当前待办内容、到期时间和状态。\n3. 闹钟名称、时间和重复设置。\n4. 倒数日名称和目标日期。\n\n示例：“读一下备忘”“我有哪些待办”“查看闹钟列表”“查询倒数日”。结果只在设备显示，不加入云端对话历史或播报；过长时提示到对应页面查看。\n\n关闭后：访问数据前拦截，隐藏聊天页已有查询结果；删除这些记录也会拦截。\n\n与添加权限独立，不影响手动操作。不包含健康、录音、会议音频或系统密码文件。日志只记类别和结果，不记正文。",
  [DM_AI_DELETE_LOCAL_DATA] = "允许删除：指定的一条备忘、待办、闹钟或倒数日。\n\n示例：“删除第2条备忘”“删除待办‘开会’”。可按当前列表编号或完整名称精确匹配；重名、目标不明、批量清空不会猜测执行。正在显示提醒的那条记录需先手动结束提醒。\n\n同时需要：AI 本地操作、读取本地生活数据、打开应用/页面，以及本删除开关。任何一项关闭都会在查找和删除前拦截，不需要添加权限。\n\n删除保存成功后打开对应的备忘、待办、闹钟或倒数日页。保存失败会回滚并停留原页面，不报告成功。日志记录类别、方式和结果，不记录正文。手动删除和 AI 添加不受本开关影响。",
  [DM_AI_DELETE_HEALTH] = "允许删除：今日指定编号或最近一条心情；今日喝水杯数的指定减量或整项清零；指定药品今天的服药打卡。\n\n示例：“删除最近一条心情记录”“删除今天2杯喝水记录”“删除今天的喝水记录”“删除服药记录‘维生素’”。服药删除只清除该药今天的打卡，保留药品与用药计划。喝水只有日累计，不能按第几条杯数历史删除。\n\n同时需要：AI 本地操作、读取健康记录、打开应用/页面，以及本删除开关，且时间已校准。任何一项关闭都会在查找和删除前拦截。\n\n删除成功后：心情打开健康页，喝水打开喝水页，服药打卡打开吃药页。失败不跳页，保存失败回滚。不删除其他日期、整份健康库、药品计划或未指明的记录。日志不记健康正文，手动操作不受影响。"
};

static bool valid_permission(dm_ai_permission_t permission)
{
  return permission >= 0 && permission < DM_AI_PERMISSION_COUNT;
}

static bool read_denied_mask(FILE *file, uint32_t *denied)
{
  char version[32];
  char number[16];
  char extra;
  const char *digit;
  uint32_t value = 0;
  int fields = fscanf(file, "%31s %15s %c", version, number, &extra);
  if (fields != 2 || ferror(file) ||
      strcmp(version, PERMISSIONS_VERSION) != 0) return false;

  for (digit = number; *digit; digit++)
    {
      unsigned int n;
      if (*digit < '0' || *digit > '9') return false;
      n = (unsigned int)(*digit - '0');
      if (value > (UINT32_MAX - n) / 10u) return false;
      value = value * 10u + n;
    }
  if (value & ~PERMISSIONS_MASK) return false;
  *denied = value;
  return true;
}

void dm_ai_permissions_init(void)
{
  FILE *file;
  uint32_t denied;
  bool valid;
  int close_rc;

  pthread_mutex_lock(&s_lock);
  s_denied = 0;
  file = fopen(PERMISSIONS_PATH, "r");
  if (file)
    {
      valid = read_denied_mask(file, &denied);
      close_rc = fclose(file);
      s_denied = valid && close_rc == 0 ? denied : PERMISSIONS_MASK;
    }
  else if (errno != ENOENT)
    {
      /* An unreadable configuration must not silently grant old denials. */
      s_denied = PERMISSIONS_MASK;
    }
  pthread_mutex_unlock(&s_lock);
}

bool dm_ai_permission_enabled(dm_ai_permission_t permission)
{
  bool enabled;
  if (!valid_permission(permission)) return false;
  pthread_mutex_lock(&s_lock);
  enabled = (s_denied & (UINT32_C(1) << permission)) == 0;
  pthread_mutex_unlock(&s_lock);
  return enabled;
}

bool dm_ai_permission_allowed(dm_ai_permission_t permission)
{
  bool allowed;
  uint32_t required;
  if (!valid_permission(permission)) return false;
  required = (UINT32_C(1) << DM_AI_LOCAL) | (UINT32_C(1) << permission);
  pthread_mutex_lock(&s_lock);
  allowed = (s_denied & required) == 0;
  pthread_mutex_unlock(&s_lock);
  return allowed;
}

int dm_ai_permission_set(dm_ai_permission_t permission, bool enabled)
{
  FILE *file;
  uint32_t next;
  int rc = -1;
  int write_ok;
  int close_rc;

  if (!valid_permission(permission)) return -1;
  pthread_mutex_lock(&s_lock);
  next = enabled ? s_denied & ~(UINT32_C(1) << permission)
                 : s_denied | (UINT32_C(1) << permission);
  if (next == s_denied)
    {
      pthread_mutex_unlock(&s_lock);
      return 0;
    }
  if (mkdir("/data", 0755) != 0 && errno != EEXIST)
    {
      pthread_mutex_unlock(&s_lock);
      return -1;
    }
  file = fopen(PERMISSIONS_TMP_PATH, "w");
  if (file)
    {
      write_ok = fprintf(file, "%s %lu\n", PERMISSIONS_VERSION,
                         (unsigned long)next) > 0;
      close_rc = fclose(file);
      if (write_ok && close_rc == 0 &&
          rename(PERMISSIONS_TMP_PATH, PERMISSIONS_PATH) == 0)
        {
          s_denied = next;
          rc = 0;
        }
      else unlink(PERMISSIONS_TMP_PATH);
    }
  pthread_mutex_unlock(&s_lock);
  return rc;
}

const char *dm_ai_permission_name(dm_ai_permission_t permission)
{
  return valid_permission(permission) ? s_names[permission] : "未知操作";
}

const char *dm_ai_permission_explanation(dm_ai_permission_t permission)
{
  return valid_permission(permission) && s_explanations[permission]
           ? s_explanations[permission] : "暂无此权限的详细说明。";
}

#endif
