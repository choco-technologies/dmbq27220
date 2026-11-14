# dmbq27220

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmbq27220/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmbq27220/actions/workflows/ci.yml)

The driver of the **BQ27220** battery gauge (Texas Instruments, one cell, I2C 0x55) - the battery of the LILYGO T-Deck Pro. Its node
is the **battery interface** of [dmdrvi](https://github.com/choco-technologies/dmdrvi) (`DMDRVI_IOCTL_BATTERY_*`): a client (the status bar
of a phone, a power service) reads the state, the charge in percent, the voltage, the current, the capacity, the temperature, the time left
and the cycles without knowing which chip is behind the node.

```
$ cat /dev/battery
state: discharging
percent: 82
voltage: 3921 mV
current: -231 mA
capacity: 1148 / 1400 mAh (design 1400)
temperature: 28.6 C
minutes: 296 to empty, 0 to full
cycles: 3
flags:
```

## Use

The board's ini file (`configs/board/<board>/battery.ini`) has the section of the gauge; the section's name is the name of the node
(`[battery]`: `/dev/battery`). In the firmware's `flash.dmd`: `dmbq27220 driver=board/t-deck-pro/battery.ini`. The gauge needs a bus: a
friend with the role `i2c` (a `dmi2c` node) in its friends_group; the T-Deck Pro's ini takes the bus of the touch panel.

```c
void *node = Dmod_FileOpen("/dev/battery", "r");
dmdrvi_battery_status_t b;
if (node != NULL && Dmod_Ioctl(node, DMDRVI_IOCTL_BATTERY_GET_STATUS, &b) == 0 && (b.valid & DMDRVI_BATTERY_VALID_PERCENT))
    Dmod_Printf("%u%%\n", b.percent);
```

Every number has its bit in `valid`: what the chip cannot tell (the time left of a battery that is neither discharging nor charging) is not
valid and is zero.

## What it does and does not

- It **only reads**. The chip is set up (the capacity of the battery, the profile) by whoever made the board; a driver that writes the data
  memory of a gauge can ruin what it has learnt. The design capacity it reports is the chip's.
- It reads the 40 bytes of the registers `0x06..0x2D` in one burst (a consistent reading) at most once in `refresh_ms` (1 s).
- It asks the chip what it is (`Control(DEVICE_NUMBER)` = 0x0220) before the first reading, again and again until it answers.
- The state comes from `BatteryStatus()`: `DSG` is discharging, `FC` is full, `CHGINH` is idle (charging inhibited), else charging, no `BATTPRES` is
  no battery. `DMDRVI_BATTERY_FLAG_EXTERNAL_POWER` is set while the chip does not say discharging.
- `low_percent` / `critical_percent` (the ini) set the flags `LOW` / `CRITICAL` of a battery that is discharged; the alarms of the chip (`TDA`,
  `SYSDWN`) set them too.
- The BQ27220 holds SCL low while it answers (clock stretching): the I2C controller has to wait for it (dmi2c for the ESP32-S3: since the
  fix of the time-out of the clock).

## Building

```bash
cmake -S . -B build -DDMOD_DIR=/path/to/dmod -DDMOD_TOOLS_NAME=arch/x86_64 -DDMOD_BUILD_TOOLS=ON
cmake --build build && ctest --test-dir build
```

The decoding of the registers (`src/gauge.c`) knows no hardware and is tested on a PC (`tests/`); the driver (`src/dmbq27220.c`) is the
dmdrvi interface over a dmi2c node.

The battery interface is in dmdrvi since the release that has `DMDRVI_IOCTL_BATTERY_GET_INFO`; before it, the headers of a checkout can be put first
(`-DCMAKE_PROJECT_INCLUDE=` a file with `include_directories(BEFORE <dmdrvi>/include)`).

## License

MIT
