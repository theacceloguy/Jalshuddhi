# Wiring & Pin Map — JALSHUDDHI

All logic runs on a single ESP32-WROOM-32 DevKit. Analog sensors are placed on
**ADC1** pins only (ADC2 is unavailable when Wi-Fi is active). The relay board
must be **opto-isolated**; pumps, the solenoid and the UV ballast are switched
on the mains/24 V side, never directly from the ESP32.

## Pin assignments

| Signal | ESP32 GPIO | Direction | Notes |
|---|---|---|---|
| pH analog out | GPIO 35 | input (ADC1_CH7) | input-only pin |
| TDS analog out | GPIO 34 | input (ADC1_CH6) | input-only pin |
| Turbidity analog out | GPIO 32 | input (ADC1_CH4) | scale 5 V→3.3 V if needed |
| DS18B20 data | GPIO 33 | bidirectional | 4.7 kΩ pull-up to 3V3 |
| I²C SDA (LCD) | GPIO 21 | bidirectional | LCD I²C backpack |
| I²C SCL (LCD) | GPIO 22 | output | LCD I²C backpack |
| Start/Stop button | GPIO 18 | input | to GND, internal pull-up |
| Submersible pump relay | GPIO 25 | output | relay IN1 (active-LOW) |
| Booster pump relay | GPIO 26 | output | relay IN2 |
| Solenoid valve relay | GPIO 27 | output | relay IN3 |
| UV lamp relay | GPIO 14 | output | relay IN4 |
| Status LED | GPIO 2 | output | on-board LED = "safe to drink" |

## Power

```
Mains ──► 24 V adapter ──┬─► Submersible pump, booster pump, solenoid (24 V)
                         └─► Buck converter ──► 5 V ──► ESP32 VIN
                                                └─► 3.3 V logic / LCD
```

- Add an inline **fuse** on the 24 V rail and a common **ground** between the
  24 V side and the ESP32 logic ground.
- The turbidity sensor board is natively 5 V; if its analog output can exceed
  3.3 V, use a divider or level shifter before GPIO 32.

## Sensor placement

- **pH, TDS, turbidity, temperature** sit in the **sensor mounting chamber** at
  the outlet side, so the values reflect the *treated* water that the safety
  logic gates on.
- Keep analog probe leads short and away from the pump/relay wiring to reduce
  electrical noise (the firmware already median-filters each reading).
