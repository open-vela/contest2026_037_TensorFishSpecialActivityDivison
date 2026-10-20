# contest2026_037_TensorFishSpecialActivityDivison

## 作品名称 / Work Name

**心流桌伴**（中文）/ **FlowDesk**（English）— openvela 智能桌搭陪伴终端 / OpenVela desk companion terminal

**Version:** 0.18.1  
**Track / 赛道:** AI 硬件产品创新 / AI Hardware Product Innovation  
**Team / 队伍:** TensorFishSpecialActivityDivison (037)  
**Author / 作者:** LR-C11

---

## 一、作品简介 / Introduction

**中文：**  
**心流桌伴（FlowDesk）** 针对桌搭产品「功能单一、缺少陪伴」的问题，基于 **Gemini-S1（全志 R528 / OpenVela）** 实现一款多功能桌面终端。本地集成专注计时、6.5 万词词典、健康情绪综合分、备忘、吃药与喝水提醒、背单词、2048、MBTI 等能力。语音聊天支持**点按录音**，完整链路为：麦克风录音 → 云端 ASR 识别 → 大模型生成回复 → TTS 合成语音 → 喇叭/蓝牙耳机播放；联网时接入小米 **MiMo**，助手自称 **openvela小助手**，断网自动降级为本地回复与离线提示音。界面以 LVGL 320×240 单屏 + 底部五栏导航组织，吉祥物贯穿专注与聊天场景。

**English:**  
**FlowDesk (心流桌伴)** is a multi-function desk companion on **Gemini-S1 (R528 / OpenVela)**. On-device features include focus timer, 65k-word dictionary, multi-factor mood score, notes, med/water reminders, word study, 2048, and MBTI. Voice chat uses **tap-to-talk**: mic capture → cloud ASR → LLM reply → TTS speech → speaker / BT headset. Online path uses Xiaomi **MiMo** (assistant: **openvela小助手**); offline falls back to local replies and alert tones. UI is LVGL 320×240 with a 5-tab dock and a shared mascot.

---

## 二、功能一览 / Feature Overview

### 导航 / Dock

专注 Focus · 聊天 Chat · 健康 Health · 功能 Features · 设定 Settings

### 详细功能 / Detailed Features

#### 专注 Focus
- 自选时长 Custom duration: 15 / 25 / 45 / 60 + custom
- 进行页 Running: 滚动 MM:SS 时钟 + 吉祥物
- 暂停 / 结束 Pause / End
- 完成页 Done: 恭喜动画、表情切换、**主动语音播报**（如「专注完成，起来活动一下」）
- 完成时长计入健康统计 Feeds health stats

#### 语音聊天 Voice Chat（重点）

**交互方式 / Interaction**

| 步骤 Step | 操作 Action | 屏幕 Screen | 按钮 Button |
|-----------|-------------|-------------|-------------|
| 1 | 点「开始说话」 | 提示「录音中… 再点一下结束」 | 变红「结束并发送」 |
| 2 | 再点一下 | 「识别中… / 发送中…」 | 「处理中…」 |
| 3 | 等待云端 | 先显示**文字回复** | — |
| 4 | 自动播报 | 「已通过喇叭播放回复」 | 恢复「开始说话」 |

- 最长录音约 12 秒，可提前结束
- 支持点吉祥物快速文本闲聊（同样走 LLM + TTS）
- 无麦克风时自动降级为文字对话

**完整链路 / Pipeline**

```text
[1] 录音
    用户点按开始/结束
    → /dev/audio/pcm0c（16kHz / 16bit / 单声道 WAV）
    → /tmp/dm_voice_in.wav

[2] 语音识别 ASR
    POST https://token-plan-sgp.xiaomimimo.com/v1/audio/transcriptions
    model = mimo-v2.5-asr
    → 用户文本

[3] 大模型对话 LLM
    POST https://token-plan-sgp.xiaomimimo.com/v1/chat/completions
    model = mimo-v2.6-flash
    人设：openvela小助手，口语化、简短（约 60 字内）
    → 回复文本（先显示在屏幕）

[4] 语音合成 TTS
    POST https://token-plan-sgp.xiaomimimo.com/v1/audio/speech
    model = mimo-v2.5-tts
    voice = 设定页所选音色（冰糖/茉莉/苏打/白桦…）
    → /tmp/dm_voice_out.wav

[5] 播放 Playback
    按「音频输出」设置：
      自动 → 有蓝牙耳机走 A2DP，否则喇叭
      喇叭 → nxplayer / play，或写 /dev/audio/pcm0p（4Ω 3W）
      耳机 → bttool a2dp play
    音量来自设定页滑杆（0–100% → 硬件 0–1000）
```

**设计说明 / Design notes**

- 网络请求在后台线程执行，不阻塞 LVGL UI
- 超时约 25s；失败提示「没听清或网络忙」
- 鉴权：`Authorization: Bearer <API_KEY>`（OpenAI 兼容）
- 文案与音色可在设定页试听切换
- 扩展：可继续用同一链路做 TTS 播报提醒（吃药/喝水/简报）

#### 健康 Health
- 情绪综合分（多因子本地计算）Multi-factor mood score
- 心情记录 Mood log
- 吃药 / 喝水提醒：到点**主动离线提示音 + 屏幕文字**，联网可语音

#### 词典 Dictionary（独立可滑动页）
- 本地词库约 **65,398** 词（BandLingo 格式 `word|definition`，a–z 分片，LRU 3 块缓存）
- 英文：只查首字母分块；中文释义：全分块扫描
- 每日一词 Word of the day
- AI 释义（`mimo-v2.6-flash`，需联网）
- 结果列表、释义详情页均可上下滑动

#### 功能页 Features（完整清单）

**学习与效率 Study**
| 中文 | English |
|------|---------|
| 词典（查词 / 每日一词 / AI） | Dictionary (lookup / daily / AI) |
| 备忘（列表/添加/拼音/持久化） | Notes (list/add/pinyin/persist) |
| 背单词 1000+（学/练/测/错题） | Word Memo 1000+ (study/quiz/wrong) |
| 计算器 | Calculator |
| 倒数日 | Countdown |

**生活健康 Life & Health**
| 中文 | English |
|------|---------|
| 吃药提醒（每天 1/2/3 次） | Med reminders (1–3×/day) |
| 喝水提醒打卡 | Water check-in |
| 吃什么随机 | What to eat |
| BMI 计算 | BMI |
| 环境传感（温湿度/光照） | Sensors (temp/humi/light) |

**游戏与测试 Games & Tests**
| 中文 | English |
|------|---------|
| 2048（滑动合并、最佳分） | 2048 (swipe merge, best score) |
| 猜数字 1–100 | Guess number |
| 24 点 | Make 24 |
| 画板涂鸦 | Doodle |
| MBTI：4 套 × 52 题，四维百分比 | MBTI: 4 banks × 52, % per dimension |

**设定 Settings（6 项，可滑动）**
| 中文 | English |
|------|---------|
| Wi-Fi：扫描 / 连接 / 开关 / 状态 | Wi-Fi: scan / connect / switch / status |
| 蓝牙耳机：A2DP 扫描/连接/断开 | BT headset: A2DP scan/connect |
| App 连接：BLE 专属 Challenge-Response 配对 | App: dedicated BLE pairing |
| 音量：滑杆 / 静音 / 试听 / **音频输出** | Volume: slider / mute / test / **output channel** |
| 播报声音：冰糖/茉莉/小云/苏打/白桦/小川/电子音/深海 | TTS voice picker (8 styles) |
| 关于：独立页，版本 + 开发者信息 | About: standalone page |

#### 吉祥物 Mascot
- 几何白色小脸，自动眨眼、注视 Geometric blinking face
- 多表情：开心 / 思考 / 害羞 / 爱心眼 / 星星眼等
- 专注页与聊天页共用 Shared on Focus & Chat

#### 自定义 Skill / 主动能力
| Skill | 路径 | 行为 |
|-------|------|------|
| `reminder` | `/resource/agent/skills/reminder.md` | 提醒/待办，到点播报 |
| `daily-briefing` | `daily-briefing.md` | 每天 08:30 早间简报 |
| `focus-coach` | `focus-coach.md` | 专注完成鼓励、建议休息 |

主动触发（离线优先）：吃药到点、喝水到期、早间简报、专注完成 → 播放预置 WAV（`/resource/audio/*.wav`）+ 屏幕文字。

---

## 三、技术要点 / Technical Highlights

| 项 Item | 说明 Description |
|---------|------------------|
| 图形 Graphics | LVGL on OpenVela/NuttX；SPI LCD 320×240 + 触摸 |
| 语音输入 Mic | `/dev/audio/pcm0c`，16kHz/16bit/Mono WAV，tap-to-talk |
| 语音输出 Speaker | `/dev/audio/pcm0p` 4Ω 3W；或 A2DP 蓝牙耳机 |
| 云端 AI Cloud | MiMo ASR / Chat / TTS（OpenAI 兼容 HTTPS） |
| 蓝牙 BT | BLE Challenge-Response 配对；A2DP Source 耳机输出 |
| 词典 Dict | 本地分片词库 + 云端 AI 释义 |
| 音量 Volume | `AUDIO_FU_VOLUME` ioctl（0–1000）+ 软件路由 |
| 降级 Degradation | 无网：本地聊天、离线提示音、词典/专注等全开 |

MiMo 接口 / Endpoint:

```text
https://token-plan-sgp.xiaomimimo.com/v1
  /audio/transcriptions   model: mimo-v2.5-asr
  /chat/completions       model: mimo-v2.6-flash
  /audio/speech           model: mimo-v2.5-tts
Auth: Authorization: Bearer <API_KEY>
```

---

## 四、目录结构 / Repository Layout

```text
app/flowdesk/           # FlowDesk 应用源码 / application source
  dm_ui_chat.c          # 语音聊天 UI（点按录音）
  dm_voice.c            # 录音 / ASR / LLM / TTS / 播放
  dm_dict.c, dm_ui_dict.c
  dm_audio_vol.c, dm_audio_route.c
  dm_proactive.c        # 主动提醒
  dm_ble_gatt.c         # BLE 配对
  docs/场景说明.md
logs/                   # AI Coding 日志 / AI coding session logs
board/                  # 板级相关（如有）
```

应用详细说明见：[`app/flowdesk/README.md`](app/flowdesk/README.md)。

---

## 五、运行方式 / How to Run

```bash
cd <openvela-root>
./build.sh vendor/allwinnertech/boards/r528/r528s3-gemini-s1/configs/nsh_minidisplay/ -j8
strings nuttx/vela.bin | grep '0.18.1'

cp -f nuttx/vela.bin \
  vendor/allwinnertech/lichee/board/r528s3/gemini-s1_nand/configs/nsh.fex
cd vendor/allwinnertech/lichee
source envsetup.sh && lunch_nuttx   # 2 → r528s3-gemini-s1
pack
```

镜像 / Image: `out/r528s3/gemini-s1_nand/rtos_nuttx_r528s3-gemini-s1_uart0_128Mnand.img`  
烧录 / Flash: PhoenixSuit。

**语音聊天试用 / Try voice chat**

1. 板子联网（Wi-Fi 或蓝牙 PAN / 手机热点）
2. 底栏 → 聊天 → 点「开始说话」
3. 对着咪头说话，再点一下结束
4. 屏幕出文字回复，喇叭自动播放

---

## 六、AI Coding 说明 / AI Coding

本作品使用 AI 辅助进行：方案拆解、LVGL 界面、词典引擎、语音链路（ASR/LLM/TTS）、蓝牙 BLE/A2DP、音量路由、主动提醒与文档整理。对话日志见 `logs/` 目录。

AI assisted architecture, LVGL UI, dictionary, voice pipeline (ASR/LLM/TTS), BT, volume routing, proactive notices, and docs. Session logs under `logs/`.

---

## 七、选题方向 / Direction

**AI 硬件产品创新** — 以健康干预与情感陪伴为核心，打造可离线使用的桌面终端；联网后通过 MiMo 完成语音对话、TTS 人格播报与主动提醒，体现「能听、会说、主动执行」的端侧 AI 体验。

**AI Hardware Innovation** — Offline-first desk companion with contest MiMo cloud for voice chat, personalized TTS, and proactive execution (hear / speak / act).
