# BlindBadge vision integration

## Current architecture

The ESP32-S3-EYE is the camera and Wi-Fi access point. It captures native
640x480 JPEG frames from `/dev/video0` and exposes them over HTTP. A Windows
computer connects directly to the board access point, pulls the MJPEG stream,
and runs the delivered `vision_assist` PyTorch/Ultralytics pipeline. The model
files remain on Windows and are not linked into the OpenVela firmware.

The current firmware uses Wi-Fi in SoftAP-only mode:

| Interface | Purpose | Address |
|---|---|---|
| `wlan0` | Windows vision link (SoftAP) | `192.168.4.1/24` |

STA and APSTA are disabled. The board does not join `rtthread` and does not
start the sub-board TCP link while this visual-link firmware is running.

## Board service

The firmware starts the vision service and open development SoftAP
automatically at boot:

```text
SSID: BlindBadge-Vision
Board address: 192.168.4.1
Health:  http://192.168.4.1/health
JPEG:    http://192.168.4.1/capture
MJPEG:   http://192.168.4.1/stream
```

The following NSH commands are available for diagnosis:

```text
blind_badge_app vision_server_status
blind_badge_app vision_server_stop
blind_badge_app vision_server 80
blind_badge_app vision_camera_test
```

Each `/stream` connection is closed after 20 seconds so a disconnected client
cannot hold the single-client server forever. The host frame source reconnects
automatically.

## Start on Windows

1. Connect Windows Wi-Fi to the open `BlindBadge-Vision` network. Windows may
   report that the network has no Internet; stay connected.
2. Open PowerShell or Command Prompt in the `vision` directory.
3. Install dependencies once with `python -m pip install -r requirements.txt`.
4. Check acceleration with `python -m vision_assist.cli --check-gpu`.
5. Run `verify_board_windows.bat`. It writes the offline result to
   `Desktop\blindbadge_stream_test.txt` and one JPEG to
   `Desktop\blindbadge_capture.jpg`.
6. Run `start_windows.bat`, then open `http://127.0.0.1:8760/`.

Windows may automatically return to a remembered Internet Wi-Fi after it
detects that the board AP has no Internet access. Choose to stay connected to
`BlindBadge-Vision` and temporarily disable automatic connection on the other
Wi-Fi profile while running the visual pipeline.

The equivalent manual command is:

```bat
python -m vision_assist.cli --serve ^
  --source mjpeg:http://192.168.4.1/stream ^
  --host 127.0.0.1 --port 8760 --device auto ^
  --target-fps 15 --height 1.2 --pitch 30
```

The height and pitch above are startup placeholders. Calibrate the mounted
camera before treating distances in metres as valid.

## Validation gates

Run these checks in order; a successful firmware build alone does not prove
the camera chain:

1. `wlan0` is `192.168.4.1/24` in AP mode, DHCP is running, and the
   `bb_vision` task is listening on port 80.
2. Windows receives a `192.168.4.x` address and `/health` returns JSON.
3. `/capture` is a decodable 640x480 JPEG with `FFD8`/`FFD9` markers.
4. `python -m tools.probe_stream mjpeg:http://192.168.4.1/stream` receives
   multiple frames without decode errors.
5. The local debug page displays the annotated image and `/api/scene` returns
   current inference output.

The older `vision_once`, `vision_start`, `vision_status`, and `vision_stop`
commands implement the first RGB565 push prototype. They remain in the tree
for comparison but are not the selected Windows pull architecture.

## Remaining integration

The current path carries camera frames from the board to Windows and performs
host-side inference. Returning compact alert results to the board state
machine is the next protocol step and should be added only after the five
stream validation gates pass on real hardware.
