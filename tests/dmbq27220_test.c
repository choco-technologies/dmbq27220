#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "gauge.h"
#include <string.h>

/* The registers of the chip as a block (what the burst read from 0x06 gives), and what a battery is made of them */

typedef struct
{
    uint16_t temperature, voltage, status;
    int16_t  current;
    uint16_t remaining, full, time_to_empty, time_to_full, cycles, percent;
} regs_t;

static void put(uint8_t block[GAUGE_BLOCK_SIZE], unsigned reg, uint16_t value)
{
    block[reg - GAUGE_REG_BLOCK] = (uint8_t)(value & 0xFFU);
    block[reg - GAUGE_REG_BLOCK + 1U] = (uint8_t)(value >> 8);
}

static void decode(const regs_t *r, dmdrvi_battery_status_t *s)
{
    uint8_t block[GAUGE_BLOCK_SIZE];
    gauge_raw_t raw;
    gauge_thresholds_t thresholds = { 15, 5 };
    memset(block, 0, sizeof(block));
    put(block, 0x06, r->temperature);
    put(block, 0x08, r->voltage);
    put(block, 0x0A, r->status);
    put(block, 0x0C, (uint16_t)r->current);
    put(block, 0x10, r->remaining);
    put(block, 0x12, r->full);
    put(block, 0x16, r->time_to_empty);
    put(block, 0x18, r->time_to_full);
    put(block, 0x2A, r->cycles);
    put(block, 0x2C, r->percent);
    gauge_parse(block, &raw);
    gauge_decode(&raw, &thresholds, s);
}

/* The battery of the T-Deck Pro on the charger, full: what the chip said on the board (BatteryStatus 0x5268, 4112 mV, 100 %, 3000 mAh) */
DMOD_TEST_STEP(a_full_battery_on_the_charger)
{
    regs_t r = { 2985, 4112, 0x5268, 0, 2999, 3000, 0xFFFF, 0xFFFF, 0, 100 };
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.state, (int)DMDRVI_BATTERY_STATE_FULL);
    DMOD_TEST_EXPECT_EQ((int)s.percent, 100);
    DMOD_TEST_EXPECT_EQ((int)s.voltage_mv, 4112);
    DMOD_TEST_EXPECT_EQ((int)s.remaining_mah, 2999);
    DMOD_TEST_EXPECT_EQ((int)s.full_mah, 3000);
    DMOD_TEST_EXPECT_EQ((int)s.temperature_dc, 254);                        /* 298.5 K = 25.4 C */
    DMOD_TEST_EXPECT_TRUE((s.flags & DMDRVI_BATTERY_FLAG_EXTERNAL_POWER) != 0);
    DMOD_TEST_EXPECT_EQ((int)(s.flags & (DMDRVI_BATTERY_FLAG_LOW | DMDRVI_BATTERY_FLAG_CRITICAL)), 0);
    DMOD_TEST_EXPECT_TRUE((s.valid & DMDRVI_BATTERY_VALID_TIME) == 0);      /* (nothing to wait for: no time) */
}

DMOD_TEST_STEP(charging_tells_the_time_to_full)
{
    regs_t r = { 3000, 3900, 0x0048, 450, 1500, 3000, 0xFFFF, 200, 12, 50 };    /* present, not discharging, not full */
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.state, (int)DMDRVI_BATTERY_STATE_CHARGING);
    DMOD_TEST_EXPECT_EQ((int)s.current_ma, 450);
    DMOD_TEST_EXPECT_TRUE((s.valid & DMDRVI_BATTERY_VALID_TIME) != 0);
    DMOD_TEST_EXPECT_EQ((int)s.minutes_to_full, 200);
    DMOD_TEST_EXPECT_EQ((int)s.minutes_to_empty, 0);
    DMOD_TEST_EXPECT_EQ((int)s.cycles, 12);
}

DMOD_TEST_STEP(discharging_tells_the_time_to_empty_and_a_negative_current)
{
    regs_t r = { 2980, 3800, 0x0009, -230, 1800, 2900, 470, 0xFFFF, 3, 62 };   /* DSG + battery present */
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.state, (int)DMDRVI_BATTERY_STATE_DISCHARGING);
    DMOD_TEST_EXPECT_EQ((int)s.current_ma, -230);
    DMOD_TEST_EXPECT_EQ((int)s.minutes_to_empty, 470);
    DMOD_TEST_EXPECT_EQ((int)s.minutes_to_full, 0);
    DMOD_TEST_EXPECT_TRUE((s.flags & DMDRVI_BATTERY_FLAG_EXTERNAL_POWER) == 0);
    DMOD_TEST_EXPECT_EQ((int)s.flags, 0);
}

DMOD_TEST_STEP(a_time_the_chip_does_not_know_is_not_valid)
{
    regs_t r = { 2980, 3800, 0x0009, -230, 1800, 2900, 0xFFFF, 0xFFFF, 3, 62 };
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_TRUE((s.valid & DMDRVI_BATTERY_VALID_TIME) == 0);
    DMOD_TEST_EXPECT_EQ((int)s.minutes_to_empty, 0);
}

DMOD_TEST_STEP(low_and_critical_only_while_discharging)
{
    regs_t r = { 2980, 3500, 0x0009, -200, 400, 2900, 60, 0xFFFF, 3, 14 };
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.flags, (int)DMDRVI_BATTERY_FLAG_LOW);
    r.percent = 5;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.flags, (int)(DMDRVI_BATTERY_FLAG_LOW | DMDRVI_BATTERY_FLAG_CRITICAL));
    r.percent = 40;
    r.status |= GAUGE_STATUS_SYSDWN;                                        /* the chip says: shut down */
    decode(&r, &s);
    DMOD_TEST_EXPECT_TRUE((s.flags & DMDRVI_BATTERY_FLAG_CRITICAL) != 0);
    r.percent = 3;
    r.status = 0x0048;                                                      /* on the charger: nothing is low */
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)(s.flags & (DMDRVI_BATTERY_FLAG_LOW | DMDRVI_BATTERY_FLAG_CRITICAL)), 0);
}

DMOD_TEST_STEP(the_alarm_of_the_chip_makes_it_low)
{
    regs_t r = { 2980, 3300, 0x000D, -100, 100, 2900, 10, 0xFFFF, 3, 30 };     /* DSG + TDA + present */
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_TRUE((s.flags & DMDRVI_BATTERY_FLAG_LOW) != 0);
}

DMOD_TEST_STEP(charging_inhibited_is_idle)
{
    regs_t r = { 3200, 3900, 0x0108, 0, 1500, 3000, 0xFFFF, 0xFFFF, 12, 50 };  /* CHGINH + present */
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.state, (int)DMDRVI_BATTERY_STATE_IDLE);
}

DMOD_TEST_STEP(no_battery_tells_only_that)
{
    regs_t r = { 2980, 4900, 0x0001, 5, 1, 2, 3, 4, 5, 6 };                 /* the battery is not present: the numbers are noise */
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.state, (int)DMDRVI_BATTERY_STATE_NOT_PRESENT);
    DMOD_TEST_EXPECT_EQ((int)s.valid, (int)DMDRVI_BATTERY_VALID_STATE);
    DMOD_TEST_EXPECT_EQ((int)s.voltage_mv, 0);
    DMOD_TEST_EXPECT_EQ((int)s.percent, 0);
}

DMOD_TEST_STEP(percent_and_temperature_are_kept_in_range)
{
    regs_t r = { 100, 4000, 0x0049, 0, 100, 100, 0xFFFF, 0xFFFF, 0, 250 };     /* 10 K: -263.1 C is the chip's lower end */
    dmdrvi_battery_status_t s;
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.percent, 100);
    DMOD_TEST_EXPECT_EQ((int)s.temperature_dc, 100 - 2731);
    r.temperature = 3231;                                                   /* 50 C */
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.temperature_dc, 500);
    r.temperature = 2631;                                                   /* -10 C: a negative one */
    decode(&r, &s);
    DMOD_TEST_EXPECT_EQ((int)s.temperature_dc, -100);
}

DMOD_TEST_STEP(the_block_is_parsed_little_endian)
{
    uint8_t block[GAUGE_BLOCK_SIZE];
    gauge_raw_t raw;
    memset(block, 0, sizeof(block));
    put(block, 0x08, 0x1010);                                               /* 4112 mV */
    put(block, 0x0C, 0xFF1A);                                               /* -230 mA */
    put(block, 0x2C, 0x0064);
    gauge_parse(block, &raw);
    DMOD_TEST_EXPECT_EQ((int)raw.voltage, 4112);
    DMOD_TEST_EXPECT_EQ((int)raw.current, -230);
    DMOD_TEST_EXPECT_EQ((int)raw.percent, 100);
}
