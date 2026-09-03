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

## AI 闭环

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
