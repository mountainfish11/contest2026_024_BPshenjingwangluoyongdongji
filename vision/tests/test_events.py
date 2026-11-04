"""播报策略的单元测试：冷却、全局限速、抢占、文案。"""
from __future__ import annotations

import unittest

from vision_assist.alerts import LIGHT_RED, OBSTACLE_NEAR, WARNING_TILE, Alert
from vision_assist.config import SpeechThresholds
from vision_assist.events import SpeechPolicy, build_alert_text, build_text
from vision_assist.state import GuideState
from vision_assist.temporal import TrackerState


def tr(**kw) -> TrackerState:
    base = dict(present=True, offset_m=0.0, heading_deg=0.0)
    base.update(kw)
    return TrackerState(**base)


class TestText(unittest.TestCase):
    def test_centered_is_silent(self):
        self.assertIsNone(build_text(GuideState.ALIGNED, tr()))
        self.assertIsNone(build_text(GuideState.NO_PATH, tr()))

    def test_turn_texts(self):
        self.assertIn("左", build_text(GuideState.TURN_LEFT, tr()))
        self.assertIn("右", build_text(GuideState.TURN_RIGHT, tr()))

    def test_drift_text_uses_centimeters_under_one_meter(self):
        text = build_text(GuideState.DRIFT_LEFT, tr(offset_m=-0.25))
        self.assertIn("25厘米", text)
        self.assertIn("向左", text)

    def test_drift_text_uses_meters_over_one_meter(self):
        text = build_text(GuideState.DRIFT_RIGHT, tr(offset_m=1.4))
        self.assertIn("1.4米", text)

    def test_warning_tile_text(self):
        text = build_alert_text(Alert(kind=WARNING_TILE, priority=70, distance_m=1.5))
        self.assertIn("提示砖", text)
        self.assertIn("1.5米", text)

    def test_obstacle_text(self):
        text = build_alert_text(Alert(kind=OBSTACLE_NEAR, priority=100,
                                      distance_m=2.0, detail="行人"))
        self.assertIn("行人", text)
        self.assertIn("2.0米", text)

    def test_red_light_text(self):
        self.assertIn("红灯", build_alert_text(Alert(kind=LIGHT_RED, priority=50)))

    def test_lost_text(self):
        self.assertIn("找不到盲道", build_text(GuideState.PATH_LOST, tr()))


class TestAlertPriorityOverState(unittest.TestCase):
    def test_alert_beats_guidance_state(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=0.0))
        alert = Alert(kind=WARNING_TILE, priority=70, distance_m=1.2)
        d = policy.decide(GuideState.DRIFT_LEFT, tr(offset_m=-0.3), [alert], now=0.0)
        self.assertTrue(d.spoke)
        self.assertEqual(d.event, WARNING_TILE)

    def test_guidance_used_when_no_alert(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=0.0))
        d = policy.decide(GuideState.TURN_LEFT, tr(), [], now=0.0)
        self.assertEqual(d.event, "TURN_LEFT")

    def test_alerts_compete_by_priority(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=0.0))
        alerts = [Alert(kind=WARNING_TILE, priority=70, distance_m=1.2),
                  Alert(kind=OBSTACLE_NEAR, priority=100, distance_m=2.0, detail="行人")]
        d = policy.decide(GuideState.ALIGNED, tr(), alerts, now=0.0)
        self.assertEqual(d.event, OBSTACLE_NEAR, "应报优先级最高的警报")


class TestCooldown(unittest.TestCase):
    def test_same_event_suppressed_within_cooldown(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=0.0,
                                              cooldown_s={"TURN_LEFT": 3.0}))
        first = policy.decide(GuideState.TURN_LEFT, tr(), now=0.0)
        self.assertTrue(first.spoke)
        second = policy.decide(GuideState.TURN_LEFT, tr(), now=1.0)
        self.assertFalse(second.spoke)
        self.assertIn("cooldown", second.reason)

    def test_allowed_after_cooldown_expires(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=0.0,
                                              cooldown_s={"TURN_LEFT": 3.0}))
        policy.decide(GuideState.TURN_LEFT, tr(), now=0.0)
        later = policy.decide(GuideState.TURN_LEFT, tr(), now=3.5)
        self.assertTrue(later.spoke)

    def test_events_have_independent_cooldowns(self):
        """参照实现用一个全局 14 秒计时器，导致转弯提示被闲聊堵住；这里必须互不影响。"""
        policy = SpeechPolicy(SpeechThresholds(
            global_min_gap_s=0.0,
            cooldown_s={"DRIFT_LEFT": 30.0, "TURN_RIGHT": 3.0},
        ))
        self.assertTrue(policy.decide(GuideState.DRIFT_LEFT, tr(), now=0.0).spoke)
        # 1 秒后换事件，不应被 DRIFT_LEFT 的长冷却影响
        self.assertTrue(policy.decide(GuideState.TURN_RIGHT, tr(), now=1.0).spoke)

    def test_silent_state_never_speaks(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=0.0))
        d = policy.decide(GuideState.ALIGNED, tr(), now=0.0)
        self.assertFalse(d.spoke)
        self.assertEqual(d.reason, "silent_state")


class TestGlobalGap(unittest.TestCase):
    def test_low_priority_blocked_by_global_gap(self):
        policy = SpeechPolicy(SpeechThresholds(
            global_min_gap_s=5.0,
            cooldown_s={"DRIFT_LEFT": 0.0, "DRIFT_RIGHT": 0.0},
        ))
        self.assertTrue(policy.decide(GuideState.DRIFT_LEFT, tr(), now=0.0).spoke)
        d = policy.decide(GuideState.DRIFT_RIGHT, tr(), now=1.0)
        self.assertFalse(d.spoke)
        self.assertIn("global_gap", d.reason)

    def test_higher_priority_preempts_global_gap(self):
        policy = SpeechPolicy(SpeechThresholds(
            global_min_gap_s=5.0,
            cooldown_s={"DRIFT_LEFT": 0.0, "PATH_LOST": 0.0},
        ))
        self.assertTrue(policy.decide(GuideState.DRIFT_LEFT, tr(), now=0.0).spoke)
        # PATH_LOST(80) 优先级高于 DRIFT_LEFT(40)，应能抢占
        d = policy.decide(GuideState.PATH_LOST, tr(), now=0.5)
        self.assertTrue(d.spoke, "高优先级事件应能抢占全局限速")

    def test_equal_priority_cannot_preempt(self):
        policy = SpeechPolicy(SpeechThresholds(
            global_min_gap_s=5.0,
            cooldown_s={"DRIFT_LEFT": 0.0, "DRIFT_RIGHT": 0.0},
        ))
        policy.decide(GuideState.DRIFT_LEFT, tr(), now=0.0)
        self.assertFalse(policy.decide(GuideState.DRIFT_RIGHT, tr(), now=0.5).spoke)


class TestResetPolicy(unittest.TestCase):
    def test_reset_clears_cooldowns(self):
        policy = SpeechPolicy(SpeechThresholds(global_min_gap_s=100.0))
        policy.decide(GuideState.TURN_LEFT, tr(), now=0.0)
        policy.reset()
        self.assertTrue(policy.decide(GuideState.TURN_LEFT, tr(), now=1.0).spoke)


if __name__ == "__main__":
    unittest.main(verbosity=2)
