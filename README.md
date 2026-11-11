# Airton ESP32: controlling a "non-communicating" Airton heat pump (7A 7A protocol)

[Version française](README.fr.md)

**ESP-IDF 5.x** firmware that replaces the Airton WiFi module (ref. 409729, "ACW02"): the ESP32 plugs into
the indoor unit's WiFi connector, reads its state over a serial link and controls it from a web page it
serves itself, with no cloud and no app. Developed and tested on an **Airton SMVH12B-2B2A3NM**
(indoor unit ref. 409935), ESP-IDF 5.3.1.

> ⚠️ Personal project, provided without warranty. You are opening a mains-powered unit and sending commands
> to a heat pump: read [Safety](#safety) first and test read-only before sending anything.

## Project status

| Feature | Status on SMVH12B |
|---|---|
| Reading: room temperature, on/off, mode, setpoint, fan (levels, turbo, silent), louvers, display | validated on the real unit |
| Commands: on/off, setpoint, mode, fan, silent, display, louvers | validated on the real unit |
| Fan percentage, eco, sleep, °C/°F, self-clean | not found in the frames |
| Error codes, filter-clean request | decoded (A6 frame), never seen on my unit |
| Tuya `55 AA` protocol (other Airton models) | parser tested on PC only |

The protocol details, with a confidence level for each byte, are in
[`docs/PROTOCOLE-7A7A.md`](docs/PROTOCOLE-7A7A.md) (in French).

## What the firmware does

- Detects the unit's protocol automatically (`7A 7A` or Tuya `55 AA`), or you fix it in `menuconfig`.
- Serves a **web page**: a control card (temperature, on/off, setpoint, mode, fan, louvers, display)
  synchronised with the state read from the unit, plus error and filter banners.
- A collapsed "Frame analysis (advanced)" block: byte-change journal, markers, CSV export, datapoints,
  advanced write commands. This is the tool for mapping the functions of another model.
- **Over-the-air update** (from the page or with `curl`), with automatic rollback.
- WiFi as an access point (default), as a client of your router, or both. CPU clocked at 80 MHz.
- Commands are locked by default: "Unlock" allows them for 5 minutes.

There is no MQTT or Home Assistant support in this version.

## Hardware

- A classic ESP32 DevKit (tested) and a bidirectional 4-channel **5 V ↔ 3.3 V** level shifter (BSS138) or equivalent.
- Access to the indoor unit's "WIFI connection cable" connector (4 wires, 5 V, 9600 baud 8N1).
- A multimeter; a USB isolator if you connect a PC while the ESP32 is wired to the unit.

## Safety

- Switch off the circuit breaker before opening the unit: the board is connected to the mains.
- The heat pump board's ground is probably **not isolated from the mains**. Never connect a mains-powered PC
  to that ground without a USB isolator. Safe procedure: flash the ESP32 away from the unit, then only do
  updates over WiFi.
- Measure the current the connector can supply before powering the ESP32 from it (WiFi peaks reach about
  300 mA). A 100 to 470 µF capacitor near the ESP32 prevents voltage drops.
- Opening the unit may affect the warranty.

## Wiring

```
Unit TX (5 V) ──[level shifter]──► GPIO35  (AC_UART_RX_PIN, input only)
Unit RX       ◄──[level shifter]──── GPIO33  (AC_UART_TX_PIN)
Unit GND ───────────────────────────── ESP32 GND
Unit +5 V ──► ESP32 VIN (if the current is sufficient)
```

- The transmit GPIO cannot be GPIO 34 to 39 (input only); the code refuses to compile in that case.
- Wire colours do not guarantee the TX/RX direction. If nothing answers, swap the two wires or enable
  "Swap TX and RX" in `menuconfig`. A loopback test (linking the two channels on the 5 V side) validates the
  level shifter and the firmware without the unit.
- If the connector's 5 V is only present with the official module, power the ESP32 separately and connect
  only GND, TX and RX.

## Building and flashing

Requirements: ESP-IDF 5.3 (the "ESP-IDF" terminal on Windows).

```bash
idf.py set-target esp32
idf.py menuconfig     # "Airton AC" menu: WiFi, pins, OTA token, protocol
idf.py build
idf.py -p COMx flash monitor
```

Set in `menuconfig` before flashing: **the access point password** (default `airton1234`),
**the OTA token** (default `changez-moi`), the UART pins and the WiFi mode.

`sdkconfig.defaults` sets: 4 MB flash, two 1.5 MB OTA partitions, rollback enabled, `-Os`,
80 MHz CPU. If you already have an `sdkconfig`, delete it or set the frequency in `menuconfig`
(*Component config* → *ESP System Settings* → *CPU frequency*).

### WiFi modes

| Mode | Effect |
|---|---|
| Access point only (default) | Network **Airton-AC**, web page at `http://192.168.4.1/`. No network time. |
| Router only | The ESP32 joins your 2.4 GHz WiFi. Web page at the address your router gives it. Time via NTP. |
| Access point + router | Both at once. |

## Usage

1. Connect to the **Airton-AC** network, open `http://192.168.4.1/` and enter the OTA token.
2. The card fills in as soon as the unit has sent its state (within a minute).
3. To act: "Unlock commands" (5 min), then use the buttons.

Text commands in the advanced section: `ac state`, `ac temp=20`,
`ac power=on mode=heat fan=3 swing=both display=on` (all fields optional), `raw <type> [bytes]`.

### Updating over WiFi

From the page: "Firmware update" section, choose `build/airton_ac.bin`. Or:

```bash
curl -H "X-Token: your_token" --data-binary @build/airton_ac.bin http://192.168.4.1/update
```

The new firmware marks itself valid once its web server starts; otherwise the bootloader returns to the
previous version on the next reboot. The API and OTA require the token (`X-Token` header), but the traffic
is not encrypted: keep it on a trusted network.

## Helping map other models

With the advanced page: tick "show raw frames", add a marker (`note eco ON`), press **a single** remote
control key, export the CSV. The bytes that change show up in the journal. Captures (CSV) are welcome in an issue.

## Tests

```bash
./test/run.sh          # Tuya and 7A 7A parsers, on a PC (g++ is enough)
```

`test/test_acw02.cpp` uses frames captured on the HACF forum and on my unit (CRC verified).

## Layout

| File | Role |
|---|---|
| `main/acw02.*` | 7A 7A protocol: CRC, parser, state, commands |
| `main/tuya_mcu.*` | Tuya `55 AA` protocol |
| `main/main.cpp` | UART, protocol detection, state, commands, WiFi, SNTP |
| `main/discovery.*` | Web page (control card, frame analysis, update) and API |
| `main/ota.*` | HTTP update with token and rollback |
| `main/ac_bridge.h` | Interface between the firmware core and the page |
| `main/Kconfig.projbuild` | `menuconfig` settings ("Airton AC" menu) |
| `docs/PROTOCOLE-7A7A.md` | Protocol notes, byte by byte (French) |
| `partitions.csv`, `sdkconfig.defaults` | OTA partitions and default settings |
| `test/` | Unit tests on PC |

## Known limitations

- Reads and commands have been validated on a single unit (SMVH12B); other Airton models may differ.
- Fan percentage, eco, sleep and the other remote keys are not decoded. Users report a beep on the first
  command on other models.
- The Tuya protocol has never been tried on a real unit.
- OTA and the page are not encrypted.

## Sources

HACF forum ("Module WIFI climatisation AIRTON"), the Tasmota script
[`ouinouin/Berryton`](https://github.com/ouinouin/Berryton), and my own captures.

## License

[MIT](LICENSE). The software is provided without warranty, as stated in the warning at the top of this file.
