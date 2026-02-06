# AULA F75 LED Controller

A lightweight Windows utility that automatically turns off your AULA F75 keyboard LEDs when your display goes to sleep, and restores them to the previous brightness when the display wakes up.

## Features

- **Automatic LED control** based on Windows display power state
- **Brightness preservation** — captures the current brightness before sleep and restores it on wake
- **Device reconnection** — automatically handles USB bus resets during sleep/wake cycles
- **Firmware version detection** — supports both firmware v24 and v26 of the AULA F75
- **Zero configuration** — runs in the background with no user interaction required

## Requirements

- Windows 11 (64-bit)
- AULA F75 keyboard (VID: 0x258A, PID: 0x010C)
- Administrator privileges (required for HID device access)

## Installation

1. Download the latest `aulaf75-led.exe` from [Releases](../../releases)
2. Run the executable — it will automatically request admin elevation
3. The program runs in the background and starts with Windows display power events

To run at startup, place a shortcut in your Windows Startup folder (`Win+R` → `shell:startup`).

## Building from Source

### Prerequisites

- Visual Studio 2022 with C++ desktop development workload
- Windows SDK (included with Visual Studio)

### Build Steps

1. Clone the repository:
   ```bash
   git clone https://github.com/aizuon/aulaf75-led.git
   cd aulaf75-led
   ```

2. Build with MSBuild:
   ```bash
   MSBuild aulaf75-led.sln /p:Configuration=Release /p:Platform=x64
   ```

The executable will be in `x64\Release\aulaf75-led.exe`.

## How It Works

The program uses the Windows power management API to listen for display state changes (`GUID_CONSOLE_DISPLAY_STATE`). When the display turns off:

1. Reads the current LED profile from the keyboard via HID feature reports
2. Saves the current brightness value
3. Writes a modified profile with brightness set to 0

When the display turns back on, it restores the saved brightness value using the same HID protocol.

## Technical Details

### HID Protocol

The AULA F75 uses a custom HID feature report protocol (520 bytes per report):

```
Byte 0:       Report ID (0x06 for fw24, 0x09 for fw26)
Byte 1:       Command (0x04=SetLED, 0x84=GetLED)
Bytes 2-3:    Sub-command (0x0000)
Byte 4:       Total packet count
Byte 5:       Packet index
Bytes 6-7:    Chunk data size (little-endian)
Bytes 8-519:  Payload data (512 bytes max)
```

The LED profile is 128 bytes with:
- **Byte 10**: Brightness level (0x00=off, 0x01-0x0B=min to max)
- **Bytes 126-127**: Validity signature (0x5A 0xA5)

### Reverse Engineering

This implementation was reverse-engineered from the official AULA driver (`CDevG5KB` class) using IDA Pro. Key findings:

- **Firmware versions**: v24 (UsagePage=0xFF00, Usage=1) vs v26 (UsagePage=0xFF02, Usage=2) use different report IDs
- **Retry logic**: Commands retry up to 3 times with 200ms delay on failure; profile signature validation retries up to 5 times with 100ms delay
- **Timing requirements**: 20ms delay after HID commands, 60ms delay between GetLED and SetLED operations
- **Device matching**: VID/PID + UsagePage/Usage + FeatureReportByteLength (520) must all match

The protocol implementation matches the OEM driver's behavior in all material aspects: command encoding, retry counts, inter-command timing, and signature validation.

## License

This project is provided as-is for educational and personal use.
