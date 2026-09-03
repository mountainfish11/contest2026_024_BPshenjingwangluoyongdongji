# BlindBadge Acceptance Checklist

This checklist is the fixed QEMU acceptance flow for BlindBadge. Start from a clean QEMU boot and run the commands in order.

## 0. Build

From the openvela workspace root:

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j2
```

Expected:

```text
build completed successfully
```

## 1. Start Clean QEMU

```bash
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap -read-only -no-window
```

Expected prompt:

```text
goldfish-armv8a-ap>
```

Known environment note: exiting QEMU with `Ctrl-A x` may make the emulator wrapper return 139 in this environment. This happens after the app commands complete.

## 2. Verify App Is Present

```bash
blind_badge_app help
```

Expected output includes:

```text
blind_badge_app obstacle <distance_cm> <front|left|right> [--ai|--agent]
blind_badge_app step_down <front|left|right> [--ai|--agent]
blind_badge_app emergency [--ai|--agent]
blind_badge_app demo [--ai|--agent]
blind_badge_app install_skill
```

## 3. Install Runtime Skill

```bash
blind_badge_app install_skill
```

Expected:

```text
[BlindBadge] skill_installed: /data/ai_agent/skills/blind-badge.md
[BlindBadge] skill_title: BlindBadge Safety Reminder Skill
[BlindBadge] skill_verify: run ai_agent, then ask /skill
```

The path comes from `ai_agent`'s `AGENT_SKILLS_DIR`; in this QEMU image it resolves to `/data/ai_agent/skills`.

## 4. Verify ai_agent Loads BlindBadge Skill

```bash
echo ask /skill | ai_agent
```

Expected output includes:

```text
[Agent]: - **BlindBadge Safety Reminder Skill**:  (read with: read_file /data/ai_agent/skills/blind-badge.md)
```

This proves the Skill is in the runtime loader path, not only in the repository.

## 5. Verify Local Event Outputs

```bash
blind_badge_app obstacle 80 front
blind_badge_app obstacle 40 left
blind_badge_app step_down front
blind_badge_app emergency
```

Each command must print:

```text
[BlindBadge] event: ...
[BlindBadge] suggestion: ...
[BlindBadge] ai_prompt: ...
```

Expected safety behavior:

- `obstacle 80 front`: warn-level suggestion, slow down and avoid.
- `obstacle 40 left`: urgent suggestion, stop and confirm.
- `step_down front`: urgent suggestion, stop or slow down and confirm.
- `emergency`: local help message is generated.

## 6. Verify Fallback Without MiMo

Do not configure MiMo. Run:

```bash
blind_badge_app obstacle 40 front --ai
```

Expected output includes:

```text
[BlindBadge] ai_error: no router backend at slot 0
[BlindBadge] fallback_reason: no router backend at slot 0
[BlindBadge] fallback_response: 前方40厘米有障碍，请立即停下确认。
[BlindBadge] ai_response: 前方40厘米有障碍，请立即停下确认。
[BlindBadge] severity: urgent
[BlindBadge] action.voice: 前方40厘米有障碍，请立即停下确认。
[BlindBadge] action.vibration: strong
[BlindBadge] fallback: active
```

## 7. Verify Emergency Works Without AI

```bash
blind_badge_app emergency
```

Expected:

```text
[BlindBadge] event: emergency
[BlindBadge] suggestion: 已触发求助。建议发送：我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] ai_prompt: BlindBadge event=emergency...
```

Then verify action execution through fallback path:

```bash
blind_badge_app emergency --ai
```

Without MiMo configured, expected output includes:

```text
[BlindBadge] fallback_response: 我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] ai_response: 我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] severity: emergency
[BlindBadge] action.voice: 我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] action.vibration: strong
[BlindBadge] action.emergency_send: triggered
[BlindBadge] fallback: active
```

## 8. Verify Real MiMo Reply

Configure the router in QEMU runtime only. Do not commit the real key.

```bash
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
ask BlindBadge event=obstacle_near distance_cm=40 direction=front
```

Expected shape:

```text
Added [0]: mimo (mimo-v2.5, tier=1)
Sent to agent: BlindBadge event=obstacle_near distance_cm=40 direction=front
[Agent]: 正在分析...
[Agent]: 前方40厘米有障碍物，请停下确认。
```

This checks standalone `ai_agent` behavior with the BlindBadge Skill context.

Exit `ai_agent`:

```bash
quit
```

## 9. Verify Main AI Demo

After MiMo is configured:

```bash
blind_badge_app demo --ai
```

Expected flow:

```text
[BlindBadge] demo: proactive_hazard_alert
[BlindBadge] demo_stage: obstacle approaching from 120cm to 40cm
...
[BlindBadge] demo_stage: obstacle warning threshold 80cm
...
[BlindBadge] demo_stage: urgent obstacle threshold 40cm
...
[BlindBadge] demo_stage: downward step detected
...
[BlindBadge] demo_stage: emergency help message
...
[BlindBadge] demo: completed
```

Each event should print `ai_response`, `severity`, and simulated actions. If MiMo fails during the demo, `fallback: active` is acceptable and expected to keep the safety loop alive.

## 10. Exit QEMU

Use:

```text
Ctrl-A x
```

Acceptance passes when:

- the app does not crash;
- Skill installation writes to `/data/ai_agent/skills/blind-badge.md`;
- `ai_agent` lists `BlindBadge Safety Reminder Skill`;
- local fallback works without MiMo;
- emergency action triggers without AI;
- MiMo returns a one-sentence safety reminder after runtime configuration;
- `demo --ai` completes the proactive scenario.
