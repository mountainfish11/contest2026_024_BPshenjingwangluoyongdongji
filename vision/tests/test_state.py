"""引导状态机的单元测试，重点验证符号约定与滞回。"""
from __future__ import annotations

import unittest

from vision_assist.config import TactileThresholds
from vision_assist.state import GuideState, StateMachine
from vision_assist.temporal import TrackerState


def ts(**kw) -> TrackerState:
    base = dict(present=True, offset_m=0.0, heading_deg=0.0, lost=False)
    base.update(kw)
    return TrackerState(**base)


class TestBasicStates(unittest.TestCase):
    def setUp(self):
        self.sm = StateMachine(TactileThresholds())

    def test_no_path_when_not_present(self):
        self.assertEqual(self.sm.update(ts(present=False)).state, GuideState.NO_PATH)

    def test_path_lost(self):
        self.assertEqual(self.sm.update(ts(present=False, lost=True)).state,
                         GuideState.PATH_LOST)

    def test_aligned_when_centered(self):
        self.assertEqual(self.sm.update(ts()).state, GuideState.ALIGNED)

    def test_alert_does_not_become_state(self):
        """警报（提示砖）是正交事件，不能占住引导状态。

        实测故障：把提示砖做成状态后，Test.mp4 有 298/300 帧被 WARNING_TILE
        占满，转向与偏移信息全被吃掉。
        """
        d = self.sm.update(ts(warning_near_m=1.0, heading_deg=40.0))
        self.assertEqual(d.state, GuideState.TURN_RIGHT)
        self.assertNotIn("WARNING_TILE", [s.value for s in GuideState])


class TestSignConvention(unittest.TestCase):
    """offset 负=盲道在左→向左靠；heading 正=盲道向右偏→向右转。"""

    def setUp(self):
        self.sm = StateMachine(TactileThresholds())

    def test_negative_offset_means_drift_left(self):
        self.assertEqual(self.sm.update(ts(offset_m=-0.5)).state, GuideState.DRIFT_LEFT)

    def test_positive_offset_means_drift_right(self):
        self.assertEqual(self.sm.update(ts(offset_m=+0.5)).state, GuideState.DRIFT_RIGHT)

    def test_positive_heading_means_turn_right(self):
        self.assertEqual(self.sm.update(ts(heading_deg=+35.0)).state, GuideState.TURN_RIGHT)

    def test_negative_heading_means_turn_left(self):
        self.assertEqual(self.sm.update(ts(heading_deg=-35.0)).state, GuideState.TURN_LEFT)

    def test_turn_outranks_drift(self):
        d = self.sm.update(ts(heading_deg=40.0, offset_m=-0.9))
        self.assertEqual(d.state, GuideState.TURN_RIGHT)


class TestHysteresis(unittest.TestCase):
    def setUp(self):
        self.th = TactileThresholds(turn_enter_deg=20.0, turn_exit_deg=10.0,
                                    drift_enter_m=0.15, drift_exit_m=0.08)
        self.sm = StateMachine(self.th)

    def test_heading_between_thresholds_does_not_enter_turn(self):
        d = self.sm.update(ts(heading_deg=15.0))
        self.assertEqual(d.state, GuideState.ALIGNED)

    def test_heading_between_thresholds_holds_existing_turn(self):
        self.sm.update(ts(heading_deg=25.0))
        self.assertEqual(self.sm.state, GuideState.TURN_RIGHT)
        d = self.sm.update(ts(heading_deg=15.0))   # 回落到 enter/exit 之间
        self.assertEqual(d.state, GuideState.TURN_RIGHT, "滞回区间内应保持转向状态")
        d = self.sm.update(ts(heading_deg=5.0))    # 低于 exit
        self.assertEqual(d.state, GuideState.ALIGNED)

    def test_offset_hysteresis(self):
        self.sm.update(ts(offset_m=-0.20))
        self.assertEqual(self.sm.state, GuideState.DRIFT_LEFT)
        d = self.sm.update(ts(offset_m=-0.10))     # 在 enter/exit 之间
        self.assertEqual(d.state, GuideState.DRIFT_LEFT, "滞回区间内应保持偏移状态")
        d = self.sm.update(ts(offset_m=-0.05))     # 低于 exit
        self.assertEqual(d.state, GuideState.ALIGNED)

    def test_missing_heading_does_not_crash(self):
        d = self.sm.update(ts(heading_deg=None, offset_m=0.0))
        self.assertEqual(d.state, GuideState.ALIGNED)


class TestReset(unittest.TestCase):
    def test_reset_returns_to_no_path(self):
        sm = StateMachine()
        sm.update(ts(heading_deg=40.0))
        sm.reset()
        self.assertEqual(sm.state, GuideState.NO_PATH)


if __name__ == "__main__":
    unittest.main(verbosity=2)
