#pragma once

#include <lvgl.h>
#include "lv_conf.h"
#include <esp_heap_caps.h>
#include "Display_SPD2010.h"
#include "Touch_SPD2010.h"

#define LCD_WIDTH     EXAMPLE_LCD_WIDTH
#define LCD_HEIGHT    EXAMPLE_LCD_HEIGHT
#define LVGL_BUF_LEN  (LCD_WIDTH * LCD_HEIGHT / 10)

#define EXAMPLE_LVGL_TICK_PERIOD_MS  2

extern lv_obj_t *lbl_severity;
extern lv_obj_t *lbl_ist_time;
extern lv_obj_t *lbl_ist_date;
extern lv_obj_t *lbl_tremor_rms;
extern lv_obj_t *lbl_ble_status;
extern lv_obj_t *lbl_rec_status;
extern lv_obj_t *lbl_sample_count;
extern lv_obj_t *lbl_battery;
extern lv_obj_t *lbl_lowbat;

void Lvgl_Init(void);
void Lvgl_Loop(void);
void Lvgl_Update_Display(const char *ist_time, const char *ist_date,
                         const char *ble_status, const char *rec_status,
                         uint32_t samples, float battery_v, uint8_t battery_pct,
                         const char *sev_text, uint32_t sev_rgb, float tremor_rms);
void Lvgl_Set_LowBat(bool low, uint8_t pct);
uint32_t Lvgl_Get_LastTouchMs(void);