# openvela Workspace Dependencies

BlindBadge has two reproduction levels:

1. QEMU demo: contained in this contest repository and the team manifest.
2. ESP32-S3-EYE hardware demo: also depends on local openvela workspace changes in `nuttx`, `packages/ai_agent`, and `apps`.

The contest repository contains the BlindBadge application, Skill, docs, and validation records. The files below describe the additional workspace deltas used for the hardware validation recorded in `docs/esp32_s3_eye_validation_2026-09-04.md`.

## Base Repositories

```text
nuttx HEAD:             861540f7d52c90d6d28f814e6f0b6088c8ecf525
packages/ai_agent HEAD: ad6a47520e9abd8a2b6a58676b527dc24013ca9c
apps HEAD:              c90b4e6c58afcdcea2ad5f0a742c5b9ffdb0d4d4
```

These repositories were in detached-HEAD state during hardware validation.

## nuttx Changes

Purpose:

- enable ESP32-S3-EYE board bring-up for BlindBadge hardware validation;
- stabilize ESP32-S3 Wi-Fi runtime behavior used by `ai_agent`;
- enable and tune I2S microphone capture for `/dev/audio/pcm_in0`;
- expose board resources needed by the ESP32-S3-EYE demo.

Modified tracked files:

```text
arch/xtensa/src/esp32s3/Make.defs
arch/xtensa/src/esp32s3/Wireless.mk
arch/xtensa/src/esp32s3/esp32s3_i2s.c
arch/xtensa/src/esp32s3/esp32s3_start.c
arch/xtensa/src/esp32s3/esp32s3_wifi_adapter.c
arch/xtensa/src/esp32s3/esp32s3_wifi_utils.c
arch/xtensa/src/esp32s3/esp32s3_wireless.c
boards/xtensa/esp32s3/common/src/esp32s3_board_i2s.c
boards/xtensa/esp32s3/esp32s3-eye/configs/wifi/defconfig
boards/xtensa/esp32s3/esp32s3-eye/src/Make.defs
boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3-eye.h
boards/xtensa/esp32s3/esp32s3-eye/src/esp32s3_bringup.c
sched/init/nx_bringup.c
sched/init/nx_start.c
```

Additional untracked local board data:

```text
boards/xtensa/esp32s3/esp32s3-eye/src/etc/
```

Current diff size:

```text
14 tracked files changed, 343 insertions, 41 deletions
```

## packages/ai_agent Changes

Purpose:

- support direct `ai_agent <cmd>` CLI use without leaving the NSH console stuck;
- support MiMo router import and runtime key handling without committing credentials;
- improve network status gating for BlindBadge AI calls;
- add voice command scaffolding used during ESP32-S3-EYE validation.

Modified tracked files:

```text
docs/cli.md
src/agent_main.c
src/channels/cmd_voice.c
src/channels/cmd_voice.h
src/channels/nsh_commands.c
src/channels/nsh_commands.h
src/infra/network_manager.c
src/infra/network_manager.h
src/infra/vela_tls.c
src/llm/llm_router.c
src/llm/llm_router.h
```

Local backup files that are not part of the submission:

```text
src/llm/llm_proxy.c.bak.before_mimo_thinking
src/llm/llm_proxy.c.bak.mimo_thinking
```

Current diff size:

```text
11 tracked files changed, 738 insertions, 112 deletions
```

## apps Changes

Purpose:

- improve `nxrecorder` raw microphone capture behavior for ESP32-S3-EYE I2S validation;
- support the microphone capture checks referenced by the hardware validation record.

Modified tracked files:

```text
include/system/nxrecorder.h
system/nxrecorder/nxrecorder.c
```

Untracked local test data not needed for BlindBadge submission:

```text
testing/drivers/nist-sts/.gitee/
testing/drivers/nist-sts/.github/
testing/drivers/nist-sts/sts/
```

Current diff size:

```text
2 tracked files changed, 129 insertions, 27 deletions
```

## Reproduction Notes

For QEMU review, judges can use the contest repository and `contest2026_024_BPshenjingwangluoyongdongji.xml`.

For ESP32-S3-EYE hardware review, apply or review the workspace deltas above before building from `/home/GGB/openvela/nuttx`. The hardware validation document records the validated runtime path:

```text
ESP32-S3-EYE boot -> nsh> -> Wi-Fi/DNS -> ai_agent direct commands
-> MiMo router import/runtime key handling -> blind_badge_app --ai --lcd
```

No real MiMo key, Wi-Fi password, private header, or build-only credential file is included in this repository. Credentials must be provided at runtime or through local untracked files only.
