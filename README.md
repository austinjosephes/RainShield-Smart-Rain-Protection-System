# RainShield – Smart Rain Protection System 🌧️

## 🪴 Why I built this

We've all been there: you hang your laundry out in the sun, step away for an hour, and come back to find it drenched by a sudden shower. I wanted to fix that for good. So I built a clothes rack that looks after itself, one that slides under cover the moment rain shows up. An ESP32, a rain sensor and a small servo motor turned out to be all I needed. Here's how it came together.

## 🌀 What it actually does

Picture an ordinary pull-out clothes rack loaded with laundry. When the first drops fall, an MG90S servo drives the mechanism and tucks the rack into a dry, sheltered spot. Once the sky clears, it slides back out on its own.

You can run it in three different modes:
- **Sensor Mode:** Relies on the rain sensor mounted on the rack.
- **Weather Mode:** Pulls live data from Open-Meteo (temperature and humidity) and works out whether rain is likely.
- **Mobile Mode:** Hands the controls to you through a web dashboard.

The dashboard is served straight from the ESP32, so any phone browser can open it, with no app needed. But what does it take to build all this?

## 🔩 What I used

- ESP32 development board
- LM393 digital rain sensor module
- MG90S micro servo
- Regulated external 5V power supply
- Jumper wires
- Pull-out clothes rack mechanism
- 470–1000 µF capacitor (smooths out servo power spikes)

## 🔌 How I wired it

| Part | Goes to |
|---|---|
| Rain sensor VCC | ESP32 5V |
| Rain sensor GND | ESP32 GND |
| Rain sensor DO | GPIO 27 |
| Servo signal | GPIO 13 |
| Servo power | External 5V supply |
| Servo ground | Common GND |

The servo runs from its own 5V supply rather than the ESP32's pin, and the two grounds are tied together. That shared ground sounds trivial, but without it the servo gets no proper signal reference, and I learned that the hard way.

## 🎛️ Getting it running

1. Install the Arduino IDE and add ESP32 board support.
2. Add the ESP32Servo and ArduinoJson libraries.
3. Open the RainShield sketch.
4. Enter your Wi-Fi details and your location for the weather lookup.
5. Pick your ESP32 board and COM port, then upload.
6. Join the ESP32's access point from your phone and visit `http://192.168.4.1`.

Please keep your real Wi-Fi name and password out of any public GitHub repo. Use placeholders instead. Before it all worked smoothly, though, one issue gave me a lot of trouble.

## 🧪 The bug that almost broke me

At first the servo would respond to some commands and ignore others. I traced it to two things: unstable power and flawed control logic. On the power side, the separate 5V supply and the capacitor helped. On the software side, my code skipped a command whenever it thought the servo was already at the target angle. Now the position is re-sent on every command, and the servo stays attached while idle so it holds its place.

After those fixes, the whole system finally ran reliably.

## 🧭 What I'd add next

I'd like to build a stronger mechanism that can carry heavier loads, add smarter weather-based decisions, and set up remote monitoring so I can check on it from anywhere.

## 🪴 Give it a try

What began as a small annoyance at home grew into a working ESP32 automation project. Take a look at the code, build your own version, or send me your ideas. I'd love to see what you make with it!
