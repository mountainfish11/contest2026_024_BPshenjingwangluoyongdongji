"""红绿灯状态判别。

流程：复用 COCO 的 ``traffic light`` 检测框 → 框内 HSV 找红/黄/绿高饱和亮点 →
多帧投票定状态。

为什么不做成纯颜色 blob 检测：单纯找"红色圆斑"在城市里误报极高（车灯、招牌、
红色雨伞）。先用模型把"红绿灯"这个物体框出来，再在框内判颜色，误报低一个量级。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import List, Optional, Sequence, Tuple

import cv2
import numpy as np

from ..config import AppConfig, TrafficLightThresholds
from .base import Box
from .obstacle import TRAFFIC_LIGHT_CLASS

#: HSV 颜色区间（OpenCV 的 H 取 0-179）
RED_LO = ((0, 120, 80), (10, 255, 255))
RED_HI = ((170, 120, 80), (179, 255, 255))
AMBER = ((11, 120, 80), (34, 255, 255))
GREEN = ((35, 100, 80), (90, 255, 255))

STATE_UNKNOWN = "unknown"
STATE_RED = "red"
STATE_GREEN = "green"
STATE_AMBER = "amber"


@dataclass
class TrafficLightResult:
    present: bool = False
    state: str = STATE_UNKNOWN
    conf: float = 0.0
    box: Optional[Box] = None
    ratios: dict = None  # type: ignore[assignment]

    def __post_init__(self):
        if self.ratios is None:
            self.ratios = {}

    def to_dict(self) -> dict:
        return {
            "present": self.present,
            "state": self.state,
            "conf": round(float(self.conf), 3),
            "box": None if self.box is None else [round(float(v), 1) for v in self.box],
            "ratios": {k: round(float(v), 3) for k, v in self.ratios.items()},
        }


def color_ratios(crop: np.ndarray) -> dict:
    """返回红/黄/绿像素在高饱和度像素中的占比。"""
    if crop is None or crop.size == 0:
        return {"red": 0.0, "amber": 0.0, "green": 0.0}
    hsv = cv2.cvtColor(crop, cv2.COLOR_BGR2HSV)
    total = float(hsv.shape[0] * hsv.shape[1]) or 1.0

    def mask_ratio(lo, hi) -> float:
        mask = cv2.inRange(hsv, np.array(lo, np.uint8), np.array(hi, np.uint8))
        return float((mask > 0).sum()) / total

    red = mask_ratio(*RED_LO) + mask_ratio(*RED_HI)
    amber = mask_ratio(*AMBER)
    green = mask_ratio(*GREEN)
    return {"red": red, "amber": amber, "green": green}


def classify_color(crop: np.ndarray, th: TrafficLightThresholds) -> Tuple[str, float, dict]:
    """在单个红绿灯框内判颜色。"""
    ratios = color_ratios(crop)
    best = max(ratios.items(), key=lambda kv: kv[1])
    if best[1] < th.min_color_ratio:
        return STATE_UNKNOWN, 0.0, ratios
    # 置信度：最大占比相对门槛的富余程度
    conf = min(0.99, best[1] / max(th.min_color_ratio * 3.0, 1e-6))
    return best[0], round(conf, 3), ratios


class TrafficLightAnalyzer:
    """把逐帧颜色判断通过投票变成稳定状态。"""

    def __init__(self, config: Optional[AppConfig] = None):
        self.config = config or AppConfig()
        self._history: List[str] = []
        self.state = STATE_UNKNOWN

    def reset(self) -> None:
        self._history.clear()
        self.state = STATE_UNKNOWN

    def update(
        self,
        frame: np.ndarray,
        raw: Sequence[Tuple[Box, float, str]],
    ) -> TrafficLightResult:
        th = self.config.traffic_light
        boxes = [(box, conf) for box, conf, name in raw
                 if name == TRAFFIC_LIGHT_CLASS and conf >= th.conf]
        if not boxes:
            self._history.clear()
            self.state = STATE_UNKNOWN
            return TrafficLightResult()

        # 取最靠下的那个（离行人最近、通常也正是要看的那个）
        boxes.sort(key=lambda bc: -bc[0][3])
        box, det_conf = boxes[0]
        h, w = frame.shape[:2]
        x1 = max(0, min(w - 1, int(round(box[0]))))
        y1 = max(0, min(h - 1, int(round(box[1]))))
        x2 = max(0, min(w, int(round(box[2]))))
        y2 = max(0, min(h, int(round(box[3]))))
        if x2 - x1 < 4 or y2 - y1 < 4:
            return TrafficLightResult(present=True, box=box)

        state, color_conf, ratios = classify_color(frame[y1:y2, x1:x2], th)

        self._history.append(state)
        if len(self._history) > max(1, th.vote_frames):
            self._history.pop(0)

        # 投票：多数一致才改状态，且 unknown 不能覆盖已知颜色
        known = [s for s in self._history if s != STATE_UNKNOWN]
        if known:
            votes = {s: known.count(s) for s in set(known)}
            top = max(votes.items(), key=lambda kv: kv[1])
            if top[1] * 2 > len(self._history):
                self.state = top[0]
            else:
                self.state = STATE_UNKNOWN
        else:
            self.state = STATE_UNKNOWN

        return TrafficLightResult(
            present=True,
            state=self.state,
            conf=round(min(0.99, det_conf * color_conf), 3) if color_conf else 0.0,
            box=box,
            ratios=ratios,
        )
