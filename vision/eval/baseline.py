"""忠实复刻参照实现的方向判定，用于同口径对比。

来源：``材料/blind_navigation-2.2.0/routes/video.py`` 的 ``generate_frames()``::

    centers = []            # 每个检测框的中心 (cx, cy)
    ...
    if len(centers) >= 2:
        ys = np.array([c[1] for c in centers])
        xs = np.array([c[0] for c in centers])
        slope, intercept = np.polyfit(ys, xs, 1)
        if slope < -THRESHOLD_SLOPE:      # THRESHOLD_SLOPE = 0.41
            左转
        elif slope > THRESHOLD_SLOPE:
            右转

注意：这里**只复刻方向判定本身**，不包含语音、LLM 调用、冷却计时等外围逻辑——
那些与"能不能判出方向"无关。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

import numpy as np

#: 与参照实现 config.example.py 保持一致
THRESHOLD_SLOPE = 0.41


@dataclass
class BaselineVerdict:
    direction: str = "none"     # left / right / straight / none
    slope: Optional[float] = None
    box_count: int = 0
    reason: str = ""

    @property
    def usable(self) -> bool:
        """参照实现是否给出了方向（不含"直行"这种兜底）。"""
        return self.direction in ("left", "right", "straight")


def judge(centers: Sequence[Tuple[float, float]],
          threshold: float = THRESHOLD_SLOPE) -> BaselineVerdict:
    """按参照实现的方式判断方向。``centers`` 为检测框中心 ``(cx, cy)``。"""
    n = len(centers)
    if n < 2:
        return BaselineVerdict(direction="none", box_count=n,
                               reason=f"只有 {n} 个框，参照实现要求 >=2")
    ys = np.array([c[1] for c in centers], dtype=np.float64)
    xs = np.array([c[0] for c in centers], dtype=np.float64)
    if np.allclose(xs, xs[0]) and np.allclose(ys, ys[0]):
        return BaselineVerdict(direction="none", box_count=n, reason="中心点重合")
    slope, _intercept = np.polyfit(ys, xs, 1)
    slope = float(slope)
    if slope < -threshold:
        return BaselineVerdict("left", slope, n, "slope < -0.41")
    if slope > threshold:
        return BaselineVerdict("right", slope, n, "slope > 0.41")
    return BaselineVerdict("straight", slope, n, "|slope| <= 0.41")


def centers_from_boxes(boxes: Sequence[Sequence[float]]) -> List[Tuple[float, float]]:
    """把 ``xyxy`` 框转成中心点，与参照实现一致。"""
    out: List[Tuple[float, float]] = []
    for x1, y1, x2, y2 in boxes:
        out.append(((x1 + x2) / 2.0, (y1 + y2) / 2.0))
    return out
