# BlindBadge Skill

`skills/blind-badge/SKILL.md` is the custom AI Agent Skill for BlindBadge.

## Purpose

The Skill tells `ai_agent` how to answer BlindBadge safety events as a constrained safety-reminder assistant:

- return exactly one short Chinese sentence;
- prioritize immediate safety action;
- keep the voice reminder low-interruption;
- avoid long explanations;
- avoid absolute safety promises;
- use different wording for `obstacle_near`, `step_down`, and `emergency`;
- follow the event priority `emergency > step_down > near obstacle > far obstacle`.

## Runtime Location

`ai_agent` loads runtime Skill files from `AGENT_SKILLS_DIR`, which is built from `CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR`.

In the verified `goldfish-arm64-v8a-ap` QEMU image this resolves to:

```text
/data/ai_agent/skills/*.md
```

The loader scans this directory and injects a summary into the Agent context. The first line of each file is used as the Skill title, so the BlindBadge Skill starts directly with:

```text
# BlindBadge Safety Reminder Skill
```

Do not add YAML frontmatter before the title for this runtime Skill, otherwise the current loader may summarize the title as `---`.

## Install Method

BlindBadge provides an offline installer command so the demo does not depend on GitHub, adb, or an HTTPS download during QEMU judging:

```bash
blind_badge_app install_skill
```

It writes the embedded BlindBadge Skill to the same runtime directory used by `ai_agent`:

```text
/data/ai_agent/skills/blind-badge.md
```

Verify that `ai_agent` sees the Skill by sending `/skill` through the Agent CLI:

```bash
echo ask /skill | ai_agent
```

Expected list entry:

```text
- **BlindBadge Safety Reminder Skill**: ... (read with: read_file /data/ai_agent/skills/blind-badge.md)
```

Then validate behavior with a BlindBadge event:

```bash
ai_agent
router_set mimo <MIMO_TOKEN_PLAN_KEY>
ask BlindBadge event=obstacle_near distance_cm=40 direction=front
```

Verified QEMU response style:

```text
前方40厘米有障碍物，请停下确认。
```

## App Prompt Contract

`blind_badge_app` now emits structured prompt fragments that match this Skill:

```text
BlindBadge event=obstacle_near distance_cm=80 direction=front
BlindBadge event=step_down direction=front
BlindBadge event=emergency
```

This keeps the event type explicit for `ai_agent` and gives the Skill a stable hook for judging danger level and response style.

## Headless QEMU Note

In the current headless serial workflow, starting `ai_agent` interactively can compete with the NSH console for stdin. For repeatable tests, pipe one command into `ai_agent` with `echo ask ... | ai_agent`, then interrupt with `Ctrl-C` after the Agent prints the response.
