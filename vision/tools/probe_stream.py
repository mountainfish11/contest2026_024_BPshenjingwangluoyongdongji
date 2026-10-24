"""逐层探测 ESP32 MJPEG 流，精确定位"读不到帧"卡在哪一步。

用法::

    python -m tools.probe_stream mjpeg:http://192.168.1.4/stream
    python -m tools.probe_stream 192.168.1.4          # 只给 IP 也行

探测思路
--------
ESP32 那套固件里 ``handleStream()`` 是个**阻塞循环**（``while (cl.connected())``），
它整段时间里 ``server.handleClient()`` 都不会被调用。所以：

- 有别的客户端正在拉流（比如浏览器开着页面）时，新连接 TCP 能连上，
  但**连 HTTP 响应头都收不到**——这和"IP 写错/端口没服务"表现完全不同。
- 而 ``/capture`` 是走 ``loop()`` 里 ``handleClient()`` 的同步接口。
  拿它交叉验证就能判断板子到底是不是卡在推流循环里。

于是分三种结果：

| /stream | /capture | 结论 |
|---|---|---|
| 有数据 | — | 流正常 |
| 无响应 | 也无响应 | **板子卡在推流循环里**，断电重上电 |
| 无响应 | 能出图 | 板子活着，是流请求本身的问题 |

最后一种情况还能顺手拿到一张快照存成文件，方便判断画面本身对不对。
"""
from __future__ import annotations

import argparse
import socket
import sys
import time
from typing import Dict, Optional, Tuple
from urllib.parse import urlparse

READ_WINDOW_S = 6.0
MAX_BYTES = 2 * 1024 * 1024
HEAD_TIMEOUT_S = 5.0


# --------------------------------------------------------------------------- #
# URL 归一化
# --------------------------------------------------------------------------- #
def normalize_url(target: str) -> str:
    """把 ``mjpeg:http://ip/stream`` / ``192.168.1.4`` / ``http://ip`` 统一成完整 URL。"""
    t = target.strip()
    low = t.lower()
    for prefix in ("mjpeg:", "http-stream:"):
        if low.startswith(prefix):
            t = t[len(prefix):]
            break

    if "://" not in t:
        t = "http://" + t

    parsed = urlparse(t)
    if parsed.scheme not in ("http", "https"):
        raise SystemExit(f"只支持 http/https 的 MJPEG 流，收到的是 {parsed.scheme!r}")
    if not parsed.hostname:
        raise SystemExit(f"解析不出主机名: {target}")
    if not parsed.path or parsed.path == "/":
        t = t.rstrip("/") + "/stream"
    return t


# --------------------------------------------------------------------------- #
# 单层探测
# --------------------------------------------------------------------------- #
class ProbeResult:
    def __init__(self) -> None:
        self.tcp_ok = False
        self.tcp_msg = ""
        self.status: Optional[int] = None
        self.headers: Dict[str, str] = {}
        self.head_bytes = 0          # 收到的响应头字节数（0 = 一个字节都没收到）
        self.body = b""
        self.elapsed = 0.0
        self.frames = 0

    def __repr__(self) -> str:
        return (f"ProbeResult(tcp_ok={self.tcp_ok}, status={self.status}, "
                f"head_bytes={self.head_bytes}, body={len(self.body)})")


def tcp_reachable(host: str, port: int, timeout: float = 4.0) -> Tuple[bool, str]:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True, f"TCP {host}:{port} 连接成功"
    except socket.timeout:
        return False, f"TCP {host}:{port} 连接超时（{timeout:.0f} 秒）"
    except ConnectionRefusedError:
        return False, f"TCP {host}:{port} 被拒绝（该端口没有服务在监听）"
    except OSError as exc:
        return False, f"TCP {host}:{port} 失败: {exc}"


def fetch(host: str, port: int, path: str,
          read_window: float = READ_WINDOW_S,
          max_frames: int = 40) -> ProbeResult:
    """发一个 GET，把"响应头"和"响应体"分开记录。"""
    res = ProbeResult()
    ok, msg = tcp_reachable(host, port)
    res.tcp_ok, res.tcp_msg = ok, msg
    if not ok:
        return res

    try:
        sock = socket.create_connection((host, port), timeout=HEAD_TIMEOUT_S)
    except OSError:
        return res

    try:
        req = (f"GET {path} HTTP/1.1\r\n"
               f"Host: {host}\r\n"
               f"Accept: multipart/x-mixed-replace, image/jpeg\r\n"
               f"Connection: close\r\n\r\n")
        sock.sendall(req.encode())

        # ---- 收响应头
        sock.settimeout(HEAD_TIMEOUT_S)
        head = b""
        deadline = time.time() + HEAD_TIMEOUT_S
        while b"\r\n\r\n" not in head and time.time() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                break
            except OSError:
                break
            if not chunk:
                break
            head += chunk
            if len(head) > 64 * 1024:
                break

        res.head_bytes = len(head)
        head_part, _, rest = head.partition(b"\r\n\r\n")
        text = head_part.decode("latin-1", "replace")
        lines = text.split("\r\n")
        if lines and lines[0].startswith("HTTP/"):
            parts = lines[0].split()
            if len(parts) >= 2 and parts[1].isdigit():
                res.status = int(parts[1])
        for line in lines[1:]:
            if ":" in line:
                k, v = line.split(":", 1)
                res.headers[k.strip().lower()] = v.strip()

        # ---- 收响应体
        body = rest
        t0 = time.time()
        deadline = t0 + read_window
        sock.settimeout(1.0)
        while time.time() < deadline and len(body) < MAX_BYTES:
            try:
                chunk = sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            body += chunk
            # 读够 max_frames 帧就停；否则读满窗口，用来测真实帧率
            if body.count(b"\xff\xd8") >= max_frames:
                break
        res.body = body
        res.elapsed = max(1e-6, time.time() - t0)
        res.frames = body.count(b"\xff\xd8")
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return res


# --------------------------------------------------------------------------- #
# 主流程
# --------------------------------------------------------------------------- #
def probe(target: str) -> int:
    url = normalize_url(target)
    parsed = urlparse(url)
    host = parsed.hostname or ""
    port = parsed.port or 80
    path = parsed.path or "/stream"

    print("=" * 64)
    print(f"探测目标: {url}")
    print("=" * 64)

    stream = fetch(host, port, path)
    print(f"[1/4] {stream.tcp_msg}")
    if not stream.tcp_ok:
        _print_network_help(host)
        return 2

    print(f"[2/4] 响应头: 收到 {stream.head_bytes} 字节"
          + (f"，状态 {stream.status}" if stream.status else "（一个字节都没收到）"))
    if stream.headers:
        for key in ("content-type", "content-length", "connection", "server"):
            if key in stream.headers:
                print(f"        {key}: {stream.headers[key]}")

    kbps = len(stream.body) / stream.elapsed / 1024
    fps = stream.frames / stream.elapsed
    per_frame = (len(stream.body) / stream.frames / 1024) if stream.frames else 0
    print(f"[3/4] 响应体: {stream.elapsed:.1f} 秒内收到 {len(stream.body)} 字节"
          f" / {stream.frames} 帧")
    print(f"       实测: {fps:.1f} fps   {kbps:.0f} KB/s   平均 {per_frame:.1f} KB/帧")
    print("[4/4] 结论:")
    print()

    ok = _judge_stream(stream, host, port, path)
    return 0 if ok else 2


def _judge_stream(s: ProbeResult, host: str, port: int, path: str) -> bool:
    ctype = s.headers.get("content-type", "").lower()
    n = len(s.body)

    # ---------- 情况 1：一切正常
    if s.status == 200 and "multipart" in ctype and n > 0 and b"\xff\xd8" in s.body:
        jpegs = s.body.count(b"\xff\xd8")
        print(f"  ✅ 流正常：{n} 字节，{jpegs} 个 JPEG 分片。")
        if s.frames >= 3:
            fps = s.frames / s.elapsed
            if fps < 12:
                print()
                print(f"  ⚠️ 但帧率只有 {fps:.1f} fps —— 这是**板子/网络侧**的问题，不是识别慢:")
                print("     · WiFi 信号弱：把电脑靠近板子；AP 模式下板子天线就在板子上")
                print("     · 2.4GHz 拥挤：换信道，或把板子的 AP 信道固定（1/6/11）")
                print("     · 单帧太大：固件里把 jpeg_quality 从 10 调到 12")
                print("     · 板子同时被别的设备访问（手机开着这个页面也算）")
            else:
                print(f"     帧率 {fps:.1f} fps —— 采集端没问题，慢的是识别端")
        _report_frame_size(s.body)
        print()
        print("  下一步：")
        print(f"    python -m tools.calibrate --source "
              f"mjpeg:http://{host}:{port}{path} --height 1.2 --pitch 30")
        return True

    # ---------- 情况 2：TCP 通，但连 HTTP 头都没收到
    if s.head_bytes == 0:
        print("  ❌ TCP 连上了，但服务器**一个字节都没回**。")
        print()
        print("  这不是 IP 写错（端口上确实有东西在监听）。ESP32 那套固件里")
        print("  handleStream() 是阻塞循环，正在给别的客户端推流时，新连接会被挂起、")
        print("  连响应头都不发。所以基本可以确定：**流已经被别人占住了**。")
        print()
        print("  现在做交叉验证：试一下 /capture（它走 loop() 里的同步接口）")
        cap = fetch(host, port, "/capture", read_window=5.0)
        print(f"    /capture -> 响应头 {cap.head_bytes} 字节, "
              f"状态 {cap.status}, 响应体 {len(cap.body)} 字节")

        if cap.status == 200 and cap.body.startswith(b"\xff\xd8"):
            saved = _save_snapshot(cap.body)
            print()
            print("  ✅ 板子活着、相机也正常（/capture 能出图"
                  + (f"，已存到 {saved}" if saved else "") + "）。")
            print("     => 是 /stream 被占用了。按顺序试：")
            print("        1) 关掉所有开着 http://" + host + "/ 的浏览器标签页")
            print("        2) 关掉之前没退干净的视觉服务 / 标定窗口（含别的终端）")
            print("        3) 手机上如果开着板子页面，也关掉")
            print("        4) 还不行就拔电重上电，再立刻重试（最快）")
        elif cap.head_bytes == 0:
            print()
            print("  ❌ /capture 也没响应 —— 板子**卡在推流循环里**了（旧的 TCP 连接还没超时）。")
            print("     => 直接给板子断电，重新上电，然后立刻重试。")
        else:
            print()
            print("  ⚠️ /capture 有响应但不是 JPEG（状态 "
                  f"{cap.status}）。先确认刷的是图传固件。")
        return False

    # ---------- 情况 3：有响应头但状态不对
    if s.status != 200:
        print(f"  ❌ HTTP 返回 {s.status}，不是 200。")
        print("     -> 路径写错（流地址是 /stream），或者刷的根本不是图传固件。")
        return False

    # ---------- 情况 4：状态 200 但 Content-Type 不是 multipart
    if "multipart" not in ctype:
        print(f"  ❌ 返回 200，但 Content-Type 是 {ctype or '(空)'}，不是 MJPEG 流。")
        print("     -> 你请求的路径不是 /stream。/capture 只返回单张 JPEG。")
        return False

    # ---------- 情况 5：200 + multipart，但没数据
    if n == 0:
        print("  ❌ 响应头完全正确，但响应体是空的（相机没出图 / 固件在送空帧）。")
        print("     -> 回串口看有没有 'camera OK PSRAM'，以及是否在反复重启。")
        return False

    # ---------- 情况 6：有数据但没有 JPEG
    head = s.body[:120].replace(b"\r\n", b"|")
    print(f"  ❌ 收到 {n} 字节，但里面没有 JPEG 起始标记 FFD8。")
    print(f"     开头内容: {head!r}")
    print("     -> 分片格式异常。若刷了自己改的固件，检查 handleStream() 的包头。")
    return False


def _path_url(url: str) -> str:
    """从完整 URL 里取出 path?query。"""
    p = urlparse(url)
    path = p.path or "/stream"
    if p.query:
        path += "?" + p.query
    return path


def _report_frame_size(body: bytes) -> None:
    try:
        import cv2
        import numpy as np

        idx = body.find(b"\xff\xd8")
        if idx < 0:
            return
        img = cv2.imdecode(np.frombuffer(body[idx:], np.uint8), cv2.IMREAD_COLOR)
        if img is None:
            return
        h, w = img.shape[:2]
        print(f"       首帧尺寸: {w}x{h}", end="")
        if (w, h) == (320, 240):
            print("   <== 还是 QVGA！固件没升到 VGA，红绿灯认不出来")
        else:
            print("   ✅")
    except Exception:  # noqa: BLE001
        pass


def _save_snapshot(body: bytes) -> Optional[str]:
    try:
        from pathlib import Path

        out = Path("out") / "probe_snapshot.jpg"
        out.parent.mkdir(parents=True, exist_ok=True)
        idx = body.find(b"\xff\xd8")
        eoi = body.rfind(b"\xff\xd9")
        data = body[idx:eoi + 2] if idx >= 0 and eoi > idx else body
        out.write_bytes(data)
        return str(out)
    except Exception:  # noqa: BLE001
        return None


def _print_network_help(host: str) -> None:
    print()
    print("结论：网络层就不通，先解决连通性。")
    print("  排查顺序：")
    print("   1) 电脑和板子是否连**同一个** WiFi/热点？手机连 4G 不算")
    print("   2) IP 是否变了？回串口看最新的 'WiFi OK: x.x.x.x'")
    print(f"   3) 能 ping 通吗:  ping {host}")
    print("   4) 手机热点可能开了「客户端隔离 / AP 隔离」，关掉它")
    print("   5) 电脑防火墙有没有拦出站（少见）")


def main(argv: Optional[list] = None) -> int:
    ap = argparse.ArgumentParser(description="ESP32 MJPEG 流逐层探测")
    ap.add_argument("target", help="mjpeg:http://ip/stream 或直接写 IP")
    args = ap.parse_args(argv)
    return probe(args.target)


if __name__ == "__main__":
    sys.exit(main())
