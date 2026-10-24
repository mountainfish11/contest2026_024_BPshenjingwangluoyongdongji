"""引导状态机。

设计要点
--------
- **判定量全部是度量空间的物理量**（米 / 度），不是像素斜率；
- **滞回**：进入与退出用不同阈值，避免在阈值附近来回跳；
- **优先级**：丢盲道 > 提示砖 > 转向 > 偏移 > 居中。

符号约定（贯穿整个项目）
------------------------
- ``offset_m`` 为盲道中心相对用户的横向位置，**正=盲道在右，负=盲道在左**。
  因此 ``offset_m < 0`` → 需要**向左**靠 → ``DRIFT_LEFT``。
- ``heading_deg`` 为 ``atan(dX/dZ)``，**正=前方盲道向右偏** → 需要**向右**转
  → ``TURN_RIGHT``。
两个量都按"用户应当往哪边走"来命名状态。
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional

from .config import TactileThresholds
from .temporal import TrackerState


class GuideState(str, Enum):
    """引导状态：只回答"路往哪走"。警报（提示砖/障碍等）见 :mod:`vision_assist.alerts`。"""

    NO_PATH = "NO_PATH"
    ALIGNED = "ALIGNED"
    DRIFT_LEFT = "DRIFT_LEFT"
    DRIFT_RIGHT = "DRIFT_RIGHT"
    TURN_LEFT = "TURN_LEFT"
    TURN_RIGHT = "TURN_RIGHT"
    PATH_LOST = "PATH_LOST"

    def __str__(self) -> str:  # 便于打印与写进 JSON
        return self.value


_TURN_STATES = (GuideState.TURN_LEFT, GuideState.TURN_RIGHT)
_DRIFT_STATES = (GuideState.DRIFT_LEFT, GuideState.DRIFT_RIGHT)


@dataclass
class StateDecision:
    state: GuideState
    reason: str = ""
    offset_m: Optional[float] = None
    heading_deg: Optional[float] = None
    warning_near_m: Optional[float] = None

    def to_dict(self) -> dict:
        def r(v, nd=3):
            return None if v is None else round(float(v), nd)

        return {
            "state": self.state.value,
            "reason": self.reason,
            "offset_m": r(self.offset_m),
            "heading_deg": r(self.heading_deg),
            "warning_near_m": r(self.warning_near_m),
        }


class StateMachine:
    """把平滑后的 :class:`TrackerState` 映射成 :class:`GuideState`。"""

    def __init__(self, thresholds: Optional[TactileThresholds] = None):
        self.th = thresholds or TactileThresholds()
        self.state = GuideState.NO_PATH

    def reset(self) -> None:
        self.state = GuideState.NO_PATH

    def update(self, tracker: TrackerState) -> StateDecision:
        th = self.th
        prev = self.state

        # 1) 丢盲道：曾经看到过、但已经不满足"看到"的判定超过阈值
        if tracker.lost:
            return self._set(GuideState.PATH_LOST, "lost",
                             tracker, f"缺失 {tracker.lost_for_s:.1f}s")

        # 2) 没看到盲道
        if not tracker.present:
            return self._set(GuideState.NO_PATH, "no_detection", tracker)

        # 3) 转向（优先级高于偏移），带滞回
        heading = tracker.heading_deg
        if heading is not None:
            thr = th.turn_exit_deg if prev in _TURN_STATES else th.turn_enter_deg
            if heading >= thr:
                return self._set(GuideState.TURN_RIGHT, "heading", tracker,
                                 f"{heading:+.1f}deg >= {thr}")
            if heading <= -thr:
                return self._set(GuideState.TURN_LEFT, "heading", tracker,
                                 f"{heading:+.1f}deg <= -{thr}")

        # 4) 横向偏移，带滞回
        offset = tracker.offset_m
        if offset is not None:
            thr = th.drift_exit_m if prev in _DRIFT_STATES else th.drift_enter_m
            if offset <= -thr:
                return self._set(GuideState.DRIFT_LEFT, "offset", tracker,
                                 f"{offset:+.2f}m <= -{thr}")
            if offset >= thr:
                return self._set(GuideState.DRIFT_RIGHT, "offset", tracker,
                                 f"{offset:+.2f}m >= {thr}")

        # 5) 居中
        return self._set(GuideState.ALIGNED, "centered", tracker)

    def _set(self, state: GuideState, reason: str, tracker: TrackerState,
             detail: str = "") -> StateDecision:
        self.state = state
        return StateDecision(
            state=state,
            reason=f"{reason}:{detail}" if detail else reason,
            offset_m=tracker.offset_m,
            heading_deg=tracker.heading_deg,
            warning_near_m=tracker.warning_near_m,
        )
