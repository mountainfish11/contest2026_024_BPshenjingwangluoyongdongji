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

`ai_agent` loads runtime Skill files from:

```text
/data/agent/skills/*.md
```

The loader scans this directory and injects a summary into the Agent context. The first line of each file is used as the Skill title, so the BlindBadge Skill starts directly with:

```text
# BlindBadge Safety Reminder Skill
```

Do not add YAML frontmatter before the title for this runtime Skill, otherwise the current loader may summarize the title as `---`.

## Install Method

`ai_agent` provides:

```bash
install_skill <name> <https-url>
```

The current implementation downloads the Markdown file from an HTTPS URL and writes it to:

```text
/data/agent/skills/<name>.md
```

After this repository is pushed, use the raw GitHub URL for the Skill file:

```bash
install_skill blind-badge https://raw.githubusercontent.com/open-vela/contest2026_024_BPshenjingwangluoyongdongji/dev-ai-contest-2026/skills/blind-badge/SKILL.md
```

Then trigger or refresh Agent context by asking a BlindBadge event:

```bash
ask BlindBadge event obstacle_near distance_cm=80 direction=front
```

Expected style:

```text
前方约80厘米有障碍，请减速并小心绕行。
```

## App Prompt Contract

`blind_badge_app` now emits structured prompt fragments that match this Skill:

```text
BlindBadge event=obstacle_near distance_cm=80 direction=front
BlindBadge event=step_down direction=front
BlindBadge event=emergency
```

This keeps the event type explicit for `ai_agent` and gives the Skill a stable hook for judging danger level and response style.

## Known Limitation

Although the CLI help mentions `install_skill <name> <url|->`, the current source implementation only accepts HTTPS URLs. A local stdin install such as `install_skill blind-badge -` is rejected because the argument does not start with `https://`.

This means QEMU Skill demo should use one of these methods:

- install from a pushed raw HTTPS URL;
- push/copy the file into `/data/agent/skills/` by another supported transport such as `adb push` when `adb` is available;
- add a future project helper that copies the repository Skill into the Agent data directory.
