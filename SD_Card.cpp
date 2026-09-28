#include "SD_Card.h"

bool SDCard_Flag = 0;
uint16_t SDCard_Size = 0;
uint16_t Flash_Size = 0;

static File sdLogFile;
static bool sdLoggerOpen = false;
static uint8_t sessionNumber = 0;

void SD_D3_Dis(){
  Set_EXIO(EXIO_PIN4,Low);
  vTaskDelay(pdMS_TO_TICKS(10));
}
void SD_D3_EN(){
  Set_EXIO(EXIO_PIN4,High);
  vTaskDelay(pdMS_TO_TICKS(10));
}

void SD_Init() {
  SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN, -1, -1, -1);
  SD_D3_EN();
  if (SD_MMC.begin("/sdcard", true, true)) {
    printf("SD card init OK\r\n");
  } else {
    printf("SD card init FAIL\r\n");
    return;
  }
  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    printf("No SD card attached\r\n");
    return;
  }
  uint64_t totalBytes = SD_MMC.totalBytes();
  SDCard_Size = totalBytes / (1024 * 1024);
  printf("SD Total: %llu MB\r\n", totalBytes / (1024 * 1024));
}

void Flash_test()
{
  uint32_t flashSize = ESP.getFlashChipSize();
  Flash_Size = flashSize / 1024 / 1024;
  printf("Flash: %d MB\r\n", Flash_Size);
}

bool SD_Logger_Open(void)
{
  if (sdLoggerOpen) return true;
  if (!SD_MMC.cardType() || SD_MMC.cardType() == CARD_NONE) {
    printf("SD Logger: no card\r\n");
    return false;
  }
  if (!SD_MMC.exists("/imu_data")) {
    SD_MMC.mkdir("/imu_data");
  }
  char filename[40];
  uint8_t n = 0;
  while (n < 250) {
    snprintf(filename, sizeof(filename), "/imu_data/session_%03u.csv", n);
    if (!SD_MMC.exists(filename)) break;
    n++;
  }
  sessionNumber = n;
  snprintf(filename, sizeof(filename), "/imu_data/session_%03u.csv", sessionNumber);
  sdLogFile = SD_MMC.open(filename, FILE_WRITE);
  if (!sdLogFile) {
    printf("SD Logger: cannot open %s\r\n", filename);
    sdLoggerOpen = false;
    return false;
  }
  sdLogFile.println("ms_since_boot,utc_epoch_s,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps");
  sdLoggerOpen = true;
  printf("SD Logger opened: %s\r\n", filename);
  return true;
}

bool SD_Logger_IsOpen(void)
{
  return sdLoggerOpen;
}

void SD_Logger_Write(const char *line)
{
  if (!sdLoggerOpen || !sdLogFile) return;
  sdLogFile.println(line);
}

void SD_Logger_Flush(void)
{
  if (!sdLoggerOpen || !sdLogFile) return;
  sdLogFile.flush();
}

void SD_Logger_Close(void)
{
  if (!sdLoggerOpen) return;
  sdLogFile.flush();
  sdLogFile.close();
  sdLoggerOpen = false;
  sessionNumber++;
  printf("SD Logger closed\r\n");
}