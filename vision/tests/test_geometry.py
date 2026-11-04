"""地面反投影几何的单元测试。

运行::

    python -m unittest discover -s tests -t . -v
"""
from __future__ import annotations

import math
import unittest

import cv2
import numpy as np

from vision_assist.config import CameraCalib
from vision_assist.geometry import CameraModel, camera_model_for


def make_model(width=640, height=480, h=1.2, pitch=30.0, hfov=65.0) -> CameraModel:
    calib = CameraCalib(width=640, height=480, hfov_deg=hfov, height_m=h, pitch_deg=pitch)
    return CameraModel(calib, width, height)


class TestRoundTrip(unittest.TestCase):
    def test_px_to_ground_and_back(self):
        model = make_model()
        for u, v in [(320, 470), (100, 300), (500, 250), (0, 480), (639, 200)]:
            g = model.px2ground(u, v)
            self.assertIsNotNone(g, f"({u},{v}) 应能看到地面")
            x, z = g  # type: ignore[misc]
            self.assertGreater(z, 0.0)
            back = model.ground2px(x, z)
            self.assertIsNotNone(back)
            u2, v2 = back  # type: ignore[misc]
            self.assertAlmostEqual(u, u2, places=5)
            self.assertAlmostEqual(v, v2, places=5)

    def test_above_horizon_returns_none(self):
        model = make_model()
        hv = model.horizon_v()
        self.assertLess(hv, 0, "pitch=30 时地平线应位于画面之上")
        # 构造一个明确在地平线以上的像素
        model_steep = make_model(pitch=10.0)
        hv2 = model_steep.horizon_v()
        self.assertGreater(hv2, 0, "pitch=10 时地平线应落在画面内")
        self.assertIsNone(model_steep.px2ground(320, max(0, hv2 - 5)))

    def test_center_column_has_zero_lateral_offset(self):
        model = make_model()
        for v in (480, 400, 300, 200):
            g = model.px2ground(model.cx, v)
            self.assertIsNotNone(g)
            self.assertAlmostEqual(g[0], 0.0, places=9)  # type: ignore[index]

    def test_ground_z_decreases_towards_image_bottom(self):
        model = make_model()
        zs = []
        for v in (100, 200, 300, 400, 478):
            g = model.px2ground(320, v)
            self.assertIsNotNone(g)
            zs.append(g[1])  # type: ignore[index]
        self.assertEqual(zs, sorted(zs, reverse=True), "越靠画面底部地面距离应越近")

    def test_image_bottom_matches_hand_calculation(self):
        """h=1.2, pitch=30, VGA+65°hfov 时画面底边应落在前方约 0.83 m。"""
        model = make_model()
        g = model.px2ground(model.cx, 479)
        self.assertIsNotNone(g)
        self.assertAlmostEqual(g[1], 0.826, delta=0.01)  # type: ignore[index]


class TestScaling(unittest.TestCase):
    def test_focal_from_hfov(self):
        calib = CameraCalib(width=640, hfov_deg=65.0)
        expected = (640 / 2.0) / math.tan(math.radians(65.0) / 2.0)
        self.assertAlmostEqual(calib.focal_px(), expected, places=6)

    def test_scaled_preserves_fov(self):
        calib = CameraCalib(width=640, height=480, hfov_deg=65.0, height_m=1.2, pitch_deg=30.0)
        half = calib.scaled(320, 240)
        self.assertAlmostEqual(half.focal_px(), calib.focal_px() / 2.0, places=6)
        # 主点也应落到新的画面中心
        self.assertAlmostEqual(half.principal_point()[0], 160.0, places=6)
        self.assertAlmostEqual(half.principal_point()[1], 120.0, places=6)

    def test_model_scales_to_actual_frame(self):
        model = make_model(width=320, height=240)
        self.assertAlmostEqual(model.fx, 320 / 2.0 / math.tan(math.radians(65.0) / 2.0), places=6)
        self.assertAlmostEqual(model.cx, 160.0, places=6)


class TestMetricGrid(unittest.TestCase):
    def test_grid_shape_and_axes(self):
        model = make_model()
        grid = model.metric_grid(x_span_m=3.0, z_min_m=0.8, z_max_m=4.0, resolution_m=0.02)
        self.assertEqual(grid.shape[0], int(round((4.0 - 0.8) / 0.02)) + 1)
        self.assertEqual(grid.shape[1], int(round(3.0 / 0.02)) + 1)
        self.assertAlmostEqual(grid.x_axis[0], -1.5, places=6)
        self.assertAlmostEqual(grid.z_axis[0], 0.8, places=6)
        self.assertAlmostEqual(grid.x_axis[-1], 1.5, places=6)

    def test_metric_grid_is_injective(self):
        """度量网格上的每个点都不应重复映射到同一个源像素（除非超出画面）。"""
        model = make_model()
        grid = model.metric_grid(x_span_m=2.0, z_min_m=1.0, z_max_m=3.0, resolution_m=0.05)
        mx = grid.mapx[grid.valid].astype(np.int32)
        my = grid.mapy[grid.valid].astype(np.int32)
        keys = mx.astype(np.int64) * 100000 + my
        self.assertGreater(len(set(keys.tolist())), 0.7 * len(keys),
                           "度量网格映射大量重合，说明内参或反投影有问题")

    def test_warp_makes_constant_x_a_straight_column(self):
        """在地面画 X=0.5 的线，反投影到度量空间后应落在同一列。

        这是整套"在度量空间做几何判断"的地基，必须守住。
        """
        model = make_model()
        grid = model.metric_grid(x_span_m=3.0, z_min_m=1.0, z_max_m=4.0, resolution_m=0.02)
        canvas = np.zeros((480, 640), np.uint8)
        zs = np.linspace(1.0, 4.0, 600)
        pts = model.ground_polyline_to_px(
            np.stack([np.full_like(zs, 0.5), zs], axis=1)
        )
        for u, v in pts:
            if not (np.isnan(u) or np.isnan(v)):
                cv2.circle(canvas, (int(round(u)), int(round(v))), 1, 255, -1)

        warped = model.warp_to_metric(canvas, grid)
        expect_col = grid.col_for_x(0.5)
        self.assertAlmostEqual(grid.x_axis[expect_col], 0.5, places=6)

        # 检查期望列附近是否真的亮起来
        band = warped[:, max(0, expect_col - 1): expect_col + 2]
        hit_ratio = float((band > 100).any(axis=1).mean())
        self.assertGreater(hit_ratio, 0.8, f"期望列 {expect_col} 命中率仅 {hit_ratio:.2%}")

        # 而远离该列的位置应当是暗的
        away = warped[:, max(0, expect_col - 12)]
        self.assertLess(float((away > 100).mean()), 0.2)


class TestDerivedMetrics(unittest.TestCase):
    def test_meters_per_pixel_matches_numeric_derivative(self):
        model = make_model()
        z = 2.0
        eps = 1e-4
        u0 = model.ground2px(0.0, z)[0]  # type: ignore[index]
        u1 = model.ground2px(1.0, z)[0]  # type: ignore[index]
        num = 1.0 / (u1 - u0)
        self.assertAlmostEqual(model.meters_per_pixel_x(z), num, delta=1e-3)

    def test_visible_ground_range_is_ordered(self):
        model = make_model()
        z_near, z_far = model.visible_ground_range()
        self.assertGreater(z_near, 0.0)
        self.assertGreater(z_far, z_near)

    def test_invalid_calibration_rejected(self):
        with self.assertRaises(ValueError):
            make_model(pitch=0.0)
        with self.assertRaises(ValueError):
            make_model(pitch=90.0)
        with self.assertRaises(ValueError):
            make_model(h=0.0)

    def test_camera_model_for_uses_frame_size(self):
        frame = np.zeros((240, 320, 3), np.uint8)
        model = camera_model_for(frame, CameraCalib(width=640, height=480, hfov_deg=65.0))
        self.assertEqual((model.width, model.height), (320, 240))
        self.assertAlmostEqual(model.cx, 160.0, places=6)


if __name__ == "__main__":
    unittest.main(verbosity=2)
