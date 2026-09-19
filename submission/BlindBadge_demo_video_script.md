# BlindBadge Demo Video Script

Generated companion script for `BlindBadge_demo_video.mp4`.

Duration: about 2 minutes 44 seconds.

1. Opening: BlindBadge is an AI assistive badge for blind and low-vision users. It turns hazard events into short safety reminders and keeps local fallback.
2. User story: nearby obstacle, downward step, and emergency help are converted into one action-oriented reminder.
3. Architecture: event -> policy -> ai_prompt -> ai_agent/MiMo -> fallback -> voice/vibration/emergency_send.
4. openvela: QEMU demo plus ESP32-S3-EYE hardware validation use openvela NSH app, Wi-Fi, LCD, and ai_agent.
5. Skill: BlindBadge Safety Reminder Skill constrains AI to one short Chinese safety sentence.
6. QEMU: demo --ai shows proactive obstacle stages, step_down, and emergency.
7. Hardware: ESP32-S3-EYE validation records Wi-Fi/DNS, real MiMo reply, LCD update, and BlindBadge AI actions.
8. Fallback: AI/network failure still returns local safe wording and keeps actions running.
9. Submission: code, logs, and workspace dependency notes are in the PR.
10. Closing: BlindBadge is a safety-first AI hardware loop, not a general chatbot.
