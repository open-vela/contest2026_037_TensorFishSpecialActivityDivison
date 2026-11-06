# FlowDesk 应用场景说明（AI 硬件产品创新赛道）
# Usage scenarios of Flowdesk AI

## 一、用户故事
## 1. Target audience

**谁在用**：把 Gemini-S1（R528）放在书桌上的学生 / 远程办公者。
**Targeted audience**: Students/ Work from home employees

**场景**：
- 白天需要专注工作，又想有人轻声提醒喝水、吃药
- 想查英文单词、和桌搭聊两句，不想掏手机
- 希望设备**主动开口**提醒，而不是只会被动问答

- **Scenarios**：
- Automatic meds/ hydration reminder
- Faster easier and faster translations
- Remind you **actively**, not passively


**解决什么**：桌面小屏 + 4Ω3W 喇叭，用语音和触屏完成专注、查词、健康提醒、联网共享。

**Solved**: Using a small monitor and a 4Ω3W speaker, it can activate focus mode, translation mode, reminders using voice and touch controls.
# 二、功能清单

| 模块 | 功能 | 主动/被动 |
|------|------|-----------|
| 专注 | 15/25/45/60 + 自选倒计时、表情互动、完成页 | 完成时**主动语音** |
| 聊天 | 点按录音 → ASR → LLM → TTS 喇叭回复 | 被动 + 语音 |
| 词典 | 6.5 万词本地库、每日一词、AI 释义 | 每日一词**主动展示** |
| 健康 | 喝水打卡、吃药计划、传感数据 | 到点**主动播报** |
| 蓝牙 PAN / App 配对 | 手机网络共享、专属 BLE Challenge-Response | — |
| WiFi | 手动扫描连接（硬件异常后降级方案） | — |
| 音量 | 独立设定页，滑杆/静音/试听 | — |
| 工具 | 计算器、2048、MBTI、备忘等 | — |

## 三、「主动 + 执行」场景（赛题核心）

| 类型 | 场景 | 执行动作 |
|------|------|----------|
| **定时主动** | 每天 08:30 | 播报「今日简报」（吃药/喝水/专注提示） |
| **阈值主动** | 吃药时间到（本地 `deskmate_med.txt`） | 喇叭：「该吃药了」 |
| **阈值主动** | 喝水提醒到期 | 喇叭：「喝水时间到」 |
| **事件主动** | 专注倒计时归零 | 喇叭：「专注完成，起来活动」 |
| **执行** | 配对成功后 | 可请求手机开启蓝牙网络共享，板端 `renew bt-pan` 上网 |

对应 Skill 文件（打包在 `/resource/agent/skills/`，可拷到 `/data/agent/skills/`）：

- `reminder.md` — 提醒/待办
- `daily-briefing.md` — 每日简报
- `focus-coach.md` — 专注教练

## 四、技术实现

### 开发模式
**模式 A**：设备通信协议 + 云端大模型（MiMo API），独立 LVGL 应用 `flowdesk`。

### 语音链路
```
麦克风 /dev/audio/pcm0c
  → mimo-v2.5-asr
  → mimo-v2.6-flash（对话）
  → mimo-v2.5-tts
  → 喇叭 /dev/audio/pcm0p（4Ω 3W）
```

### 主动执行模块
`dm_proactive.c`：在 `dm_tick` 中检查吃药/喝水/简报条件，触发 TTS 播报。

### 专属 BLE 配对
Service `12345678-1234-5678-1234-56789abcdef0`，
Challenge-Response：`response = challenge XOR hash(GEMINI-S1-2026)`。

### 数据持久化
- 词库：`/resource/dict/*.txt`
- 音量：`/data/deskmate_vol.txt`
- 吃药/喝水：`/data/deskmate_med.txt`、`/data/deskmate_water.txt`

## 五、环境依赖

- openvela `dev-ai-contest-2026` + Gemini-S1（R528）
- 网络：WiFi（若 SDIO 正常）或蓝牙 PAN / App 热点共享
- 喇叭 4Ω 3W，咪头，320x240 触摸屏
