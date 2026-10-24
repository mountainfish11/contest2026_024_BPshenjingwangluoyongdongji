"""Scene JSON 的 schema 校验。

不引入 ``jsonschema`` 依赖，手写一个够用的校验器：检查必需字段、类型、取值范围。
用途有两个：
1. 单元测试里守住输出协议，防止改代码时悄悄破坏对接方依赖的字段；
2. 服务器 ``/events`` 推送前自检，避免把坏数据发出去。
"""
from __future__ import annotations

from typing import Any, Dict, List

#: 输出协议版本。字段只增不改；不兼容变更必须升版本号。
SCHEMA_VERSION = "1.0"

#: 引导状态全集
GUIDE_STATES = (
    "NO_PATH", "ALIGNED", "DRIFT_LEFT", "DRIFT_RIGHT",
    "TURN_LEFT", "TURN_RIGHT", "PATH_LOST",
)
#: 砖型全集
TILE_TYPES = ("guide", "warning", "unknown")
#: 警报种类全集
ALERT_KINDS = (
    "WARNING_TILE", "CROSSWALK", "STAIRS",
    "OBSTACLE_NEAR", "LIGHT_RED", "LIGHT_GREEN",
)
#: 红绿灯状态全集
TRAFFIC_LIGHT_STATES = ("red", "green", "amber", "unknown")


def _is_num(v: Any) -> bool:
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def validate_scene(scene: Dict[str, Any]) -> List[str]:
    """校验一帧 Scene，返回问题列表（空列表表示通过）。"""
    problems: List[str] = []

    def require(key: str, types, container=scene):
        if key not in container:
            problems.append(f"缺少字段: {key}")
            return None
        value = container[key]
        if value is None and type(None) in (types if isinstance(types, tuple) else (types,)):
            return value
        if not isinstance(value, types if isinstance(types, tuple) else (types,)):
            problems.append(f"字段 {key} 类型错误: {type(value).__name__}")
        return value

    require("ts", (int, float))
    require("frame", int)
    require("state", str)
    require("latency_ms", dict)

    if scene.get("state") not in GUIDE_STATES:
        problems.append(f"未知的 state: {scene.get('state')!r}")

    # quality
    quality = scene.get("quality")
    if not isinstance(quality, dict):
        problems.append("quality 必须是对象")
    else:
        for key in ("ok", "brightness", "blur", "warnings"):
            if key not in quality:
                problems.append(f"quality 缺少字段: {key}")
        if isinstance(quality.get("warnings"), list) is False and "warnings" in quality:
            problems.append("quality.warnings 必须是数组")

    # tactile
    tactile = scene.get("tactile")
    if not isinstance(tactile, dict):
        problems.append("tactile 必须是对象")
    else:
        for key in ("present", "tile_type", "lateral_offset_m", "heading_deg",
                    "near_m", "far_m", "warning_near_m", "on_warning_tile"):
            if key not in tactile:
                problems.append(f"tactile 缺少字段: {key}")
        if tactile.get("tile_type") not in TILE_TYPES:
            problems.append(f"未知的 tile_type: {tactile.get('tile_type')!r}")
        if not isinstance(tactile.get("present"), bool):
            problems.append("tactile.present 必须是布尔值")
        offset = tactile.get("lateral_offset_m")
        if offset is not None and not _is_num(offset):
            problems.append("tactile.lateral_offset_m 必须是数字或 null")
        warn = tactile.get("warning_near_m")
        if warn is not None and (_is_num(warn) and warn < 0):
            problems.append("tactile.warning_near_m 不能为负（负距离应截断为 0 或 null）")

    # tracker
    tracker = scene.get("tracker")
    if not isinstance(tracker, dict):
        problems.append("tracker 必须是对象")
    else:
        for key in ("present", "offset_m", "heading_deg", "lost"):
            if key not in tracker:
                problems.append(f"tracker 缺少字段: {key}")

    # alerts
    alerts = scene.get("alerts")
    if not isinstance(alerts, dict):
        problems.append("alerts 必须是对象")
    else:
        items = alerts.get("alerts")
        if not isinstance(items, list):
            problems.append("alerts.alerts 必须是数组")
        else:
            for i, alert in enumerate(items):
                if not isinstance(alert, dict):
                    problems.append(f"alerts[{i}] 必须是对象")
                    continue
                if alert.get("kind") not in ALERT_KINDS:
                    problems.append(f"alerts[{i}] 未知 kind: {alert.get('kind')!r}")
                if not _is_num(alert.get("priority")):
                    problems.append(f"alerts[{i}].priority 必须是数字")
                dist = alert.get("distance_m")
                if dist is not None and _is_num(dist) and dist < 0:
                    problems.append(f"alerts[{i}].distance_m 不能为负")

    # speech
    speech = scene.get("speech")
    if speech is not None:
        if not isinstance(speech, dict):
            problems.append("speech 必须是对象或 null")
        else:
            for key in ("event", "spoke", "reason"):
                if key not in speech:
                    problems.append(f"speech 缺少字段: {key}")
            if isinstance(speech.get("spoke"), bool) is False:
                problems.append("speech.spoke 必须是布尔值")

    # obstacles
    obstacles = scene.get("obstacles")
    if not isinstance(obstacles, dict):
        problems.append("obstacles 必须是对象")
    else:
        for key in ("count", "nearest_m", "items"):
            if key not in obstacles:
                problems.append(f"obstacles 缺少字段: {key}")
        if not isinstance(obstacles.get("items"), list):
            problems.append("obstacles.items 必须是数组")
        else:
            for i, ob in enumerate(obstacles["items"]):
                if not isinstance(ob, dict):
                    problems.append(f"obstacles[{i}] 必须是对象")
                    continue
                for key in ("label", "label_cn", "conf", "box", "distance_m"):
                    if key not in ob:
                        problems.append(f"obstacles[{i}] 缺少字段: {key}")
                dist = ob.get("distance_m")
                if dist is not None and _is_num(dist) and dist < 0:
                    problems.append(f"obstacles[{i}].distance_m 不能为负")

    # crosswalk
    crosswalk = scene.get("crosswalk")
    if not isinstance(crosswalk, dict):
        problems.append("crosswalk 必须是对象")
    else:
        for key in ("present", "confidence", "bar_count"):
            if key not in crosswalk:
                problems.append(f"crosswalk 缺少字段: {key}")

    # traffic_light
    tl = scene.get("traffic_light")
    if not isinstance(tl, dict):
        problems.append("traffic_light 必须是对象")
    else:
        for key in ("present", "state", "conf"):
            if key not in tl:
                problems.append(f"traffic_light 缺少字段: {key}")
        if tl.get("state") not in TRAFFIC_LIGHT_STATES:
            problems.append(f"未知的红绿灯状态: {tl.get('state')!r}")

    # stairs
    stairs = scene.get("stairs")
    if not isinstance(stairs, dict):
        problems.append("stairs 必须是对象")
    else:
        for key in ("suspected", "confidence", "distance_m"):
            if key not in stairs:
                problems.append(f"stairs 缺少字段: {key}")

    return problems


def assert_valid_scene(scene: Dict[str, Any]) -> None:
    problems = validate_scene(scene)
    if problems:
        raise ValueError("Scene schema 校验失败:\n  - " + "\n  - ".join(problems))
