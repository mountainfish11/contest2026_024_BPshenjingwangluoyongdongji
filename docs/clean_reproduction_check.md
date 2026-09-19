# Clean Reproduction Check

This document records the checks needed before final submission so another machine or a clean QEMU data image can reproduce the demo.

## Repository Mapping

Team manifest:

```text
contest2026_024_BPshenjingwangluoyongdongji.xml
```

Required linkfile:

```xml
<linkfile src="app/blind_badge_app" dest="packages/demos/contest2026_024_blind_badge_app"/>
```

Status:

```text
PASS - the linkfile maps the whole app/blind_badge_app directory, so new module files are included.
```

## Build Configuration

Board config:

```text
vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/defconfig
```

Required option:

```text
CONFIG_LVX_USE_DEMO_CONTEST2026_024_BLIND_BADGE_APP=y
```

Status:

```text
PASS - defconfig enables blind_badge_app.
```

## Hardware Workspace Dependencies

QEMU reproduction is contained in this contest repository. ESP32-S3-EYE hardware reproduction additionally depends on local openvela workspace changes in:

```text
nuttx
packages/ai_agent
apps
```

The dependency file list and purpose are recorded in:

```text
docs/openvela_workspace_dependencies.md
```

Status:

```text
DOCUMENTED - public-repository deltas are listed for hardware review.
```

## Runtime Skill Directory

`blind_badge_app install_skill` creates the needed directories and writes to `AGENT_SKILLS_DIR`.

Verified QEMU path:

```text
/data/ai_agent/skills/blind-badge.md
```

Status:

```text
PASS - no pre-existing data directory is required.
```

## Local Absolute Paths

The source and docs do not require local workspace paths such as `/home/GGB/openvela` for runtime behavior. Build and QEMU commands are run from the openvela workspace root.

Status:

```text
PASS - runtime paths are openvela/QEMU paths, not this machine's private absolute paths.
```

## Uncommitted File Dependency

Known untracked local files:

```text
app/blind_badge_app/blind_badge_app_main.c.bak
docs/task_board.md
```

Status:

```text
PASS - these files are not needed for build, QEMU, Skill install, fallback, or AI demo.
```

## Secret Handling

The real MiMo key must only be provided at runtime or through a local untracked build-only secret file:

```bash
router_set mimo <MIMO_TOKEN_PLAN_KEY>
ai_agent router_import_mimo
```

Do not commit the real key. For ESP32-S3-EYE validation, `router_import_mimo` may read `/data/agent/config/mimo.key` or a local ignored `ai_agent_local_secrets.h` file used only for a private firmware build.

Status:

```text
PASS - repository docs use <MIMO_TOKEN_PLAN_KEY> placeholder.
```

## Clean QEMU Behavior

Without MiMo configured:

```bash
blind_badge_app demo --ai
```

Expected:

```text
fallback: active
```

With MiMo configured:

```bash
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
ask BlindBadge event=obstacle_near distance_cm=40 direction=front
quit
blind_badge_app demo --ai
```

Expected:

```text
MiMo returns one short safety reminder, or fallback keeps the safety loop alive if network/backend fails.
```

On ESP32-S3-EYE, if official long-key serial input is unreliable, use:

```bash
ai_agent router_import_mimo
ai_agent ask hi
blind_badge_app obstacle 40 front --ai --lcd
```

Expected:

```text
ai_agent ask hi returns a real MiMo response
blind_badge_app prints ai_response without fallback: active
LCD displays the final MiMo reminder text
```

## Pre-Submission Commands

Run from `/home/GGB/openvela`:

```bash
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap --cmake -j2
git -C contest2026_024_BPshenjingwangluoyongdongji diff --check
rg -n "tp-c1" contest2026_024_BPshenjingwangluoyongdongji packages/ai_agent -S
git -C contest2026_024_BPshenjingwangluoyongdongji status --short
```

Expected:

```text
build completed successfully
diff --check prints nothing
secret search prints nothing
status shows no unintended tracked changes
```
