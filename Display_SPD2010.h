#pragma once
#include "TCA9554PWR.h"
#include "Touch_SPD2010.h"
#define LCD_Backlight_PIN   5
#define PWM_Channel     1
#define Frequency       20000
#define Resolution      10
#define Dutyfactor      500
#define Backlight_MAX   100

#define EXAMPLE_LCD_WIDTH                   (412)
#define EXAMPLE_LCD_HEIGHT                  (412)
#define EXAMPLE_LCD_COLOR_BITS              (16)

#define ESP_PANEL_HOST_SPI_ID_DEFAULT       (SPI2_HOST)
#define ESP_PANEL_LCD_SPI_MODE              (3)
#define ESP_PANEL_LCD_SPI_CLK_HZ            (40 * 1000 * 1000)
#define ESP_PANEL_LCD_SPI_TRANS_QUEUE_SZ    (10)
#define ESP_PANEL_LCD_SPI_CMD_BITS          (32)
#define ESP_PANEL_LCD_SPI_PARAM_BITS        (8)

#define ESP_PANEL_LCD_SPI_IO_TE             (18)
#define ESP_PANEL_LCD_SPI_IO_SCK            (40)
#define ESP_PANEL_LCD_SPI_IO_DATA0          (46)
#define ESP_PANEL_LCD_SPI_IO_DATA1          (45)
#define ESP_PANEL_LCD_SPI_IO_DATA2          (42)
#define ESP_PANEL_LCD_SPI_IO_DATA3          (41)
#define ESP_PANEL_LCD_SPI_IO_CS             (21)
#define EXAMPLE_LCD_PIN_NUM_RST             (-1)
#define EXAMPLE_LCD_PIN_NUM_BK_LIGHT        (-1)

#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL       (1)
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL

#define ESP_PANEL_HOST_SPI_MAX_TRANSFER_SIZE   (2048)

extern uint8_t LCD_Backlight;

bool SPD2010_Init();
void LCD_Init();
void LCD_addWindow(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend, uint16_t* color);
void Backlight_Init();
void Set_Backlight(uint8_t Light);
