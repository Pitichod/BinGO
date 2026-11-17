# BinGO

BinGO is an ESP32 waste sorter. It waits for an item to settle on the weighing tray, photographs it, and uses a YOLO classification to choose a bin. The tray rotates into position, tips toward the selected bin, and returns home. The load cell checks whether the tray emptied. If the item remains, the node retries the same bin and reports a stuck item on the gateway LCD and Serial Monitor.

| Board | Hardware and role |
| --- | --- |
| [Sensor node](arduino/BinGoSensorNode/BinGoSensorNode.ino) | ESP32 Dev Module, HX711 with a 5 kg load cell, two HC-SR04 distance sensors with pins reserved for two more, and two MG996R servos |
| [Gateway](arduino/BinGoGateway/BinGoGateway.ino) | ESP32 Dev Module, 16×2 I²C LCD, DHT11 and BH1750FVI. Receives ESP-NOW telemetry, calls YOLO over HTTPS, and sends the destination back to the node. |
| [Camera](arduino/BinGoCamera/BinGoCamera.ino) | AI Thinker ESP32-CAM with OV3660 and onboard flash. Sends JPEGs to the gateway over UART. |

## Setup

1. Follow the [wiring and calibration guide](RESTART.md). Use a separate supply for the servos, a voltage divider on each HC-SR04 ECHO, and an I²C level shifter for a 5 V LCD backpack.
2. In Arduino IDE, install the **esp32 by Espressif Systems** board package (3.x). In Library Manager, install **HX711 Arduino Library** by Bogdan Necula, **LiquidCrystal_I2C** by Martin Kubovčík and Frank de Brabander, **DHT sensor library** by Adafruit, and **Adafruit Unified Sensor**.
3. Copy [OtaSecrets.example.h](arduino/OtaSecrets.example.h) into each sketch folder as `OtaSecrets.h` and fill in the Wi-Fi and OTA settings. Copy [EspNowPeer.example.h](arduino/EspNowPeer.example.h) into each main board's sketch folder as `EspNowPeer.h` and enter the other board's station MAC address. The boards must use the same 2.4 GHz Wi-Fi channel.
4. Copy [YoloSecrets.example.h](arduino/BinGoGateway/YoloSecrets.example.h) to `YoloSecrets.h` in the gateway folder. Set the YOLO deployment URL and API key. Git ignores these local settings files.
5. Flash each board once over USB with an OTA-compatible partition scheme. Subsequent uploads can use `bingo-gateway`, `bingo-sensor-node`, or `bingo-camera` under **Tools → Port**. Upload the gateway and sensor node together when their ESP-NOW packet format changes.

## Test the sorter

Start with the tray empty for automatic tare. Open Serial Monitor at **115200**.

| Board | Command | Result |
| --- | --- | --- |
| Gateway | `n` | Read weight and bin distances |
| Gateway | `e` | Read temperature, humidity, and light |
| Gateway | `c` | Check the camera UART link |
| Gateway | `y` | Capture, classify, and sort if the class maps to a bin |
| Gateway | `s` | Test the bin 1 servo cycle over ESP-NOW |
| Sensor node | `w` / `z` / `k` | Read weight, tare, or calibrate with a 1000 g reference |
| Sensor node | `1`–`4` / `x` | Test a bin cycle or stop the PWM |

For an automatic run, place one item on the tray and let it settle. The gateway logs `YOLO auto: stable item` when it requests the capture. Test a mapped class and confirm that the item lands in the intended bin and the tray returns home.

## Configuration

- [BinClasses.h](arduino/BinGoGateway/BinClasses.h) maps YOLO labels to the four bins. Empty lists accept no classes.
- [ItemTrigger.h](arduino/BinGoSensorNode/ItemTrigger.h) controls minimum item weight, settling time, and re-arm conditions.
- [BinGoSensorNode.ino](arduino/BinGoSensorNode/BinGoSensorNode.ino) holds the servo angles, `maxDumpRetries`, and the number of fitted distance sensors.

The current firmware displays readings and alerts locally. Blynk telemetry, dashboard history, and remote notifications are pending.
