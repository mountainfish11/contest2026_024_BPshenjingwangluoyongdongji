# BlindBadge Event Protocol

## 事件类型

### obstacle_near
前方障碍物接近。

字段：
- distance_cm: 距离，单位 cm
- direction: front / left / right
- confidence: 置信度 0-100

示例：
```json
{"type":"obstacle_near","distance_cm":80,"direction":"front","confidence":90}
