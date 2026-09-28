#include "Touch_SPD2010.h"
#include "I2C_Driver.h"

struct SPD2010_Touch touch_data = {0};
uint8_t Touch_interrupts=0;

bool I2C_Read_Touch(uint8_t Driver_addr, uint16_t Reg_addr, uint8_t *Reg_data, uint32_t Length)
{
  I2C_Lock();
  Wire.beginTransmission(Driver_addr);
  Wire.write((uint8_t)(Reg_addr >> 8));
  Wire.write((uint8_t)Reg_addr);
  if (Wire.endTransmission(true)){
    printf("Touch I2C read fail\r\n");
    I2C_Unlock();
    return false;
  }
  Wire.requestFrom(Driver_addr, Length);
  while (Wire.available()) {
    *Reg_data++ = Wire.read();
  }
  I2C_Unlock();
  return true;
}

bool I2C_Write_Touch(uint8_t Driver_addr, uint16_t Reg_addr, const uint8_t *Reg_data, uint32_t Length)
{
  I2C_Lock();
  Wire.beginTransmission(Driver_addr);
  Wire.write((uint8_t)(Reg_addr >> 8));
  Wire.write((uint8_t)Reg_addr);
  for (int i = 0; i < Length; i++) {
    Wire.write(*Reg_data++);
  }
  if (Wire.endTransmission(true)) {
    printf("Touch I2C write fail\r\n");
    I2C_Unlock();
    return false;
  }
  I2C_Unlock();
  return true;
}

void IRAM_ATTR Touch_SPD2010_ISR(void) {
  Touch_interrupts = true;
}

uint8_t Touch_Init(void) {
  SPD2010_Touch_Reset();
  SPD2010_Read_cfg();
  pinMode(EXAMPLE_PIN_NUM_TOUCH_INT, INPUT_PULLUP);
  attachInterrupt(EXAMPLE_PIN_NUM_TOUCH_INT, Touch_SPD2010_ISR, FALLING);
  return true;
}

uint8_t SPD2010_Touch_Reset(void)
{
  Set_EXIO(EXIO_PIN1,Low);
  delay(50);
  Set_EXIO(EXIO_PIN1,High);
  delay(50);
  return true;
}

uint16_t SPD2010_Read_cfg(void) {
  read_fw_version();
  return 1;
}

void Touch_Read_Data(void) {
  uint8_t touch_cnt = 0;
  struct SPD2010_Touch touch = {0};
  tp_read_data(&touch);
  noInterrupts();
  touch_cnt = (touch.touch_num > CONFIG_ESP_LCD_TOUCH_MAX_POINTS ? CONFIG_ESP_LCD_TOUCH_MAX_POINTS : touch.touch_num);
  touch_data.touch_num = touch_cnt;
  for (int i = 0; i < touch_cnt; i++) {
    touch_data.rpt[i].x = touch.rpt[i].x;
    touch_data.rpt[i].y = touch.rpt[i].y;
    touch_data.rpt[i].weight = touch.rpt[i].weight;
  }
  interrupts();
}

bool Touch_Get_xy(uint16_t *x, uint16_t *y, uint16_t *strength, uint8_t *point_num, uint8_t max_point_num){
  Touch_Read_Data();
  *point_num = (touch_data.touch_num > max_point_num ? max_point_num : touch_data.touch_num);
  for (size_t i = 0; i < *point_num; i++) {
    x[i] = touch_data.rpt[i].x;
    y[i] = touch_data.rpt[i].y;
    if (strength) {
      strength[i] = touch_data.rpt[i].weight;
    }
  }
  touch_data.touch_num = 0;
  return (*point_num > 0);
}

void example_touchpad_read(void){
  bool tp_pressed = false;
  uint16_t tp_x = 0;
  uint16_t tp_y = 0;
  uint8_t tp_cnt = 0;
  tp_pressed = Touch_Get_xy(&tp_x, &tp_y, NULL, &tp_cnt, 1);
}

void Touch_Loop(void){
  example_touchpad_read();
}

esp_err_t write_tp_point_mode_cmd()
{
  uint8_t sample_data[4];
  sample_data[0] = 0x50;
  sample_data[1] = 0x00;
  sample_data[2] = 0x00;
  sample_data[3] = 0x00;
  I2C_Write_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])), &sample_data[2], 2);
  esp_rom_delay_us(200);
  return ESP_OK;
}

esp_err_t write_tp_start_cmd()
{
  uint8_t sample_data[4];
  sample_data[0] = 0x46;
  sample_data[1] = 0x00;
  sample_data[2] = 0x00;
  sample_data[3] = 0x00;
  I2C_Write_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])), &sample_data[2], 2);
  esp_rom_delay_us(200);
  return ESP_OK;
}

esp_err_t write_tp_cpu_start_cmd()
{
  uint8_t sample_data[4];
  sample_data[0] = 0x04;
  sample_data[1] = 0x00;
  sample_data[2] = 0x01;
  sample_data[3] = 0x00;
  I2C_Write_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),&sample_data[2], 2);
  esp_rom_delay_us(200);
  return ESP_OK;
}

esp_err_t write_tp_clear_int_cmd()
{
  uint8_t sample_data[4];
  sample_data[0] = 0x02;
  sample_data[1] = 0x00;
  sample_data[2] = 0x01;
  sample_data[3] = 0x00;
  I2C_Write_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),&sample_data[2], 2);
  esp_rom_delay_us(200);
  return ESP_OK;
}

esp_err_t read_tp_status_length(tp_status_t *tp_status)
{
  uint8_t sample_data[4];
  sample_data[0] = 0x20;
  sample_data[1] = 0x00;
  I2C_Read_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),sample_data, 4);
  esp_rom_delay_us(200);
  tp_status->status_low.pt_exist = (sample_data[0] & 0x01);
  tp_status->status_low.gesture = (sample_data[0] & 0x02);
  tp_status->status_low.aux = ((sample_data[0] & 0x08));
  tp_status->status_high.tic_busy = ((sample_data[1] & 0x80) >> 7);
  tp_status->status_high.tic_in_bios = ((sample_data[1] & 0x40) >> 6);
  tp_status->status_high.tic_in_cpu = ((sample_data[1] & 0x20) >> 5);
  tp_status->status_high.tint_low = ((sample_data[1] & 0x10) >> 4);
  tp_status->status_high.cpu_run = ((sample_data[1] & 0x08) >> 3);
  tp_status->read_len = (sample_data[3] << 8 | sample_data[2]);
  return ESP_OK;
}

esp_err_t read_tp_hdp(tp_status_t *tp_status, SPD2010_Touch *touch)
{
  uint8_t sample_data[4+(10*6)];
  uint8_t i, offset;
  uint8_t check_id;
  sample_data[0] = 0x00;
  sample_data[1] = 0x03;
  I2C_Read_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),sample_data, tp_status->read_len);

  check_id = sample_data[4];
  if ((check_id <= 0x0A) && tp_status->status_low.pt_exist) {
    touch->touch_num = ((tp_status->read_len - 4)/6);
    touch->gesture = 0x00;
    for (i = 0; i < touch->touch_num; i++) {
      offset = i*6;
      touch->rpt[i].id = sample_data[4 + offset];
      touch->rpt[i].x = (((sample_data[7 + offset] & 0xF0) << 4)| sample_data[5 + offset]);
      touch->rpt[i].y = (((sample_data[7 + offset] & 0x0F) << 8)| sample_data[6 + offset]);
      touch->rpt[i].weight = sample_data[8 + offset];
    }
    if ((touch->rpt[0].weight != 0) && (touch->down != 1)) {
      touch->down = 1;
      touch->up = 0;
      touch->down_x = touch->rpt[0].x;
      touch->down_y = touch->rpt[0].y;
    } else if ((touch->rpt[0].weight == 0) && (touch->down == 1)) {
      touch->up = 1;
      touch->down = 0;
      touch->up_x = touch->rpt[0].x;
      touch->up_y = touch->rpt[0].y;
    }
  } else if ((check_id == 0xF6) && tp_status->status_low.gesture) {
    touch->touch_num = 0x00;
    touch->up = 0;
    touch->down = 0;
    touch->gesture = sample_data[6] & 0x07;
  } else {
    touch->touch_num = 0x00;
    touch->gesture = 0x00;
  }
  return ESP_OK;
}

esp_err_t read_tp_hdp_status(tp_hdp_status_t *tp_hdp_status)
{
  uint8_t sample_data[8];
  sample_data[0] = 0xFC;
  sample_data[1] = 0x02;
  I2C_Read_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),sample_data, 8);
  tp_hdp_status->status = sample_data[5];
  tp_hdp_status->next_packet_len = (sample_data[2] | sample_data[3] << 8);
  return ESP_OK;
}

esp_err_t Read_HDP_REMAIN_DATA(tp_hdp_status_t *tp_hdp_status)
{
  uint8_t sample_data[32];
  sample_data[0] = 0x00;
  sample_data[1] = 0x03;
  I2C_Read_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),sample_data, tp_hdp_status->next_packet_len);
  return ESP_OK;
}

esp_err_t read_fw_version()
{
  uint8_t sample_data[18];
  uint16_t DVer;
  uint32_t Dummy, PID, ICName_H, ICName_L;
  sample_data[0] = 0x26;
  sample_data[1] = 0x00;
  I2C_Read_Touch(SPD2010_ADDR, (((uint16_t)sample_data[0] << 8) | (sample_data[1])),sample_data, 18);
  Dummy = ((sample_data[0] << 24) | (sample_data[1] << 16) | (sample_data[3] << 8) | (sample_data[0]));
  DVer = ((sample_data[5] << 8) | (sample_data[4]));
  PID = ((sample_data[9] << 24) | (sample_data[8] << 16) | (sample_data[7] << 8) | (sample_data[6]));
  ICName_L = ((sample_data[13] << 24) | (sample_data[12] << 16) | (sample_data[11] << 8) | (sample_data[10]));
  ICName_H = ((sample_data[17] << 24) | (sample_data[16] << 16) | (sample_data[15] << 8) | (sample_data[14]));
  printf("Touch FW: DVer[%d], PID[%d], Name[%d-%d]\r\n", DVer, PID, ICName_H, ICName_L);
  return ESP_OK;
}

esp_err_t tp_read_data(SPD2010_Touch *touch)
{
  tp_status_t tp_status = {0};
  tp_hdp_status_t tp_hdp_status = {0};
  read_tp_status_length(&tp_status);
  if (tp_status.status_high.tic_in_bios) {
    write_tp_clear_int_cmd();
    write_tp_cpu_start_cmd();
  } else if (tp_status.status_high.tic_in_cpu) {
    write_tp_point_mode_cmd();
    write_tp_start_cmd();
    write_tp_clear_int_cmd();
  } else if (tp_status.status_high.cpu_run && tp_status.read_len == 0) {
    write_tp_clear_int_cmd();
  } else if (tp_status.status_low.pt_exist || tp_status.status_low.gesture) {
    read_tp_hdp(&tp_status, touch);
hdp_done_check:
    read_tp_hdp_status(&tp_hdp_status);
    if (tp_hdp_status.status == 0x82) {
      write_tp_clear_int_cmd();
    }
    else if (tp_hdp_status.status == 0x00) {
      Read_HDP_REMAIN_DATA(&tp_hdp_status);
      goto hdp_done_check;
    }
  } else if (tp_status.status_high.cpu_run && tp_status.status_low.aux) {
    write_tp_clear_int_cmd();
  }
  return ESP_OK;
}
