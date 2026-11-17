# BinGO restart

Wiring, calibration, and course requirements for the firmware on `codex/bingo-restart`. See the [README](README.md) for setup and a quick test.

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

The node reports weight and compartment clearance. The gateway triggers its camera module and classifies the image, then sends the destination back to the node. The node controls both servos and checks the tray weight after dumping. The gateway shows local readings and alerts on its LCD and Serial Monitor. Cloud telemetry and remote notifications are pending.

## Camera UART wiring

This pin map assumes a classic ESP32 DevKit gateway and an AI-Thinker ESP32-CAM with GPIO13 and GPIO14 exposed. Use 3.3 V UART.

| Transmitter | Receiver |
| --- | --- |
| Gateway GPIO26 (TX) | ESP32-CAM GPIO14 (RX) |
| ESP32-CAM GPIO13 (TX) | Gateway GPIO27 (RX) |

Connect ESP32-CAM GND to gateway GND. Power each board from its own suitable input supply during the bench test. Disconnect the old GPIO32/GPIO33 UART wires between the sensor node and gateway.

Use 115200 baud, 8N1 and no hardware flow control. Arduino `begin` takes RX before TX. Use `Serial1.begin(115200, SERIAL_8N1, 27, 26)` on the gateway and `Serial1.begin(115200, SERIAL_8N1, 14, 13)` on the camera. Keep UART0 for flashing and logging. GPIO13 and GPIO14 share the ESP32-CAM microSD interface, so leave microSD disabled. [Espressif SDMMC pin map](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/peripherals/sdmmc_host.html)

Send `c` from the gateway Serial Monitor at 115200 baud to verify `Camera OK`.

## Sensor node ESP-NOW

1. Get the Wi-Fi station MAC for each main board from your router's client list or upload both updated sketches and read their `ESP-NOW STA MAC` lines at 115200.
2. Copy `arduino/EspNowPeer.example.h` to `EspNowPeer.h` in each of the two sketch folders. Put the sensor node's MAC in the gateway copy and the gateway's MAC in the sensor node copy. For a MAC written as `AA:BB:CC:DD:EE:FF`, use `{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF}`.
3. Upload both sketches via OTA. Both boards must associate to a 2.4 GHz network on the same channel. Compare the channel printed in each boot log and check for `ESP-NOW init: ESP_OK`. The firmware uses the current Wi-Fi channel so OTA and the gateway's HTTPS request can share the radio.
4. Send `n` from the gateway Serial Monitor. Expect `Sensor node OK (ESP-NOW)` followed by the enabled bin distances. A missing reply prints `Sensor node ESP-NOW TIMEOUT` after one second.

## HC-SR04 on the sensor node

| Bin | TRIG | ECHO through divider | Status |
| --- | --- | --- | --- |
| 1 | GPIO18 | GPIO34 | Enabled |
| 2 | GPIO19 | GPIO35 | Enabled |
| 3 | GPIO21 | GPIO36 (VP) | Reserved |
| 4 | GPIO22 | GPIO39 (VN) | Reserved |

Power both modules from 5 V with a common ground to the sensor node. Give each ECHO its own divider. Connect ECHO through 2.2 kΩ to the ESP32 input and connect 3.3 kΩ from that input to GND. This reduces the 5 V ECHO to about 3.0 V. TRIG connects directly. Leave the reserved channels unwired until needed.

Upload both the gateway and sensor-node sketches together since the reply packet has changed. Send `n` on the gateway at 115200, then move a flat target in front of each sensor separately and repeat. No echo or a reading outside 2–400 cm prints `invalid echo`. To enable more distance readings, wire their reserved pins and increase `distanceSensorCount` on the sensor node to 3 or 4. The gateway already accepts four slots.

## MG996R bench test on the sensor node

Mark bin 1 as 0° on the rim. Viewed from above, the intended bearings are 0°, 90°, 180°, and 270° clockwise. Bins 1 and 3 share one rotation axis and bins 2 and 4 share the other. The tilt servo selects opposite ends of each axis.

Send `1` through `4` on the sensor node's USB Serial Monitor for a manual dump cycle. Start with an empty tray and check clearance. The tray levels, rotates into alignment, tips to the chosen side, then levels again before rotating home to the 0° axis. Both servos keep receiving PWM until the home move has had time to settle. The commands also exercise positions 3 and 4 before their distance sensors are installed.

Edit the calibration block near the top of the sensor-node sketch. `rotateAxisDeg` sets both alignments, with the first also used for Home. `tiltLevelDeg` sets tray level, and `dumpTiltDeg` sets how far the servo moves from level in both directions. `rotateOffsetDeg` and `tiltOffsetDeg` shift all commands for their respective servo. `testAnglesDeg` sets the two bench-test positions. All settings use commanded servo degrees, including decimals. Effective commands must be within 0–180°. Test with an empty tray and verify actual angles on the mechanism.

Connect the rotation servo signal to GPIO25 and the tilt servo signal to GPIO26. Connect both servo power leads to a separate regulated 5–6 V supply rated for the servos' combined current. Connect both servo grounds, the supply negative, and sensor-node GND together. Keep servo power off the ESP32 supply pins.

Assuming standard positional MG996R units, take the horns or linkage off the mechanism for the first test. Update only the sensor node. Open its USB Serial Monitor at 115200. Send `r` and `R` to test the rotation servo, and `t` and `T` to test the tilt servo. Lowercase uses 72° and uppercase uses 108° before offsets, as set in `testAnglesDeg`. The bench test drives one servo at a time and stops generating PWM after 600 ms. Send `x` to stop PWM earlier. This does not cut servo power or guarantee a mechanical stop.

## HX711 and 5 kg load cell

Install the **HX711 Arduino Library** by Bogdan Necula (bogde) in Arduino IDE. For a breakout with a single VCC pin, start with VCC to sensor-node 3V3, GND to GND, DT or DOUT to GPIO32, and SCK or CLK to GPIO33. Connect the four-wire load cell to E+, E−, A+, and A− using its model's color chart. Wire colors vary. Mount the cell so the tray's load passes through the measuring end without the tray rubbing against the frame.

The integration uses HX711 gain 128 and reads the factor from the standalone sketch's `hx711-scale` / `factor` preference. If that entry is unavailable, it uses the recorded reference factor of −404.067871 counts/g. For the integrated build, use GPIO32/33 because the distance sensors use GPIO18/19. Keep the same HX711 supply voltage, load cell, and gain when reusing the factor.

Upload the sensor node and gateway because the ESP-NOW reply includes weight in grams. At each boot, keep the tray empty until the sensor node prints `HX711: automatic tare complete`. The node waits one second, then averages eight readings to set zero. Use `z` at 115200 to set zero again if needed. Place the 1000 g reference, wait a second, and press `w` to verify the reading. If recalibration is needed, place the 1000 g reference after taring and send `k` to save a new factor. Send `n` to the gateway to read weight and bin distances. It reports weight as unavailable during servo motion, before taring, or while fresh samples are missing.

## Dump verification and retries

After each dump, the sensor node returns Home and checks for a stable empty tray. If weight remains, it repeats the same bin cycle up to `maxDumpRetries` extra times, initially two. If weight still remains, the gateway shows an alert on its LCD and logs it in Serial. Clearing the tray clears the stuck-item status. If fresh weight cannot be verified after `dumpVerifyMs`, the node stops and reports a scale warning. Update both ESP32 sketches together for the new status field.

## Wi-Fi and OTA

1. Copy `arduino/OtaSecrets.example.h` into each sketch folder as `OtaSecrets.h`. Set `wifiSsid`, `wifiPassword`, and a nonempty `otaPassword` in each copy. Git ignores the three local copies.
2. Flash each sketch over a cable once with an OTA-capable partition scheme. On the ESP32-CAM use **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** if available. Keep the chosen partition scheme for subsequent uploads.
3. Connect your computer and boards to the same local network using a 2.4 GHz Wi-Fi network for the boards. Check the serial log for `OTA READY` and an IP address. In Arduino IDE select the matching Network Port under **Tools → Port**: `bingo-gateway`, `bingo-sensor-node`, or `bingo-camera`. Upload and enter that board's OTA password when prompted.
4. Verify an upload and reboot for each board. Send `n` after the two ESP-NOW peers initialize and `c` to verify camera UART.

## Gateway LCD1602

Use an LCD1602 with a PCF8574 I²C backpack. Install **LiquidCrystal_I2C** by Martin Kubovčík and Frank de Brabander in Arduino IDE. The gateway uses GPIO21 for SDA and GPIO22 for SCL.

For a 5 V display, put a bidirectional I²C level shifter between the gateway and backpack. Connect LV to 3V3 and HV to 5 V, connect grounds together, and place the gateway SDA/SCL on the LV side and the backpack SDA/SCL on the HV side. Power the backpack from 5 V. A display and backpack explicitly rated for 3.3 V can instead be powered at 3.3 V and connected directly.

Upload only the gateway for display changes. Serial at 115200 reports the detected address (0x27 or 0x3F). The home screen shows Bingo centered with a status below it. The gateway checks the sensor node about once a second. Send `n` to display weight and the first two distances in rounded centimeters. After inference the display shows the top YOLO class and returns home after 10 seconds. If the address is found but the text is blank, adjust the contrast on the backpack.

## Gateway temperature, humidity and light

Connect a 3.3 V-compatible DHT11 module to 3V3, GND and DATA on GPIO23. If using a bare sensor without a pull-up, add 4.7–10 kΩ from DATA to 3V3. Check the sensor's pin labels or pinout before connecting.

Connect the BH1750FVI breakout to 3V3, GND, GPIO21 (SDA) and GPIO22 (SCL). Tie ADDR to GND for address 0x23 if the breakout exposes it. The display shares the I²C bus. Keep the BH1750 on the 3.3 V side of the level shifter when the LCD uses 5 V.

Install **DHT sensor library** by Adafruit and its **Adafruit Unified Sensor** dependency. BH1750 uses the existing Wire library. Upload only the gateway. After startup, send `e` at 115200 to see temperature, relative humidity and lux on Serial and LCD. Cover the BH1750 and try again after three seconds to check the light reading.

The default Gateway OTA partition allows 1,310,720 bytes per app. If the future Blynk build exceeds this limit on a 4 MB ESP32 Dev Module, **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)** allows 1,966,080 bytes per app. Changing the partition layout requires a USB upload. Use that same layout for subsequent OTA uploads.

## Automatic capture from weight

The sensor node reports a new placement when the tray reads at least 10 g for 1.2 seconds and the highest and lowest samples in that window differ by at most 4 g. It re-arms after the tray reads 5 g or less for 400 ms. During servo motion or missing samples, the settling timer resets. The gateway runs YOLO once per observed placement once Wi-Fi and the TLS clock are ready. `y` is still available for an explicit manual capture. Tune the thresholds in `arduino/BinGoSensorNode/ItemTrigger.h`.

Upload the gateway and sensor node together for the new ESP-NOW reply. Leave the tray empty for automatic tare, then put an item over 10 g on the tray and let it settle until `YOLO auto: stable item` appears. Try a moving target and a brief tap, then remove the item and place it again. Use an unmapped class or disconnect servo power during initial camera-only tests.

## YOLO camera test

Copy `YoloSecrets.example.h` to `YoloSecrets.h` beside the gateway sketch and set the deployment API key there. Upload the updated camera and gateway sketches via their OTA ports. The sensor-node sketch is unchanged.

Open the gateway serial monitor at 115200, put an item in view, and send `y`. Clock sync starts when Wi-Fi connects. If `y` arrives before TLS time is ready, the gateway holds the command for up to 12 seconds and starts automatically once synchronized. The camera sends one VGA JPEG over UART and the gateway checks its length and CRC before uploading it over HTTPS. The serial monitor prints the response body and HTTP status. An HTTP 200 with model results verifies the path. Use one request at a time, and wait for it to finish before starting an OTA upload.

## Proposed lean scope

Edit the four lists in `arduino/BinGoGateway/BinClasses.h` to assign YOLO class names to bins. Use exact names and assign each class once. Empty lists accept nothing. Both automatic capture and `y` select the highest-confidence class and can move the tray to any mapped bin from 1 to 4. The ultrasonic sensor count only controls which distances are reported. The node rejects remote commands while the servos are busy. Sorting does not yet check a calibrated fullness threshold.

One item at a time: detect item, capture image, classify it, validate the destination, move the mechanism, return to idle. Invalid classification, stale sensor data, full destination, or a timeout must leave the system in a defined safe state.

Use ESP-NOW between the sensor node and gateway and UART between the camera and gateway.

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

