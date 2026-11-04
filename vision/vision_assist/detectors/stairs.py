"""台阶 / 地面落差检测（规则级，**保守**）。

必须说明的局限
--------------
这是全套能力里最不可靠的一项。真实台阶需要深度信息才能可靠判断，单目 + 地面平面
假设下只能靠"地面亮度/纹理沿 Z 出现强不连续"来猜。因此：

- 输出字段叫 ``suspected``，不叫 ``detected``；
- 置信度上限压在 0.6，且必须多帧一致才报；
- 播报文案是"疑似有台阶，请放慢脚步"，绝不能说成确定结论。

宁可漏报也不误报——误报会让用户对系统失去信任。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional

import cv2
import numpy as np

from ..config import AppConfig, StairsThresholds
from .base import Detector, FrameContext, largest_valid_span

#: 只看中线附近这个横向范围（米）
CENTER_HALF_WIDTH_M = 0.9
#: 归一化亮度曲线的最小长度
MIN_PROFILE_LEN = 10


def circular_hue_mean(values: np.ndarray) -> float:
    """色相是 0–180 的环形量，不能用算术平均；用单位向量平均。"""
    if values.size == 0:
        return 0.0
    ang = values.astype(np.float64) * (np.pi / 90.0)  # 180 -> 2pi
    s = np.sin(ang).mean()
    c = np.cos(ang).mean()
    deg = np.degrees(np.arctan2(s, c)) / 2.0  # 回到 0–180
    return float(deg % 180.0)


def circular_hue_diff(a: float, b: float) -> float:
    d = abs(a - b) % 180.0
    return float(min(d, 180.0 - d))


@dataclass
class StairsResult:
    suspected: bool = False
    confidence: float = 0.0
    distance_m: Optional[float] = None
    detail: str = ""

    def to_dict(self) -> dict:
        return {
            "suspected": self.suspected,
            "confidence": round(float(self.confidence), 3),
            "distance_m": None if self.distance_m is None else round(self.distance_m, 3),
            "detail": self.detail,
        }


def find_step(
    profile: np.ndarray,
    z_axis: np.ndarray,
    th: StairsThresholds,
    hue_profile: Optional[np.ndarray] = None,
) -> StairsResult:
    """在沿 Z 的亮度曲线上找一个"单一、显著"的跳变。

    ``edge_response_min`` 衡量的是**边缘两侧的平均亮度差**（0–1），而不是平滑后的
    梯度峰值——平滑会把阶跃幅度打掉近一半，用梯度峰值当阈值会让门槛变成两倍高。

    若提供 ``hue_profile``，还会要求边缘两侧的色相有明显变化：**阴影只改亮度不改
    色相**，靠这一条可以压掉大量"阴影被当成台阶"的误报。
    """
    result = StairsResult()
    if profile.size < MIN_PROFILE_LEN:
        result.detail = "profile_too_short"
        return result

    norm = profile.astype(np.float64) / 255.0
    # 轻度平滑，抑制单像素噪声，但不要把台阶边缘抹平。
    # 用 edge 填充再 valid 卷积：直接 'same' 会零填充，在首尾造出假梯度。
    kernel = np.array([0.25, 0.5, 0.25])
    smooth = np.convolve(np.pad(norm, (1, 1), mode="edge"), kernel, mode="valid")
    grad = np.abs(np.diff(smooth))
    if grad.size < 2:
        return result

    peak = int(np.argmax(grad))
    win = 5

    # 边缘两侧的平均亮度差 = 真实的"跳变幅度"
    before = float(smooth[max(0, peak - win): peak + 1].mean())
    after = float(smooth[peak + 1: peak + 1 + win].mean())
    step = abs(after - before)
    result.detail = f"step={step:.2f}"

    if step < th.edge_response_min:
        return result

    # 色相不变性：亮度变了但颜色没变 → 更可能是阴影
    if hue_profile is not None and hue_profile.size == profile.size:
        h_before = circular_hue_mean(hue_profile[max(0, peak - win): peak + 1])
        h_after = circular_hue_mean(hue_profile[peak + 1: peak + 1 + win])
        dh = circular_hue_diff(h_before, h_after)
        result.detail += f" dHue={dh:.1f}"
        if dh < th.min_hue_jump_deg:
            result.detail = f"shadow_like(step={step:.2f} dHue={dh:.1f})"
            return result

    # 唯一性检查：若在别处还有强度相近的跳变，说明更像普通纹理变化
    mask = np.ones(grad.size, bool)
    mask[max(0, peak - 3): peak + 4] = False
    second = float(grad[mask].max()) if mask.any() else 0.0
    if second > 0.6 * float(grad[peak]):
        result.detail = f"ambiguous(second={second:.2f})"
        return result

    result.suspected = True
    result.confidence = round(
        min(0.6, 0.3 + 0.5 * min(1.0, step / (2 * th.edge_response_min))), 3
    )
    result.distance_m = float(z_axis[min(peak + 1, z_axis.size - 1)])
    return result


class StairsDetector(Detector):
    name = "stairs"

    def __init__(self, config: Optional[AppConfig] = None):
        super().__init__(config)
        self.th: StairsThresholds = self.config.stairs
        self._hits = 0
        self._last = StairsResult()

    def reset(self) -> None:
        self._hits = 0
        self._last = StairsResult()

    def process(self, ctx: FrameContext) -> StairsResult:
        th = self.th
        # 度量空间分辨率粗一些即可，这里只关心大尺度跳变
        grid = ctx.metric_grid(
            x_span_m=2.0,
            z_min_m=th.z_min_m,
            z_max_m=th.z_max_m,
            resolution_m=0.02,
        )
        if grid.valid_ratio < 0.2:
            self._hits = max(0, self._hits - 1)
            return StairsResult(detail="grid_invalid")

        gray = ctx.frame if ctx.frame.ndim == 2 else cv2.cvtColor(ctx.frame, cv2.COLOR_BGR2GRAY)
        warped = cv2.remap(
            gray, grid.mapx, grid.mapy,
            interpolation=cv2.INTER_LINEAR,
            borderMode=cv2.BORDER_CONSTANT,
            borderValue=0,
        )
        center_cols = np.abs(grid.x_axis) <= CENTER_HALF_WIDTH_M
        if not np.any(center_cols):
            return StairsResult(detail="no_center_columns")

        sub = warped[:, center_cols]
        sub_valid = grid.valid[:, center_cols]
        row_valid = sub_valid.sum(axis=1)

        # 只分析有有效像素的最长连续行区间，否则有效区边界会被当成巨大阶跃
        span = largest_valid_span(row_valid)
        if span is None:
            self._hits = max(0, self._hits - 1)
            return StairsResult(detail="no_valid_rows")
        r0, r1 = span
        row_valid = row_valid[r0:r1]
        sub = sub[r0:r1]
        sub_valid = sub_valid[r0:r1]
        z_axis = grid.z_axis[r0:r1]

        with np.errstate(invalid="ignore", divide="ignore"):
            profile = np.where(
                row_valid > 0,
                sub.sum(axis=1) / np.maximum(row_valid, 1),
                0.0,
            )

        # 色相曲线：用于区分"阴影"（只改亮度）与"材质/台阶"（也改色相）
        hue_profile = None
        if ctx.frame.ndim == 3:
            hsv = cv2.cvtColor(ctx.frame, cv2.COLOR_BGR2HSV)
            hue_w = cv2.remap(
                hsv[:, :, 0], grid.mapx, grid.mapy,
                interpolation=cv2.INTER_NEAREST,
                borderMode=cv2.BORDER_CONSTANT,
                borderValue=0,
            ).astype(np.float64)[:, center_cols][r0:r1]
            ang = hue_w * (np.pi / 90.0)
            sin_sum = (np.sin(ang) * sub_valid).sum(axis=1)
            cos_sum = (np.cos(ang) * sub_valid).sum(axis=1)
            hue_profile = (np.degrees(np.arctan2(sin_sum, cos_sum)) / 2.0) % 180.0

        found = find_step(profile, z_axis, th, hue_profile=hue_profile)

        # 多帧确认：连续命中足够多次才真的报
        if found.suspected:
            self._hits += 1
        else:
            self._hits = max(0, self._hits - 1)

        if self._hits >= th.confirm_frames:
            self._last = found
        else:
            self._last = StairsResult(
                detail=f"unconfirmed(hits={self._hits}/{th.confirm_frames}) "
                       + found.detail
            )
        return self._last
