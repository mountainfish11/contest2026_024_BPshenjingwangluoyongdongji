"""斑马线 / 台阶 / 障碍物 / 红绿灯 的单元测试。

全部用构造数据验证后处理逻辑，不依赖真实图像或模型。
"""
from __future__ import annotations

import unittest

import cv2
import numpy as np

from vision_assist.config import (
    AppConfig,
    CameraCalib,
    CrosswalkThresholds,
    ObstacleThresholds,
    StairsThresholds,
    TrafficLightThresholds,
)
from vision_assist.detectors.crosswalk import (
    CrosswalkResult,
    analyze_profile,
    count_runs,
)
from vision_assist.detectors.obstacle import build_obstacles, ground_from_box
from vision_assist.detectors.stairs import StairsResult, find_step
from vision_assist.detectors.traffic_light import (
    STATE_GREEN,
    STATE_RED,
    STATE_UNKNOWN,
    TrafficLightAnalyzer,
    classify_color,
    color_ratios,
)
from vision_assist.geometry import CameraModel


def make_camera() -> CameraModel:
    return CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                   height_m=1.20, pitch_deg=30.0), 640, 480)


def make_z_axis(n: int, z0: float = 0.8, step: float = 0.02) -> np.ndarray:
    return z0 + np.arange(n) * step


# --------------------------------------------------------------------------- #
# 斑马线
# --------------------------------------------------------------------------- #
class TestCountRuns(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(count_runs(np.array([], bool)), 0)

    def test_all_false(self):
        self.assertEqual(count_runs(np.zeros(10, bool)), 0)

    def test_all_true(self):
        self.assertEqual(count_runs(np.ones(10, bool)), 1)

    def test_alternating(self):
        self.assertEqual(count_runs(np.array([1, 0, 1, 0, 1], bool)), 3)


class TestCrosswalkProfile(unittest.TestCase):
    def setUp(self):
        self.th = CrosswalkThresholds(min_bars=4, min_white_ratio=0.04,
                                      alert_min_confidence=0.5)

    def _square_wave(self, n=200, period=20, duty=0.5, high=0.7, low=0.02):
        p = np.full(n, low)
        for i in range(0, n, period):
            p[i:i + int(period * duty)] = high
        return p

    def test_periodic_bars_detected(self):
        prof = self._square_wave()
        z = make_z_axis(prof.size)
        r = analyze_profile(prof, z, self.th)
        self.assertTrue(r.present, f"规则方波应判为斑马线（detail={r.detail}）")
        self.assertGreaterEqual(r.bar_count, 4)
        self.assertGreaterEqual(r.confidence, 0.5)
        self.assertIsNotNone(r.near_m)

    def test_flat_road_not_detected(self):
        prof = np.full(200, 0.01)
        r = analyze_profile(prof, make_z_axis(prof.size), self.th)
        self.assertFalse(r.present)
        self.assertEqual(r.bar_count, 0)

    def test_uniformly_white_not_detected(self):
        """整片发白（比如逆光过曝）不该被当成斑马线。"""
        prof = np.full(200, 0.9)
        r = analyze_profile(prof, make_z_axis(prof.size), self.th)
        self.assertFalse(r.present, "整片全白没有周期结构，不应判为斑马线")

    def test_too_few_bars(self):
        prof = np.zeros(200)
        prof[50:70] = 0.8
        r = analyze_profile(prof, make_z_axis(prof.size), self.th)
        self.assertFalse(r.present)
        self.assertLess(r.bar_count, self.th.min_bars)

    def test_short_profile_is_safe(self):
        r = analyze_profile(np.zeros(4), make_z_axis(4), self.th)
        self.assertFalse(r.present)
        self.assertEqual(r.detail, "profile_too_short")

    def test_serializes(self):
        r = CrosswalkResult(present=True, confidence=0.8123, bar_count=5,
                            white_ratio=0.4, near_m=1.234)
        d = r.to_dict()
        self.assertEqual(d["confidence"], 0.812)
        self.assertEqual(d["near_m"], 1.234)


# --------------------------------------------------------------------------- #
# 台阶
# --------------------------------------------------------------------------- #
class TestStairsStep(unittest.TestCase):
    def setUp(self):
        self.th = StairsThresholds(edge_response_min=0.35, confirm_frames=4,
                                   z_min_m=0.8, z_max_m=2.5)

    def test_clear_step_detected(self):
        prof = np.full(100, 60.0)
        prof[50:] = 200.0            # 亮度陡变，相当于台阶/阴影
        z = make_z_axis(prof.size)
        r = find_step(prof, z, self.th)
        self.assertTrue(r.suspected, f"明显台阶应命中（detail={r.detail}）")
        self.assertIsNotNone(r.distance_m)
        self.assertLessEqual(r.confidence, 0.6, "台阶置信度上限必须压在 0.6")

    def test_smooth_profile_not_detected(self):
        prof = np.linspace(50, 210, 100)   # 缓慢渐变
        r = find_step(prof, make_z_axis(prof.size), self.th)
        self.assertFalse(r.suspected)

    def test_two_equal_edges_is_ambiguous(self):
        prof = np.full(120, 60.0)
        prof[40:] = 200.0
        prof[80:] = 60.0
        r = find_step(prof, make_z_axis(prof.size), self.th)
        self.assertFalse(r.suspected, "两个强度相近的跳变更像纹理变化，应放弃")

    def test_short_profile_is_safe(self):
        r = find_step(np.zeros(3), make_z_axis(3), self.th)
        self.assertFalse(r.suspected)

    def test_shadow_edge_rejected_by_hue_invariance(self):
        """阴影只改亮度不改色相，不应被判成台阶。

        Test.mp4 里有强斜向阴影，实测会被误判成台阶，加上色相不变性检查后压掉。
        """
        prof = np.full(100, 60.0)
        prof[50:] = 200.0                     # 亮度陡变
        hue = np.full(100, 30.0)              # 但色相不变 → 阴影
        r = find_step(prof, make_z_axis(prof.size), self.th, hue_profile=hue)
        self.assertFalse(r.suspected, f"阴影不应判为台阶（detail={r.detail}）")
        self.assertIn("shadow_like", r.detail)

    def test_material_edge_with_hue_change_detected(self):
        prof = np.full(100, 60.0)
        prof[50:] = 200.0
        hue = np.full(100, 30.0)
        hue[50:] = 80.0                       # 色相也变 → 材质变化
        r = find_step(prof, make_z_axis(prof.size), self.th, hue_profile=hue)
        self.assertTrue(r.suspected, f"材质变化应判为台阶（detail={r.detail}）")

    def test_hue_diff_wraps_around(self):
        from vision_assist.detectors.stairs import circular_hue_diff
        self.assertAlmostEqual(circular_hue_diff(5.0, 175.0), 10.0, places=6)
        self.assertAlmostEqual(circular_hue_diff(10.0, 20.0), 10.0, places=6)

    def test_serializes(self):
        d = StairsResult(suspected=True, confidence=0.5123, distance_m=1.234).to_dict()
        self.assertEqual(d["confidence"], 0.512)
        self.assertEqual(d["distance_m"], 1.234)


class TestLargestValidSpan(unittest.TestCase):
    """度量网格边界伪影的回归测试。

    真实 bug：度量空间里无有效像素的行被填 0，直接对整条剖面做跳变分析时，
    "有效区边界"本身被当成巨大阶跃 —— 实测台阶检测因此在 Test.mp4 上
    误报 56/147 帧，且报出的距离永远是网格边界值 1.78m。
    """

    def test_finds_longest_run(self):
        from vision_assist.detectors.base import largest_valid_span
        #            0  1  2  3  4  5  6  7  8  9 10
        row_valid = np.array([0, 3, 3, 0, 0, 5, 5, 5, 0, 2, 2])
        self.assertEqual(largest_valid_span(row_valid), (5, 8))

    def test_all_valid(self):
        from vision_assist.detectors.base import largest_valid_span
        self.assertEqual(largest_valid_span(np.array([2, 2, 2])), (0, 3))

    def test_all_invalid(self):
        from vision_assist.detectors.base import largest_valid_span
        self.assertIsNone(largest_valid_span(np.zeros(5, int)))

    def test_trailing_zeros_do_not_create_a_step(self):
        """有效区后接一段 0，绝不能因此判出台阶。"""
        prof = np.full(100, 60.0)
        prof[40:] = 0.0                 # 无有效像素的行，被填 0
        z_axis = make_z_axis(prof.size)
        th = StairsThresholds(edge_response_min=0.35)
        # 交给检测器时应先裁掉无效行；这里直接验证"裁剪后"的行为
        span = (0, 40)
        r = find_step(prof[span[0]:span[1]], z_axis[span[0]:span[1]], th)
        self.assertFalse(r.suspected, "有效区边界不应被判成台阶")


# --------------------------------------------------------------------------- #
# 障碍物
# --------------------------------------------------------------------------- #
class TestObstacles(unittest.TestCase):
    def setUp(self):
        self.cam = make_camera()
        self.th = ObstacleThresholds(conf=0.3, near_m=2.5)

    def test_ground_from_box_returns_positive_distance(self):
        dist, lateral = ground_from_box(self.cam, (280, 300, 360, 470))
        self.assertIsNotNone(dist)
        self.assertGreater(dist, 0.0)
        self.assertIsNotNone(lateral)

    def test_ground_from_box_clamps_negative(self):
        cam = CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                      height_m=1.3, pitch_deg=85.0), 640, 480)
        dist, _lateral = ground_from_box(cam, (200, 400, 400, 479))
        self.assertIsNotNone(dist)
        self.assertGreaterEqual(dist, 0.0, "负距离必须截断到 0")

    def test_filters_irrelevant_classes(self):
        raw = [((100, 100, 200, 400), 0.9, "person"),
               ((100, 100, 200, 200), 0.9, "zebra"),
               ((100, 100, 200, 300), 0.2, "car")]     # 置信度不足
        r = build_obstacles(raw, self.cam, self.th)
        self.assertEqual(len(r.obstacles), 1)
        self.assertEqual(r.obstacles[0].label, "person")
        self.assertEqual(r.obstacles[0].label_cn, "行人")

    def test_sorted_by_distance(self):
        raw = [((100, 150, 200, 250), 0.9, "person"),     # 远
               ((100, 380, 200, 470), 0.9, "car")]        # 近
        r = build_obstacles(raw, self.cam, self.th)
        self.assertEqual(len(r.obstacles), 2)
        self.assertLessEqual(r.obstacles[0].distance_m, r.obstacles[1].distance_m)
        self.assertAlmostEqual(r.nearest_m, r.obstacles[0].distance_m)

    def test_empty_input(self):
        r = build_obstacles([], self.cam, self.th)
        self.assertEqual(r.obstacles, [])
        self.assertIsNone(r.nearest_m)

    def test_ignores_own_feet(self):
        """胸挂相机必然拍到自己的鞋，COCO 会识别成 person；太近的检测必须丢掉。

        实测漏过这条时，报告里出现了"前方0厘米有行人"。
        """
        cam = CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                      height_m=1.3, pitch_deg=85.0), 640, 480)
        raw = [((150, 400, 350, 479), 0.8, "person")]   # 画面最底部 = 自己的脚
        r = build_obstacles(raw, cam, ObstacleThresholds(ignore_below_m=0.7))
        self.assertEqual(r.obstacles, [], "自己脚下的检测不应算作障碍物")
        self.assertIsNone(r.nearest_m)

    def test_keeps_obstacle_beyond_ignore_threshold(self):
        raw = [((100, 380, 200, 470), 0.9, "car")]
        r = build_obstacles(raw, self.cam, ObstacleThresholds(ignore_below_m=0.01))
        self.assertEqual(len(r.obstacles), 1)

    def test_ignores_box_truncated_by_frame_bottom(self):
        """被画面底边截断的框基本都是佩戴者自己的身体。

        实测漏过这条时会报出"前方74厘米有行人"——距离刚好越过 0.7m 的近场过滤。
        """
        raw = [((150, 300, 350, 478), 0.8, "person")]   # 下边缘几乎贴住画面底部
        r = build_obstacles(raw, self.cam, ObstacleThresholds(ignore_below_m=0.0,
                                                             ignore_bottom_margin_px=6))
        self.assertEqual(r.obstacles, [], "贴住画面底边的框应被判为自身身体")

    def test_obstacle_not_touching_bottom_is_kept(self):
        raw = [((280, 150, 360, 400), 0.8, "person")]   # 瘦高，未贴底边
        r = build_obstacles(raw, self.cam, ObstacleThresholds(ignore_below_m=0.0,
                                                             ignore_bottom_margin_px=6))
        self.assertEqual(len(r.obstacles), 1)

    def test_rejects_frame_wide_false_person(self):
        """实测误报：一个横跨 717px 宽、占屏 34% 的 "person" 框。

        COCO 把大片盲道纹理当成了行人，播报出"前方74厘米有行人"。
        """
        raw = [((3, 2, 720, 382), 0.72, "person")]   # 宽 717 > 高 380，且面积巨大
        th = ObstacleThresholds(ignore_below_m=0.0, ignore_bottom_margin_px=6,
                                max_area_ratio=0.5, person_max_aspect=1.2)
        r = build_obstacles(raw, self.cam, th)
        self.assertEqual(r.obstacles, [], "宽大于高的 person 框应被丢弃")

    def test_keeps_normal_standing_person(self):
        raw = [((280, 180, 360, 460), 0.85, "person")]   # 瘦高，正常行人
        th = ObstacleThresholds(ignore_below_m=0.0, ignore_bottom_margin_px=6,
                                max_area_ratio=0.5, person_max_aspect=1.2)
        r = build_obstacles(raw, self.cam, th)
        self.assertEqual(len(r.obstacles), 1)

    def test_rejects_oversized_box(self):
        raw = [((0, 0, 640, 480), 0.9, "car")]           # 占满整帧
        th = ObstacleThresholds(ignore_below_m=0.0, max_area_ratio=0.5)
        r = build_obstacles(raw, self.cam, th)
        self.assertEqual(r.obstacles, [], "占满整帧的框应被丢弃")

    def test_serializes(self):
        raw = [((100, 380, 200, 470), 0.9, "car")]
        d = build_obstacles(raw, self.cam, self.th).to_dict()
        self.assertEqual(d["count"], 1)
        self.assertEqual(len(d["items"]), 1)
        self.assertIn("distance_m", d["items"][0])


# --------------------------------------------------------------------------- #
# 红绿灯
# --------------------------------------------------------------------------- #
class TestTrafficLightColor(unittest.TestCase):
    def setUp(self):
        self.th = TrafficLightThresholds(conf=0.25, min_color_ratio=0.06, vote_frames=3)

    def test_pure_red(self):
        crop = np.zeros((40, 40, 3), np.uint8)
        crop[:, :] = (0, 0, 255)
        state, conf, ratios = classify_color(crop, self.th)
        self.assertEqual(state, STATE_RED)
        self.assertGreater(conf, 0.5)
        self.assertGreater(ratios["red"], 0.5)

    def test_pure_green(self):
        crop = np.zeros((40, 40, 3), np.uint8)
        crop[:, :] = (0, 255, 0)
        state, _conf, _r = classify_color(crop, self.th)
        self.assertEqual(state, STATE_GREEN)

    def test_dark_crop_is_unknown(self):
        crop = np.full((40, 40, 3), 10, np.uint8)
        state, conf, _r = classify_color(crop, self.th)
        self.assertEqual(state, STATE_UNKNOWN)
        self.assertEqual(conf, 0.0)

    def test_empty_crop(self):
        ratios = color_ratios(np.zeros((0, 0, 3), np.uint8))
        self.assertEqual(ratios["red"], 0.0)


class TestTrafficLightVoting(unittest.TestCase):
    def setUp(self):
        self.cfg = AppConfig()
        self.cfg.traffic_light = TrafficLightThresholds(conf=0.25, min_color_ratio=0.06,
                                                        vote_frames=3)
        self.analyzer = TrafficLightAnalyzer(self.cfg)

    def _frame_with_light(self, color):
        frame = np.full((480, 640, 3), 40, np.uint8)
        cv2.rectangle(frame, (300, 100), (340, 180), color, -1)
        return frame

    def test_votes_to_stable_state(self):
        frame = self._frame_with_light((0, 0, 255))
        raw = [((290, 90, 350, 190), 0.9, "traffic light")]
        states = [self.analyzer.update(frame, raw).state for _ in range(3)]
        self.assertEqual(states[-1], STATE_RED, f"投票后应为 red，实际 {states}")

    def test_no_detection_resets(self):
        frame = self._frame_with_light((0, 0, 255))
        raw = [((290, 90, 350, 190), 0.9, "traffic light")]
        for _ in range(3):
            self.analyzer.update(frame, raw)
        r = self.analyzer.update(frame, [])
        self.assertFalse(r.present)
        self.assertEqual(r.state, STATE_UNKNOWN)

    def test_ignores_non_traffic_light_classes(self):
        frame = self._frame_with_light((0, 0, 255))
        raw = [((290, 90, 350, 190), 0.9, "person")]
        r = self.analyzer.update(frame, raw)
        self.assertFalse(r.present)

    def test_serializes(self):
        frame = self._frame_with_light((0, 0, 255))
        raw = [((290, 90, 350, 190), 0.9, "traffic light")]
        for _ in range(3):
            r = self.analyzer.update(frame, raw)
        d = r.to_dict()
        self.assertIn("state", d)
        self.assertIn("ratios", d)


if __name__ == "__main__":
    unittest.main(verbosity=2)
