# BlindBadge Technical Architecture

BlindBadge is structured as a small AI hardware application rather than a loose set of shell commands. The current QEMU version uses simulated events in place of real sensors, but the internal pipeline is the same path expected on hardware.

## Data Flow

```text
sensor event / simulated CLI event
  -> event normalization
  -> local safety policy
  -> AI prompt generation
  -> ai_agent / MiMo request
  -> fallback guard
  -> final one-sentence reminder
  -> simulated device actions
```

## Runtime Flow

```text
blind_badge_app obstacle 40 front --ai
  -> blind_badge_event: type=obstacle_near, distance_cm=40, direction=front
  -> blind_badge_policy: severity=urgent, local suggestion=stop and confirm
  -> blind_badge_ai: call ai_agent llm_router / llm_proxy
  -> MiMo: return one short Chinese reminder
  -> blind_badge_policy: normalize to one short sentence
  -> blind_badge_action: voice + strong vibration
```

If AI is not available:

```text
blind_badge_ai error
  -> fallback_reason printed
  -> fallback_response = local safety reminder
  -> same action path still runs
```

## Modules

`app/blind_badge_app/blind_badge_app_main.c`

CLI entry point. It parses commands, validates arguments, starts demo scenarios, and prints the structured event output.

`blind_badge_event.c/.h`

Defines the shared event model:

- event type: `status`, `obstacle_near`, `step_down`, `emergency`;
- direction: `front`, `left`, `right`;
- severity: `info`, `warn`, `urgent`, `emergency`;
- distance in centimeters.

This module is where future real sensor input should be converted into the standard event model.

`blind_badge_policy.c/.h`

Contains deterministic safety rules:

- obstacle below 50cm: urgent, stop and confirm;
- obstacle from 50cm to 99cm: warn, slow down and avoid;
- obstacle 100cm or farther: info, keep attention;
- step-down event: urgent;
- emergency event: emergency.

It also builds the local `suggestion`, the `ai_prompt`, the fallback reminder, and normalizes AI output into one short sentence.

`blind_badge_ai.c/.h`

Adapts BlindBadge to `ai_agent`:

- `--ai`: directly calls the existing `llm_router` / `llm_proxy` path;
- `--agent`: message-bus prototype for a future background Agent service mode;
- all AI errors return a local fallback instead of breaking the safety loop.

Fallback triggers include:

- no router backend;
- init failure;
- message bus send failure;
- timeout;
- empty AI response;
- overlong AI output after normalization.

`blind_badge_action.c/.h`

Simulates hardware execution:

- `info`: voice only, vibration off;
- `warn`: voice + short vibration;
- `urgent`: voice + strong vibration;
- `emergency`: voice + strong vibration + emergency-send trigger.

These printed actions are the placeholders for real TTS, vibration motor, and emergency messaging integration.

`blind_badge_skill.c/.h`

Installs the embedded BlindBadge Skill into the runtime Skill directory used by `ai_agent`.

In the verified QEMU image:

```text
AGENT_SKILLS_DIR = /data/ai_agent/skills/
```

The installer command is:

```bash
blind_badge_app install_skill
```

## Skill Role

The runtime Skill is `skills/blind-badge/SKILL.md`. It tells `ai_agent` that BlindBadge is not a generic chatbot. It must answer event prompts as a safety reminder assistant:

- output exactly one Chinese sentence;
- keep it short and suitable for voice playback;
- put the safest immediate action first;
- avoid long explanations;
- avoid absolute safety promises;
- use obstacle, step-down, and emergency-specific behavior.

## Safety Properties

BlindBadge keeps safety-critical behavior local:

- every event has a deterministic local suggestion;
- emergency has a local help message;
- `--ai` failure is visible through `fallback_reason`;
- AI output is normalized before action execution;
- the action path runs even when MiMo is unavailable.

This means the prototype can demonstrate AI value while still behaving like an assistive device that must fail gracefully.

## Hardware Extension Plan

The current CLI simulator can be replaced incrementally:

- sensor driver reads distance or camera obstacle events;
- sensor adapter creates `struct blind_badge_event`;
- existing policy, AI, fallback, and action modules stay unchanged;
- `action.voice` maps to TTS playback;
- `action.vibration` maps to a vibration motor driver;
- `action.emergency_send` maps to SMS, Feishu, MQTT, or phone companion integration.
