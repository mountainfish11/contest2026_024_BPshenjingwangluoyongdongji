# BlindBadge ESP32-S3-EYE Hardware Plan

## Positioning

BlindBadge v1 hardware is positioned as:

```text
An ESP32-S3-EYE based safety-reminder prototype for blind and low-vision users.
```

It is not positioned as a mature blind navigation device, and it does not replace a cane, guide dog, mobility training, or professional assistive equipment.

The v1 goal is to demonstrate a credible hardware loop:

```text
on-board input / simulated hazard -> local policy -> MiMo ai_agent reminder -> fallback -> visible/audible/tactile action
```

## Board Capability Mapping

ESP32-S3-EYE resource mapping for BlindBadge:

| Board resource | BlindBadge v1 use |
| --- | --- |
| OV2640 camera | Forward scene input, obstacle image capture, motion or scene-change signal. Do not claim reliable centimeter-level distance from the monocular camera. |
| I2S MEMS microphone | Voice trigger, voice command, or later conversational input. |
| 5 configurable function buttons | Emergency, mode switch, confirm, mute, and replay reminder. RST is reset only, not a normal app button. |
| QMA7981 accelerometer | Wearing posture, strong motion, fall-like motion prototype, and posture hints for camera interpretation. |
| LCD | Developer and judge display for event type, reminder text, network state, and fallback state. It is not the primary user output for blind users. |
| Wi-Fi | Network path for MiMo Token Plan and ai_agent / LLM Router integration. |
| Battery pads and charger | Mobile wearable prototype story. Use a protected lithium battery, recommended capacity above 1000mAh. |
| MicroSD | Optional logs, captured samples, and offline test records. |

## Parts Not On Board

The ESP32-S3-EYE guide does not describe these as board resources:

- speaker;
- vibration motor;
- ToF or ultrasonic distance sensor;
- reliable depth sensor.

BlindBadge must not imply these exist on the base board.

Recommended v1 output choices:

- minimum real output: external vibration motor or buzzer;
- development output: LCD and serial logs;
- later output: speaker or bone-conduction audio module.

Recommended v1 input choices:

- minimum real input: function button triggers `emergency`;
- simulated obstacle input: serial command or preset event;
- camera input: scene capture or motion/change signal, not proven distance;
- accelerometer input: posture or strong-motion signal.

## Recommended v1 Hardware Loop

1. A function button triggers `emergency`.
2. Camera or preset serial event triggers `obstacle_near`.
3. BlindBadge policy maps the event to severity.
4. MiMo generates one short safety reminder when network and key are available.
5. Local fallback generates the reminder when MiMo is unavailable.
6. LCD and serial logs display the final reminder.
7. External vibration motor or buzzer simulates the real reminder action.

Expected demo behavior:

- pressing a function button shows an `emergency` event on serial/LCD;
- emergency always produces a local help message;
- emergency triggers vibration/buzzer and `emergency_send` simulation;
- obstacle event produces a short reminder such as `前方40厘米有障碍，请先停下确认。`;
- network or MiMo failure does not crash the app and activates fallback.

## What Not To Claim In v1

Do not claim:

- mature blind navigation;
- replacement for cane or guide dog;
- reliable centimeter-level distance from only the OV2640 monocular camera;
- real step-down detection unless a tested algorithm or sensor proof is added;
- LCD as the primary output for blind users;
- built-in speaker, vibration motor, ToF, or ultrasonic hardware on ESP32-S3-EYE.

## Layered Delivery Plan

### Current Software Layer

- QEMU event simulator;
- openvela `blind_badge_app`;
- `ai_agent` Skill installation;
- MiMo AI loop;
- local fallback;
- simulated voice/vibration/emergency-send actions.

### Minimum Hardware Layer

- ESP32-S3-EYE board;
- function button for `emergency`;
- LCD and serial logs for judging/debug;
- Wi-Fi for MiMo;
- optional external vibration motor or buzzer.

### Later Sensor Layer

- external ToF or ultrasonic sensor for real distance;
- speaker or bone-conduction audio for voice output;
- better camera pipeline for crosswalk, traffic light, door, or sign recognition;
- accelerometer-based wearing state and fall-like motion detection.

## Integration Boundary With Current Code

The existing openvela app already separates the software pipeline:

```text
event -> policy -> ai -> fallback -> action
```

ESP32-S3-EYE integration should first replace the event source, not the whole application:

```text
button/camera/accelerometer/Wi-Fi event -> struct blind_badge_event
```

After that, the same policy, Skill, fallback, and action rules can be reused or mirrored on the hardware target.
