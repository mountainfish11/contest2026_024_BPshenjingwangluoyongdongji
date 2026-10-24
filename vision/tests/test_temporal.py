"""时序平滑与去抖的单元测试。"""
from __future__ import annotations

import unittest

from vision_assist.config import TemporalThresholds
from vision_assist.detectors.tactile import TactileResult
from vision_assist.temporal import DebouncedFlag, Ema, TactileTracker, Vote


class TestEma(unittest.TestCase):
    def test_initializes_to_first_value(self):
        e = Ema(0.5)
        self.assertEqual(e.update(1.0), 1.0)

    def test_smooths_towards_new_value(self):
        e = Ema(0.25)
        e.update(0.0)
        v = e.update(1.0)
        self.assertAlmostEqual(v, 0.25, places=6)

    def test_none_keeps_previous(self):
        e = Ema(0.5)
        e.update(2.0)
        self.assertEqual(e.update(None), 2.0)

    def test_suppresses_spike(self):
        e = Ema(0.2)
        for _ in range(20):
            e.update(0.0)
        spike = e.update(1.0)
        self.assertLess(spike, 0.25, "单帧尖峰应被显著抑制")


class TestDebouncedFlag(unittest.TestCase):
    def test_needs_confirm_frames_to_turn_on(self):
        f = DebouncedFlag(confirm=3, release=2)
        self.assertFalse(f.update(True))
        self.assertFalse(f.update(True))
        self.assertTrue(f.update(True))

    def test_needs_release_frames_to_turn_off(self):
        f = DebouncedFlag(confirm=2, release=3)
        for _ in range(2):
            f.update(True)
        self.assertTrue(f.state)
        self.assertTrue(f.update(False))
        self.assertTrue(f.update(False))
        self.assertFalse(f.update(False))

    def test_single_frame_dropout_does_not_flip(self):
        f = DebouncedFlag(confirm=2, release=3)
        f.update(True); f.update(True)
        f.update(False)          # 单帧漏检
        self.assertTrue(f.update(True))
        self.assertTrue(f.state)


class TestVote(unittest.TestCase):
    def test_majority_switch(self):
        v = Vote(window=5, min_ratio=0.6)
        for _ in range(5):
            v.update("guide")
        self.assertEqual(v.state, "guide")

    def test_ignores_single_outlier(self):
        v = Vote(window=5, min_ratio=0.6)
        for _ in range(5):
            v.update("guide")
        v.update("warning")
        self.assertEqual(v.state, "guide")


class TestTracker(unittest.TestCase):
    def _result(self, present=True, offset=0.0, heading=0.0, tile="guide", warn=None):
        return TactileResult(present=present, confidence=0.8, lateral_offset_m=offset,
                             heading_deg=heading, tile_type=tile,
                             warning_near_m=warn, near_m=1.0, far_m=3.0)

    def test_presence_debounced(self):
        tr = TactileTracker(TemporalThresholds(confirm_frames=3, release_frames=5),
                            lost_after_s=1.0)
        self.assertFalse(tr.update(self._result(present=True), 0.0).present)
        self.assertFalse(tr.update(self._result(present=True), 0.1).present)
        self.assertTrue(tr.update(self._result(present=True), 0.2).present)

    def test_lost_after_timeout(self):
        tr = TactileTracker(TemporalThresholds(confirm_frames=2, release_frames=2),
                            lost_after_s=1.0)
        for i in range(4):
            tr.update(self._result(present=True), i * 0.1)
        # 突然间完全看不到
        state = None
        for i in range(100):
            state = tr.update(self._result(present=False), 0.4 + i * 0.05)
        self.assertFalse(state.present)
        self.assertTrue(state.lost, "持续缺失超过阈值后应标记 lost")

    def test_values_are_smoothed(self):
        tr = TactileTracker(TemporalThresholds(confirm_frames=1, ema_alpha=0.3))
        tr.update(self._result(offset=0.0), 0.0)
        state = tr.update(self._result(offset=1.0), 0.1)
        self.assertLess(state.offset_m, 0.4, "横向偏移应被平滑而不是直接跳变")

    def test_warning_cleared_when_absent(self):
        tr = TactileTracker(TemporalThresholds(confirm_frames=1, release_frames=1))
        s1 = tr.update(self._result(warn=1.5), 0.0)
        self.assertAlmostEqual(s1.warning_near_m, 1.5, places=3)
        s2 = tr.update(self._result(present=False), 0.1)
        self.assertIsNone(s2.warning_near_m)

    def test_tile_type_voted(self):
        tr = TactileTracker(TemporalThresholds(confirm_frames=1))
        for _ in range(7):
            state = tr.update(self._result(tile="guide"), 0.0)
        self.assertEqual(state.tile_type, "guide")


if __name__ == "__main__":
    unittest.main(verbosity=2)
