import socket
import threading
import unittest
import zlib
from unittest import mock

from tools.speech_bridge import (
    SpeechBridge,
    encode_speech_frame,
    parse_ack,
    send_speech,
)


class SpeechBridgeProtocolTests(unittest.TestCase):
    def test_frame_preserves_chinese_utf8_and_crc(self):
        frame = encode_speech_frame(42, 90, "前方有障碍物")
        header, payload = frame.split(b"\n", 1)
        fields = header.decode("ascii").split()
        self.assertEqual(fields[:5], ["BBSP/1", "SPEAK", "42", "90", str(len(payload))])
        self.assertEqual(int(fields[5], 16), zlib.crc32(payload) & 0xFFFFFFFF)
        self.assertEqual(payload.decode("utf-8"), "前方有障碍物")

    def test_parse_ack_checks_sequence(self):
        self.assertEqual(parse_ack(b"BBSP/1 ACK 7 QUEUED\n", 7), "QUEUED")
        with self.assertRaises(ValueError):
            parse_ack(b"BBSP/1 ACK 8 QUEUED\n", 7)

    def test_send_speech_accepts_duplicate_as_protocol_result(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]

        def server():
            connection, _ = listener.accept()
            with connection:
                data = connection.recv(1024)
                self.assertIn("测试".encode(), data)
                connection.sendall(b"BBSP/1 ACK 99 DUPLICATE\n")
            listener.close()

        thread = threading.Thread(target=server)
        thread.start()
        self.assertEqual(
            send_speech("127.0.0.1", port, 99, 50, "测试"), "DUPLICATE"
        )
        thread.join(timeout=2)
        self.assertFalse(thread.is_alive())

    @mock.patch("tools.speech_bridge.fetch_events")
    @mock.patch("tools.speech_bridge.send_speech")
    def test_retry_reuses_sequence_to_prevent_duplicate_playback(
        self, send_mock, fetch_mock
    ):
        event = {
            "ts": 123.0,
            "kind": "speech",
            "event": "obstacle",
            "text": "前方有障碍物",
            "priority": 90,
        }
        fetch_mock.return_value = [event]
        send_mock.side_effect = [TimeoutError("ACK lost"), "DUPLICATE"]
        bridge = SpeechBridge("http://vision", "192.0.2.1", replay_existing=True)

        with self.assertRaises(TimeoutError):
            bridge.poll_once()
        self.assertEqual(bridge.poll_once(), 1)
        first_sequence = send_mock.call_args_list[0].args[2]
        second_sequence = send_mock.call_args_list[1].args[2]
        self.assertEqual(first_sequence, second_sequence)


if __name__ == "__main__":
    unittest.main()
