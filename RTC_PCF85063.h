#pragma once

#include "I2C_Driver.h"

#define PCF85063_ADDRESS    (0x51)
#define YEAR_OFFSET         (1970)

// Returned by PCF85063_GetEpoch() when the RTC has never been set, so callers
// can tell "no time available" apart from a legitimate epoch value.
#define RTC_EPOCH_UNSET     (0xFFFFFFFFu)

#define RTC_CTRL_1_ADDR     (0x00)
#define RTC_CTRL_2_ADDR     (0x01)
#define RTC_OFFSET_ADDR     (0x02)
#define RTC_RAM_by_ADDR     (0x03)
#define RTC_SECOND_ADDR     (0x04)
#define RTC_MINUTE_ADDR     (0x05)
#define RTC_HOUR_ADDR       (0x06)
#define RTC_DAY_ADDR        (0x07)
#define RTC_WDAY_ADDR       (0x08)
#define RTC_MONTH_ADDR      (0x09)
#define RTC_YEAR_ADDR       (0x0A)

#define RTC_CTRL_1_EXT_TEST (0x80)
#define RTC_CTRL_1_STOP     (0x20)
#define RTC_CTRL_1_SR       (0X10)
#define RTC_CTRL_1_CIE      (0X04)
#define RTC_CTRL_1_12_24    (0X02)
#define RTC_CTRL_1_CAP_SEL  (0X01)

#define RTC_CTRL_2_AIE      (0X80)
#define RTC_CTRL_2_AF       (0X40)
#define RTC_CTRL_2_MI       (0X20)
#define RTC_CTRL_2_HMI      (0X10)
#define RTC_CTRL_2_TF       (0X08)

#define RTC_ALARM           (0x80)
#define RTC_CTRL_1_DEFAULT  (0x00)
#define RTC_CTRL_2_DEFAULT  (0x00)

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t dotw;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} datetime_t;

extern datetime_t datetime;

void PCF85063_Init(void);
void PCF85063_Loop(void);
void PCF85063_Reset(void);
void PCF85063_Set_Time(datetime_t time);
void PCF85063_Set_Date(datetime_t date);
void PCF85063_Set_All(datetime_t time);
void PCF85063_Read_Time(datetime_t *time);
uint32_t PCF85063_GetEpoch(void);
void PCF85063_SetFromEpoch(uint32_t epoch);
void datetime_to_str(char *datetime_str, datetime_t time);
