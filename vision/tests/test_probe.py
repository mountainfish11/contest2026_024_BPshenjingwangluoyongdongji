"""流探测工具的测试。

重点是 URL 归一化——用户会直接把 ``--source`` 的写法（带 ``mjpeg:`` 前缀）粘进来，
前缀没剥掉会导致 host 为空、误报"端口没开服务"。
"""
from __future__ import annotations

import unittest

from tools.probe_stream import ProbeResult, _judge_stream, normalize_url


class TestNormalizeUrl(unittest.TestCase):
    def test_strips_mjpeg_prefix(self):
        self.assertEqual(normalize_url("mjpeg:http://192.168.1.4/stream"),
                         "http://192.168.1.4/stream")

    def test_bare_ip_gets_stream_path(self):
        self.assertEqual(normalize_url("192.168.1.4"), "http://192.168.1.4/stream")

    def test_root_path_becomes_stream(self):
        self.assertEqual(normalize_url("http://192.168.1.4"), "http://192.168.1.4/stream")
        self.assertEqual(normalize_url("http://192.168.1.4/"), "http://192.168.1.4/stream")

    def test_keeps_explicit_port_and_path(self):
        self.assertEqual(normalize_url("192.168.1.4:8080/stream"),
                         "http://192.168.1.4:8080/stream")

    def test_keeps_query_string(self):
        self.assertEqual(normalize_url("http://ip/stream?overlay=0"),
                         "http://ip/stream?overlay=0")

    def test_rejects_non_http(self):
        with self.assertRaises(SystemExit):
            normalize_url("rtsp://192.168.1.4/stream")


def make_result(status=None, ctype="", body=b"", head_bytes=100) -> ProbeResult:
    r = ProbeResult()
    r.tcp_ok = True
    r.head_bytes = head_bytes
    r.status = status
    r.body = body
    if ctype:
        r.headers["content-type"] = ctype
    return r


JPEG = b"\xff\xd8" + b"\x00" * 200 + b"\xff\xd9"
MULTIPART = "multipart/x-mixed-replace; boundary=f"


class TestJudgeStream(unittest.TestCase):
    def test_good_stream_passes(self):
        r = make_result(200, MULTIPART, JPEG * 2)
        self.assertTrue(_judge_stream(r, "1.2.3.4", 80, "/stream"))

    def test_non_200_fails(self):
        r = make_result(404, "text/plain", b"not found")
        self.assertFalse(_judge_stream(r, "1.2.3.4", 80, "/stream"))

    def test_html_response_fails(self):
        """访问 / 会返回网页，不是流；要明确指出路径不对。"""
        r = make_result(200, "text/html; charset=utf-8", b"<html>")
        self.assertFalse(_judge_stream(r, "1.2.3.4", 80, "/"))

    def test_empty_body_fails(self):
        r = make_result(200, MULTIPART, b"")
        self.assertFalse(_judge_stream(r, "1.2.3.4", 80, "/stream"))

    def test_body_without_jpeg_fails(self):
        r = make_result(200, MULTIPART, b"some garbage but no jpeg marker")
        self.assertFalse(_judge_stream(r, "1.2.3.4", 80, "/stream"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
