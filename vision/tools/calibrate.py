"""标定工具：把相机标定参数调到与实际情况吻合。

原理
----
把地面上"前方 1/2/3/5 米"的横线和"横向 ±0.5/1.0/1.5 米"的纵线按当前标定参数
投影回画面。参数正确时，画面里的线应与实际地面位置对齐。

用法
----
交互调参（需要桌面环境）::

    python -m tools.calibrate --source mjpeg:http://192.168.1.20/stream

无界面导出图片（任何环境都行）::

    python -m tools.calibrate --source file:data/Test.mp4 \
        --frame 0 --pitch 85 --height 1.3 --save calib.jpg

交互模式按键：``s`` 打印/保存配置，``q`` 退出。
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path
from typing import Optional

import cv2
import numpy as np

from vision_assist.config import AppConfig, CameraCalib
from vision_assist.geometry import CameraModel
from vision_assist.render import draw_ground_grid
from vision_assist.sources import open_source

def draw_ground_overlay(frame: np.ndarray, model: CameraModel) -> np.ndarray:
    """在画面上叠加地面距离网格 + 参数文字。

    网格绘制复用 :func:`vision_assist.render.draw_ground_grid`，保证标定工具和
    浏览器调试页画出来的是同一套线——不然两边对不上会让人怀疑人生。
    """
    img = draw_ground_grid(frame.copy(), model)

    info = model.describe()
    lines = [
        f"h={info['height_m']:.2f}m  pitch={info['pitch_deg']:.1f}deg  f={info['focal_px']:.1f}px",
        f"frame={info['frame'][0]}x{info['frame'][1]}  horizon_v={info['horizon_v']}",
        f"visible Z={info['visible_ground_z_m'][0]}..{info['visible_ground_z_m'][1]}m"
        f"  m/px@2m={info['meters_per_px_at_2m']}",
    ]
    for i, text in enumerate(lines):
        cv2.putText(img, text, (8, 22 + i * 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (0, 0, 0), 3, cv2.LINE_AA)
        cv2.putText(img, text, (8, 22 + i * 22), cv2.FONT_HERSHEY_SIMPLEX, 0.55,
                    (255, 255, 255), 1, cv2.LINE_AA)
    return img


def _grab_frame(source_spec: str, frame_index: int = 0,
                timeout_s: float = 15.0) -> np.ndarray:
    """取一帧，最多等 ``timeout_s`` 秒。

    MJPEG 流的首帧往往要等几百毫秒到几秒（连 TCP、等分片、等相机出图），
    只读一次就放弃会误报"读不到帧"。
    """
    src = open_source(source_spec)
    try:
        needed = max(1, frame_index + 1)
        frame = None
        got = 0
        deadline = time.time() + timeout_s
        last_report = 0.0
        while got < needed and time.time() < deadline:
            f = src.read()
            if f is not None:
                frame = f
                got += 1
                continue
            now = time.time()
            if now - last_report >= 3.0:
                last_report = now
                print(f"[calib] 还在等帧…已等 {timeout_s - (deadline - now):.0f}s", flush=True)
            time.sleep(1 / 30)

        if frame is None:
            info = src.info
            detail = info.get("last_error") or "无额外错误信息"
            raise RuntimeError(
                f"从 {source_spec} 在 {timeout_s:.0f} 秒内读不到任何帧。\n"
                f"  帧源信息: {info}\n"
                f"  最后错误: {detail}\n"
                f"  排查请先跑: python -m tools.probe_stream {source_spec}"
            )
        return frame
    finally:
        src.close()


def _browser_calib_help(source: str) -> None:
    """GUI 起不来时，指向浏览器标定（更稳，不依赖 OpenCV 窗口）。"""
    print()
    print("=" * 62)
    print("建议改用浏览器标定（不受 OpenCV 窗口问题影响）：")
    print()
    print("  1) 起服务：")
    print(f"       python -m vision_assist.cli --serve --source {source} "
          "--height 1.2 --pitch 30")
    print("  2) 浏览器打开 http://127.0.0.1:8760/")
    print("  3) 画面上的绿线 = 地面 1/2/3/5 米，蓝线 = 正前方中线")
    print("  4) 改页面下方『标定』里的离地高度 / 俯仰角，点『应用』，看线怎么动")
    print("  5) 对齐后点『保存为 calibration.json』")
    print("=" * 62)


def _run_gui(frame: np.ndarray, args: argparse.Namespace) -> bool:
    """交互调参。成功跑完返回 True；OpenCV 窗口出问题返回 False。"""
    win = "calibrate"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    cv2.createTrackbar("pitch x0.1", win, int(args.pitch * 10), 890, lambda _v: None)
    cv2.createTrackbar("height cm", win, int(args.height * 100), 300, lambda _v: None)
    cv2.createTrackbar("hfov deg", win, int(args.hfov), 150, lambda _v: None)

    print("[calib] 拖动滑块使地面网格与实景对齐；s=保存配置，q=退出")
    while True:
        pitch = max(1.0, cv2.getTrackbarPos("pitch x0.1", win) / 10.0)
        height = max(0.1, cv2.getTrackbarPos("height cm", win) / 100.0)
        hfov = max(10, cv2.getTrackbarPos("hfov deg", win))
        cur = CameraCalib(height_m=height, pitch_deg=pitch, hfov_deg=hfov)
        model = CameraModel(cur, frame.shape[1], frame.shape[0])
        cv2.imshow(win, draw_ground_overlay(frame, model))
        key = cv2.waitKey(30) & 0xFF
        if key == ord("q"):
            break
        if key == ord("s"):
            cfg = AppConfig(camera=cur)
            path = args.save_config or "calibration.json"
            cfg.save(path)
            print(f"[calib] 已保存: {path}")
            print(json.dumps(model.describe(), ensure_ascii=False, indent=2))
    cv2.destroyAllWindows()
    return True


def main(argv: Optional[list[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="相机地面标定工具")
    ap.add_argument("--source", default="file:data/Test.mp4",
                    help="帧源：mjpeg:http://ip/stream / file:xxx.mp4 / usb:0")
    ap.add_argument("--frame", type=int, default=0, help="取第几帧（仅对文件源有效）")
    ap.add_argument("--height", type=float, default=1.20, help="相机离地高度（米）")
    ap.add_argument("--pitch", type=float, default=30.0, help="俯仰角（度，正=朝下）")
    ap.add_argument("--hfov", type=float, default=65.0, help="水平视场角（度）")
    ap.add_argument("--save", default=None, help="不弹窗，直接保存标注图到该路径")
    ap.add_argument("--save-config", default=None, help="把当前标定写成 JSON 配置文件")
    ap.add_argument("--no-gui", action="store_true", help="强制不弹窗")
    args = ap.parse_args(argv)

    frame = _grab_frame(args.source, args.frame)
    calib = CameraCalib(height_m=args.height, pitch_deg=args.pitch, hfov_deg=args.hfov)

    # ---------------- 交互模式（失败则自动退回无界面） ----------------
    if (not args.no_gui) and args.save is None:
        try:
            return 0 if _run_gui(frame, args) else 0
        except cv2.error as exc:
            msg = str(exc).splitlines()[-1] if str(exc) else repr(exc)
            print(f"[calib] OpenCV 窗口不可用: {msg}")
            try:
                cv2.destroyAllWindows()
            except cv2.error:
                pass
            _browser_calib_help(args.source)
            print()
            print("[calib] 继续以无界面模式输出当前参数的真实几何描述……")
        except KeyboardInterrupt:
            print("\n[calib] 已取消")
            return 130

    # ---------------- 无界面模式 ----------------
    model = CameraModel(calib, frame.shape[1], frame.shape[0])
    img = draw_ground_overlay(frame, model)
    if args.save:
        out = Path(args.save)
        out.parent.mkdir(parents=True, exist_ok=True)
        # cv2.imwrite 不支持中文路径，用 imencode
        ok, buf = cv2.imencode(out.suffix or ".jpg", img)
        if not ok:
            print(f"[calib] 编码失败: {out}")
            return 1
        out.write_bytes(buf.tobytes())
        print(f"[calib] 已保存: {out.resolve()}")
    print(json.dumps(model.describe(), ensure_ascii=False, indent=2))
    if args.save_config:
        cfg = AppConfig(camera=calib)
        cfg.save(args.save_config)
        print(f"[calib] 配置已写入: {args.save_config}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
