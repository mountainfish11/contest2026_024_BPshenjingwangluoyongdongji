"""盲道检测的单元测试。

用合成图验证纯逻辑（纹理判别、颜色分割、地面几何），不需要加载 YOLO 模型；
再用一个 ``detect_boxes`` 被替换成固定框的子类，把真实的"掩码→地面→指标"
链路跑一遍。
"""
from __future__ import annotations

import math
import unittest

import cv2
import numpy as np

from vision_assist.config import AppConfig, CameraCalib, TactileThresholds
from vision_assist.detectors.tactile import (
    TactileDetector,
    TactileResult,
    box_near_far,
    classify_tile,
    color_mask_in_box,
    ground_from_mask,
    pick_tile_crop,
)
from vision_assist.geometry import CameraModel


def make_camera(width=640, height=480) -> CameraModel:
    return CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                   height_m=1.20, pitch_deg=30.0), width, height)


def bar_texture(size=(200, 200), period=20, thickness=6) -> np.ndarray:
    """合成"行进砖"：等间距的长条脊线。"""
    img = np.full((size[1], size[0], 3), 30, np.uint8)
    for x in range(period // 2, size[0], period):
        cv2.line(img, (x, 0), (x, size[1] - 1), (200, 200, 60), thickness)
    return img


def dot_texture(size=(200, 200), step=35, radius=10) -> np.ndarray:
    """合成"提示砖"：二维圆点阵列。"""
    img = np.full((size[1], size[0], 3), 30, np.uint8)
    for y in range(step // 2, size[1], step):
        for x in range(step // 2, size[0], step):
            cv2.circle(img, (x, y), radius, (200, 200, 60), -1)
    return img


class TestTileClassification(unittest.TestCase):
    def test_bar_texture_is_guide(self):
        tile, conf, m = classify_tile(bar_texture())
        self.assertEqual(tile, "guide", f"条纹被误判为 {tile}，指标={m}")
        self.assertGreater(conf, 0.5)
        self.assertGreaterEqual(m["n_lines"], 5)

    def test_dot_texture_is_warning(self):
        tile, conf, m = classify_tile(dot_texture())
        self.assertEqual(tile, "warning", f"圆点被误判为 {tile}，指标={m}")
        self.assertGreater(conf, 0.5)
        self.assertLessEqual(m["n_lines"], 3)

    def test_tiny_crop_is_unknown(self):
        tile, conf, m = classify_tile(np.zeros((10, 10, 3), np.uint8))
        self.assertEqual(tile, "unknown")
        self.assertEqual(conf, 0.0)
        self.assertEqual(m.get("reason"), "crop_too_small")

    def test_blank_crop_is_warning_not_crash(self):
        """纯色块没有长线段，按规则归为 warning，且不能抛异常。"""
        tile, _conf, m = classify_tile(np.full((120, 120, 3), 128, np.uint8))
        self.assertIn(tile, ("warning", "unknown"))
        self.assertEqual(m["n_lines"], 0)

    def test_higher_thresholds_flip_result(self):
        """调高 guide 门槛后，条纹图会掉进"中间地带"，置信度应明显降到 0.4。"""
        strict = TactileThresholds(guide_min_lines=40)
        tile, conf, _m = classify_tile(bar_texture(), strict)
        self.assertLessEqual(conf, 0.40, f"压低置信度失败: tile={tile} conf={conf}")

    def test_metrics_expose_parallelism(self):
        """平行度指标必须真实反映纹理：条纹高、圆点低。"""
        _t, _c, bar_m = classify_tile(bar_texture())
        _t2, _c2, dot_m = classify_tile(dot_texture())
        self.assertGreater(bar_m["parallel_ratio"], 0.6)
        self.assertGreater(bar_m["n_lines"], dot_m["n_lines"])


class TestColorMask(unittest.TestCase):
    def test_mask_picks_painted_region(self):
        frame = np.zeros((300, 300, 3), np.uint8)
        # 画一条亮黄色的"盲道"
        cv2.rectangle(frame, (100, 50), (200, 290), (40, 200, 240), -1)
        rows, cols, mask = color_mask_in_box(frame, (80, 40, 220, 299))
        self.assertGreater(rows.size, 1000)
        # 选出的像素应集中在盲道内部
        self.assertTrue(np.all(cols >= 95))
        self.assertTrue(np.all(cols <= 205))
        self.assertGreater(mask.shape[0], 0)

    def test_degenerate_box_returns_empty(self):
        frame = np.zeros((100, 100, 3), np.uint8)
        rows, cols, mask = color_mask_in_box(frame, (10, 10, 12, 12))
        self.assertEqual(rows.size, 0)
        self.assertEqual(mask.size, 0)


class TestGroundFromMask(unittest.TestCase):
    def test_constant_offset_line(self):
        cam = make_camera()
        zs = np.linspace(1.0, 5.0, 400)
        xs = np.full_like(zs, 0.30)
        u, v, ok = cam.ground2px_batch(xs, zs)
        self.assertTrue(np.all(ok))
        geo = ground_from_mask(cam, u, v, z_min_m=0.5, z_max_m=8.0)
        self.assertIsNotNone(geo["lateral_offset_m"])
        self.assertAlmostEqual(geo["lateral_offset_m"], 0.30, delta=0.02)
        self.assertAlmostEqual(geo["heading_deg"], 0.0, delta=0.5)

    def test_slanted_line_gives_heading(self):
        cam = make_camera()
        zs = np.linspace(1.0, 5.0, 400)
        slope = math.tan(math.radians(15.0))
        xs = 0.30 + slope * (zs - 1.0)
        u, v, ok = cam.ground2px_batch(xs, zs)
        geo = ground_from_mask(cam, u, v, z_min_m=0.5, z_max_m=8.0)
        self.assertAlmostEqual(geo["heading_deg"], 15.0, delta=1.0)

    def test_left_slant_is_negative(self):
        cam = make_camera()
        zs = np.linspace(1.0, 5.0, 400)
        xs = 0.0 - math.tan(math.radians(20.0)) * (zs - 1.0)
        u, v, ok = cam.ground2px_batch(xs, zs)
        geo = ground_from_mask(cam, u, v, z_min_m=0.5, z_max_m=8.0)
        self.assertLess(geo["heading_deg"], 0.0)

    def test_empty_points_are_safe(self):
        geo = ground_from_mask(make_camera(), np.empty(0), np.empty(0))
        self.assertIsNone(geo["lateral_offset_m"])
        self.assertIsNone(geo["heading_deg"])
        self.assertEqual(geo["count"], 0)

    def test_all_invalid_points_are_safe(self):
        """全在地平线以上的点必须安全返回空结果。"""
        cam = CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                      height_m=1.2, pitch_deg=8.0), 640, 480)
        u = np.full(50, 320.0)
        v = np.full(50, 10.0)  # 地平线以上
        geo = ground_from_mask(cam, u, v)
        self.assertIsNone(geo["lateral_offset_m"])
        self.assertEqual(geo["count"], 0)

    def test_z_filter_rejects_behind_camera(self):
        cam = make_camera()
        zs = np.linspace(-2.0, 3.0, 300)
        xs = np.zeros_like(zs)
        u, v, _ = cam.ground2px_batch(xs, zs)
        geo = ground_from_mask(cam, u, v, z_min_m=0.05)
        self.assertGreaterEqual(geo["count"], 0)
        if geo["near_m"] is not None:
            self.assertGreaterEqual(geo["near_m"], 0.05)


class TestPickTileCrop(unittest.TestCase):
    def test_picks_largest_box(self):
        frame = np.zeros((400, 400, 3), np.uint8)
        crop = pick_tile_crop(frame, [(10, 10, 60, 60), (100, 100, 300, 380)])
        self.assertIsNotNone(crop)
        self.assertEqual(crop.shape[0], 280)
        self.assertEqual(crop.shape[1], 200)

    def test_no_boxes_returns_none(self):
        self.assertIsNone(pick_tile_crop(np.zeros((100, 100, 3), np.uint8), []))

    def test_too_small_boxes_are_skipped(self):
        frame = np.zeros((100, 100, 3), np.uint8)
        self.assertIsNone(pick_tile_crop(frame, [(10, 10, 20, 20)]))


class TestBoxNearFar(unittest.TestCase):
    def test_distances_are_non_negative(self):
        """近景俯视时框底边会落到相机后方，距离必须截断到 0 而不是给负数。"""
        # pitch=85° 模拟手机近乎垂直向下拍，画面底部的地面点在相机后方
        cam = CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                      height_m=1.3, pitch_deg=85.0), 640, 480)
        near, far = box_near_far(cam, (200, 400, 400, 479))
        self.assertIsNotNone(near)
        self.assertIsNotNone(far)
        self.assertGreaterEqual(near, 0.0)
        self.assertGreaterEqual(far, near)

    def test_normal_mount_distances_are_sane(self):
        cam = make_camera()
        near, far = box_near_far(cam, (280, 320, 360, 470))
        self.assertGreater(near, 0.0)
        self.assertGreater(far, near)
        self.assertLess(far, 30.0)

    def test_box_above_horizon_returns_none(self):
        cam = CameraModel(CameraCalib(width=640, height=480, hfov_deg=65.0,
                                      height_m=1.2, pitch_deg=8.0), 640, 480)
        near, far = box_near_far(cam, (100, 5, 200, 20))
        self.assertIsNone(near)
        self.assertIsNone(far)


class TestResultSerialization(unittest.TestCase):
    def test_to_dict_rounds_and_handles_none(self):
        r = TactileResult(present=True, confidence=0.812345, lateral_offset_m=-0.1834,
                          heading_deg=12.3456, near_m=0.9, far_m=4.2)
        d = r.to_dict()
        self.assertEqual(d["confidence"], 0.812)
        self.assertEqual(d["lateral_offset_m"], -0.183)
        self.assertEqual(d["heading_deg"], 12.346)
        self.assertTrue(d["present"])

    def test_none_confidence_stays_none(self):
        d = TactileResult().to_dict()
        self.assertIsNone(d["lateral_offset_m"])
        self.assertIsNone(d["heading_deg"])
        self.assertFalse(d["present"])


class _StubTactileDetector(TactileDetector):
    """把模型替换成固定框，用来测真实的掩码→地面链路。"""

    def __init__(self, config, boxes):
        super().__init__(config)
        self._stub_boxes = boxes

    def _ensure_model(self):  # 不加载 torch
        return None

    def detect_boxes(self, frame):
        return [(b, 0.9, 0) for b in self._stub_boxes]


class TestDetectorEndToEndWithoutModel(unittest.TestCase):
    def _frame(self):
        frame = np.zeros((480, 640, 3), np.uint8)
        # 在画面下部画一条竖直的"盲道"，覆盖近场
        cv2.rectangle(frame, (300, 300), (360, 479), (40, 200, 240), -1)
        return frame

    def test_box_produces_geometry(self):
        cfg = AppConfig()
        det = _StubTactileDetector(cfg, [(290, 290, 370, 479)])
        frame = self._frame()
        from vision_assist.detectors.base import FrameContext
        ctx = FrameContext.create(frame, make_camera(), cfg)
        result = det.process(ctx)
        self.assertTrue(result.present)
        self.assertEqual(len(result.boxes), 1)
        self.assertIsNotNone(result.lateral_offset_m)
        self.assertGreater(result.ground_point_count, 0)
        self.assertIsNotNone(result.near_m)
        # 盲道画在画面右侧 → 横向偏移应为正
        self.assertGreater(result.lateral_offset_m, 0.0)

    def test_zero_boxes_is_safe(self):
        det = _StubTactileDetector(AppConfig(), [])
        from vision_assist.detectors.base import FrameContext
        result = det.process(FrameContext.create(self._frame(), make_camera(), AppConfig()))
        self.assertFalse(result.present)
        self.assertIsNone(result.lateral_offset_m)
        self.assertIsNone(result.heading_deg)
        self.assertIsNone(result.warning_near_m)
        self.assertEqual(result.boxes_detail, [])

    def test_nearest_box_decides_tile_type(self):
        """同帧混合砖型时，脚下(最近)的砖型必须优先于"面积最大"的框。

        这是安全相关的语义：若按面积取，脚下的提示砖会被远处的行进砖盖掉。
        """
        cfg = AppConfig()
        frame = np.zeros((480, 640, 3), np.uint8)
        frame[330:480, 240:400] = dot_texture((160, 150))   # 近处：提示砖（面积小）
        frame[120:280, 200:400] = bar_texture((200, 160))   # 远处：行进砖（面积更大）

        far_box = (200, 120, 400, 280)    # 面积 200*160=32000
        near_box = (240, 330, 400, 480)   # 面积 160*150=24000，但底边更低=更近
        det = _StubTactileDetector(cfg, [far_box, near_box])
        from vision_assist.detectors.base import FrameContext

        result = det.process(FrameContext.create(frame, make_camera(), cfg))
        self.assertEqual(len(result.boxes_detail), 2)
        self.assertEqual(result.tile_type, "warning",
                         f"最近的框是提示砖，却报告 {result.tile_type}")
        self.assertIsNotNone(result.warning_near_m)
        self.assertGreater(result.warning_near_m, 0.0)

    def test_no_warning_tile_gives_none(self):
        """只有行进砖时 warning_near_m 必须为 None，避免误触发路口预警。"""
        cfg = AppConfig()
        frame = np.zeros((480, 640, 3), np.uint8)
        frame[330:480, 240:400] = bar_texture((160, 150))
        det = _StubTactileDetector(cfg, [(240, 330, 400, 480)])
        from vision_assist.detectors.base import FrameContext

        result = det.process(FrameContext.create(frame, make_camera(), cfg))
        self.assertEqual(result.tile_type, "guide")
        self.assertIsNone(result.warning_near_m)

    def test_boxes_detail_serializes(self):
        cfg = AppConfig()
        frame = np.zeros((480, 640, 3), np.uint8)
        frame[330:480, 240:400] = bar_texture((160, 150))
        det = _StubTactileDetector(cfg, [(240, 330, 400, 480)])
        from vision_assist.detectors.base import FrameContext

        result = det.process(FrameContext.create(frame, make_camera(), cfg))
        d = result.boxes_detail[0].to_dict()
        self.assertEqual(len(d["box"]), 4)
        self.assertIn("tile_type", d)
        self.assertIn("near_m", d)

    def test_model_missing_raises_clear_error(self):
        det = TactileDetector(AppConfig(), weights="models/does_not_exist.pt")
        with self.assertRaises(FileNotFoundError) as ctx:
            det._ensure_model()
        self.assertIn("does_not_exist.pt", str(ctx.exception))


if __name__ == "__main__":
    unittest.main(verbosity=2)
