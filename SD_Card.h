#pragma once
#include "Arduino.h"
#include <cstring>
#include "FS.h"

// The SD socket on this board is wired to the chip's SDMMC peripheral, so the
// whole SD_MMC API is conditional on the target having an SDMMC host. Chips
// without one (ESP32-C3/C6/S2/H2/C5) therefore see an empty header, and the
// failure surfaces much later as "'SD_MMC' was not declared" plus the same for
// CARD_NONE, repeated across every call site. Catch the real cause here instead.
#include "soc/soc_caps.h"
#if !defined(SOC_SDMMC_HOST_SUPPORTED)
#error "This sketch needs an SDMMC host (ESP32, ESP32-S3, ESP32-P4). The selected board is not one -- switch to 'Waveshare ESP32-S3-Touch-LCD-1.46'. Note the SD socket is on SDMMC, so it cannot be moved to SPI."
#endif
#include "SD_MMC.h"
#include "TCA9554PWR.h"

#define SD_CLK_PIN      14
#define SD_CMD_PIN      17
#define SD_D0_PIN       16

extern uint16_t SDCard_Size;
extern uint16_t Flash_Size;

void SD_Init();
void Flash_test();

bool SD_Logger_Open(void);
bool SD_Logger_IsOpen(void);
void SD_Logger_Write(const char *line);
void SD_Logger_Flush(void);
void SD_Logger_Close(void);