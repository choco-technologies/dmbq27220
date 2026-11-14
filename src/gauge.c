#include "gauge.h"
#include <string.h>

#define TIME_UNKNOWN        0xFFFFU
#define KELVIN_ZERO_DC      2731        /* 0 degrees Celsius in tenths of a kelvin */

static uint16_t word(const uint8_t *block, unsigned reg)
{
    unsigned i = reg - GAUGE_REG_BLOCK;
    return (uint16_t)(block[i] | (block[i + 1U] << 8));
}

void gauge_parse(const uint8_t block[GAUGE_BLOCK_SIZE], gauge_raw_t *raw)
{
    raw->temperature   = word(block, 0x06);
    raw->voltage       = word(block, 0x08);
    raw->status        = word(block, 0x0A);
    raw->current       = (int16_t)word(block, 0x0C);
    raw->remaining     = word(block, 0x10);
    raw->full          = word(block, 0x12);
    raw->time_to_empty = word(block, 0x16);
    raw->time_to_full  = word(block, 0x18);
    raw->cycles        = word(block, 0x2A);
    raw->percent       = word(block, 0x2C);
}

static uint8_t state_of(const gauge_raw_t *raw)
{
    if ((raw->status & GAUGE_STATUS_BATTPRES) == 0U)
        return DMDRVI_BATTERY_STATE_NOT_PRESENT;
    if ((raw->status & GAUGE_STATUS_DSG) != 0U)
        return DMDRVI_BATTERY_STATE_DISCHARGING;
    if ((raw->status & GAUGE_STATUS_FC) != 0U)
        return DMDRVI_BATTERY_STATE_FULL;
    if ((raw->status & GAUGE_STATUS_CHGINH) != 0U)
        return DMDRVI_BATTERY_STATE_IDLE;
    return DMDRVI_BATTERY_STATE_CHARGING;
}

void gauge_decode(const gauge_raw_t *raw, const gauge_thresholds_t *thresholds, dmdrvi_battery_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->state = state_of(raw);
    status->valid = DMDRVI_BATTERY_VALID_STATE;
    if (status->state == DMDRVI_BATTERY_STATE_NOT_PRESENT)
        return;                                         /* without a battery the other registers mean nothing */

    status->valid |= DMDRVI_BATTERY_VALID_VOLTAGE | DMDRVI_BATTERY_VALID_CURRENT | DMDRVI_BATTERY_VALID_PERCENT |
                     DMDRVI_BATTERY_VALID_CAPACITY | DMDRVI_BATTERY_VALID_TEMPERATURE | DMDRVI_BATTERY_VALID_CYCLES;
    status->voltage_mv = raw->voltage;
    status->current_ma = raw->current;
    status->percent = raw->percent > 100U ? 100U : (uint8_t)raw->percent;
    status->remaining_mah = raw->remaining;
    status->full_mah = raw->full;
    int temperature = (int)raw->temperature - KELVIN_ZERO_DC;
    status->temperature_dc = (int16_t)(temperature < -32768 ? -32768 : temperature > 32767 ? 32767 : temperature);
    status->cycles = raw->cycles;

    /* The time that makes sense for what the battery does */
    if (status->state == DMDRVI_BATTERY_STATE_DISCHARGING && raw->time_to_empty != TIME_UNKNOWN)
    {
        status->minutes_to_empty = raw->time_to_empty;
        status->valid |= DMDRVI_BATTERY_VALID_TIME;
    }
    else if (status->state == DMDRVI_BATTERY_STATE_CHARGING && raw->time_to_full != TIME_UNKNOWN)
    {
        status->minutes_to_full = raw->time_to_full;
        status->valid |= DMDRVI_BATTERY_VALID_TIME;
    }

    /* The device has power from outside when no charger is missing: the chip says "discharging" only when it is the battery that works */
    if (status->state != DMDRVI_BATTERY_STATE_DISCHARGING)
        status->flags |= DMDRVI_BATTERY_FLAG_EXTERNAL_POWER;
    if (status->state == DMDRVI_BATTERY_STATE_DISCHARGING)
    {
        if ((raw->status & GAUGE_STATUS_SYSDWN) != 0U || status->percent <= thresholds->critical_percent)
            status->flags |= DMDRVI_BATTERY_FLAG_CRITICAL | DMDRVI_BATTERY_FLAG_LOW;
        else if ((raw->status & GAUGE_STATUS_TDA) != 0U || (thresholds->low_percent != 0U && status->percent <= thresholds->low_percent))
            status->flags |= DMDRVI_BATTERY_FLAG_LOW;
    }
}
