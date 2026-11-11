# Ultrasonic-Proximity-Node

## Overview

The Ultrasonic Bed Edge Motion Detector is a non-blocking, network-connected monitoring system designed to detect a patient moving to the edge of a bed before an actual exit occurs. Using an ultrasonic distance sensor, it establishes a configurable containment zone. When a physical barrier/obstacle enters this zone, it fires a local audible warning, goes into a smart caregiver-quiet lockout phase to prevent audio fatigue, and securely latches a remote status flag over UDP until a supervisor script explicitly resets it.

## Hardware Interface & Pin Map

* **Microcontroller:** ESP32
* **Sensor:** HC-SR04 (or compatible 3.3V/5V ultrasonic distance sensor)
* **Trig Pin:** GPIO 12
* **Echo Pin:** GPIO 14


* **Buzzer/Indicator:** GPIO 2 (shares the onboard ESP32 blue LED)

## Operational Behavior

1. **Entry Delay:** An object must remain continuously closer than the configured `threshold` distance for a specified trigger delay (default: 1000ms) to filter out transient movements.
2. **Local One-Shot Alarm:** Once triggered, the local buzzer sounds for exactly `alarm_dur` (default: 1000ms) and then turns off instantly. It will not sound again as long as the obstruction remains in range, preventing caregiver audio fatigue.
3. **Asynchronous Push Alert:** The exact millisecond the local alarm activates, the device actively dispatches an immediate push notification packet (`MOTION_EXIT_ALERT`) over UDP to port `9999` of all tracked supervisors.
4. **Network State Latch:** The remote network parameter `status` flips to `1`. It remains locked at `1` even if the patient moves out of range, ensuring a supervisor script cannot miss the event.
5. **Clear & Re-arm Hysteresis:** Once the patient moves away from the edge (distance > `threshold`), a 2-second timeout window begins. If the area stays clear for the full 2.0 seconds, the local buzzer sounds a crisp 10ms click to acknowledge re-arming, and the local engine returns to ready.

## UDP Network API Reference

All commands are plain text, case-sensitive, and utilize the default controller port `8888`. Responses are prefixed with `OK` or `ERR`.

### Parameters & Commands

* **`status` (System State Latch)**
* *Get Status:* `status ?`
* *Response:* `OK status 0` (Ready) or `OK status 1` (Motion Detected / Latched)
* *Clear Latch:* `status 0`
* *Response:* `OK status 0`
* *Note:* Setting status back to `0` clears the remote tracking flag. If the patient is still sitting in front of the sensor, the network status will safely remain `0` and cannot flip back to `1` until they move away and re-trigger the zone.


* **`threshold` (Target Distance Boundary in cm)**
* *Get Current Boundary:* `threshold ?`
* *Response:* `OK threshold 180.00` (Example value)
* *Set New Boundary:* `threshold <value>` (e.g., `threshold 150.5`)
* *Response:* `OK threshold 150.50` (Instantly saved to persistent flash)


* **`alarm_dur` (Local Buzzer Duration in milliseconds)**
* *Get Current Duration:* `alarm_dur ?`
* *Response:* `OK alarm_dur 1000`
* *Set New Duration:* `alarm_dur <value>` (e.g., `alarm_dur 1500`)
* *Response:* `OK alarm_dur 1500` (Instantly saved to persistent flash)


* **`distance` (Live Sensor Reading - Read Only)**
* *Get Real-time Distance:* `distance ?`
* *Response:* `OK distance 214.32` (Returns actual distance in centimeters)
* *Note:* Useful for initial physical placement and boundary mapping without needing a wired serial monitor connection.



### Asynchronous Outbound Alerts

The device independently transmits UDP packets to target monitoring clients on **Port 9999**:

* **`MOTION_EXIT_ALERT`**: Broadcast instantly when the entry threshold confirmation timer is satisfied.
