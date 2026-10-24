"""时序平滑与去抖。

参照实现最致命的问题之一是"单帧抖动直接播报"。这里做三件事：

1. **EMA 平滑**：横向偏移与航向角按指数移动平均，抑制单帧噪声；
2. **存在性去抖**：盲道"有/无"必须连续 ``confirm_frames`` 帧一致才确认，
   撤销需要连续 ``release_frames`` 帧，避免一帧漏检就喊"盲道丢失"；
3. **砖型投票**：砖型是分类量，用最近若干帧的多数票决定，而不是信单帧。
"""
from __future__ import annotations

from collections import Counter, deque
from dataclasses import dataclass, field
from typing import Deque, Optional

from .config import TemporalThresholds
from .detectors.tactile import TactileResult


class Ema:
    """带初始化的指数移动平均。"""

    def __init__(self, alpha: float = 0.35):
        self.alpha = float(alpha)
        self.value: Optional[float] = None

    def update(self, raw: Optional[float]) -> Optional[float]:
        if raw is None:
            return self.value
        if self.value is None:
            self.value = float(raw)
        else:
            self.value = self.alpha * float(raw) + (1.0 - self.alpha) * self.value
        return self.value

    def reset(self) -> None:
        self.value = None


class DebouncedFlag:
    """布尔去抖：进入需要连续命中，退出需要连续未命中。"""

    def __init__(self, confirm: int = 3, release: int = 5):
        self.confirm = max(1, int(confirm))
        self.release = max(1, int(release))
        self.state = False
        self._on_hits = 0
        self._off_hits = 0

    def update(self, raw: bool) -> bool:
        if raw:
            self._off_hits = 0
            self._on_hits += 1
            if not self.state and self._on_hits >= self.confirm:
                self.state = True
        else:
            self._on_hits = 0
            self._off_hits += 1
            if self.state and self._off_hits >= self.release:
                self.state = False
        return self.state


class Vote:
    """分类量多数票：窗口内某类别占比达到阈值才切换。"""

    def __init__(self, window: int = 7, min_ratio: float = 0.6):
        self.window = max(1, int(window))
        self.min_ratio = float(min_ratio)
        self._buf: Deque[str] = deque(maxlen=self.window)
        self.state: str = "unknown"

    def update(self, value: str) -> str:
        if value:
            self._buf.append(value)
        if not self._buf:
            return self.state
        counter = Counter(self._buf)
        top, count = counter.most_common(1)[0]
        if count / len(self._buf) >= self.min_ratio:
            self.state = top
        return self.state


@dataclass
class TrackerState:
    """平滑后的盲道状态，供状态机消费。"""

    present: bool = False
    offset_m: Optional[float] = None
    heading_deg: Optional[float] = None
    near_m: Optional[float] = None
    far_m: Optional[float] = None
    tile_type: str = "unknown"
    warning_near_m: Optional[float] = None
    #: 曾经看到过盲道、但已经连续丢失超过阈值的秒数
    lost: bool = False
    lost_for_s: float = 0.0

    def to_dict(self) -> dict:
        def r(v, nd=3):
            return None if v is None else round(float(v), nd)

        return {
            "present": self.present,
            "offset_m": r(self.offset_m),
            "heading_deg": r(self.heading_deg),
            "near_m": r(self.near_m),
            "far_m": r(self.far_m),
            "tile_type": self.tile_type,
            "warning_near_m": r(self.warning_near_m),
            "lost": self.lost,
            "lost_for_s": round(self.lost_for_s, 2),
        }


class TactileTracker:
    """把逐帧的 :class:`TactileResult` 变成稳定的 :class:`TrackerState`。"""

    def __init__(self, thresholds: Optional[TemporalThresholds] = None,
                 lost_after_s: float = 1.0):
        th = thresholds or TemporalThresholds()
        self.th = th
        self._offset = Ema(th.ema_alpha)
        self._heading = Ema(th.ema_alpha)
        self._near = Ema(th.ema_alpha)
        self._far = Ema(th.ema_alpha)
        self._warning = Ema(th.ema_alpha)
        self._presence = DebouncedFlag(th.confirm_frames, th.release_frames)
        self._tile = Vote(window=max(5, th.confirm_frames * 2 + 1), min_ratio=0.6)
        self._lost_after_s = float(lost_after_s)
        self._ever_present = False
        self._last_seen_ts: Optional[float] = None
        self.state = TrackerState()

    def update(self, result: TactileResult, now: float) -> TrackerState:
        present = self._presence.update(bool(result.present))

        if result.present:
            # 只在检到时更新数值，避免丢失期间被 None "拉回"旧值
            self._offset.update(result.lateral_offset_m)
            self._heading.update(result.heading_deg)
            self._near.update(result.near_m)
            self._far.update(result.far_m)
            self._warning.update(result.warning_near_m)
            self._tile.update(result.tile_type)
            self._last_seen_ts = now
            self._ever_present = True

        lost_for = 0.0
        if self._last_seen_ts is not None:
            lost_for = max(0.0, now - self._last_seen_ts)

        self.state = TrackerState(
            present=present,
            offset_m=self._offset.value,
            heading_deg=self._heading.value,
            near_m=self._near.value,
            far_m=self._far.value,
            tile_type=self._tile.state,
            warning_near_m=self._warning.value if present else None,
            lost=bool(self._ever_present and not present and lost_for >= self._lost_after_s),
            lost_for_s=lost_for if self._ever_present else 0.0,
        )
        return self.state

    def reset(self) -> None:
        for ema in (self._offset, self._heading, self._near, self._far, self._warning):
            ema.reset()
        self.state = TrackerState()
