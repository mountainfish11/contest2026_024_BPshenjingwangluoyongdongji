"""地面度量空间几何：像素 ↔ 地面坐标 (X, Z) 的双向映射。

坐标系约定
----------
- 世界坐标：X 向右为正（米），Y 向下为正，Z 向前为正（米），相机光心为原点。
- 地面平面：Y = h（h = 相机离地高度，米）。
- 相机安装：胸/腰前挂，光学轴低于水平线 ``pitch`` 弧度，无滚转、无偏航。

反投影闭式解（针对单个像素 ``(u, v)``，逐像素单位视线取 ``z_c = 1``）::

    x_c = (u - cx) / fx
    y_c = (v - cy) / fy
    den = y_c * cos(pitch) + sin(pitch)
    t   = h / den
    X   = x_c * t
    Z   = (-y_c * sin(pitch) + cos(pitch)) * t

``den <= 0`` 表示该像素位于地平线以上（射线与地面不相交），判为无效。
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional, Tuple

import cv2
import numpy as np

from .config import CameraCalib

_EPS = 1e-6


@dataclass
class MetricGrid:
    """度量空间重采样网格（供 ``cv2.remap`` 使用）。

    ``mapx``/``mapy`` 的第 ``j`` 行对应地面 ``Z = z_min + j * resolution``，
    第 ``i`` 列对应 ``X = x_min + i * resolution``。
    """

    mapx: np.ndarray
    mapy: np.ndarray
    valid: np.ndarray
    x_min: float
    x_max: float
    z_min: float
    z_max: float
    resolution: float

    @property
    def shape(self) -> Tuple[int, int]:
        return self.mapx.shape  # (rows, cols)

    @property
    def x_axis(self) -> np.ndarray:
        """每一列对应的 X 坐标（米）。"""
        return self.x_min + np.arange(self.shape[1], dtype=np.float64) * self.resolution

    @property
    def z_axis(self) -> np.ndarray:
        """每一行对应的 Z 坐标（米）。"""
        return self.z_min + np.arange(self.shape[0], dtype=np.float64) * self.resolution

    @property
    def valid_ratio(self) -> float:
        return float(self.valid.mean())

    def row_for_z(self, z_m: float) -> int:
        return int(round((z_m - self.z_min) / self.resolution))

    def col_for_x(self, x_m: float) -> int:
        return int(round((x_m - self.x_min) / self.resolution))

    def clip_roi(self, x_half_span: float, z_min: float, z_max: float) -> Tuple[slice, slice]:
        """返回覆盖指定地面区域的 (行切片, 列切片)，自动裁剪到网格范围内。"""
        r0 = max(0, self.row_for_z(z_min))
        r1 = min(self.shape[0], self.row_for_z(z_max) + 1)
        c0 = max(0, self.col_for_x(-x_half_span))
        c1 = min(self.shape[1], self.col_for_x(x_half_span) + 1)
        return slice(r0, max(r0, r1)), slice(c0, max(c0, c1))


class CameraModel:
    """把 :class:`CameraCalib` 换算成便于逐帧使用的几何模型。"""

    def __init__(self, calib: CameraCalib, frame_width: int, frame_height: int):
        scaled = calib.scaled(frame_width, frame_height)
        self.calib = scaled
        self.width = int(frame_width)
        self.height = int(frame_height)
        f = scaled.focal_px()
        self.fx = float(f)
        self.fy = float(f)
        self.cx, self.cy = scaled.principal_point()
        self.h = float(scaled.height_m)
        self.pitch = math.radians(float(scaled.pitch_deg))
        self._sin = math.sin(self.pitch)
        self._cos = math.cos(self.pitch)
        if self.h <= 0:
            raise ValueError("相机离地高度 height_m 必须为正数")
        # 视线必须朝下才能看到地面
        if not (0.0 < scaled.pitch_deg < 90.0):
            raise ValueError("俯仰角 pitch_deg 必须在 (0, 90) 度之间")

    # ------------------------------------------------------------------ 标量
    def px2ground(self, u: float, v: float) -> Optional[Tuple[float, float]]:
        """像素 → 地面 (X, Z)，单位米；位于地平线以上时返回 ``None``。"""
        x_c = (u - self.cx) / self.fx
        y_c = (v - self.cy) / self.fy
        den = y_c * self._cos + self._sin
        if den <= _EPS:
            return None
        t = self.h / den
        return x_c * t, (-y_c * self._sin + self._cos) * t

    def ground2px(self, x_m: float, z_m: float) -> Optional[Tuple[float, float]]:
        """地面 (X, Z)，单位米 → 像素 (u, v)；位于相机后方时返回 ``None``。"""
        if z_m <= 0:
            return None
        z_c = z_m * self._cos + self.h * self._sin
        if z_c <= _EPS:
            return None
        y_c = -z_m * self._sin + self.h * self._cos
        return self.fx * x_m / z_c + self.cx, self.fy * y_c / z_c + self.cy

    # ------------------------------------------------------------------ 批量
    def px2ground_batch(
        self, u: np.ndarray, v: np.ndarray
    ) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
        """向量化反投影。返回 ``(X, Z, valid)``，无效位置填 ``nan``。"""
        u = np.asarray(u, dtype=np.float64)
        v = np.asarray(v, dtype=np.float64)
        x_c = (u - self.cx) / self.fx
        y_c = (v - self.cy) / self.fy
        den = y_c * self._cos + self._sin
        valid = den > _EPS
        with np.errstate(divide="ignore", invalid="ignore"):
            t = np.where(valid, self.h / np.where(valid, den, 1.0), np.nan)
            X = x_c * t
            Z = (-y_c * self._sin + self._cos) * t
        return X, Z, valid

    def ground2px_batch(
        self, x_m: np.ndarray, z_m: np.ndarray
    ) -> Tuple[np.ndarray, np.ndarray, np.ndarray]:
        """向量化正向投影。返回 ``(u, v, valid)``。"""
        x_m = np.asarray(x_m, dtype=np.float64)
        z_m = np.asarray(z_m, dtype=np.float64)
        valid = z_m > 0
        z_c = np.where(valid, z_m * self._cos + self.h * self._sin, 1.0)
        y_c = -z_m * self._sin + self.h * self._cos
        with np.errstate(divide="ignore", invalid="ignore"):
            u = self.fx * x_m / z_c + self.cx
            v = self.fy * y_c / z_c + self.cy
        u = np.where(valid, u, np.nan)
        v = np.where(valid, v, np.nan)
        return u, v, valid

    # -------------------------------------------------------------- 派生指标
    def horizon_v(self) -> float:
        """地平线所在的行坐标（可能为负或超过画面高度）。"""
        return self.cy - self.fx * math.tan(self.pitch)

    def visible_ground_range(self) -> Tuple[float, float]:
        """画面可见的地面 Z 范围（米），从画面底边到顶边。"""
        near = self.px2ground(self.cx, self.height - 1)
        far = self.px2ground(self.cx, 0)
        z_near = near[1] if near else 0.0
        z_far = far[1] if far else float("inf")
        return z_near, z_far

    def meters_per_pixel_x(self, z_m: float) -> float:
        """在给定前方距离 Z 处，一个像素横向对应多少米。"""
        return (self.h * self._sin + z_m * self._cos) / self.fx

    def meters_per_pixel_z(self, z_m: float) -> float:
        """在给定前方距离 Z 处，一个像素纵向对应多少米。

        由 ``v = fy * (h cos - Z sin) / (h sin + Z cos) + cy`` 对 Z 求导得到。
        """
        z_c = self.h * self._sin + z_m * self._cos
        dydz = self.fy * (-self._sin * z_c - (self.h * self._cos - z_m * self._sin) * self._cos) / (z_c * z_c)
        if abs(dydz) < _EPS:
            return float("inf")
        return abs(1.0 / dydz)

    def planar_distance(self, x_m: float, z_m: float) -> float:
        """从相机正下方地面点到 (X, Z) 的水平直线距离（米）。"""
        return math.hypot(x_m, z_m)

    # ------------------------------------------------------------ 度量空间网格
    def metric_grid(
        self,
        x_span_m: float = 3.0,
        z_min_m: float = 0.8,
        z_max_m: float = 5.0,
        resolution_m: float = 0.02,
    ) -> MetricGrid:
        """构建地面度量空间的重采样网格。

        行索引 → Z（近到远），列索引 → X（左到右）。
        """
        if z_max_m <= z_min_m:
            raise ValueError("z_max_m 必须大于 z_min_m")
        if resolution_m <= 0:
            raise ValueError("resolution_m 必须为正数")
        n_rows = int(round((z_max_m - z_min_m) / resolution_m)) + 1
        n_cols = int(round(x_span_m / resolution_m)) + 1
        x_min = -x_span_m / 2.0
        zs = z_min_m + np.arange(n_rows, dtype=np.float64) * resolution_m
        xs = x_min + np.arange(n_cols, dtype=np.float64) * resolution_m
        X, Z = np.meshgrid(xs, zs)  # 形状 (n_rows, n_cols)

        z_c = Z * self._cos + self.h * self._sin
        y_c = -Z * self._sin + self.h * self._cos
        mapx = (self.fx * X / z_c + self.cx).astype(np.float32)
        mapy = (self.fy * y_c / z_c + self.cy).astype(np.float32)
        valid = (
            (mapx >= 0)
            & (mapx < self.width)
            & (mapy >= 0)
            & (mapy < self.height)
        )
        return MetricGrid(
            mapx=mapx,
            mapy=mapy,
            valid=valid,
            x_min=x_min,
            x_max=x_min + (n_cols - 1) * resolution_m,
            z_min=z_min_m,
            z_max=z_min_m + (n_rows - 1) * resolution_m,
            resolution=resolution_m,
        )

    def warp_to_metric(self, frame: np.ndarray, grid: MetricGrid) -> np.ndarray:
        """把原始画面重采样到度量空间（鸟瞰图），无效区域填 0。"""
        if frame.shape[1] != self.width or frame.shape[0] != self.height:
            raise ValueError(
                f"帧尺寸 {frame.shape[1]}x{frame.shape[0]} 与相机模型 "
                f"{self.width}x{self.height} 不一致，请用 CameraModel(calib, w, h) 重建"
            )
        return cv2.remap(
            frame,
            grid.mapx,
            grid.mapy,
            interpolation=cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=0,
        )

    # ------------------------------------------------------------------ 其它
    def ground_polyline_to_px(self, points: np.ndarray) -> np.ndarray:
        """把一串地面点 ``[[X, Z], ...]`` 投到像素坐标，返回 ``[[u, v], ...]``。"""
        pts = np.asarray(points, dtype=np.float64).reshape(-1, 2)
        u, v, valid = self.ground2px_batch(pts[:, 0], pts[:, 1])
        out = np.stack([u, v], axis=1)
        out[~valid] = np.nan
        return out

    def describe(self) -> dict:
        z_near, z_far = self.visible_ground_range()
        return {
            "frame": [self.width, self.height],
            "focal_px": round(self.fx, 2),
            "principal_point": [round(self.cx, 2), round(self.cy, 2)],
            "height_m": self.h,
            "pitch_deg": round(math.degrees(self.pitch), 2),
            "horizon_v": round(self.horizon_v(), 2),
            "visible_ground_z_m": [round(z_near, 2), round(z_far, 2)],
            "meters_per_px_at_2m": round(self.meters_per_pixel_x(2.0), 4),
        }


def camera_model_for(frame: np.ndarray, calib: CameraCalib) -> CameraModel:
    """由一帧图像和标定参数构造 :class:`CameraModel`。"""
    if frame is None or frame.ndim < 2:
        raise ValueError("需要一张有效图像来确定帧尺寸")
    return CameraModel(calib, int(frame.shape[1]), int(frame.shape[0]))
