# Final Submission Materials

Use this page as the submission index for BlindBadge.

## Required Reading Order

1. `README.md`

   Product overview, build command, QEMU run command, basic demos, Skill verification entry point.

2. `docs/acceptance_checklist.md`

   Fixed end-to-end验收流程：clean QEMU, Skill install, Skill loader verification, fallback, MiMo, `demo --ai`, emergency action.

3. `docs/technical_architecture.md`

   Architecture and data flow:

   ```text
   simulated/sensor event -> event -> policy -> ai -> fallback -> action
   ```

4. `docs/demo_script_3min.md`

   3-minute video or live-demo script.

5. `docs/blind_badge_skill.md`

   Custom Skill role, runtime install method, `/skill` verification, standalone `ai_agent` behavior.

6. `tests/qemu_manual_test.md`

   Historical and latest QEMU verification logs.

7. `docs/clean_reproduction_check.md`

   Manifest, defconfig, runtime path, secret handling, and clean data checks.

8. `docs/product_design.md`

   User scenario and product MVP.

9. `protocol/blind_badge_protocol.md`

   Event protocol direction for later sensor integration.

## Key Demo Commands

```bash
blind_badge_app help
blind_badge_app install_skill
echo ask /skill | ai_agent
blind_badge_app obstacle 80 front
blind_badge_app obstacle 40 front --ai
blind_badge_app step_down front --ai
blind_badge_app emergency --ai
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
ask BlindBadge event=obstacle_near distance_cm=40 direction=front
quit
blind_badge_app demo --ai
```

## What Judges Should See

- A clear assistive hardware scenario for blind and low-vision users.
- A custom `ai_agent` Skill that is actually installed into the runtime loader path.
- Safety reminders constrained to one short Chinese sentence.
- Local deterministic fallback when MiMo is unavailable.
- Emergency help behavior that does not depend on AI.
- Simulated voice, vibration, and emergency-send execution actions.
- A proactive demo that presents the product loop, not just isolated commands.

## Risk And Fallback Summary

Risk:

- network unavailable;
- MiMo key missing;
- MiMo timeout or empty response;
- AI response too long;
- emergency event arrives during AI failure.

Fallback:

- local policy produces the final reminder;
- `fallback_reason` is printed;
- `ai_response` is filled with local safe wording;
- action simulation still runs;
- emergency always has a local help message and `action.emergency_send: triggered`.

## Later Hardware Integration

Replace only the event source first:

```text
ultrasonic/camera/cane input -> struct blind_badge_event
```

Keep the existing modules:

- policy thresholds;
- AI prompt generation;
- fallback;
- Skill rules;
- action execution mapping.

Then connect actions:

- `action.voice` -> TTS playback;
- `action.vibration` -> vibration motor;
- `action.emergency_send` -> phone companion, SMS, Feishu, or MQTT gateway.
