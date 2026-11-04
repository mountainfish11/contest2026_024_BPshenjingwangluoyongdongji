"""斑马线检测：在**地面度量空间**里找白条的周期性。

为什么不直接用颜色/边缘找白条：透视投影下近处白条宽、远处白条窄，阈值根本没法
统一。先把画面反投影成鸟瞰（每像素对应固定地面尺寸），斑马线就变成"沿 Z 方向
等宽交替的方波"，判据一下子干净了。

这也顺带解释了为什么整套系统必须以地面反投影为核心。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Optional, Tuple

import cv2
import numpy as np

from ..config import AppConfig, CrosswalkThresholds
from .base import Detector, FrameContext, largest_valid_span

#: 只看中线附近这个横向范围（米），斑马线通常正对行人前方
CENTER_HALF_WIDTH_M = 1.0
#: 判定某一行"是白条"所需的白色像素占比
BAR_WHITE_RATIO = 0.15


@dataclass
class CrosswalkResult:
    present: bool = False
    confidence: float = 0.0
    bar_count: int = 0
    white_ratio: float = 0.0
    near_m: Optional[float] = None
    detail: str = ""

    def to_dict(self) -> dict:
        return {
            "present": self.present,
            "confidence": round(float(self.confidence), 3),
            "bar_count": int(self.bar_count),
            "white_ratio": round(float(self.white_ratio), 3),
            "near_m": None if self.near_m is None else round(self.near_m, 3),
            "detail": self.detail,
        }


def count_runs(mask: np.ndarray) -> int:
    """统计布尔数组里"True 连续段"的个数。"""
    if mask.size == 0:
        return 0
    m = mask.astype(np.int8)
    edges = np.diff(np.concatenate(([0], m, [0])))
    return int((edges == 1).sum())


def analyze_profile(profile: np.ndarray, z_axis: np.ndarray,
                    th: CrosswalkThresholds) -> CrosswalkResult:
    """从"沿 Z 的白色像素占比曲线"判定斑马线。"""
    result = CrosswalkResult()
    if profile.size < 8:
        result.detail = "profile_too_short"
        return result

    white_ratio = float(profile.mean())
    result.white_ratio = white_ratio

    is_white = profile >= BAR_WHITE_RATIO
    bar_count = count_runs(is_white)
    result.bar_count = bar_count

    if bar_count < th.min_bars:
        result.detail = f"bars={bar_count}<{th.min_bars}"
        return result
    if white_ratio < th.min_white_ratio:
        result.detail = f"white={white_ratio:.3f}<{th.min_white_ratio}"
        return result

    # 周期性检验：白条与非白条应有清晰交替，用曲线的"翻转密度"衡量
    flips = float(np.abs(np.diff(is_white.astype(np.int8))).sum())
    expected = max(1.0, 2.0 * bar_count)
    regularity = min(1.0, flips / expected)

    conf = 0.45
    conf += 0.20 * min(1.0, (bar_count - th.min_bars) / 4.0)
    conf += 0.20 * min(1.0, white_ratio / max(th.min_white_ratio * 3.0, 1e-6))
    conf += 0.15 * regularity

    result.present = True
    result.confidence = round(min(0.99, conf), 3)
    result.detail = f"bars={bar_count} regular={regularity:.2f}"
    if np.any(is_white):
        result.near_m = float(z_axis[int(np.argmax(is_white))])
    return result


class CrosswalkDetector(Detector):
    name = "crosswalk"

    def __init__(self, config: Optional[AppConfig] = None):
        super().__init__(config)
        self.th: CrosswalkThresholds = self.config.crosswalk

    def process(self, ctx: FrameContext) -> CrosswalkResult:
        th = self.th
        grid = ctx.metric_grid(
            x_span_m=th.x_span_m,
            z_min_m=th.z_min_m,
            z_max_m=th.z_max_m,
            resolution_m=th.metric_resolution_m,
        )
        if grid.valid_ratio < 0.2:
            return CrosswalkResult(detail=f"grid_invalid({grid.valid_ratio:.2f})")

        gray = ctx.frame if ctx.frame.ndim == 2 else cv2.cvtColor(ctx.frame, cv2.COLOR_BGR2GRAY)
        warped = cv2.remap(
            gray, grid.mapx, grid.mapy,
            interpolation=cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=0,
        )

        center_cols = np.abs(grid.x_axis) <= CENTER_HALF_WIDTH_M
        if not np.any(center_cols):
            return CrosswalkResult(detail="no_center_columns")

        sub = warped[:, center_cols]
        sub_valid = grid.valid[:, center_cols]
        row_valid = sub_valid.sum(axis=1)

        # 只分析有效像素的最长连续行区间：度量空间里无有效像素的行被填 0，
        # 直接对整条剖面分析会把"有效区边界"当成周期性结构的一部分。
        span = largest_valid_span(row_valid)
        if span is None:
            return CrosswalkResult(detail="no_valid_rows")
        r0, r1 = span
        sub = sub[r0:r1]
        sub_valid = sub_valid[r0:r1]
        row_valid = row_valid[r0:r1]
        z_axis = grid.z_axis[r0:r1]

        white = (sub >= th.white_threshold) & sub_valid
        denom = np.maximum(row_valid, 1)
        with np.errstate(invalid="ignore", divide="ignore"):
            profile = np.where(row_valid > 0, white.sum(axis=1) / denom, 0.0)

        return analyze_profile(profile.astype(np.float64), z_axis, th)
