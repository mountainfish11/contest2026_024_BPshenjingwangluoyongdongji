"""播报策略：优先级 + 按事件冷却 + 全局限速。

与参照实现的关键差别：参照实现是**一个全局 14 秒计时器**，所有提示共用一个
冷却窗口。结果是"刚说完一句闲聊，就该报转弯了却说不了"。

这里改成：
- 每种事件（警报种类或引导状态）**独立冷却**；
- 全局限速只约束"最低间隔"，且**高优先级事件可以抢占**；
- 警报优先于引导状态：有警报时先报警报，没有才报"往哪走"。
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, List, Optional

from .alerts import (
    CROSSWALK,
    LIGHT_GREEN,
    LIGHT_RED,
    OBSTACLE_NEAR,
    STAIRS,
    WARNING_TILE,
    Alert,
)
from .config import SpeechThresholds
from .state import GuideState
from .temporal import TrackerState


@dataclass
class SpeechDecision:
    event: str = ""
    text: Optional[str] = None
    priority: int = 0
    spoke: bool = False
    reason: str = ""

    def to_dict(self) -> dict:
        return {
            "event": self.event,
            "text": self.text,
            "priority": self.priority,
            "spoke": self.spoke,
            "reason": self.reason,
        }


def _fmt_distance(d_m: float) -> str:
    """距离口语化：小于 1 米用厘米，否则用米。"""
    if d_m < 1.0:
        return f"{max(0.0, d_m) * 100:.0f}厘米"
    return f"{d_m:.1f}米"


def build_alert_text(alert: Alert) -> Optional[str]:
    """把警报翻译成一句给盲人听的话。"""
    d = alert.distance_m
    if alert.kind == WARNING_TILE:
        if d is None:
            return "前方有提示砖，注意路口或台阶"
        return f"前方{_fmt_distance(d)}有提示砖，注意路口或台阶"
    if alert.kind == CROSSWALK:
        return "前方是斑马线，注意车辆"
    if alert.kind == STAIRS:
        return "疑似有台阶或落差，请放慢脚步"
    if alert.kind == OBSTACLE_NEAR:
        name = alert.detail or "障碍物"
        if d is None:
            return f"前方有{name}"
        return f"前方{_fmt_distance(d)}有{name}"
    if alert.kind == LIGHT_RED:
        return "前方红灯，请停下等待"
    if alert.kind == LIGHT_GREEN:
        return "前方绿灯，可以通行"
    return None


def build_text(state: GuideState, tracker: Optional[TrackerState] = None) -> Optional[str]:
    """把引导状态翻译成一句给盲人听的话；居中/无盲道时不播报。"""
    tracker = tracker or TrackerState()
    if state == GuideState.TURN_LEFT:
        return "盲道要往左拐了，慢一点"
    if state == GuideState.TURN_RIGHT:
        return "盲道要往右拐了，慢一点"
    if state == GuideState.DRIFT_LEFT:
        off = tracker.offset_m
        if off is None:
            return "盲道偏左了，向左靠一点"
        return f"盲道偏左{_fmt_distance(abs(off))}，向左靠一点"
    if state == GuideState.DRIFT_RIGHT:
        off = tracker.offset_m
        if off is None:
            return "盲道偏右了，向右靠一点"
        return f"盲道偏右{_fmt_distance(abs(off))}，向右靠一点"
    if state == GuideState.PATH_LOST:
        return "暂时找不到盲道了，请停下确认方向"
    # ALIGNED / NO_PATH：不说话
    return None


class SpeechPolicy:
    """决定"这一帧要不要说话、说什么"。"""

    def __init__(self, thresholds: Optional[SpeechThresholds] = None):
        self.th = thresholds or SpeechThresholds()
        self._last_by_event: Dict[str, float] = {}
        self._last_spoken_ts: Optional[float] = None
        self._last_priority: int = -1
        self._last_text: Optional[str] = None

    def reset(self) -> None:
        self._last_by_event.clear()
        self._last_spoken_ts = None
        self._last_priority = -1
        self._last_text = None

    def _candidate(self, state: GuideState, tracker: Optional[TrackerState],
                   alerts: Optional[List[Alert]]) -> tuple[str, Optional[str], int]:
        """挑出这一帧最该说的那条：警报优先，其次引导状态。"""
        if alerts:
            top = max(alerts, key=lambda a: a.priority)
            text = build_alert_text(top)
            if text is not None:
                return top.kind, text, int(top.priority)

        event = state.value
        text = build_text(state, tracker)
        priority = int(self.th.priority.get(event, 0))
        return event, text, priority

    def decide(self, state: GuideState, tracker: Optional[TrackerState] = None,
               alerts: Optional[List[Alert]] = None,
               now: float = 0.0) -> SpeechDecision:
        event, text, priority = self._candidate(state, tracker, alerts)

        if text is None:
            return SpeechDecision(event=event, text=None, priority=priority,
                                  spoke=False, reason="silent_state")

        # 1) 同事件冷却
        cooldown = float(self.th.cooldown_s.get(event, 0.0))
        last = self._last_by_event.get(event)
        if last is not None and (now - last) < cooldown:
            return SpeechDecision(event=event, text=text, priority=priority,
                                  spoke=False,
                                  reason=f"cooldown({cooldown - (now - last):.1f}s left)")

        # 2) 全局限速：更高优先级可抢占
        if self._last_spoken_ts is not None:
            gap = now - self._last_spoken_ts
            if gap < self.th.global_min_gap_s and priority <= self._last_priority:
                return SpeechDecision(event=event, text=text, priority=priority,
                                      spoke=False,
                                      reason=f"global_gap({self.th.global_min_gap_s - gap:.1f}s left)")

        self._last_by_event[event] = now
        self._last_spoken_ts = now
        self._last_priority = priority
        self._last_text = text
        return SpeechDecision(event=event, text=text, priority=priority,
                              spoke=True, reason="ok")
