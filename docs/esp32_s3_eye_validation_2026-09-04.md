# ESP32-S3-EYE Hardware Validation Record

Date: 2026-09-04

Board: ESP32-S3-EYE

Firmware context:

- Contest repo commit: `8c90aa6`
- ai_agent repo commit: `ad6a475`
- Local workspace also contains uncommitted firmware/app changes from this validation round.
- MiMo Token Plan key was injected only through runtime serial commands and is not recorded here.

## Build And Flash

Result: PASS

Evidence:

```text
Chip is ESP32-S3 (QFN56) (revision v0.2)
Features: WiFi, BLE, Embedded PSRAM 8MB (AP_3v3)
Auto-detected Flash size: 8MB
Wrote 976204 bytes ... at 0x00000000
Hash of data verified.
Hard resetting via RTS pin...
```

Boot result:

```text
NuttShell (NSH)
nsh>
```

## Direct ai_agent Commands

Result: PASS

`ai_agent help` lists the direct runtime key commands:

```text
key_clear            - Clear temporary API key buffer
key_add <chunk>      - Append to temporary API key buffer
router_set_key <preset> - Add backend using temporary API key
router_import_mimo [path] - Import MiMo key from local key file
```

The preferred official path remains `vela> router_set mimo <key>`. `router_import_mimo` is the ESP32-S3-EYE fallback path for unstable long serial input; it loads the key from `/data/agent/config/mimo.key` or from a local untracked build-only header and does not print the raw key.

## Wi-Fi And DNS

Result: PASS

Runtime Wi-Fi connected to the test AP and obtained:

```text
wlan0 inet addr:192.168.170.63 DRaddr:192.168.170.3 Mask:255.255.255.0
```

MiMo endpoint DNS resolved:

```text
Host: token-plan-cn.xiaomimimo.com Addr: 220.181.104.191
```

Earlier runs showed that Internet packet loss can fluctuate, so MiMo tests should keep fallback enabled.

## Runtime MiMo Router Setup

Result: PASS

The MiMo key was configured in RAM using chunked commands:

```text
ai_agent key_clear
Temporary API key buffer cleared.

ai_agent key_add <chunk-1>
Temporary API key buffer length: 23

ai_agent key_add <chunk-2>
Temporary API key buffer length: 51

ai_agent router_set_key mimo
Added [0]: mimo (mimo-v2.5, tier=1)
```

Router status:

```text
"backend_count": 1
"host": "token-plan-cn.xiaomimimo.com"
"model": "mimo-v2.5"
"total_calls": 0
"total_failures": 0
"status": "ok"
```

`config_show` does not display router slot credentials; `router_status` is the authoritative check for the router backend.

## ai_agent Ask

Result: PASS

Command:

```text
ai_agent ask hi
```

Observed reply:

```text
Hello! How can I help you today?
```

Router status after ask:

```text
"backend_count": 1
"total_calls": 1
"total_failures": 0
"avg_latency_ms": 2250
"success_rate_pct": 100
```

## BlindBadge AI Loop

Result: PASS

Obstacle command:

```text
blind_badge_app obstacle 40 front --ai
```

Observed:

```text
[BlindBadge] event: obstacle_near
[BlindBadge] distance_cm: 40
[BlindBadge] direction: front
[BlindBadge] ai_status: calling ai_agent llm router...
[BlindBadge] ai_response: 前方障碍，请先停下确认。
[BlindBadge] severity: urgent
[BlindBadge] action.voice: 前方障碍，请先停下确认。
[BlindBadge] action.vibration: strong
```

Emergency command:

```text
blind_badge_app emergency --ai
```

Observed:

```text
[BlindBadge] event: emergency
[BlindBadge] ai_status: calling ai_agent llm router...
[BlindBadge] ai_response: 我遇到了紧急情况，请立即联系我并确认我的位置。
[BlindBadge] severity: emergency
[BlindBadge] action.voice: 我遇到了紧急情况，请立即联系我并确认我的位置。
[BlindBadge] action.vibration: strong
[BlindBadge] action.emergency_send: triggered
```

Full proactive demo:

```text
blind_badge_app demo --ai
```

Observed:

```text
[BlindBadge] demo: proactive_hazard_alert
[BlindBadge] demo_stage: obstacle approaching from 120cm to 40cm
[BlindBadge] ai_response: 前方有障碍，请保持注意。
[BlindBadge] action.vibration: off
[BlindBadge] demo_stage: obstacle warning threshold 80cm
[BlindBadge] ai_response: 前方80厘米有障碍，请减速并绕行。
[BlindBadge] action.vibration: short
[BlindBadge] demo_stage: urgent obstacle threshold 40cm
[BlindBadge] ai_response: 前方40厘米有障碍，请先停下确认。
[BlindBadge] action.vibration: strong
[BlindBadge] demo_stage: downward step detected
[BlindBadge] ai_response: 前方可能有台阶，请停下或放慢脚步，小心确认。
[BlindBadge] action.vibration: strong
[BlindBadge] demo_stage: emergency help message
[BlindBadge] ai_response: 紧急情况，我需要帮助，请立即联系我！
[BlindBadge] action.emergency_send: triggered
[BlindBadge] demo: completed
```

## LCD Display Validation

Date: 2026-09-06

Result: PASS for LCD status display and limited Chinese AI/fallback text.

The ESP32-S3-EYE LCD path uses the board framebuffer registered as `/dev/fb0`. LVGL was not required for the current BlindBadge display layer.

Minimal LCD command:

```text
blind_badge_app lcd_test
```

Observed:

```text
[BlindBadge] lcd: updated 240x240 bpp=16
```

Board LCD displayed:

```text
BlindBadge
LCD OK
```

Chinese font check:

```text
blind_badge_app lcd_test zh
```

Earlier firmware rendered Chinese text as question marks. The 2026-09-06 firmware adds a limited built-in 16x16 CJK bitmap set for BlindBadge safety phrases.

Observed result after reflashing: PASS. The board LCD displayed:

```text
前方障碍
```

Local obstacle display:

```text
blind_badge_app obstacle 40 front --lcd
```

Observed LCD content:

```text
BlindBadge
DANGER / obstacle_near
front 40cm
stop first
```

AI fallback display without a configured router:

```text
blind_badge_app obstacle 40 front --ai --lcd
```

Observed serial output included:

```text
[BlindBadge] ai_error: no router backend at slot 0
[BlindBadge] fallback_reason: no router backend at slot 0
[BlindBadge] fallback_response: 前方40厘米有障碍，请立即停下确认。
[BlindBadge] fallback: active
[BlindBadge] lcd_note: AI text is not ASCII; LCD shows English safety action.
[BlindBadge] lcd: updated 240x240 bpp=16
```

Observed LCD content shows the final Chinese AI/fallback reminder text:

```text
BlindBadge
DANGER / AI Fallback
前方40厘米有障碍
请立即停下确认。
```

Emergency display without a configured router:

```text
blind_badge_app emergency --ai --lcd
```

Observed:

```text
[BlindBadge] event: emergency
[BlindBadge] ai_error: no router backend at slot 0
[BlindBadge] fallback_response: 我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] ai_response: 我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] severity: emergency
[BlindBadge] action.voice: 我需要帮助，请联系我或前往我的当前位置。
[BlindBadge] action.vibration: strong
[BlindBadge] action.emergency_send: triggered
[BlindBadge] fallback: active
[BlindBadge] lcd_note: LCD shows UTF-8 AI text with limited built-in CJK glyphs.
[BlindBadge] lcd: updated 240x240 bpp=16
```

The board LCD showed the emergency/SOS state and stayed usable even when MiMo was not configured.

Proactive LCD demo:

```text
blind_badge_app demo --ai --lcd
```

Observed result: PASS. The LCD refreshed continuously through the 120 cm, 80 cm, 40 cm, `step_down`, and `emergency` demo stages. MiMo was not configured for this LCD validation pass, so the app used local fallback where needed.

LCD scope note: LCD is for development and judging visibility only. It is not the primary output for blind users; the product output remains voice, vibration, buzzer, or emergency-send actions. The current Chinese renderer is intentionally limited to common BlindBadge safety phrases; it is not a general-purpose CJK font engine.

## Fallback Notes

Fallback was previously validated on the same board with no router configured:

```text
[BlindBadge] ai_error: no router backend at slot 0
[BlindBadge] fallback_response: 前方40厘米有障碍，请立即停下确认。
[BlindBadge] fallback: active
```

Emergency does not depend on MiMo for the simulated safety action:

```text
[BlindBadge] action.emergency_send: triggered
```

## Issues Fixed During Validation

- `ai_agent` interactive `vela>` mode did not reliably consume serial input on ESP32-S3-EYE, so direct commands were added: `ai_agent <cmd>`.
- Long API key input was truncated by the NSH serial command line, so chunked runtime key input was added.
- `router_status.backend_count` was corrected to reflect active configured backends.
- BlindBadge AI response truncation was made UTF-8 safe to avoid malformed Chinese output.
