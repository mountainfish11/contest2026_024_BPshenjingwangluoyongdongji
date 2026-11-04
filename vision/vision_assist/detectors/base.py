"""检测器基类与公共数据结构。"""
from __future__ import annotations

import time
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

import numpy as np

from ..config import AppConfig
from ..device import resolve_device
from ..geometry import CameraModel, MetricGrid

Box = Tuple[float, float, float, float]  # x1, y1, x2, y2（像素）


@dataclass
class FrameContext:
    """传给各个检测器的逐帧上下文。"""

    index: int
    timestamp: float
    frame: np.ndarray
    camera: CameraModel
    config: AppConfig
    #: 惰性构建的度量空间网格（只有需要鸟瞰图的检测器才用）
    _metric_grid: Optional[MetricGrid] = None
    _metric_grid_key: Optional[Tuple[float, float, float, float]] = None

    def metric_grid(
        self,
        x_span_m: float,
        z_min_m: float,
        z_max_m: float,
        resolution_m: float,
    ) -> MetricGrid:
        key = (x_span_m, z_min_m, z_max_m, resolution_m)
        if self._metric_grid is None or self._metric_grid_key != key:
            self._metric_grid = self.camera.metric_grid(
                x_span_m=x_span_m,
                z_min_m=z_min_m,
                z_max_m=z_max_m,
                resolution_m=resolution_m,
            )
            self._metric_grid_key = key
        return self._metric_grid

    @classmethod
    def create(cls, frame: np.ndarray, camera: CameraModel, config: AppConfig,
               index: int = 0) -> "FrameContext":
        return cls(index=index, timestamp=time.time(), frame=frame,
                   camera=camera, config=config)


@dataclass
class Detection:
    """一条检测结果（像素框 + 地面坐标）。"""

    label: str
    conf: float
    box: Box
    ground_x_m: Optional[float] = None
    ground_z_m: Optional[float] = None
    extra: Dict[str, Any] = field(default_factory=dict)

    def clamped_box(self, width: int, height: int) -> Tuple[int, int, int, int]:
        x1, y1, x2, y2 = self.box
        return (
            max(0, min(width - 1, int(round(x1)))),
            max(0, min(height - 1, int(round(y1)))),
            max(0, min(width, int(round(x2)))),
            max(0, min(height, int(round(y2)))),
        )


class Detector(ABC):
    """检测器抽象基类。"""

    name: str = "detector"

    def __init__(self, config: Optional[AppConfig] = None):
        self.config = config or AppConfig()
        self._device, self._device_desc = resolve_device(
            getattr(self.config, "device", "auto"))

    def refresh_device(self) -> None:
        """配置变更后重新解析推理设备（例如从 CPU 切到 GPU）。"""
        self._device, self._device_desc = resolve_device(
            getattr(self.config, "device", "auto"))

    @property
    def device(self) -> str:
        return self._device

    @property
    def device_description(self) -> str:
        return self._device_desc

    @abstractmethod
    def process(self, ctx: FrameContext) -> Any:
        """处理一帧，返回该检测器的结果对象。"""

    def close(self) -> None:  # pragma: no cover
        pass


def box_bottom_center_ground(
    camera: CameraModel, box: Box
) -> Tuple[Optional[Tuple[float, float]], Optional[Tuple[float, float]]]:
    """返回框"底边中点"(近处) 与"框中心"对应的地面坐标。"""
    x1, y1, x2, y2 = box
    bottom = camera.px2ground((x1 + x2) / 2.0, y2)
    center = camera.px2ground((x1 + x2) / 2.0, (y1 + y2) / 2.0)
    return bottom, center


def subsample_points(u: np.ndarray, v: np.ndarray, stride: int = 1,
                     limit: int = 4000) -> Tuple[np.ndarray, np.ndarray]:
    """对像素坐标降采样，控制单帧计算量。"""
    if stride > 1:
        u = u[::stride]
        v = v[::stride]
    if u.size > limit:
        idx = np.linspace(0, u.size - 1, limit).astype(np.int64)
        u = u[idx]
        v = v[idx]
    return u, v


def largest_valid_span(row_valid: np.ndarray) -> Optional[Tuple[int, int]]:
    """找出"有有效像素"的最长连续行区间 ``[start, stop)``。

    这一步很关键：度量空间里超出原图范围的行会被填成 0，如果直接拿整条剖面
    做跳变/周期性分析，**有效区的边界本身就会被当成一个巨大的阶跃**。实测台阶
    检测因此在 ``Test.mp4`` 上误报 38%，且报出的距离永远是网格边界值。
    """
    idx = np.flatnonzero(np.asarray(row_valid) > 0)
    if idx.size == 0:
        return None
    breaks = np.flatnonzero(np.diff(idx) > 1)
    groups = np.split(idx, breaks + 1)
    best = max(groups, key=len)
    return int(best[0]), int(best[-1]) + 1


__all__ = [
    "Box",
    "Detection",
    "Detector",
    "FrameContext",
    "box_bottom_center_ground",
    "subsample_points",
]
