#ifndef DMBQ27220_H
#define DMBQ27220_H

/**
 * @file dmbq27220.h
 * @brief dmbq27220: the driver of the BQ27220 battery gauge. It has no functions to call: its node (/dev/battery - the name of the section of the ini; without one /dev/dmbq27220<instance>) is the battery
 *        interface of dmdrvi (DMDRVI_IOCTL_BATTERY_*, dmdrvi_ioctl.h), and cat of the node gives the state as text.
 *
 * What a board's ini file (configs/board/<board>/battery.ini) can set in the section of the driver:
 *   instance          the number of the node (0)
 *   address           the chip on the bus (0x55 = 85)
 *   refresh_ms        the chip is asked at most this often (1000)
 *   low_percent       DMDRVI_BATTERY_FLAG_LOW at or below this charge, while discharging (15)
 *   critical_percent  DMDRVI_BATTERY_FLAG_CRITICAL at or below this charge, while discharging (5)
 * and the role of the friend: i2c (the dmi2c node the chip is on).
 */

#define DMBQ27220_DRIVER_NAME       "dmbq27220"
#define DMBQ27220_DEFAULT_ADDRESS   0x55

#endif
