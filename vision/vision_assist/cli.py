"""命令行入口。

示例::

    # 跑一段本地视频，输出逐帧场景 JSON
    python -m vision_assist.cli --source file:xxx.mp4 --json --frames 100

    # 接 ESP32-S3 图传，并把带叠加的视频存下来
    python -m vision_assist.cli --source mjpeg:http://192.168.1.20/stream \
        --pitch 30 --height 1.2 --save-video out/run.mp4

    # 只验证几何、不加载模型
    python -m vision_assist.cli --source file:xxx.mp4 --no-detect
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import List, Optional

import cv2
import numpy as np

from .config import AppConfig, CameraCalib
from .pipeline import Pipeline
from .sources import open_source


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="vision_assist",
        description="盲人辅助设备视觉识别子系统",
    )
    ap.add_argument("--source", default=None,
                    help="帧源：mjpeg:http://ip/stream / file:xxx.mp4 / usb:0")
    ap.add_argument("--config", default=None, help="JSON 配置文件路径")
    ap.add_argument("--height", type=float, default=None, help="相机离地高度（米）")
    ap.add_argument("--pitch", type=float, default=None, help="俯仰角（度，正=朝下）")
    ap.add_argument("--hfov", type=float, default=None, help="水平视场角（度）")
    ap.add_argument("--frames", type=int, default=0, help="处理多少帧后退出（0=一直跑）")
    ap.add_argument("--duration", type=float, default=0.0, help="跑多少秒后退出（0=不限）")
    ap.add_argument("--print-every", type=int, default=20, help="每多少帧打印一次")
    ap.add_argument("--json", action="store_true", help="逐帧输出场景 JSON")
    ap.add_argument("--no-detect", action="store_true", help="不加载模型，只算几何与画面质量")
    ap.add_argument("--save-video", default=None, help="把带叠加的视频存到该路径")
    ap.add_argument("--save-sheet", default=None, help="把若干帧叠加图拼成缩略图墙存到该路径")
    ap.add_argument("--sheet-frames", type=int, default=12, help="缩略图墙取样帧数")
    ap.add_argument("--sheet-cols", type=int, default=4, help="缩略图墙列数")
    ap.add_argument("--serve", action="store_true", help="起可视化调试服务（浏览器打开）")
    ap.add_argument("--host", default="127.0.0.1", help="调试服务监听地址")
    ap.add_argument("--port", type=int, default=8760, help="调试服务端口")
    # ---- 性能相关（帧数低 / 卡顿时调这几个）
    ap.add_argument("--imgsz", type=int, default=None,
                    help="盲道模型推理尺寸，默认 640。降到 416/320 明显提速，代价是远端精度")
    ap.add_argument("--imgsz-obstacle", type=int, default=None,
                    help="障碍物模型推理尺寸，默认 320")
    ap.add_argument("--obstacle-every", type=int, default=None,
                    help="障碍物模型每 N 帧跑一次，默认 1。设 2 可提速约 15-20%%，"
                         "代价是障碍物位置最多滞后 N 帧")
    ap.add_argument("--device", default=None,
                    help='推理设备：auto（默认，有 GPU 就用）/ cpu / cuda:0')
    ap.add_argument("--check-gpu", action="store_true",
                    help="只检查推理后端（GPU 能不能用），不启动任何东西")
    ap.add_argument("--target-fps", type=float, default=None,
                    help="处理帧率上限，默认 15。设 0 表示不限（会吃满 CPU）")
    ap.add_argument("--width-limit", type=int, default=None,
                    help="调试页画面压缩到多宽，默认 960。板子出 VGA 时设 0 表示不压缩")
    return ap


def resolve_config(args: argparse.Namespace) -> AppConfig:
    try:
        cfg = AppConfig.load(args.config)
    except FileNotFoundError as exc:
        # 别甩 traceback，直接告诉用户两条可行路径
        src = args.source or "<你的帧源>"
        print(f"[vision] {exc}", file=sys.stderr)
        print("", file=sys.stderr)
        print("[vision] 两个选择：", file=sys.stderr)
        print("[vision]   1) 不用配置文件，直接命令行传标定值：", file=sys.stderr)
        print(f"[vision]        python -m vision_assist.cli --serve --source {src} "
              "--height 1.2 --pitch 30", file=sys.stderr)
        print("[vision]   2) 先生成配置文件（无界面也能生成）：", file=sys.stderr)
        print(f"[vision]        python -m tools.calibrate --source {src} "
              "--height 1.2 --pitch 30 --no-gui --save-config calibration.json",
              file=sys.stderr)
        raise SystemExit(2) from None

    if args.source:
        cfg.source = args.source

    cam = cfg.camera.to_dict()
    changed = False
    for key, value in (("height_m", args.height), ("pitch_deg", args.pitch),
                       ("hfov_deg", args.hfov)):
        if value is not None:
            cam[key] = value
            changed = True
    if changed:
        cfg.camera = CameraCalib(**cam)

    if args.imgsz is not None:
        cfg.imgsz = int(args.imgsz)
    if args.imgsz_obstacle is not None:
        cfg.imgsz_obstacle = int(args.imgsz_obstacle)
    if args.obstacle_every is not None:
        cfg.obstacle_every = max(1, int(args.obstacle_every))
    if args.device is not None:
        cfg.device = str(args.device)
    if args.target_fps is not None:
        cfg.target_fps = float(args.target_fps)

    # 标定没做就起服务，所有"米"都是错的，必须显式提醒
    if args.config is None and not changed:
        print("", file=sys.stderr)
        print("[vision] ⚠️  正在使用**默认标定值**：离地 %.2f m、俯仰 %.1f°。"
              % (cfg.camera.height_m, cfg.camera.pitch_deg), file=sys.stderr)
        print("[vision]    如果这不是你实测的值，横向偏移和距离会整体偏掉。",
              file=sys.stderr)
        print("[vision]    标定方法：python -m tools.calibrate --source " +
              (args.source or "<你的帧源>"), file=sys.stderr)
        print("", file=sys.stderr)
    return cfg


def _write_image(path: Path, img: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    ok, buf = cv2.imencode(path.suffix or ".jpg", img)
    if not ok:
        raise RuntimeError(f"图像编码失败: {path}")
    path.write_bytes(buf.tobytes())


def run_serve(args: argparse.Namespace, cfg: AppConfig) -> int:
    """起可视化调试服务。"""
    from .server import VisionService, serve

    width_limit = 960 if args.width_limit is None else int(args.width_limit)
    service = VisionService(
        cfg, source=cfg.source,
        width_limit=width_limit,
        pipeline_kwargs={"enable_tactile": not args.no_detect} if args.no_detect else None,
    )
    service.start()
    server = serve(service, host=args.host, port=args.port)
    actual_port = server.server_address[1]
    url = f"http://{args.host}:{actual_port}/"
    print(f"[vision] 调试页面: {url}   <-- 请打开这个地址")
    print(f"[vision] MJPEG 流: {url}stream.mjpg")
    print(f"[vision] 事件流:   {url}events")
    print(f"[vision] 推理设备: {service._pipeline.device_description}")
    print(f"[vision] 帧源:     {cfg.source}")
    print("[vision] Ctrl+C 退出")
    try:
        while True:
            time.sleep(0.5)
    except KeyboardInterrupt:
        print("\n[vision] 正在退出…")
    finally:
        server.shutdown()
        server.server_close()
        service.stop()
    return 0


def run(args: argparse.Namespace) -> int:
    if getattr(args, "check_gpu", False):
        from .device import gpu_report
        print(gpu_report())
        return 0

    cfg = resolve_config(args)
    if args.serve:
        return run_serve(args, cfg)

    try:
        src = open_source(cfg.source)
    except Exception as exc:  # noqa: BLE001
        print(f"[vision] 打不开帧源 {cfg.source}: {exc}", file=sys.stderr)
        return 2

    pipeline = Pipeline(cfg, enable_tactile=not args.no_detect)
    writer = None
    sheet: List[np.ndarray] = []
    sheet_stride = 1
    if args.save_sheet and args.sheet_frames > 0:
        limit = args.frames if args.frames else 600
        sheet_stride = max(1, limit // max(1, args.sheet_frames))

    print(f"[vision] 帧源: {cfg.source}")
    n = 0
    t0 = time.time()
    last_report = t0
    frames_since = 0
    last_frame_time = None
    fps_ema = None
    try:
        while True:
            frame = src.read()
            if frame is None:
                if n == 0:
                    print("[vision] 读不到任何帧，退出", file=sys.stderr)
                    return 2
                time.sleep(0.005)
                continue

            now = time.time()
            if last_frame_time is not None:
                inst = 1.0 / max(1e-6, now - last_frame_time)
                fps_ema = inst if fps_ema is None else 0.9 * fps_ema + 0.1 * inst
            last_frame_time = now

            scene = pipeline.process(frame, n, source_info=src.info)
            scene.fps = fps_ema
            n += 1
            frames_since += 1

            annotated = None
            if args.save_video or args.save_sheet:
                annotated = pipeline.annotate(frame, scene)

            if args.save_video:
                if writer is None:
                    h, w = annotated.shape[:2]
                    out = Path(args.save_video)
                    out.parent.mkdir(parents=True, exist_ok=True)
                    writer = cv2.VideoWriter(
                        str(out), cv2.VideoWriter_fourcc(*"mp4v"), 15.0, (w, h)
                    )
                    if not writer.isOpened():
                        print(f"[vision] 无法创建视频: {out}", file=sys.stderr)
                        return 3
                writer.write(annotated)

            if args.save_sheet and annotated is not None and (n - 1) % sheet_stride == 0:
                if len(sheet) < args.sheet_frames:
                    sheet.append(annotated)

            now = time.time()
            report_by_time = (now - last_report) >= 1.0
            report_by_count = bool(args.print_every) and (n % args.print_every == 0)
            if args.json and report_by_count:
                print(json.dumps(scene.to_dict(), ensure_ascii=False))
            elif report_by_time or report_by_count:
                fps = frames_since / max(1e-6, now - last_report) if report_by_time else None
                t = scene.tactile
                msg = (
                    f"frame={n} "
                    f"fps={'?' if fps is None else round(fps, 1)} "
                    f"tactile={'Y' if t.present else 'N'} "
                    f"tile={t.tile_type} "
                    f"offset={'?' if t.lateral_offset_m is None else f'{t.lateral_offset_m:+.2f}m'} "
                    f"heading={'?' if t.heading_deg is None else f'{t.heading_deg:+.1f}deg'} "
                    f"lat={scene.latency_ms.get('total')}ms"
                )
                print(f"[vision] {msg}")
                if fps is not None:
                    frames_since = 0
                    last_report = now

            if args.frames and n >= args.frames:
                break
            if args.duration and (time.time() - t0) >= args.duration:
                break

        elapsed = time.time() - t0
        print(f"[vision] 结束：处理 {n} 帧，用时 {elapsed:.2f}s，平均 {n / max(elapsed, 1e-6):.1f} FPS")
        print("[vision] 帧源信息: " + json.dumps(src.info, ensure_ascii=False))

        if args.save_sheet and sheet:
            from .render import contact_sheet
            out = Path(args.save_sheet)
            _write_image(out, contact_sheet(sheet, cols=args.sheet_cols))
            print(f"[vision] 缩略图墙已保存: {out.resolve()}")
        return 0
    finally:
        if writer is not None:
            writer.release()
        pipeline.close()
        src.close()


def main(argv: Optional[list[str]] = None) -> int:
    return run(build_parser().parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main())
