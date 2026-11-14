#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "gauge.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmosi.h"
#include "dmi2c_types.h"
#include <errno.h>
#include <string.h>

/*
 * dmbq27220: the driver of the BQ27220 battery gauge (Texas Instruments, one cell, I2C) - the battery of the LILYGO T-Deck Pro. Its node
 * follows the battery interface of dmdrvi (DMDRVI_IOCTL_BATTERY_*): the state, the charge in percent, the voltage, the current, the
 * capacity, the temperature, the time left, the cycles. The driver only reads: the chip is set up (the capacity of the battery, the
 * profile) by whoever made the board, and a driver that changes the memory of a gauge can ruin its learning.
 *
 * The wiring is done by dmdevfs: the section of the gauge and the section of its I2C bus (dmi2c, with the address of the chip) are one
 * friends_group, and the friend_role says who is who:
 *
 *   i2c      the dmi2c node the chip is on (required)
 *
 * The chip is asked when a client asks (there is no thread): a reading is taken from the chip at most once in refresh_ms, a client that asks
 * more often gets the last one. The chip is identified (Control(): DEVICE_NUMBER) before the first reading, again and again until it
 * answers: a bus that is not ready yet, or a chip that is not there, is not an error of the driver.
 *
 * read() of the node gives the state as lines "key: value" (cat /dev/battery), for a shell; a client uses the ioctls.
 */

#define CONTEXT_MAGIC       0x42513237U             /* "BQ27" */
#define IDENTIFY_DELAY_MS   2U                      /* between a sub-command of the chip and its answer */

typedef struct
{
    char   *path;                                   /**< Node of the friend (NULL: not there) */
    void   *file;                                   /**< Kept open once used */
} node_t;

struct dmdrvi_context
{
    uint32_t                    magic;
    uint8_t                     instance;
    uint16_t                    address;            /**< The chip on the bus */
    uint32_t                    refresh_ms;
    gauge_thresholds_t          thresholds;
    node_t                      i2c;
    dmosi_mutex_t               lock;
    bool                        identified;
    uint16_t                    design_mah;
    bool                        have_status;        /**< `status` is a reading (not older than refresh_ms) */
    uint32_t                    read_at;
    dmdrvi_battery_status_t     status;
    char                        report[320];
};

static bool is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == CONTEXT_MAGIC;
}

/* ---- The node of the bus ---- */

static void node_close(node_t *node)
{
    if (node->file != NULL)
        Dmod_FileClose(node->file);
    node->file = NULL;
}

static void node_forget(node_t *node)
{
    node_close(node);
    Dmod_Free(node->path);
    node->path = NULL;
}

static int i2c_transfer(dmdrvi_context_t context, dmi2c_message_t *messages, size_t count)
{
    if (context->i2c.file == NULL && context->i2c.path != NULL)
        context->i2c.file = Dmod_FileOpen(context->i2c.path, "r+");
    if (context->i2c.file == NULL)
        return -ENODEV;
    dmi2c_transfer_t transfer = { messages, count };
    int ret = Dmod_Ioctl(context->i2c.file, dmi2c_ioctl_cmd_transfer, &transfer);
    return ret == 0 ? 0 : (ret < 0 ? ret : -EIO);
}

/* ---- The chip ---- */

/* Registers from `reg` on: the address, then a read with a repeated start */
static int read_registers(dmdrvi_context_t context, uint8_t reg, uint8_t *data, size_t size)
{
    dmi2c_message_t messages[2] = {
        { .address = context->address, .read = false, .data = &reg, .size = 1 },
        { .address = context->address, .read = true,  .data = data, .size = size },
    };
    return i2c_transfer(context, messages, 2);
}

static int read_word(dmdrvi_context_t context, uint8_t reg, uint16_t *value)
{
    uint8_t data[2];
    int ret = read_registers(context, reg, data, sizeof(data));
    if (ret == 0)
        *value = (uint16_t)(data[0] | (data[1] << 8));
    return ret;
}

/* Control(DEVICE_NUMBER): the chip says what it is (a gauge of another make is not read as this one); then what the battery is made for */
static int identify(dmdrvi_context_t context)
{
    uint8_t command[3] = { GAUGE_REG_CONTROL, GAUGE_CONTROL_DEVICE_NUMBER & 0xFFU, GAUGE_CONTROL_DEVICE_NUMBER >> 8 };
    dmi2c_message_t write = { .address = context->address, .read = false, .data = command, .size = sizeof(command) };
    int ret = i2c_transfer(context, &write, 1);
    if (ret != 0)
        return ret;
    dmosi_thread_sleep(IDENTIFY_DELAY_MS);
    uint16_t number = 0;
    ret = read_word(context, GAUGE_REG_MAC_DATA, &number);        /* (the answer of a sub-command is in the MAC data) */
    if (ret != 0)
        return ret;
    if (number != GAUGE_DEVICE_NUMBER)
    {
        DMOD_LOG_ERROR("BQ27220: the chip at 0x%02X says it is 0x%04X\n", (unsigned)context->address, (unsigned)number);
        return -ENODEV;
    }
    uint16_t design = 0;
    if (read_word(context, GAUGE_REG_DESIGN_CAPACITY, &design) == 0)
        context->design_mah = design;
    context->identified = true;
    return 0;
}

/* A reading, from the chip or the last one: the lock is held */
static int reading_locked(dmdrvi_context_t context, dmdrvi_battery_status_t *status)
{
    uint32_t now = dmosi_get_tick_count();
    if (!context->have_status || (uint32_t)(now - context->read_at) >= context->refresh_ms)
    {
        uint8_t block[GAUGE_BLOCK_SIZE];
        gauge_raw_t raw;
        context->have_status = false;
        int ret = context->identified ? 0 : identify(context);
        if (ret == 0)
            ret = read_registers(context, GAUGE_REG_BLOCK, block, sizeof(block));
        if (ret != 0)
        {
            node_close(&context->i2c);                  /* (a handle that failed is opened again) */
            context->identified = false;
            memset(status, 0, sizeof(*status));
            return ret == -ENODEV ? -ENODEV : -EIO;
        }
        gauge_parse(block, &raw);
        gauge_decode(&raw, &context->thresholds, &context->status);
        context->read_at = now;
        context->have_status = true;
    }
    *status = context->status;
    return 0;
}

static int get_status(dmdrvi_context_t context, dmdrvi_battery_status_t *status)
{
    dmosi_mutex_lock(context->lock);
    int ret = reading_locked(context, status);
    dmosi_mutex_unlock(context->lock);
    return ret;
}

static int get_info(dmdrvi_context_t context, dmdrvi_battery_info_t *info)
{
    dmdrvi_battery_status_t status;
    memset(info, 0, sizeof(*info));
    Dmod_SnPrintf(info->driver, sizeof(info->driver), "dmbq27220");
    Dmod_SnPrintf(info->chip, sizeof(info->chip), "BQ27220");
    info->capabilities = DMDRVI_BATTERY_VALID_STATE | DMDRVI_BATTERY_VALID_VOLTAGE | DMDRVI_BATTERY_VALID_CURRENT |
                         DMDRVI_BATTERY_VALID_PERCENT | DMDRVI_BATTERY_VALID_CAPACITY | DMDRVI_BATTERY_VALID_TEMPERATURE |
                         DMDRVI_BATTERY_VALID_TIME | DMDRVI_BATTERY_VALID_CYCLES;
    (void)get_status(context, &status);                 /* (the chip is identified: the design capacity is known) */
    dmosi_mutex_lock(context->lock);
    info->design_capacity_mah = context->identified ? context->design_mah : 0U;
    dmosi_mutex_unlock(context->lock);
    return 0;
}

/* ---- The driver interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
        return NULL;
    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->magic = CONTEXT_MAGIC;
    context->instance = (uint8_t)dmini_get_int(config, NULL, "instance", 0);
    context->address = (uint16_t)dmini_get_int(config, NULL, "address", (int)GAUGE_ADDRESS);
    context->refresh_ms = (uint32_t)dmini_get_int(config, NULL, "refresh_ms", 1000);
    context->thresholds.low_percent = (uint8_t)dmini_get_int(config, NULL, "low_percent", 15);
    context->thresholds.critical_percent = (uint8_t)dmini_get_int(config, NULL, "critical_percent", 5);
    context->lock = dmosi_mutex_create(false);
    if (context->lock == NULL)
    {
        context->magic = 0;
        Dmod_Free(context);
        return NULL;
    }
    /* The node is named after the section of the board's ini ([battery]: /dev/battery, the name clients look for - dmdevfs fills alt_name in);
     * without such a section it is /dev/dmbq27220<instance> */
    memset(dev_num, 0, sizeof(*dev_num));
    dev_num->flags = DMDRVI_NUM_MAJOR | DMDRVI_NUM_ALT_NAME;
    dev_num->major = (dmdrvi_dev_id_t)context->instance;
    return context;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;
    /* The node of the bus is not touched: dmdevfs tears it down before this call */
    Dmod_Free(context->i2c.path);
    dmosi_mutex_destroy(context->lock);
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, void, _friend_changed, ( dmdrvi_context_t context, const dmdrvi_friend_info_t* info ))
{
    if (!is_valid_context(context) || info == NULL || info->friend_role == NULL || strcmp(info->friend_role, "i2c") != 0)
        return;
    dmosi_mutex_lock(context->lock);
    node_forget(&context->i2c);
    context->identified = false;
    context->have_status = false;
    if (info->state == dmdrvi_dev_state_ready && info->node_path != NULL)
    {
        context->i2c.path = Dmod_StrDup(info->node_path);
        if (context->i2c.path == NULL)
            DMOD_LOG_ERROR("BQ27220: out of memory for the path of the bus\n");
    }
    dmosi_mutex_unlock(context->lock);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    return is_valid_context(context) ? context : NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, void, _close, ( dmdrvi_context_t context, void* handle ))
{
}

static const char *const g_state_names[] = { "unknown", "not present", "discharging", "charging", "full", "idle" };

/* The text of read(): the battery as lines "key: value" */
static int make_report(dmdrvi_context_t context)
{
    dmdrvi_battery_status_t s;
    int ret = get_status(context, &s);
    if (ret != 0)
    {
        Dmod_SnPrintf(context->report, sizeof(context->report), "error: %d\n", ret);
        return ret;
    }
    Dmod_SnPrintf(context->report, sizeof(context->report),
                  "state: %s\npercent: %u\nvoltage: %u mV\ncurrent: %d mA\ncapacity: %u / %u mAh (design %u)\ntemperature: %d.%u C\n"
                  "minutes: %u to empty, %u to full\ncycles: %u\nflags: %s%s%s\n",
                  s.state < sizeof(g_state_names) / sizeof(g_state_names[0]) ? g_state_names[s.state] : "?", (unsigned)s.percent,
                  (unsigned)s.voltage_mv, (int)s.current_ma, (unsigned)s.remaining_mah, (unsigned)s.full_mah, (unsigned)context->design_mah,
                  (int)(s.temperature_dc / 10), (unsigned)((s.temperature_dc < 0 ? -s.temperature_dc : s.temperature_dc) % 10),
                  (unsigned)s.minutes_to_empty, (unsigned)s.minutes_to_full, (unsigned)s.cycles,
                  (s.flags & DMDRVI_BATTERY_FLAG_LOW) ? "low " : "", (s.flags & DMDRVI_BATTERY_FLAG_CRITICAL) ? "critical " : "",
                  (s.flags & DMDRVI_BATTERY_FLAG_EXTERNAL_POWER) ? "external-power" : "");
    return 0;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || (buffer == NULL && size != 0) || offset < 0)
        return -EINVAL;
    if (offset == 0 && make_report(context) != 0)
        return -EIO;
    size_t length = strlen(context->report);
    if ((size_t)offset >= length)
        return 0;
    size_t count = length - (size_t)offset < size ? length - (size_t)offset : size;
    memcpy(buffer, context->report + offset, count);
    return (dmdrvi_ssize_t)count;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    return -ENOTSUP;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    if (!is_valid_context(context))
        return -EINVAL;
    switch (command)
    {
        case DMDRVI_IOCTL_BATTERY_GET_INFO:
            return arg == NULL ? -EINVAL : get_info(context, (dmdrvi_battery_info_t *)arg);
        case DMDRVI_IOCTL_BATTERY_GET_STATUS:
            return arg == NULL ? -EINVAL : get_status(context, (dmdrvi_battery_status_t *)arg);
        default:
            return -ENOTTY;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    return 0;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmbq27220, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
        return -EINVAL;
    stat->size = 0;
    stat->mode = 0444;
    return 0;
}

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    return 0;
}

int dmod_deinit(void)
{
    return 0;
}
