// SPI SDCard Commands
//
// Copyright (C) 2024 Eric Callahan <arksine.code@gmail.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h>         // memmove
#include "autoconf.h"       // CONFIG_SD_SPI_CS_PIN
#include "command.h"        // DECL_CONSTANT_STR
#include "canboot.h"        // udelay
#include "sdcard.h"         // sdcard_init
#include "board/gpio.h"     // gpio_out_setup
#include "board/misc.h"     // timer_read_time
#include "board/io.h"       // readb
#include "sched.h"          // udelay
#include "spi_software.h"   // spi_software_setup

DECL_CTR("DECL_SD_SPI_CS_PIN " __stringify(CONFIG_SD_SPI_CS_PIN));
extern uint32_t sdcard_cs_gpio; // Generated by buildcommands.py
DECL_CONSTANT_STR("SD_SPI_BUS_NAME", CONFIG_SD_SPI_BUS_NAME);
extern uint32_t sdcard_spi_bus; // Generated by buildcommands.py

#define CRC7_POLY           0x12
#define CRC16_POLY          0x1021
#define SPI_INIT_RATE       400000
#define SPI_XFER_RATE       4000000
#define RESPONSE_TRIES      32

static struct {
    struct spi_config config;
    struct gpio_out cs_pin;
    uint8_t flags;
    uint8_t err;
} sd_spi;

enum {SDF_INITIALIZED = 1, SDF_HIGH_CAPACITY = 2, SDF_WRITE_PROTECTED = 4,
      SDF_DEINIT =  8};

// COMMAND FLAGS
enum {CF_APP_CMD = 1, CF_NOT_EXPECT = 4};

/**********************************************************
 *
 * CRC Helper Methods
 *
 * ********************************************************/

// Standard CRC
static uint8_t
calc_crc7(uint8_t *data, uint16_t length)
{
    uint8_t crc = 0;
    while (length--)
    {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ CRC7_POLY;
            else
                crc = crc << 1;
        }
    }
    return crc | 1;
}

static uint16_t
calc_crc16(uint8_t *data, uint16_t length)
{
    uint16_t crc = 0;
    while (length--)
    {
        crc ^= (*data++ << 8);
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ CRC16_POLY;
            else
                crc = crc << 1;
        }
    }
    return crc;
}

/**********************************************************
 *
 * SPI Device Methods
 *
 * ********************************************************/

static void
spidev_setup(void)
{
    if (CONFIG_SDCARD_SOFTWARE_SPI)
        spi_software_setup(0);
    else
        sd_spi.config = spi_setup(sdcard_spi_bus, 0, SPI_INIT_RATE);
}

static void
spidev_set_rate(uint32_t rate)
{
    if (CONFIG_SDCARD_SOFTWARE_SPI)
        return;
    spi_set_rate(&(sd_spi.config), rate);
}

static void
spidev_prepare(void)
{
    if (CONFIG_SDCARD_SOFTWARE_SPI)
        spi_software_prepare();
    else
        spi_prepare(sd_spi.config);
}

static void
spidev_transfer(uint8_t receive_data, uint16_t len, uint8_t *data)
{
    if (CONFIG_SDCARD_SOFTWARE_SPI)
        spi_software_transfer(receive_data, len, data);
    else
        spi_transfer(sd_spi.config, receive_data, len, data);
}

/**********************************************************
 *
 * SD Card Commands
 *
 * ********************************************************/

static uint8_t
get_response(uint8_t* buf, uint8_t full_resp)
{
    uint8_t ret = 0;
    memset(buf, 0xFF, 8);
    for (uint8_t i = 0; i < RESPONSE_TRIES; i++) {
        spidev_transfer(1, 1, buf);
        ret = readb(buf);
        if (ret != 0xFF) {
            if (full_resp) {
                spidev_transfer(1, 7, buf + 1);
            }
            break;
        }
    }
    return ret;
}

static uint8_t
xfer_cmd(uint8_t command, uint32_t arg, uint8_t* buf, uint8_t full_resp)
{
    memset(buf, 0, 8);
    buf[0] = command | 0x40;
    buf[1] = (arg >> 24) & 0xFF;
    buf[2] = (arg >> 16) & 0xFF;
    buf[3] = (arg >> 8) & 0xFF;
    buf[4] = (arg & 0xFF);
    uint8_t crc = calc_crc7(buf, 5);
    buf[5] = crc;
    spidev_transfer(0, 6, buf);
    return get_response(buf, full_resp);
}

static uint8_t
send_command(uint8_t command, uint32_t arg, uint8_t* buf)
{
    uint8_t ret;
    spidev_prepare();
    gpio_out_write(sd_spi.cs_pin, 0);
    ret = xfer_cmd(command, arg, buf, 1);
    gpio_out_write(sd_spi.cs_pin, 1);
    return ret;
}

static uint8_t
check_command(uint8_t cmd, uint32_t arg, uint8_t* buf, uint8_t flags,
              uint8_t expect, uint8_t attempts)
{
    while (attempts) {
        if (flags & CF_APP_CMD) {
            send_command(SDCMD_APP_CMD, 0, buf);
        }
        uint8_t ret = send_command(cmd, arg, buf);
        if (flags & CF_NOT_EXPECT) {
            if (ret != expect)
                return 1;
        }
        else if (ret == expect)
            return 1;
        attempts--;
        if (attempts)
            udelay((cmd == SDCMD_SEND_OP_COND) ? 100000 : 1000);
    }
    return 0;
}

static uint8_t
find_token(uint8_t token, uint32_t timeout_ms)
{
    uint8_t buf[1];
    while(timeout_ms)
    {
        writeb(buf, 0xFF);
        spidev_transfer(1, 1, buf);
        if (readb(buf) == token) {
            return 1;
        }
        timeout_ms--;
        udelay(1000);
    }
    return 0;
}

uint8_t
sdcard_write_sector(uint8_t *buf, uint32_t sector)
{
    if (!(sd_spi.flags & SDF_INITIALIZED))
        return 0;
    uint32_t offset = sector;
    if (!(sd_spi.flags & SDF_HIGH_CAPACITY))
        offset = sector * SD_SECTOR_SIZE;
    uint16_t crc = calc_crc16(buf, SD_SECTOR_SIZE);
    uint8_t cmd_buf[8];
    uint8_t ret = 0;
    spidev_prepare();
    gpio_out_write(sd_spi.cs_pin, 0);
    ret = xfer_cmd(SDCMD_WRITE_BLOCK, offset, cmd_buf, 0);
    if (ret == 0xFF) {
        gpio_out_write(sd_spi.cs_pin, 1);
        sd_spi.err = SD_ERROR_WRITE_BLOCK;
        return 0;
    }
    // Send Header
    cmd_buf[0] = 0xFE;
    spidev_transfer(0, 1, cmd_buf);
    // Send Data
    spidev_transfer(0, SD_SECTOR_SIZE, buf);
    // Send CRC
    cmd_buf[0] = (crc >> 8) & 0xFF;
    cmd_buf[1] = crc & 0xFF;
    spidev_transfer(0, 2, cmd_buf);
    // Check for successful response
    ret = get_response(cmd_buf, 0);
    if ((ret & 0x1F) == 5)
        ret = 0;
    else
        ret = 2;
    // Wait for card to come out of busy state, 150ms timeout
    if (!find_token(0xFF, 150))
        ret += 1;
    gpio_out_write(sd_spi.cs_pin, 1);
    if (ret > 0) {
        sd_spi.err = SD_ERROR_WRITE_BLOCK;
        return 0;
    }
    return 1;
}

static uint8_t
read_data_block(uint8_t cmd, uint32_t arg, uint8_t* buf, uint32_t length)
{
    uint8_t ret = 0;
    uint8_t cmd_buf[8];
    spidev_prepare();
    gpio_out_write(sd_spi.cs_pin, 0);
    ret = xfer_cmd(cmd, arg, cmd_buf, 0);
    if (ret != 0)
        // Invalid Response
        ret = 1;

    // Find Transfer Start token, 50ms timeout
    if (!find_token(0xFE, 50))
        ret = 2;

    // Read out the response.  This is done regardless
    // of success to make sure the sdcard response is
    // flushed
    memset(buf, 0xFF, length);
    spidev_transfer(1, length, buf);
    // Read and check crc
    memset(cmd_buf, 0xFF, 8);
    spidev_transfer(1, 2, cmd_buf);
    // Exit busy state
    find_token(0xFF, 50);
    gpio_out_write(sd_spi.cs_pin, 1);
    uint16_t recd_crc = (cmd_buf[0] << 8) | cmd_buf[1];
    uint16_t crc = calc_crc16(buf, length);
    if (recd_crc != crc) {
        ret = 3;
    }
    if (ret > 0) {
        sd_spi.err = SD_ERROR_READ_BLOCK;
        return 0;
    }
    return 1;
}

uint8_t
sdcard_read_sector(uint8_t *buf, uint32_t sector)
{
    if (!(sd_spi.flags & SDF_INITIALIZED))
        return 0;
    uint32_t offset = sector;
    if (!(sd_spi.flags & SDF_HIGH_CAPACITY))
        offset = sector * SD_SECTOR_SIZE;
    return read_data_block(
        SDCMD_READ_SINGLE_BLOCK, offset, buf, SD_SECTOR_SIZE
    );
}

static uint8_t
sdcard_check_write_protect(void)
{
    // Read the CSD register to check the write protect
    // bit.  A name change can't be performed on a
    // write protected card.
    uint8_t csd_buf[16];
    if (!read_data_block(SDCMD_SEND_CSD, 0, csd_buf, 16))
        return 0;
    if ((csd_buf[14] & 0x30) != 0)
        // card is write protected
        return 0;
    return 1;
}

uint8_t
sdcard_init(void)
{
    sd_spi.cs_pin = gpio_out_setup(sdcard_cs_gpio, 1);
    spidev_setup();
    // per the spec, delay for 1ms and apply a minimum of 74 clocks
    // with CS high
    udelay(1000);
    spidev_prepare();
    uint8_t buf[8];
    memset(buf, 0xFF, 8);
    for (uint8_t i = 0; i < 10; i++) {
        spidev_transfer(0, 8, buf);
    }
    // attempt to go idle
    if (!check_command(SDCMD_GO_IDLE_STATE, 0, buf, 0, 1, 50)) {
        sd_spi.err = SD_ERROR_NO_IDLE;
        return 0;
    }
    // Check SD Card Version
    uint8_t sd_ver = 0;
    if (!check_command(SDCMD_SEND_IF_COND, 0x10A, buf,
                       CF_NOT_EXPECT, 0xFF, 3))
    {
        sd_spi.err = SD_ERROR_SEND_IF_COND;
        return 0;
    }
    if (buf[0] & 4)
        sd_ver = 1;
    else if (buf[0] == 1 && buf[3] == 1 && buf[4] == 10)
        sd_ver = 2;
    else {
        sd_spi.err = SD_ERROR_SEND_IF_COND;
        return 0;
    }
    // Enable CRC Checks
    if (!check_command(SDCMD_CRC_ON_OFF, 1, buf, 0, 1, 3)) {
        sd_spi.err = SD_ERROR_CRC_ON_OFF;
        return 0;
    }

    // Read OCR Register to determine if voltage is acceptable
    if (!check_command(SDCMD_READ_OCR, 0, buf, 0, 1, 20)) {
        sd_spi.err = SD_ERROR_BAD_OCR;
        return 0;
    }

    if ((buf[2] & 0x30) != 0x30) {
        // Voltage between 3.2-3.4v not supported by this
        // card
        sd_spi.err = SD_ERROR_BAD_OCR;
        return 0;

    }

    // Finsh init and come out of idle.  This can take some time,
    // give up to 20 attempts (100 ms delay between attempts)
    if (!check_command(SDCMD_SEND_OP_COND, (sd_ver == 1) ? 0 : (1 << 30),
                       buf, CF_APP_CMD, 0, 20))
    {
        sd_spi.err = SD_ERROR_SEND_OP_COND;
        return 0;
    }

    if (sd_ver == 2) {
        // read OCR again to determine capacity
        if (!check_command(SDCMD_READ_OCR, 0, buf, 0, 0, 5)) {
            sd_spi.err = SD_ERROR_BAD_OCR;
            return 0;
        }
        if (buf[1] & 0x40)
            sd_spi.flags |= SDF_HIGH_CAPACITY;
    }

    if (!check_command(SDCMD_SET_BLOCKLEN, SD_SECTOR_SIZE, buf, 0, 0, 3)) {
        sd_spi.err = SD_ERROR_SET_BLOCKLEN;
        return 0;
    }
    if (!sdcard_check_write_protect()) {
        sd_spi.flags |= SDF_WRITE_PROTECTED;
        return 0;
    }
    sd_spi.flags |= SDF_INITIALIZED;
    spidev_set_rate(SPI_XFER_RATE);
    return 1;
}

void
sdcard_deinit(void)
{
    if (sd_spi.flags & SDF_DEINIT)
        return;
    sd_spi.flags |= SDF_DEINIT;
    uint8_t buf[8];
    // return to idle and disable crc checks
    check_command(SDCMD_GO_IDLE_STATE, 0, buf, 0, 1, 50);
    send_command(SDCMD_CRC_ON_OFF, 0, buf);
}

uint16_t
sdcard_report_status(void)
{
    return (sd_spi.err << 8) | (sd_spi.flags);
}
