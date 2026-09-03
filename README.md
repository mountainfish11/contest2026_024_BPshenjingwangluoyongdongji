# BlindBadge

BlindBadge is an AI hardware prototype for blind and low-vision users. It simulates a wearable safety badge that detects nearby hazards, generates a local safety suggestion, and can call MiMo through openvela `ai_agent` to produce one short voice-friendly reminder.

## Track

AI hardware product innovation.

The current prototype focuses on the product loop that matters most for a wearable assistive device:

```text
hazard event -> local safety rule -> ai_prompt -> ai_agent / MiMo -> final short reminder
```

## Highlights

- Proactive alert demo: one command simulates obstacles approaching from 120cm to 80cm to 40cm, then a downward step and emergency help.
- Safety-first local fallback: every event has a deterministic `suggestion`, and `--ai` falls back locally when MiMo is unavailable, times out, or returns empty content.
- AI loop verified in QEMU: `blind_badge_app --ai` calls the existing `ai_agent` `llm_router` and `llm_proxy` path.
- Custom Skill: `skills/blind-badge/SKILL.md` defines BlindBadge-specific response rules for `obstacle_near`, `step_down`, and `emergency`.
- Runtime Skill installer: `blind_badge_app install_skill` writes the Skill into the `ai_agent` runtime Skill directory for QEMU verification.
- Execution action simulation: events print voice, vibration, and emergency-send actions based on severity.
- Low-interruption output style: one short Chinese sentence, clear action, no long explanation, no absolute safety promise.

## Repository Layout

```text
app/blind_badge_app/       NSH demo app for BlindBadge events and AI loop
skills/blind-badge/       Custom ai_agent Skill for safety reminder behavior
docs/                     Skill/runtime notes
tests/qemu_manual_test.md QEMU build and manual verification record
logs/                     AI Coding logs for contest submission
```

The manifest maps:

```text
app/blind_badge_app -> packages/demos/contest2026_024_blind_badge_app
```

## Build

Run from the openvela workspace root:

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j2
```

Verified result:

```text
build completed successfully
```

## Run In QEMU

Use headless mode in this environment:

```bash
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap -read-only -no-window
```

Expected prompt:

```text
goldfish-armv8a-ap>
```

## Basic Event Simulation

```bash
blind_badge_app help
blind_badge_app status
blind_badge_app obstacle 80 front
blind_badge_app obstacle 40 left
blind_badge_app step_down front
blind_badge_app emergency
```

Each event prints:

```text
event
suggestion
ai_prompt
```

## Proactive Alert Demo

```bash
blind_badge_app demo
```

This is the main non-network demo. It shows the badge proactively detecting:

- a far obstacle at 120cm;
- a warning-level obstacle at 80cm;
- an urgent obstacle at 40cm;
- a possible downward step;
- an emergency help event.

## MiMo AI Demo

Configure the runtime router in QEMU:

```bash
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
quit
```

Then run:

```bash
blind_badge_app obstacle 40 front --ai
```

Expected shape:

```text
[BlindBadge] event: obstacle_near
[BlindBadge] suggestion: 前方40厘米有障碍，请立即停下确认。
[BlindBadge] ai_prompt: BlindBadge event=obstacle_near distance_cm=40 direction=front...
[BlindBadge] ai_status: calling ai_agent llm router...
[BlindBadge] ai_response: 前方40厘米有障碍，请先停下确认。
```

The full AI demo is:

```bash
blind_badge_app demo --ai
```

It calls MiMo for every proactive event. Use it when network/backend latency is acceptable.

## Skill

Formal Skill file:

```text
skills/blind-badge/SKILL.md
```

Runtime ai_agent Skill location:

```text
AGENT_SKILLS_DIR/*.md
```

In the verified `goldfish-arm64-v8a-ap` QEMU image this is `/data/ai_agent/skills/*.md`.

Install and verify inside QEMU:

```bash
blind_badge_app install_skill
echo ask /skill | ai_agent
```

Expected `/skill` output includes `BlindBadge Safety Reminder Skill`.

The Skill constrains BlindBadge responses to one short Chinese safety sentence and defines event-specific behavior:

- `obstacle_near`: distance-based warning level;
- `step_down`: stop or slow down and confirm;
- `emergency`: short help message for a contact or nearby helper.

See `docs/blind_badge_skill.md` for install and verification notes.

## Verification

Fixed acceptance and submission docs:

```text
docs/acceptance_checklist.md
docs/technical_architecture.md
docs/demo_script_3min.md
docs/clean_reproduction_check.md
docs/final_submission_materials.md
```

Manual verification is recorded in:

```text
tests/qemu_manual_test.md
```

Known environment note: QEMU exits with an emulator wrapper segmentation fault after `Ctrl-A x` in this environment. The app commands complete before exit and were not observed to crash.
