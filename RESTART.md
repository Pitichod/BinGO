# BinGO restart

Start from the required waste-sorting behavior and the hardware actually available. The previous implementation remains at https://github.com/Pitichod/BinGO; this workspace starts independently. No remote changes have been made.

## What the existing implementation tells us

The inspected `main` branch has three Arduino sketches: camera, sensor node, and gateway. The node forwards camera JPEGs and ultrasonic distances over UART. The gateway calls Ultralytics, publishes to Blynk, and displays results. Its main sketch also contains optional photo serving, dashboard capture control, OTA integration, and a separate MPU6050 diagnostic mode.

The node sends a constant test weight of 42 g. The README describes the two-servo sorting mechanism and some gateway sensors as planned. These are not evidence of a completed sorter. Preserve useful wiring knowledge and verified external API behavior, but do not copy the existing structure by default.

## Requirements to preserve

Source: the supplied Embedded System (CEDT) Final Project 2026 brief, sections 2–5. This is assignment reference material, not authorization to submit work or operate external services.

- Five distinct sensor types total, at least two on the sensor node and two on the gateway. Repeated ultrasonic modules count as one type. Count sensors against the board they are physically connected to; do not assume a camera on a third board counts toward the main node's minimum.
- At least one physical actuator controlled by the system, with a safe stopping method.
- Sensor-node-to-gateway communication.
- Real AI output used in system decisions or actuator control.
- Raw sensor storage in the cloud and a dashboard; verify retention and readback rather than treating a live widget as proof of storage.
- Event/status notifications.
- Applicable electrical and mechanical safety requirements.
- Internet-accessible Git history with contributions from each member.

The brief lists November 20, 2026 at 23:59 for the end-to-end checkpoint video and December 8 for the final presentation. Smart bins also need the new angle, real user, or deployment site described in the proposal.

## Hardware allocation

| Board | Sensors | Outputs |
| --- | --- | --- |
| Sensor node | HC-SR04 ×2–4 for compartment clearance, HX711 + load cell for tray weight | MG996R ×2 for rotation and tilt |
| Gateway | ESP32-CAM + OV3660 camera module, BH1750, DHT11 | LCD |

Three HC-SR04 units are on hand. The design counts five sensor types, with two on the sensor node and three on the gateway. The camera assembly is one gateway peripheral in the design. Confirm its eligibility separately under the brief’s physical wiring rule. Even without the camera credit, both main boards each have two directly connected sensor types.

The node reports weight and compartment clearance. The gateway triggers its camera module and classifies the image, then sends the destination back to the node. The node verifies current clearance and controls both servos locally, including stopping on timeout. The gateway displays status and handles cloud uploads and notifications.

## UART wiring

This pin map assumes classic ESP32 DevKit boards and an AI-Thinker ESP32-CAM with GPIO13 and GPIO14 exposed. Use two independent 3.3 V UART links with the gateway in the middle.

| Transmitter | Receiver |
| --- | --- |
| Sensor node GPIO32 (TX) | Gateway GPIO33 (RX) |
| Gateway GPIO32 (TX) | Sensor node GPIO33 (RX) |
| Gateway GPIO26 (TX) | ESP32-CAM GPIO14 (RX) |
| ESP32-CAM GPIO13 (TX) | Gateway GPIO27 (RX) |

Connect GND from each board to gateway GND. Power each board from its own suitable input supply during the bench test. The UART wiring carries only TX, RX and common ground.

Use 115200 baud, 8N1 and no hardware flow control. Arduino `begin` takes RX before TX. Use `Serial2.begin(115200, SERIAL_8N1, 33, 32)` on both main boards for the node link, `Serial1.begin(115200, SERIAL_8N1, 27, 26)` on the gateway for the camera link, and `Serial1.begin(115200, SERIAL_8N1, 14, 13)` on the camera. Keep UART0 for flashing and logging. GPIO13 and GPIO14 share the ESP32-CAM microSD interface, so leave microSD disabled. [Espressif SDMMC pin map](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sdmmc_host.html)

Upload the three sketches under `arduino/` to their matching boards. Open the gateway Serial Monitor at 115200 baud. Send `n` for `Sensor node OK` and `c` for `Camera OK`. The gateway sends one byte (`?`), and the peer replies with `N` or `C`. Test each link separately, then connect both links and repeat. A missing reply prints `TIMEOUT` after one second.

## Wi-Fi and OTA

1. Copy `arduino/OtaSecrets.example.h` into each sketch folder as `OtaSecrets.h`. Set `wifiSsid`, `wifiPassword`, and a nonempty `otaPassword` in each copy. Git ignores the three local copies.
2. Flash each sketch over a cable once with an OTA-capable partition scheme. On the ESP32-CAM use **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** if available. Keep the chosen partition scheme for subsequent uploads.
3. Connect your computer and boards to the same local network using a 2.4 GHz Wi-Fi network for the boards. Check the serial log for `OTA READY` and an IP address. In Arduino IDE select the matching Network Port under **Tools → Port**: `bingo-gateway`, `bingo-sensor-node`, or `bingo-camera`. Upload and enter that board's OTA password when prompted.
4. Verify an upload and reboot for each board, then repeat `n` and `c` on the gateway. The UART test is available while Wi-Fi connects and when local credentials are absent.

## YOLO camera test

Copy `YoloSecrets.example.h` to `YoloSecrets.h` beside the gateway sketch and set the deployment API key there. Upload the updated camera and gateway sketches via their OTA ports. The sensor-node sketch is unchanged.

Open the gateway serial monitor at 115200, put an item in view, and send `y`. The first request may start clock sync. Wait a few seconds and send `y` again if prompted. The camera sends one VGA JPEG over UART and the gateway checks its length and CRC before uploading it over HTTPS. The serial monitor prints the response body and HTTP status. An HTTP 200 with model results verifies the path. Use one request at a time, and wait for it to finish before starting an OTA upload.

## Proposed lean scope

One item at a time: detect item, capture image, classify it, validate the destination, move the mechanism, return to idle. Invalid classification, stale sensor data, full destination, or a timeout must leave the system in a defined safe state.

Keep the existing boards and wiring only after confirming the actual inventory. Keep UART if the current wiring works. Avoid changing hardware and software architecture at the same time without a concrete benefit.

Start with one cloud integration and one notification route. Use Wi-Fi OTA for development as requested. Defer photo galleries, reverse proxies, remote capture controls, optional compartments, and terminal graphics. Keep hardware diagnostic sketches separate from production firmware.

Use simple functions and explicit state. Separate only the responsibilities that need independent timing or testing: sensor acquisition, message handling, sorter control, and blocking cloud work. Cloud delays must not prevent stop handling or local sensor processing. Add RTOS tasks for these concrete needs, not just to increase the task count.

Define one bounded message format with protocol version, node ID, sequence number, sensor validity, and integrity checking. Define framing, timeouts, and recovery before implementing it. Never substitute zero or a demonstration constant for an unavailable measurement.

## Implementation order and acceptance checks

1. Confirm boards, connected sensor models, pin assignments, motor types, and the intended compartments/classes. Reconcile the sensor allocation with the brief. Confirm the existing cloud/model services to retain.
2. Implement the smallest real telemetry path. A physical measurement must reach the gateway, persist in cloud storage, and appear on the dashboard. Disconnect a sensor and verify that invalid/stale data is visible.
3. Add image capture and classification with bounded transfers and explicit failures. Confirm a real API response before implementing its parser. Keep actuator outputs disabled during this stage.
4. Add the local sorting state machine after confirming the mechanism, calibration, and safe-stop behavior. Test unknown classes, full bins, timeout, duplicate commands, and restart during a cycle.
5. Add notifications and demonstrate recovery after Wi-Fi and power loss. Choose engineering-depth items from the proposal and rubric after the baseline works.
6. Collect actual measurements: packet delivery, latency, and AI accuracy are candidates. Keep methods and results separate; do not claim hardware validation from host-side tests.

## Decisions awaiting the owner

- Minimum compliant scope versus all existing intended features.
- Board variants, pin assignments, servo power, and mechanism details. All wiring starts fresh.
- Existing approved proposal: project angle and chosen engineering-depth items.

No replacement firmware has been written yet: hardware-dependent code before these decisions would repeat the speculative implementation this restart is intended to remove.
