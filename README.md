# RainShield-Smart-Rain-Protection-System

```markdown
# RainShield – Smart Rain Protection System 🌧️

## 🪴 Why I built this

I built RainShield because I wanted to solve a simple everyday problem: clothes drying outside can get completely wet when unexpected rain arrives. My idea was to create a smart cloth rack that automatically folds inside when rain is detected. I used an ESP32, a rain sensor, and a servo motor to make it happen. Here's how I put everything together.

## 🌀 What it actually does

Imagine a normal extended cloth rack with clothes hanging on it. When rain starts, the MG90S servo moves the mechanism and pulls the rack inside to a sheltered position. When conditions become safe again, it can extend back outside.

I also added three operating modes:
- **Sensor Mode:** Uses the physical rain sensor.
- **Weather Mode:** Uses Open-Meteo weather data, including humidity and temperature, to calculate possible rain conditions.
- **Mobile Mode:** Lets me control the rack manually through a web dashboard.

The dashboard runs on the ESP32 and can be opened using a phone. But what hardware makes this possible?

## 🔩 What I used

- ESP32 development board
- LM393 digital rain sensor
- MG90S servo motor
- External regulated 5V power supply
- Connecting wires
- Extended cloth rack mechanism
- 470–1000 µF capacitor (recommended for servo power stability)

## 🔌 How I wired it

| Component | ESP32 connection |
|---|---|
| Rain sensor VCC | 5V |
| Rain sensor GND | GND |
| Rain sensor DO | GPIO 27 |
| Servo signal | GPIO 13 |
| Servo VCC | External 5V |
| Servo GND | Common GND |

I powered the servo separately instead of relying on the ESP32's power output. Both grounds must be connected together. That small detail matters more than I initially expected.

## 🎛️ Getting it running

1. Install Arduino IDE and configure the ESP32 board package.
2. Install the ESP32Servo and ArduinoJson libraries.
3. Open the RainShield source code.
4. Configure your home Wi-Fi credentials and weather location.
5. Select the correct ESP32 board and COM port.
6. Upload the program.
7. Connect your phone to the ESP32 access point and open `http://192.168.4.1`.

Keep private Wi-Fi credentials out of your public GitHub repository. There's one problem I had to work through before everything behaved properly.

## 🧪 The bug that almost broke me

The servo sometimes refused to respond consistently. I had to investigate both power stability and the control logic. I improved the servo command handling so it resends its position when a command arrives, even if the software believes it is already there. I also kept the servo attached while idle to help it hold its position.

After testing and corrections, the complete system is now functional.

## 🧭 What I'd add next

I'd like to improve the mechanical design, make the rack more suitable for heavier clothes, and explore additional weather-based decisions and remote monitoring.

## 🪴 Give it a try

This project started with a simple problem at home and turned into a working ESP32-based automation system. Feel free to explore the code, build your own version, or share suggestions. I'd be glad to hear what you come up with!
```
