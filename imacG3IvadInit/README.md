# iMac G3 IVAD Board Init

Arduino firmware to initialize the IVAD board in a slot-loading iMac G3, allowing the CRT to be used as a standalone VGA monitor.

Based on the original project by [Rocky Hill (qbancoffee)](https://github.com/qbancoffee/imac_g3_ivad_board_init).

## Features

- **IVAD board initialization** via SoftwareWire (I2C master on pins D4/D5)
- **EDID responder** via hardware I2C (Wire library, address 0x50 on A4/A5) so the video source can identify the display
- **Serial console** (115200 baud) for real-time adjustment of all display settings
- **EEPROM persistence** with wear leveling — geometry, color, and brightness settings survive power cycles
- **Auto power-on** when VSYNC is detected (optional, saved to EEPROM)
- **Auto power-off** after 180 seconds of VSYNC signal loss
- **Physical power button** on pin D3
- **I2C diagnostics** — error counters reported after init and settings writes

## Supported Display Modes

From the EDID (extracted from an iMac G3 DV by oshimai):

| Resolution | Refresh Rate |
|------------|-------------|
| 1024x768   | 75 Hz       |
| 800x600    | 95 Hz       |
| 640x480    | 117 Hz      |

## Pin Assignments

| Pin | Function |
|-----|----------|
| D3  | Power button (active LOW) |
| D4  | IVAD SDA (SoftwareWire) |
| D5  | IVAD SCL (SoftwareWire) |
| D7  | Solid-state relay control |
| D10 | VSYNC input from VGA |
| A4  | VGA DDC data (Wire library, VGA pin 12) |
| A5  | VGA DDC clock (Wire library, VGA pin 15) |
| PD0 | Hardware serial RX |
| PD1 | Hardware serial TX |

## Prerequisites

### Hardware

- Arduino Uno or Nano (ATmega328P)
- iMac G3 slot-loading CRT with IVAD board
- Solid-state relay on pin D7
- VGA connector wired to the Arduino (DDC, VSYNC) and analog board (RGB, sync)

### Wire Library Modification

The Arduino Wire library buffers must be increased from 32 to 128 bytes to transmit the full 128-byte EDID. Edit these two files in your Arduino installation:

**`Wire.h`** — change `BUFFER_LENGTH`:
```
arduino_install_folder/hardware/arduino/avr/libraries/Wire/src/Wire.h
```
```c
#define BUFFER_LENGTH 128
```

**`twi.h`** — change `TWI_BUFFER_LENGTH`:
```
arduino_install_folder/hardware/arduino/avr/libraries/Wire/src/utility/twi.h
```
```c
#define TWI_BUFFER_LENGTH 128
```

### Libraries

Install via the Arduino Library Manager:

- [SoftwareWire](https://github.com/Testato/SoftwareWire) — bit-banged I2C for IVAD communication
- [EEPROMWearLevel](https://github.com/PRosenb/EEPROMWearLevel) — EEPROM wear leveling for settings persistence

## Serial Commands

Connect at **115200 baud**. Send `?` for the full command list.

### Power

| Key | Action |
|-----|--------|
| `e` | Power on |
| `o` | Power off |
| `l` | Re-run IVAD init sequence (display must be on) |
| `q` | Toggle auto power-on when VSYNC detected (saved to EEPROM) |

### Position

| Key | Action |
|-----|--------|
| `a` | Move left |
| `s` | Move right |
| `w` | Move up |
| `z` | Move down |

### Size

| Key | Action |
|-----|--------|
| `d` | Narrower |
| `f` | Wider |
| `r` | Taller |
| `c` | Shorter |

### Geometry

| Key | Action |
|-----|--------|
| `t` | Rotate CW |
| `y` | Rotate CCW |
| `x` | Parallelogram + |
| `v` | Parallelogram − |
| `b` | Keystone pinch top |
| `n` | Keystone pinch bottom |
| `u` | Pincushion out |
| `i` | Pincushion in |

### Color / Brightness

| Key | Action |
|-----|--------|
| `g` | Contrast − |
| `h` | Contrast + |
| `j` | Brightness − |
| `k` | Brightness + |

### Save / Info

| Key | Action |
|-----|--------|
| `m` | Save current settings to EEPROM |
| `p` | Print current settings |
| `?` | Print help |

## HDMI-to-VGA Adapter Notes

Most HDMI-to-VGA adapters have their own built-in EDID and **do not pass through** the Arduino's EDID to the host. The host reads the adapter's EDID (typically advertising 1080p) and outputs an incompatible resolution.

**Workarounds:**

1. **Force resolution on the host** — Use your GPU control panel or [Custom Resolution Utility (CRU)](https://www.monitortests.com/forum/Thread-Custom-Resolution-Utility-CRU) to set 1024x768 @ 75 Hz on the HDMI output.
2. **Disconnect DDC lines** — If the adapter's DDC conflicts with the Arduino's EDID slave (causing a connect/disconnect loop), disconnect A4/A5 from VGA pins 12/15. The adapter will use its own EDID and you force the resolution manually.
3. **Use native VGA** — A video source with a real VGA port reads the Arduino's EDID directly, no manual configuration needed.

## Files

| File | Description |
|------|-------------|
| `imacG3IvadInit.ino` | Main sketch — setup, loop, serial commands, IVAD init, EEPROM, EDID |
| `imacG3IvadInit.h` | EEPROM layout constants, serial protocol constants, EDID byte array |
| `ivad.h` | IVAD register definitions, default/min/max setting values |
| `useful_code_and_notes.txt` | Reference init sequences, alternate EDIDs |

## Changes from Upstream

Key changes from the [original qbancoffee repository](https://github.com/qbancoffee/imac_g3_ivad_board_init):

- **Serial input fix** — `handleSerial()` was receiving the wrong byte; corrected to use `SERIAL_BUFFER[0]`
- **Serial always active** — `serial_processing()` runs every loop iteration regardless of CRT power state
- **Interactive serial commands** — power on/off (`e`/`o`), re-init (`l`), save to EEPROM (`m`), auto power-on toggle (`q`), help (`?`)
- **Descriptive feedback** — setting changes print action names (e.g., `[SET] rotate CW = 50`) instead of raw register numbers
- **Formatted settings display** — `p` command shows all settings with decimal values, percentage, and min/max range
- **Display status in settings** — power state, VSYNC detection, countdown timer, auto power-on, manual-off lock
- **VSYNC diagnostics** — logs signal detection, loss, countdown, and restoration events
- **Auto power-on** — optional mode where the display powers on automatically when VSYNC is detected; saved to EEPROM
- **Manual-off lock** — prevents auto power-on from immediately re-enabling the display after manual power-off; clears when VSYNC is absent
- **EEPROM checksum recovery** — on checksum mismatch, resets to defaults before saving (original saved corrupted data)
- **Version-dependent first-run sentinel** — bumping `CONFIG_EEPROM_VERSION` forces a full EEPROM reset
- **Verbose EEPROM logging** — save operations print each slot with old/new values
- **Value rollover fix** — `ivad_change_setting()` accepts `int` to prevent byte overflow when incrementing 255 or decrementing 0
- **Widened geometry limits** — all geometry registers accept the full 0–255 range to accommodate different iMac G3 variants and video timings
- **I2C error reporting** — init and settings-write report `endTransmission()` error counts for diagnostics
- **Removed unused globals** — legacy per-setting variables replaced by `CURRENT_CONFIG[]` array
