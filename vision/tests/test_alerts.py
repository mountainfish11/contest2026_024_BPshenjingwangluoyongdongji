"""警报评估的单元测试。

重点验证"脚下提示砖"与"前方提示砖"的区分——这是实测踩出来的坑：
Test.mp4 里提示砖常在脚下（约 0 米），若一律当"前方有提示砖"报警，
警报会永久触发，把所有引导信息淹没。
"""
from __future__ import annotations

import unittest

from vision_assist.alerts import (
    WARNING_TILE,
    Alert,
    AlertEvaluator,
    AlertResult,
)
from vision_assist.config import AppConfig, TactileThresholds
from vision_assist.detectors.tactile import TactileBox, TactileResult


def make_tactile(items):
    """items: [(tile_type, near_m), ...]"""
    r = TactileResult(present=True)
    r.boxes_detail = [TactileBox(box=(0, 0, 10, 10), conf=0.9,
                                 tile_type=t, near_m=n, far_m=(n or 0) + 1)
                      for t, n in items]
    return r


class TestAlertResult(unittest.TestCase):
    def test_top_picks_highest_priority(self):
        r = AlertResult(alerts=[Alert(kind="A", priority=10),
                                Alert(kind="B", priority=50)])
        self.assertEqual(r.top().kind, "B")

    def test_top_of_empty_is_none(self):
        self.assertIsNone(AlertResult().top())

    def test_serializes(self):
        r = AlertResult(alerts=[Alert(kind=WARNING_TILE, priority=70, distance_m=1.234)])
        d = r.to_dict()
        self.assertEqual(d["alerts"][0]["distance_m"], 1.234)
        self.assertFalse(d["on_warning_tile"])


class TestWarningTile(unittest.TestCase):
    def setUp(self):
        self.cfg = AppConfig()
        self.cfg.tactile = TactileThresholds(warning_tile_near_m=2.5,
                                             warning_tile_underfoot_m=0.6)
        self.ev = AlertEvaluator(self.cfg)

    def test_no_detection_no_alerts(self):
        r = self.ev.evaluate(TactileResult(present=False))
        self.assertEqual(r.alerts, [])

    def test_tile_underfoot_is_not_an_alert(self):
        """踩在提示砖上只是"位置事实"，不该报警。"""
        r = self.ev.evaluate(make_tactile([("warning", 0.0)]))
        self.assertEqual(r.alerts, [])
        self.assertTrue(r.on_warning_tile)

    def test_tile_ahead_raises_alert(self):
        r = self.ev.evaluate(make_tactile([("warning", 1.8)]))
        self.assertEqual(len(r.alerts), 1)
        self.assertEqual(r.alerts[0].kind, WARNING_TILE)
        self.assertAlmostEqual(r.alerts[0].distance_m, 1.8)
        self.assertFalse(r.on_warning_tile)

    def test_tile_beyond_range_is_not_an_alert(self):
        r = self.ev.evaluate(make_tactile([("warning", 5.0)]))
        self.assertEqual(r.alerts, [])

    def test_guide_tile_never_alerts(self):
        r = self.ev.evaluate(make_tactile([("guide", 1.0), ("guide", 0.2)]))
        self.assertEqual(r.alerts, [])
        self.assertFalse(r.on_warning_tile)

    def test_picks_nearest_warning_ahead(self):
        r = self.ev.evaluate(make_tactile([("warning", 2.0), ("warning", 1.1)]))
        self.assertAlmostEqual(r.alerts[0].distance_m, 1.1)

    def test_underfoot_and_ahead_together(self):
        r = self.ev.evaluate(make_tactile([("warning", 0.1), ("warning", 1.5)]))
        self.assertTrue(r.on_warning_tile, "脚下有提示砖也应记录")
        self.assertEqual(len(r.alerts), 1, "但只对前方那个报警")

    def test_mixed_tile_types(self):
        r = self.ev.evaluate(make_tactile([("guide", 0.5), ("warning", 2.0)]))
        self.assertEqual(len(r.alerts), 1)
        self.assertEqual(r.alerts[0].kind, WARNING_TILE)


class TestOtherAlertSources(unittest.TestCase):
    """障碍物 / 斑马线 / 台阶 / 红绿灯 的警报接线。"""

    def setUp(self):
        self.cfg = AppConfig()
        self.ev = AlertEvaluator(self.cfg)

    def _obstacles(self, nearest):
        from vision_assist.detectors.obstacle import Obstacle, ObstacleResult
        return ObstacleResult(
            obstacles=[Obstacle(label="person", label_cn="行人", conf=0.8,
                                box=(0, 0, 10, 10), distance_m=nearest)],
            nearest_m=nearest,
        )

    def test_near_obstacle_alerts(self):
        from vision_assist.alerts import OBSTACLE_NEAR
        r = self.ev.evaluate(TactileResult(), obstacles=self._obstacles(1.5))
        kinds = [a.kind for a in r.alerts]
        self.assertIn(OBSTACLE_NEAR, kinds)
        self.assertEqual(r.alerts[0].detail, "行人")

    def test_far_obstacle_does_not_alert(self):
        from vision_assist.alerts import OBSTACLE_NEAR
        r = self.ev.evaluate(TactileResult(), obstacles=self._obstacles(9.0))
        self.assertNotIn(OBSTACLE_NEAR, [a.kind for a in r.alerts])

    def test_crosswalk_alerts_above_confidence(self):
        from vision_assist.alerts import CROSSWALK
        from vision_assist.detectors.crosswalk import CrosswalkResult
        cw = CrosswalkResult(present=True, confidence=0.8, bar_count=5, near_m=2.0)
        r = self.ev.evaluate(TactileResult(), crosswalk=cw)
        self.assertIn(CROSSWALK, [a.kind for a in r.alerts])

    def test_low_confidence_crosswalk_silent(self):
        from vision_assist.alerts import CROSSWALK
        from vision_assist.detectors.crosswalk import CrosswalkResult
        cw = CrosswalkResult(present=True, confidence=0.2, bar_count=5)
        r = self.ev.evaluate(TactileResult(), crosswalk=cw)
        self.assertNotIn(CROSSWALK, [a.kind for a in r.alerts])

    def test_stairs_alert(self):
        from vision_assist.alerts import STAIRS
        from vision_assist.detectors.stairs import StairsResult
        r = self.ev.evaluate(TactileResult(),
                             stairs=StairsResult(suspected=True, confidence=0.5, distance_m=1.5))
        self.assertIn(STAIRS, [a.kind for a in r.alerts])

    def test_red_and_green_lights(self):
        from vision_assist.alerts import LIGHT_GREEN, LIGHT_RED
        from vision_assist.detectors.traffic_light import TrafficLightResult
        red = self.ev.evaluate(TactileResult(),
                               traffic_light=TrafficLightResult(present=True, state="red"))
        self.assertIn(LIGHT_RED, [a.kind for a in red.alerts])
        green = self.ev.evaluate(TactileResult(),
                                 traffic_light=TrafficLightResult(present=True, state="green"))
        self.assertIn(LIGHT_GREEN, [a.kind for a in green.alerts])
        unknown = self.ev.evaluate(TactileResult(),
                                   traffic_light=TrafficLightResult(present=True, state="unknown"))
        self.assertEqual(unknown.alerts, [])

    def test_obstacle_outranks_warning_tile(self):
        from vision_assist.alerts import OBSTACLE_NEAR
        tactile = make_tactile([("warning", 1.0)])
        r = self.ev.evaluate(tactile, obstacles=self._obstacles(1.2))
        top = r.top()
        self.assertIsNotNone(top)
        self.assertEqual(top.kind, OBSTACLE_NEAR, "障碍物优先级应高于提示砖")


if __name__ == "__main__":
    unittest.main(verbosity=2)
