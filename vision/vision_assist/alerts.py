"""警报：与"引导状态"正交的一类事件。

为什么不把提示砖/障碍物塞进 :class:`GuideState`
-----------------------------------------------
引导状态回答的是"**路往哪走**"（居中/偏移/转向/丢失）；而提示砖、障碍物、
红绿灯回答的是"**现在该注意什么**"。两者是正交的。

把警报当成状态会导致一个真实故障：实测 ``Test.mp4`` 里提示砖常在脚下（约 0 米），
阈值 2.5 米就永远触发，于是状态被 ``WARNING_TILE`` 占满 298/300 帧，转向与偏移
信息全被吃掉。因此这里把它们拆开：

- :class:`GuideState` —— 引导状态，每帧只有一个；
- :class:`Alert` —— 警报列表，可以有多个，按优先级抢占播报。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional

from .config import AppConfig, TactileThresholds
from .detectors.tactile import TactileResult
from .temporal import TrackerState

#: 警报种类
WARNING_TILE = "WARNING_TILE"
CROSSWALK = "CROSSWALK"
STAIRS = "STAIRS"
OBSTACLE_NEAR = "OBSTACLE_NEAR"
LIGHT_RED = "LIGHT_RED"
LIGHT_GREEN = "LIGHT_GREEN"


@dataclass
class Alert:
    kind: str
    priority: int
    distance_m: Optional[float] = None
    detail: str = ""

    def to_dict(self) -> dict:
        return {
            "kind": self.kind,
            "priority": self.priority,
            "distance_m": None if self.distance_m is None else round(self.distance_m, 3),
            "detail": self.detail,
        }


@dataclass
class AlertResult:
    alerts: List[Alert] = field(default_factory=list)
    #: 是否正踩在提示砖上（不是"前方有提示砖"，因此不报警，但要在 UI 上体现）
    on_warning_tile: bool = False

    def to_dict(self) -> dict:
        return {
            "alerts": [a.to_dict() for a in self.alerts],
            "on_warning_tile": self.on_warning_tile,
        }

    def top(self) -> Optional[Alert]:
        return max(self.alerts, key=lambda a: a.priority) if self.alerts else None


class AlertEvaluator:
    """把各路检测结果翻译成警报列表。"""

    def __init__(self, config: Optional[AppConfig] = None):
        self.config = config or AppConfig()

    def _prio(self, kind: str) -> int:
        return int(self.config.speech.priority.get(kind, 50))

    def evaluate(
        self,
        tactile: TactileResult,
        tracker: Optional[TrackerState] = None,
        obstacles=None,
        crosswalk=None,
        stairs=None,
        traffic_light=None,
    ) -> AlertResult:
        th: TactileThresholds = self.config.tactile
        result = AlertResult()

        # ---- 提示砖（必须 present 才看，缺失期间的框不可信）
        if tactile.present:
            ahead: List[float] = []
            for item in tactile.boxes_detail:
                if item.tile_type != "warning" or item.near_m is None:
                    continue
                if item.near_m < th.warning_tile_underfoot_m:
                    result.on_warning_tile = True
                elif item.near_m <= th.warning_tile_near_m:
                    ahead.append(float(item.near_m))
            if ahead:
                result.alerts.append(Alert(
                    kind=WARNING_TILE, priority=self._prio(WARNING_TILE),
                    distance_m=min(ahead), detail="前方提示砖",
                ))

        # ---- 障碍物
        if obstacles is not None and obstacles.nearest_m is not None:
            if obstacles.nearest_m <= self.config.obstacle.near_m:
                nearest = next(
                    (o for o in obstacles.obstacles if o.distance_m is not None), None
                )
                result.alerts.append(Alert(
                    kind=OBSTACLE_NEAR, priority=self._prio(OBSTACLE_NEAR),
                    distance_m=obstacles.nearest_m,
                    detail=nearest.label_cn if nearest is not None else "障碍物",
                ))

        # ---- 斑马线
        if crosswalk is not None and getattr(crosswalk, "present", False):
            if crosswalk.confidence >= self.config.crosswalk.alert_min_confidence:
                result.alerts.append(Alert(
                    kind=CROSSWALK, priority=self._prio(CROSSWALK),
                    distance_m=getattr(crosswalk, "near_m", None),
                    detail=f"{crosswalk.bar_count}条白条",
                ))

        # ---- 台阶（只有"疑似"）
        if stairs is not None and getattr(stairs, "suspected", False):
            result.alerts.append(Alert(
                kind=STAIRS, priority=self._prio(STAIRS),
                distance_m=getattr(stairs, "distance_m", None),
                detail="疑似台阶",
            ))

        # ---- 红绿灯
        if traffic_light is not None and getattr(traffic_light, "present", False):
            state = getattr(traffic_light, "state", "unknown")
            if state == "red":
                result.alerts.append(Alert(
                    kind=LIGHT_RED, priority=self._prio(LIGHT_RED), detail="红灯"))
            elif state == "green":
                result.alerts.append(Alert(
                    kind=LIGHT_GREEN, priority=self._prio(LIGHT_GREEN), detail="绿灯"))

        return result
