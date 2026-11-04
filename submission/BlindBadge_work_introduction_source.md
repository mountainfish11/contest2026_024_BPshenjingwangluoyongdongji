# BlindBadge Work Introduction Source

This source accompanies `BlindBadge_work_introduction.pdf`.

## Project Background

BlindBadge is an AI hardware prototype for blind and low-vision users. It is designed as a wearable safety badge that converts nearby hazards into short, action-oriented reminders.

The project is positioned as a safety-reminder prototype. It does not replace a cane, guide dog, professional navigation device, or trained assistive workflow.

## User Story

A user wears BlindBadge while walking. The badge detects or receives a hazard event, such as:

- an obstacle approaching from the front;
- a possible downward step;
- an emergency help trigger.

The device should respond with one short reminder, such as:

```text
前方40厘米有障碍，请先停下确认。
```

If AI or network access is unavailable, BlindBadge must still produce a safe local reminder and continue the simulated action path.

## System Architecture

```text
hazard event
-> local safety policy
-> ai_prompt
-> ai_agent / MiMo
-> fallback guard
-> voice / vibration / emergency_send
```

The current QEMU demo uses command-line events in place of real sensors. The ESP32-S3-EYE validation uses Wi-Fi, `ai_agent`, LCD output, and hardware build/flash evidence to validate the hardware path.

## openvela Capabilities

- NSH demo application: `blind_badge_app`
- QEMU target: `goldfish-arm64-v8a-ap`
- ESP32-S3-EYE hardware target
- framebuffer LCD path on `/dev/fb0`
- Wi-Fi and DNS path for MiMo
- I2S microphone validation path
- `ai_agent` Skill and LLM router integration

## AI Agent And Skill

BlindBadge includes a custom Skill:

```text
skills/blind-badge/SKILL.md
```

The Skill constrains replies to one short Chinese safety sentence. It covers:

- `obstacle_near`
- `step_down`
- `emergency`

Runtime installation is verified with:

```text
blind_badge_app install_skill
echo ask /skill | ai_agent
```

## Hardware Validation

The ESP32-S3-EYE validation record covers:

- firmware build and flash;
- Wi-Fi and DNS;
- direct `ai_agent ask hi`;
- BlindBadge `obstacle`, `emergency`, and `demo --ai`;
- LCD status and limited Chinese reminder display.

See:

```text
docs/esp32_s3_eye_validation_2026-09-04.md
docs/openvela_workspace_dependencies.md
```

## Demo Video Content

The generated MP4 demonstrates:

- QEMU event flow;
- openvela app and ai_agent integration;
- MiMo-style AI response;
- BlindBadge proactive demo;
- local fallback behavior;
- voice, vibration, and emergency-send action simulation;
- ESP32-S3-EYE validation summary.
