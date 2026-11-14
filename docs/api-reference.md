# dmbq27220 - reference

The module has no functions to call: its node is a dmdrvi device.

## Node

`/dev/<section>` - the name of the section of the board's ini (`[battery]`: `/dev/battery`); without a section name `/dev/dmbq27220<instance>`.

| Operation | |
|-----------|--|
| `ioctl(DMDRVI_IOCTL_BATTERY_GET_INFO)` | `dmdrvi_battery_info_t`: driver `dmbq27220`, chip `BQ27220`, what the node can tell, the design capacity of the chip |
| `ioctl(DMDRVI_IOCTL_BATTERY_GET_STATUS)` | `dmdrvi_battery_status_t`: a reading not older than `refresh_ms`. `-EIO` when the chip does not answer, `-ENODEV` when it is not the chip it should be or the bus is not there |
| `read()` | the state as lines `key: value`, for a shell |
| `write()` | `-ENOTSUP` |

## Configuration (the section of the driver)

| Key | Default | Meaning |
|-----|---------|---------|
| `instance` | 0 | the number of the node |
| `address` | 85 (0x55) | the chip on the bus |
| `refresh_ms` | 1000 | the chip is asked at most this often |
| `low_percent` | 15 | `DMDRVI_BATTERY_FLAG_LOW` at or below it while discharging (0: only the alarm of the chip) |
| `critical_percent` | 5 | `DMDRVI_BATTERY_FLAG_CRITICAL` at or below it while discharging |

Friends: `i2c` - the `dmi2c` node the chip is on.
