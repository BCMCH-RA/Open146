#include "I2C_Driver.h"

static SemaphoreHandle_t i2cMutex = NULL;

void I2C_Init(void) {
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_MASTER_FREQ_HZ);
  if (i2cMutex == NULL) {
    i2cMutex = xSemaphoreCreateMutex();
  }
}

void I2C_Lock(void)  { if (i2cMutex) xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)); }
void I2C_Unlock(void){ if (i2cMutex) xSemaphoreGive(i2cMutex); }

esp_err_t I2C_Read(uint8_t Driver_addr, uint8_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    printf("I2C Read lock timeout\r\n");
    return ESP_ERR_TIMEOUT;
  }
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr);
  uint8_t err = Wire.endTransmission(true);
  if (err) {
    printf("I2C Read fail 0x%02X addr 0x%02X\r\n", err, Driver_addr);
    if (i2cMutex) xSemaphoreGive(i2cMutex);
    return err;
  }
  size_t got = Wire.requestFrom(Driver_addr, Length);
  if (got != Length) {
    printf("I2C Read short %u/%u addr 0x%02X\r\n", (unsigned)got, (unsigned)Length, Driver_addr);
    if (i2cMutex) xSemaphoreGive(i2cMutex);
    return ESP_ERR_INVALID_RESPONSE;
  }
  for (uint32_t i = 0; i < Length; i++) {
    *Reg_data++ = Wire.read();
  }
  if (i2cMutex) xSemaphoreGive(i2cMutex);
  return ESP_OK;
}

esp_err_t I2C_Write(uint8_t Driver_addr, uint8_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    printf("I2C Write lock timeout\r\n");
    return ESP_ERR_TIMEOUT;
  }
  Wire.beginTransmission(Driver_addr);
  Wire.write(Reg_addr);
  for (uint32_t i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  uint8_t err = Wire.endTransmission(true);
  if (err) {
    printf("I2C Write fail 0x%02X addr 0x%02X\r\n", err, Driver_addr);
    if (i2cMutex) xSemaphoreGive(i2cMutex);
    return err;
  }
  if (i2cMutex) xSemaphoreGive(i2cMutex);
  return ESP_OK;
}