# RainShield: Automatic Rain-Safe Clothes Rack

Anyone who has dried laundry outdoors knows the feeling: you step out for an hour, a cloud rolls in, and everything is soaked. RainShield is my fix. It is an ESP32-controlled clothes rack that retracts itself under cover when rain is detected and slides back out once the weather clears.

---

## Overview

A standard extendable drying rack is driven by an MG90S servo. The ESP32 decides when to pull the rack in or push it out, based on one of three control modes you can switch between from your phone.

| Mode | How it decides |
|---|---|
| **Sensor** | Reads the LM393 rain sensor directly on the rack |
| **Weather** | Pulls Open-Meteo forecast data (temperature, humidity) and estimates whether rain is likely |
| **Mobile** | You open or close the rack yourself from a web dashboard |

The dashboard is hosted by the ESP32 itself, so there is no app to install. Any phone browser will do.

---

## Parts List

- ESP32 development board
- LM393 digital rain sensor module
- MG90S micro servo
- Regulated external 5V supply
- 470 to 1000 µF capacitor (smooths out servo current spikes)
- Jumper wires
- A pull-out clothes rack mechanism

---

## Wiring

| From | To |
|---|---|
| Rain sensor VCC | ESP32 5V |
| Rain sensor GND | ESP32 GND |
| Rain sensor DO | GPIO 27 |
| Servo signal | GPIO 13 |
| Servo power | External 5V supply |
| Servo ground | Shared ground |

**Important:** the servo gets its own 5V supply, and that supply's ground must be tied to the ESP32's ground. Without a common ground the servo signal has no reference and behaves erratically. Placing the capacitor across the servo's power lines also helps.

---

## Setup

1. Install the Arduino IDE and add ESP32 board support.
2. Add the **ESP32Servo** and **ArduinoJson** libraries from the Library Manager.
3. Open the RainShield sketch.
4. Enter your Wi-Fi details and your location (used for the weather lookup).
5. Choose your ESP32 board and the correct COM port, then upload.
6. On your phone, join the ESP32's Wi-Fi access point and browse to `http://192.168.4.1`.

> Do not commit your real Wi-Fi name or password to a public repository. Use placeholders in anything you publish.

---

## Troubleshooting Notes

**Servo responded unpredictably.** Early on, the servo would sometimes ignore commands. Two causes turned out to matter:

- **Power:** the servo was drawing more than the board could supply cleanly, hence the separate 5V supply and capacitor.
- **Software:** the code skipped commands when it thought the servo was already in position. It now re-sends the target angle every time a command arrives, and the servo stays attached while idle so it holds its position.

With both fixes in place, the rack works reliably.

---

## Future Improvements

- A sturdier mechanical design that can carry heavier loads
- Smarter weather-based decision rules
- Remote monitoring from outside the home network

---

## Contributing

Ideas, forks and suggestions are all welcome. If you build your own version, I'd like to see it.
