#include "RTC_PCF85063.h"
#include <time.h>

datetime_t datetime = {0};

static uint8_t decToBcd(int val);
static int bcdToDec(uint8_t val);

void PCF85063_Init()
{
    uint8_t Value = RTC_CTRL_1_DEFAULT | RTC_CTRL_1_CAP_SEL;
    I2C_Write(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1);
    I2C_Read(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1);
    if (Value & RTC_CTRL_1_STOP)
        printf("PCF85063 init fail, state:%d\r\n", Value);
    else
        printf("PCF85063 running, state:%d\r\n", Value);
}

void PCF85063_Loop(void)
{
    PCF85063_Read_Time(&datetime);
}

void PCF85063_Reset()
{
    uint8_t Value = RTC_CTRL_1_DEFAULT | RTC_CTRL_1_CAP_SEL | RTC_CTRL_1_SR;
    I2C_Write(PCF85063_ADDRESS, RTC_CTRL_1_ADDR, &Value, 1);
}

void PCF85063_Set_Time(datetime_t time)
{
    uint8_t buf[3] = {decToBcd(time.second), decToBcd(time.minute), decToBcd(time.hour)};
    I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, sizeof(buf));
}

void PCF85063_Set_Date(datetime_t date)
{
    uint8_t buf[4] = {decToBcd(date.day), decToBcd(date.dotw), decToBcd(date.month), decToBcd(date.year - YEAR_OFFSET)};
    I2C_Write(PCF85063_ADDRESS, RTC_DAY_ADDR, buf, sizeof(buf));
}

void PCF85063_Set_All(datetime_t time)
{
    uint8_t buf[7] = {decToBcd(time.second), decToBcd(time.minute), decToBcd(time.hour),
                      decToBcd(time.day), decToBcd(time.dotw), decToBcd(time.month),
                      decToBcd(time.year - YEAR_OFFSET)};
    I2C_Write(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, sizeof(buf));
}

void PCF85063_Read_Time(datetime_t *time)
{
    uint8_t buf[7] = {0};
    esp_err_t ret = I2C_Read(PCF85063_ADDRESS, RTC_SECOND_ADDR, buf, sizeof(buf));
    if (ret != ESP_OK)
        printf("PCF85063 read fail\r\n");
    else {
        time->second = bcdToDec(buf[0] & 0x7F);
        time->minute = bcdToDec(buf[1] & 0x7F);
        time->hour = bcdToDec(buf[2] & 0x3F);
        time->day = bcdToDec(buf[3] & 0x3F);
        time->dotw = bcdToDec(buf[4] & 0x07);
        time->month = bcdToDec(buf[5] & 0x1F);
        time->year = bcdToDec(buf[6]) + YEAR_OFFSET;
    }
}

uint32_t PCF85063_GetEpoch(void)
{
    PCF85063_Read_Time(&datetime);
    // An uninitialised RTC reads back as year 0 / month 0 / day 0, which mktime
    // would happily turn into a plausible-looking 1900 epoch. Flag it instead.
    if (datetime.year == 0 || datetime.month == 0 || datetime.day == 0) {
        return RTC_EPOCH_UNSET;
    }
    struct tm t = {0};
    t.tm_year = datetime.year - 1900;
    t.tm_mon = datetime.month - 1;
    t.tm_mday = datetime.day;
    t.tm_hour = datetime.hour;
    t.tm_min = datetime.minute;
    t.tm_sec = datetime.second;
    t.tm_isdst = 0;
    time_t epoch = mktime(&t);
    return (uint32_t)epoch;
}

void PCF85063_SetFromEpoch(uint32_t epoch)
{
    if (epoch == RTC_EPOCH_UNSET) return;
    time_t rawtime = (time_t)epoch;
    struct tm *ti = localtime(&rawtime);
    if (!ti) {
        printf("RTC set failed: localtime(%lu) returned NULL\r\n", (unsigned long)epoch);
        return;
    }
    datetime_t dt;
    dt.year = ti->tm_year + 1900;
    dt.month = ti->tm_mon + 1;
    dt.day = ti->tm_mday;
    dt.dotw = ti->tm_wday;
    dt.hour = ti->tm_hour;
    dt.minute = ti->tm_min;
    dt.second = ti->tm_sec;
    PCF85063_Set_All(dt);
    printf("RTC set to epoch %lu\r\n", (unsigned long)epoch);
}

static uint8_t decToBcd(int val)
{
    return (uint8_t)((val / 10 * 16) + (val % 10));
}

static int bcdToDec(uint8_t val)
{
    return (int)((val / 16 * 10) + (val % 16));
}

void datetime_to_str(char *datetime_str, datetime_t time)
{
    sprintf(datetime_str, "%04d-%02d-%02d %02d:%02d:%02d", time.year, time.month,
            time.day, time.hour, time.minute, time.second);
}
