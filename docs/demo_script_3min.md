# BlindBadge 3-Minute Demo Script

This script is for video recording or live judging. Keep the QEMU prompt visible and run commands exactly as written.

## Setup Before Recording

Build once:

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j2
```

Start QEMU:

```bash
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap -read-only -no-window
```

## 0:00-0:20 Opening

Say:

```text
BlindBadge is an AI assistive badge for blind and low-vision users. It turns nearby hazard events into short voice reminders, keeps a local safety fallback, and simulates device actions like voice, vibration, and emergency send.
```

Show app help:

```bash
blind_badge_app help
```

## 0:20-0:45 Normal Obstacle Warning

Say:

```text
First, a normal obstacle at 80 centimeters. This should be a warning, not a panic alert.
```

Run:

```bash
blind_badge_app obstacle 80 front
```

Point out:

```text
event, suggestion, and ai_prompt are all printed. The local rule says slow down and avoid.
```

## 0:45-1:05 High-Risk 40cm Obstacle

Say:

```text
At 40 centimeters the priority changes. The device should tell the user to stop first.
```

Run:

```bash
blind_badge_app obstacle 40 front --ai
```

If MiMo is not configured yet, point out:

```text
MiMo is unavailable, so fallback activates. The final response is still safe, and the simulated action uses strong vibration.
```

## 1:05-1:25 Downward Step

Say:

```text
A downward step is treated as urgent because falling is a high-impact risk.
```

Run:

```bash
blind_badge_app step_down front --ai
```

Point out:

```text
The reminder asks the user to stop or slow down and confirm with a cane or foot.
```

## 1:25-1:45 Emergency Help

Say:

```text
Emergency does not depend on AI. It always has a local help message and triggers the emergency-send action.
```

Run:

```bash
blind_badge_app emergency --ai
```

Point out:

```text
The output includes severity emergency, strong vibration, and action.emergency_send: triggered.
```

## 1:45-2:10 Runtime Skill Verification

Say:

```text
Now I install the BlindBadge Skill into ai_agent's runtime Skill directory and verify that ai_agent can see it.
```

Run:

```bash
blind_badge_app install_skill
echo ask /skill | ai_agent
```

Point out:

```text
The Skill list includes BlindBadge Safety Reminder Skill from /data/ai_agent/skills/blind-badge.md.
```

## 2:10-2:35 MiMo AI Reply

Say:

```text
With MiMo configured, standalone ai_agent also follows the BlindBadge Skill and returns one short safety sentence.
```

Run inside QEMU, using the runtime key only:

```bash
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
ask BlindBadge event=obstacle_near distance_cm=40 direction=front
quit
```

Expected style:

```text
前方40厘米有障碍物，请停下确认。
```

## 2:35-3:00 Main Proactive Demo

Say:

```text
Finally, the main demo simulates the badge proactively detecting an obstacle getting closer, then a downward step, then an emergency.
```

Run:

```bash
blind_badge_app demo --ai
```

Close with:

```text
This shows proactive sensing, local safety judgment, AI wording, fallback, and simulated hardware execution in one loop.
```
