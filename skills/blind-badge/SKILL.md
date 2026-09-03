# BlindBadge Safety Reminder Skill

BlindBadge is a wearable assistive badge for blind and low-vision users. This Skill turns sensor events into short Chinese safety reminders for voice playback.

## When to use

Use this Skill when the user mentions BlindBadge, 盲人辅助胸牌, obstacle_near, step_down, emergency, 障碍, 台阶, 下行台阶, 求助, 避障, or asks for a safety reminder based on a BlindBadge event.

Expected event shape:

```text
BlindBadge event=<obstacle_near|step_down|emergency> distance_cm=<number> direction=<front|left|right>
```

## Output rules

- Output exactly one Chinese sentence.
- Keep the sentence short, clear, and low-interruption for voice playback.
- Put the safest immediate action first.
- Use calm wording; do not scare the user.
- Do not provide a long explanation.
- Do not promise absolute safety.
- Do not say the environment is definitely safe.
- Do not mention that this is a simulation unless the user asks.
- Do not include Markdown, bullet points, labels, or extra commentary.
- Prefer concrete direction words: 前方, 左侧, 右侧.
- If confidence is low or information is incomplete, use cautious wording such as 可能 or 疑似.
- If the input conflicts, prioritize the more dangerous signal.

## Event rules

For `obstacle_near`:

- If `distance_cm` is below 50, tell the user to stop first and confirm.
- If `distance_cm` is 50 to 99, tell the user to slow down and avoid the obstacle.
- If `distance_cm` is 100 or greater, tell the user to keep attention without over-warning.
- Include direction when present.
- Avoid saying the path is blocked unless that was explicitly detected.

For `step_down`:

- Tell the user to stop or slow down first.
- Mention that there may be a downward step.
- Suggest confirming with a cane, foot, handrail, or nearby support.
- Treat `step_down` as higher priority than a far obstacle.

For `emergency`:

- Generate a help message suitable for a contact or nearby helper.
- Keep it calm, direct, and actionable.
- Do not claim a precise location unless a location is provided.
- If no location is provided, ask the helper to contact or come nearby to confirm.

## Priority

Use this danger priority when multiple events are present:

```text
emergency > step_down > obstacle below 50cm > obstacle 50-99cm > obstacle 100cm or greater > status
```

## Examples

User: BlindBadge event=obstacle_near distance_cm=120 direction=front

Assistant: 前方较远处有障碍，请保持注意。

User: BlindBadge event=obstacle_near distance_cm=80 direction=front

Assistant: 前方约80厘米有障碍，请减速并小心绕行。

User: BlindBadge event=obstacle_near distance_cm=40 direction=left

Assistant: 左侧约40厘米有障碍，请先停下确认。

User: BlindBadge event=step_down direction=front

Assistant: 前方疑似下行台阶，请停下并用手杖确认。

User: BlindBadge event=emergency

Assistant: 我可能需要帮助，请尽快联系我或到我身边确认情况。
