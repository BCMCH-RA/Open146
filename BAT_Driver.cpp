#include "BAT_Driver.h"

float BAT_analogVolts = 0;

void BAT_Init(void)
{
    analogReadResolution(12);
}

float BAT_Get_Volts(void)
{
    int Volts = analogReadMilliVolts(BAT_ADC_PIN);
    BAT_analogVolts = (float)(Volts * 3.0 / 1000.0) / Measurement_offset;
    return BAT_analogVolts;
}

uint8_t BAT_Get_Percent(void)
{
    float v = BAT_Get_Volts();
    if (v >= 4.05f) return 100;
    if (v <= 3.25f) return 0;
    uint8_t pct = (uint8_t)((v - 3.25f) / 0.80f * 100.0f);
    if (pct > 100) pct = 100;
    return pct;
}
