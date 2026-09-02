# BlindBadge 比赛背景与赛题对齐

## 1. 大赛背景

2026 首届 openvela AI 硬件开发者大赛面向全球开发者开放，要求参赛者基于 openvela 生态构建可运行、可展示、可提交的创新作品。大赛不限定固定命题，鼓励围绕智能硬件、AI Agent、手表/手环应用、新硬件适配等方向自由发挥。

openvela 是小米开源的 AIoT 操作系统，面向轻量化、低功耗、AI 原生的智能硬件场景。比赛评审会同时关注作品本身、代码仓库、运行 Demo、说明文档、演示视频以及 AI Coding 日志。

## 2. 选题方向

BlindBadge 盲人辅助胸牌适合归入「AI 硬件产品创新」方向。

该方向的核心要求是基于 openvela + ai_agent 开发一个「能主动、会执行」的嵌入式 AI Agent 应用，而不是单纯的聊天机器人。官方文档强调端侧 AI 应用应从被动问答升级为主动感知、主动提醒、能够调用工具或执行动作的设备端智能体。

BlindBadge 的定位与该方向天然匹配：

- 佩戴形态是小型智能硬件，符合手表、眼镜、胸牌等轻量设备场景。
- 核心交互不是等待用户提问，而是根据障碍、台阶、求助等事件主动提醒。
- QEMU 阶段可先用命令模拟传感器事件，后续再替换为摄像头、超声波、ToF、IMU 或按键输入。
- ai_agent/MiMo 可负责把结构化事件转换为更自然、更短、更安全的中文提醒。

## 3. 官方能力要求与 BlindBadge 对齐

### 3.1 运行在 openvela 设备侧

当前项目已经选择 `goldfish-arm64-v8a-ap` QEMU 配置，先完成 openvela 设备侧的最小演示闭环：

```text
事件输入 -> openvela app 解析 -> 输出安全提醒 -> 生成 ai_prompt -> ai_agent/MiMo 回复
```

这能证明作品不是纯云端应用，而是有实际 openvela 运行入口的端侧 Demo。

### 3.2 至少一个交互渠道

第一阶段采用 CLI 作为交互渠道：

```bash
blind_badge_app obstacle 80 front
blind_badge_app step_down front
blind_badge_app emergency
```

后续可以增加语音、按键、QuickApp 或 WebSocket，但初赛演示优先保证 CLI 闭环稳定。

### 3.3 至少一个自定义 Skill

比赛要求 AI 硬件方向提供至少 1 个自定义 Skill。BlindBadge 已具备合适的 Skill 方向：

```text
blind-badge safety assistant
```

Skill 应定义 ai_agent 在接收到障碍、台阶、紧急求助等事件时如何生成短句提醒，例如：

- 只输出面向视障用户的安全提醒，不输出冗长解释。
- 优先给出方向、距离、动作建议。
- 遇到 emergency 事件时生成求助信息。
- 对低置信度事件使用谨慎措辞。

当前仓库已有 `skills/blind-badge/SKILL.md`，后续应继续完善并在演示文档中说明使用方式。

### 3.4 至少一个「主动 + 执行」场景

BlindBadge 最适合做「阈值主动」和「事件主动」：

- 阈值主动：当障碍距离低于安全阈值，例如 80cm 或 40cm，主动发出提醒。
- 事件主动：检测到下行台阶、紧急求助、异常静止等事件时主动输出提示。
- 执行动作：终端输出、生成 `ai_prompt`、调用 ai_agent、写入日志，后续可扩展为语音播报或通知。

第一阶段的目标不是做复杂硬件，而是让主动事件链路稳定可演示。

## 4. 评分点拆解

### 技术难度

重点体现 openvela app 接入、Kconfig/CMake 构建、QEMU 运行、ai_agent 配置、事件协议设计、后续传感器替换路径。当前不宜把技术难度押在真实摄像头上，应先把端侧事件处理和 Agent 连接做扎实。

### 产品创新性

BlindBadge 的创新点是把视障出行辅助做成胸牌形态，强调「主动提醒」而非手机 App 式被动查询。场景痛点清晰：视障用户在陌生环境、夜间、室内楼梯、拥挤通道中需要低延迟、短句、可执行的安全反馈。

### 项目完整度

需要保证评委能复现：

- 如何构建 openvela 固件。
- 如何启动 QEMU。
- 如何运行 `blind_badge_app`。
- 每个事件命令的预期输出。
- 如何手动把 `ai_prompt` 交给 `ai_agent ask`。
- 后续自动调用 ai_agent 的规划与限制。

### AI 开发

需要提交 AI Coding 日志，并说明 AI 在需求拆解、openvela 接入、构建调试、MiMo 配置、文档生成等环节的作用。比赛明确要求日志进入 `logs/` 目录，最终需要随代码提交。

### 商业潜力与展示效果

BlindBadge 的展示应尽量贴近真实用户故事：

```text
用户佩戴胸牌走到楼梯口，设备检测到 step_down 事件，主动播报：
前方疑似下行台阶，请停步并确认扶手位置。
```

演示视频中建议用「事件发生 -> openvela 输出 -> ai_agent 生成提醒 -> 用户动作建议」的顺序展示，避免把作品讲成普通聊天助手。

## 5. 当前开发优先级

短期优先级如下：

1. 稳定 `blind_badge_app` 事件模拟器，支持 `help/status/obstacle/step_down/emergency`。
2. 修复或规避 `step_down` 曾触发的栈问题，确认 app stack size 为 16384。
3. 重新构建并在 QEMU 中逐条测试事件命令。
4. 输出包含 `event/distance_cm/direction/suggestion/ai_prompt` 的结构化结果。
5. 手动复制 `ai_prompt` 到 `vela> ask ...`，验证 MiMo 回复质量。
6. 完善 `skills/blind-badge/SKILL.md`，把安全提醒规则固化为自定义 Skill。
7. 再研究自动调用 ai_agent 的真实 C/API 或消息总线接口，避免臆造接口。

## 6. 提交物提醒

最终提交至少需要：

- 代码：`app/blind_badge_app/`、`skills/blind-badge/`、必要的协议和测试文档。
- README：替换模板 README，说明作品简介、赛道、目录结构、运行方式、AI Coding 使用说明。
- 测试文档：记录 QEMU 中每个命令的实际输出。
- 演示视频：不超过 5 分钟，突出主动提醒闭环。
- AI Coding 日志：提交到 `logs/` 目录。

## 7. 项目叙事建议

建议后续对外介绍时使用这个主线：

```text
BlindBadge 是一个基于 openvela + ai_agent 的视障出行安全胸牌。
它在端侧持续接收环境事件，并在障碍、台阶、求助等关键场景下主动生成短句安全提醒。
当前版本在 QEMU 中用命令模拟传感器输入，验证 openvela app、事件协议、ai_agent/MiMo 提醒生成的最小闭环。
后续可替换为真实摄像头、超声波或 ToF 输入，并扩展语音播报和一键求助。
```
