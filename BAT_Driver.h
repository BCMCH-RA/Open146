#pragma once
#include <Arduino.h>

#define BAT_ADC_PIN   8
#define Measurement_offset 0.990476

#define BAT_LOW_VOLTS       3.55f
#define BAT_CRITICAL_VOLTS  3.40f

extern float BAT_analogVolts;

void BAT_Init(void);
float BAT_Get_Volts(void);
uint8_t BAT_Get_Percent(void);
