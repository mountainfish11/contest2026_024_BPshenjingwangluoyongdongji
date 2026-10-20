# blind_badge_app

BlindBadge 的 openvela NSH 演示应用，映射到 `packages/demos/contest2026_024_blind_badge_app`。

## 作用

这个应用把盲人辅助胸牌的传感器事件模拟出来，并输出三层结果：

- `event`: 结构化事件类型；
- `suggestion`: 本地确定性安全提醒；
- `ai_prompt`: 发给 `ai_agent` / MiMo 的事件提示；
- `ai_response`: 使用 `--ai` 时由 MiMo 返回的一句最终提醒。
- `severity`: 本地危险等级；
- `action.voice` / `action.vibration` / `action.emergency_send`: 模拟胸牌执行动作。

## 单事件命令

```bash
blind_badge_app status
blind_badge_app obstacle 80 front
blind_badge_app obstacle 40 left
blind_badge_app step_down front
blind_badge_app emergency
```

## LCD 显示

ESP32-S3-EYE 当前使用 framebuffer 显示路径，LCD 作为开发和评委展示输出，不作为盲人用户的主反馈。framebuffer 固件中，普通事件、AI 回复和 fallback 默认都会更新 LCD，不再要求添加 `--lcd`。

LCD 顶部固定显示网络状态：已关联 Wi-Fi 并取得有效 IPv4 地址时显示绿色 `WiFi OK`，否则显示红色 `WiFi ERROR`。请求 AI 前会先检查该状态；未联网时不会等待网络超时，而是立即显示本地安全 fallback。

直接执行 `ai_agent ask <message>` 时，成功回复也会更新 LCD，画面标题为 `AI CHAT / AI OK`；请求失败时显示 `AI CHAT / AI ERROR`。LCD 显示前两行可容纳的回复文本，完整回复仍保留在串口输出中。

复位启动后，`rcS` 同时启动一次 `boot_display` 连接任务和一次 `boot_timeout` 状态任务。连接任务通过 NuttX netlib/WAPI API 直接完成接口启用、热点关联和 DHCP，不依赖启动早期可能阻塞的 NSH 控制台子命令，成功后立即显示 `WiFi OK / READY`。独立进程中的状态任务会在约 12 秒仍未取得真实网络连接时将 LCD 切换为 `WiFi ERROR / LOCAL FALLBACK`，此时 AI 事件立即使用本地 fallback；首次热点关联超时后只重试一次关联，不重复整套初始化。

```bash
blind_badge_app lcd_test
blind_badge_app lcd_test zh
blind_badge_app obstacle 40 front --lcd
blind_badge_app obstacle 40 front --ai --lcd
blind_badge_app emergency --ai --lcd
blind_badge_app demo --ai --lcd
```

`lcd_test` 显示 `BlindBadge / LCD OK`，`lcd_test zh` 显示 `前方障碍`。
显示层内置 16x16 中文点阵，覆盖 U+4E00 至 U+9FFF 汉字、U+3000 至
U+303F 中日韩标点以及 U+FF01 至 U+FF60 全角字符，支持普通语音识别结果。
区间外的字符仍显示问号。字库由 `tools/generate_lcd_font.py` 使用 Pillow
和文泉驿正黑生成，授权说明见 `app/blind_badge_app/FONT-LICENSE.txt`。

## 主板状态机与 BOOT 按键

主板状态统一为 `IDLE`、`LISTENING`、`RECORDING`、`CAPTURING`、
`PROCESSING`、`RESULT`、`OFFLINE`、`ERROR` 和 `EMERGENCY`。状态变化会
写入串口并刷新 LCD，可使用以下命令检查或手动切换：

```text
blind_badge_app state
blind_badge_app state PROCESSING
blind_badge_app button_status
```

启动脚本会在后台运行 `button_service`，通过标准 `/dev/buttons` 设备读取
ESP32-S3-EYE 的 BOOT 键：

- 短按：启动后台 3 秒录音和语音识别，依次进入 `LISTENING`、`RECORDING`、
  `PROCESSING`、`RESULT`，将识别文字打印到串口并显示在 LCD；
- 双击：显示“重复上次提醒”请求并向副板发送短震标志；
- 长按 1.5 秒：触发本地 `EMERGENCY` 事件，下发强震和紧急发送标志；
- 双击目前只确认重复请求，尚未配置扬声器，因此不会伪装成已经播放语音。

## AI 闭环

## UART_LINK 主副板通信

`UART_LINK` 是主板 ESP32-S3-EYE 与副板 ESP32-S3 的统一应用协议名称。由于主板没有可用的外设扩展 UART，当前传输使用同一 Wi-Fi 局域网：UDP 广播发现主板，随后建立 TCP 长连接。

主板启动服务：

```text
blind_badge_app uart_link_start
```

模块接口统一为 `UART_LINK_Start()`、`UART_LINK_Stop()`、
`UART_LINK_IsConnected()` 和 `UART_LINK_SendFlag()`。主板服务默认监听
TCP `45679`，并在 UDP `45678` 响应副板发现请求。主板其他任务调用
`UART_LINK_SendFlag()` 时通过命名消息队列交给唯一的链路线程，不直接
共享 TCP 描述符。

协议帧格式：

```text
UART_LINK/1 <TYPE> <SEQ> <LEN> <PAYLOAD>\n
```

基础类型为 `DISCOVER`、`HELLO`、`HELLO_ACK`、`PING`、`PONG`。事件类型为
`OBSTACLE`、`STEP_DOWN`、`EMERGENCY`，主板用 `EVENT_ACK` 确认接收。
主板使用 `COMMAND` 下发控制标志，子板用 `COMMAND_ACK` 确认。收到事件后
会直接进入本地策略、LCD 和动作路径，不依赖 MiMo。

主板手动下发测试标志：

```text
blind_badge_app uart_link_send TEST_FLAG
```

## ESP32-S3-EYE 麦克风录音

主板板载数字麦克风通过 I2S0 接入，设备节点是 `/dev/audio/pcm_in0`。
硬件以单声道、16 kHz、32-bit I2S 采集，应用先丢弃 100 ms 启动瞬态，
再取每个采样的高 16 位，保存为小端 signed 16-bit raw PCM 并交给 ASR。
一次录音实际采集时长比指定时长多 100 ms，最终 PCM 保留指定时长。
命令自动停止，并打印非零样本数、最小值、最大值和峰值：

```text
blind_badge_app mic_record
blind_badge_app mic_record 5 /tmp/mic.pcm
blind_badge_app mic_stats /tmp/mic.pcm
blind_badge_app mic_dump /tmp/mic.pcm
blind_badge_app mic_dump /tmp/mic.pcm 4096
```

默认录制 3 秒到 `/tmp/mic.pcm`，时长允许 1 到 30 秒。录音时对着主板
麦克风先保持安静、再说话或拍手；正常结果应满足 `nonzero > 0`、`peak > 0`，
并且安静段与发声段的幅度有明显变化。`/tmp` 是 RAM 文件系统，复位后文件
会消失。`mic_dump` 会在 `mic_dump_begin` 和 `mic_dump_end` 标记之间按 hex
打印 PCM 字节，主机脚本可以据此还原成 `.pcm` / `.wav` 做离线 ASR。

## 按键语音识别与 AI/LCD 闭环

短按 BOOT 键后直接说话，主板会录制 3 秒并只执行 ASR，不调用 MiMo。也可用
命令单独验证同一条语音识别路径：

```text
blind_badge_app voice_asr
blind_badge_app voice_asr 3
```

成功时串口打印 `[BlindBadge] asr_text: ...`，LCD 显示 `VOICE / RECOGNIZED`
和识别文字。录音识别在独立线程运行，按键服务仍可检测长按紧急操作。

主板固件已经接入 ai_agent 的 voice ASR 抽象。`voice_ai` 会打开麦克风录音，
把 16 kHz / mono / signed 16-bit PCM 交给当前 ASR 后端识别，再把识别文字
发送给 ai_agent/MiMo，并将 AI 回复显示到 LCD：

```text
blind_badge_app voice_ai
blind_badge_app voice_ai 3 --ai
```

默认录制 3 秒，允许 1 到 4 秒。命令会依次在 LCD 显示 `VOICE / RECORDING`、
`VOICE / RECOGNIZING`、`VOICE / ASR TEXT`，最后显示 `AI CHAT / AI OK`
或 `AI CHAT / AI ERROR`。

首次使用云端 ASR 前，需要在板端配置火山/豆包 ASR 凭证并选择后端：

```text
ai_agent set_volc_asr <app_id> <token> <cluster>
ai_agent set_voice_asr volcengine
```

豆包流式语音识别 2.0 小时版需要将第三个参数设为资源 ID：

```text
ai_agent set_volc_asr <app_id> <token> volc.seedasr.sauc.duration
ai_agent set_voice_asr volcengine
```

固件根据第三个参数选择协议：`volc.seedasr.sauc.*` 或
`volc.bigasr.sauc.*` 使用 V3 接口及资源鉴权，旧版 cluster 使用 V2。
`voice_asr` 的录音文件使用 `bigmodel_nostream` 接口，流式会话使用
`bigmodel_async` 接口。当前板端配置存放在 RAM 文件系统，重启后需重新配置。

如果 ASR 暂时不可用，也可以继续用 `voice_ai_text` 验证
“识别文字 -> ai_agent -> LCD”的后半段链路，直接把外部 ASR 识别后的文字作为
参数输入：

```text
blind_badge_app voice_ai_text 你好 请介绍一下盲人胸牌
blind_badge_app voice_ai_text 前方有什么危险 --ai
```

`voice_ai_text` 会先在 LCD 显示 `VOICE / ASR TEXT`，随后调用 ai_agent/MiMo，
拿到 `ai_response` 后显示 `AI CHAT / AI OK`。

先在 QEMU 中配置 MiMo router：

```bash
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
quit
```

再运行：

```bash
blind_badge_app obstacle 80 front --ai
```

应用会内部调用 `ai_agent` 的 `llm_router` / `llm_proxy`，并打印 `ai_response`。

如果 MiMo 未配置、超时、初始化失败或返回空内容，应用会打印 `ai_error`、`fallback_reason` 和 `fallback_response`，并继续使用本地安全提醒作为 `ai_response`。

## Skill 安装验证

```bash
blind_badge_app install_skill
echo ask /skill | ai_agent
```

`/skill` 输出中应包含 `BlindBadge Safety Reminder Skill`。当前 `goldfish-arm64-v8a-ap` 镜像的 `ai_agent` Skill 目录是 `/data/ai_agent/skills`，安装器直接复用 `ai_agent` 的 `AGENT_SKILLS_DIR` 配置。

## 主演示命令

```bash
blind_badge_app demo
blind_badge_app demo --ai
```

`demo` 会模拟主动发现风险的流程：前方障碍从 120cm 接近到 80cm、40cm，随后检测下行台阶，并触发 emergency 求助文案。默认使用本地提醒；`--ai` 会为每个事件调用 MiMo。
