# BlindBadge QEMU Manual Test

Test time: 2026-09-02 19:07:16 CST

Board config:

```bash
vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap
```

Build command:

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j2
```

Build result:

```text
build completed successfully
```

QEMU command:

```bash
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap -read-only -no-window
```

NSH prompt:

```text
goldfish-armv8a-ap>
```

## Commands

### help

```bash
blind_badge_app help
```

Result:

```text
BlindBadge usage:
  blind_badge_app help
  blind_badge_app status
  blind_badge_app obstacle <distance_cm> <front|left|right>
  blind_badge_app step_down <front|left|right>
  blind_badge_app emergency
```

Status: pass, no crash.

### status

```bash
blind_badge_app status
```

Required fields:

```text
event: status
suggestion: 系统正在运行，可继续模拟障碍、台阶或求助事件。
ai_prompt: 你是盲人辅助胸牌。设备状态正常，请生成一句简短的系统就绪提示。
```

Status: pass, no crash.

### obstacle 80 front

```bash
blind_badge_app obstacle 80 front
```

Required fields:

```text
event: obstacle_near
suggestion: 前方80厘米有障碍，请减速并绕行。
ai_prompt: 你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
```

Status: pass, no crash.

### obstacle 40 left

```bash
blind_badge_app obstacle 40 left
```

Required fields:

```text
event: obstacle_near
suggestion: 左侧40厘米有障碍，请立即停下确认。
ai_prompt: 你是盲人辅助胸牌。检测到左侧40厘米有障碍，请生成一句简短安全提醒。
```

Status: pass, no crash.

### step_down front

```bash
blind_badge_app step_down front
```

Required fields:

```text
event: step_down
suggestion: 前方可能有下行台阶，请停一下，用手杖或脚尖确认。
ai_prompt: 你是盲人辅助胸牌。检测到前方可能有下行台阶，请生成一句简短安全提醒。
```

Status: pass, no crash.

### emergency

```bash
blind_badge_app emergency
```

Required fields:

```text
event: emergency
suggestion: 已触发求助。建议发送：我需要帮助，请联系我或前往我的当前位置。
ai_prompt: 你是盲人辅助胸牌。用户触发了求助按钮，请生成一句适合发给紧急联系人的求助信息。
```

Status: pass, no crash.

## Notes

The app stack size is set to 16384 in both `CMakeLists.txt` and `Makefile` to avoid the previous `step_down` stack crash risk.

QEMU was started with `-read-only -no-window` because this environment cannot initialize the default Qt window reliably. The application itself did not crash during the command tests.

## AI Agent Manual Bridge Test

Test goal:

```text
blind_badge_app ai_prompt -> manual copy -> ai_agent ask
```

Command used to generate the prompt:

```bash
blind_badge_app obstacle 80 front
```

Generated prompt:

```text
你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
```

AI Agent startup:

```bash
ai_agent
```

Configuration check:

```bash
config_show
router_status
```

Observed state:

```text
API Key: (not set)
Model: (not set)
LLM Host: (not set)
backend_count: 0
Network: connected / 10.0.2.15
```

Manual ask command:

```bash
ask 你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
```

Observed result:

```text
Sent to agent: 你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
[Agent]: 稍等，处理中...
[Agent]: Sorry, I encountered an error.
```

Conclusion:

```text
The first manual bridge path is valid: the BlindBadge prompt can be copied into ai_agent ask.
The real LLM response is blocked because no MiMo backend is configured in the current QEMU runtime.
```

To complete the real AI loop, configure MiMo in `vela>`:

```bash
router_set mimo <MIMO_TOKEN_PLAN_KEY>
router_model 0 mimo-v2.5
router_status
ask 你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
```

## AI Agent MiMo Loop Test

Test goal:

```text
blind_badge_app ai_prompt -> manual copy -> ai_agent ask -> MiMo response
```

MiMo runtime configuration:

```bash
router_set mimo <MIMO_TOKEN_PLAN_KEY>
router_model 0 mimo-v2.5
router_status
```

Router status after configuration:

```text
backend_count: 1
host: token-plan-cn.xiaomimimo.com
model: mimo-v2.5
status: ok
```

Manual ask command:

```bash
ask 你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
```

Observed result:

```text
Sent to agent: 你是盲人辅助胸牌。检测到前方80厘米有障碍，请生成一句简短安全提醒。
[Agent]: 马上好...
[Agent]: 前方约80厘米处有障碍，请小心绕行。
```

Router statistics after the call:

```text
total_calls: 1
total_failures: 0
avg_latency_ms: 2392
success_rate_pct: 100
```

Conclusion:

```text
The first AI loop is complete. BlindBadge can generate an ai_prompt, and the prompt can be manually sent to ai_agent ask. MiMo returns a short safety reminder through the configured router backend.
```

## Skill Install Limitation Test

Test goal:

```text
Verify whether ai_agent can install a local Skill through stdin.
```

Command:

```bash
install_skill blind-badge -
```

Observed result:

```text
Only HTTPS URLs are allowed
```

Conclusion:

```text
The current install_skill command cannot load a local stdin Skill. BlindBadge Skill demo should install from an HTTPS raw URL after the repository is pushed, or copy the Skill file into /data/agent/skills/ through another supported transport such as adb push.
```
