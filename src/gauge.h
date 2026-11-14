#ifndef GAUGE_H
#define GAUGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dmdrvi_ioctl.h"

/**
 * @file gauge.h
 * @brief The BQ27220 (Texas Instruments, a battery gauge of one cell, I2C 0x55): its registers and what they mean as a battery.
 *
 * No hardware, no allocation: the registers read from the chip go in, a dmdrvi_battery_status_t comes out. Tested on a PC.
 * The standard commands of the chip are 16-bit words, little endian, at even addresses; a burst read from the first of them to the last
 * gives all of them at one moment.
 */

#define GAUGE_ADDRESS               0x55U       /**< The address of the chip on the bus */
#define GAUGE_DEVICE_NUMBER         0x0220U     /**< What the chip says it is (Control(): DEVICE_NUMBER) */

#define GAUGE_REG_CONTROL           0x00U       /**< The sub-commands (Control()) are written here */
#define GAUGE_REG_MAC_DATA          0x40U       /**< ... and their answers read from here, after at least 1 ms */
#define GAUGE_CONTROL_DEVICE_NUMBER 0x0001U
#define GAUGE_REG_BLOCK             0x06U       /**< The first register of the burst: Temperature */
#define GAUGE_BLOCK_SIZE            40U         /**< ... to StateOfCharge (0x2C), the last byte is 0x2D */
#define GAUGE_REG_DESIGN_CAPACITY   0x3CU

/** The registers of the burst, as the chip gives them (words, little endian, from GAUGE_REG_BLOCK) */
typedef struct
{
    uint16_t    temperature;        /**< 0x06, 0.1 K */
    uint16_t    voltage;            /**< 0x08, mV */
    uint16_t    status;             /**< 0x0A, BatteryStatus: GAUGE_STATUS_* */
    int16_t     current;            /**< 0x0C, mA, positive: charging */
    uint16_t    remaining;          /**< 0x10, mAh */
    uint16_t    full;               /**< 0x12, mAh */
    uint16_t    time_to_empty;      /**< 0x16, minutes, 0xFFFF: not known */
    uint16_t    time_to_full;       /**< 0x18, minutes, 0xFFFF: not known */
    uint16_t    cycles;             /**< 0x2A */
    uint16_t    percent;            /**< 0x2C */
} gauge_raw_t;

/** BatteryStatus() */
#define GAUGE_STATUS_DSG            (1u << 0)   /**< Discharging (or relaxing): no charger works */
#define GAUGE_STATUS_SYSDWN         (1u << 1)   /**< The system should shut down */
#define GAUGE_STATUS_TDA            (1u << 2)   /**< Terminate discharge alarm: the battery is empty for the chip */
#define GAUGE_STATUS_BATTPRES       (1u << 3)   /**< A battery is there */
#define GAUGE_STATUS_CHGINH         (1u << 8)   /**< Charging is inhibited (too hot or too cold) */
#define GAUGE_STATUS_FC             (1u << 9)   /**< Fully charged */

/** The thresholds of the flags of a battery that is discharged: the percent of the charge */
typedef struct
{
    uint8_t     low_percent;        /**< DMDRVI_BATTERY_FLAG_LOW at or below this (0: only the alarm of the chip) */
    uint8_t     critical_percent;   /**< DMDRVI_BATTERY_FLAG_CRITICAL at or below this */
} gauge_thresholds_t;

/** The burst (GAUGE_BLOCK_SIZE bytes from GAUGE_REG_BLOCK) as registers */
void gauge_parse(const uint8_t block[GAUGE_BLOCK_SIZE], gauge_raw_t *raw);

/** The registers as the battery of the interface: what is not valid is zero and has no bit in `valid` */
void gauge_decode(const gauge_raw_t *raw, const gauge_thresholds_t *thresholds, dmdrvi_battery_status_t *status);

#endif
