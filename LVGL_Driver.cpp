#include "LVGL_Driver.h"

static lv_display_t *disp_handle = NULL;
static lv_indev_t *indev_handle = NULL;
static uint8_t draw_buf1[ LVGL_BUF_LEN * 2 ];
static uint8_t draw_buf2[ LVGL_BUF_LEN * 2 ];
static volatile uint32_t last_touch_ms = 0;

lv_obj_t *lbl_severity = NULL;
lv_obj_t *lbl_ist_time = NULL;
lv_obj_t *lbl_ist_date = NULL;
lv_obj_t *lbl_tremor_rms = NULL;
lv_obj_t *lbl_ble_status = NULL;
lv_obj_t *lbl_rec_status = NULL;
lv_obj_t *lbl_sample_count = NULL;
lv_obj_t *lbl_battery = NULL;
lv_obj_t *lbl_lowbat = NULL;

void Lvgl_print(const char * buf)
{
}

static void lv_tick_cb(void *arg)
{
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void Lvgl_Display_LCD(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    LCD_addWindow(area->x1, area->y1, area->x2, area->y2, (uint16_t *)px_map);
    lv_display_flush_ready(disp_drv);
}

void Lvgl_Touchpad_Read(lv_indev_t * indev_drv, lv_indev_data_t * data)
{
    uint16_t x[1] = {0};
    uint16_t y[1] = {0};
    uint8_t cnt = 0;
    bool pressed = Touch_Get_xy(&x[0], &y[0], NULL, &cnt, 1);
    if (pressed && (cnt > 0)) {
        data->point.x = x[0];
        data->point.y = y[0];
        data->state = LV_INDEV_STATE_PRESSED;
        last_touch_ms = millis();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

uint32_t Lvgl_Get_LastTouchMs(void)
{
    return last_touch_ms;
}

void Lvgl_Init(void)
{
    lv_init();

    disp_handle = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    lv_display_set_buffers(disp_handle, draw_buf1, draw_buf2, sizeof(draw_buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_color_format(disp_handle, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp_handle, Lvgl_Display_LCD);

    indev_handle = lv_indev_create();
    lv_indev_set_type(indev_handle, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_handle, Lvgl_Touchpad_Read);

    esp_timer_handle_t lvgl_tick_timer;
    esp_timer_create_args_t tick_args = {
        .callback = &lv_tick_cb,
        .name = "lvgl_tick"
    };
    esp_timer_create(&tick_args, &lvgl_tick_timer);
    esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "TREMOR MONITOR");
    lv_obj_set_style_text_color(title, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lbl_severity = lv_label_create(scr);
    lv_label_set_text(lbl_severity, "CALM");
    lv_obj_set_width(lbl_severity, LCD_WIDTH - 24);
    lv_obj_set_style_bg_color(lbl_severity, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_bg_opa(lbl_severity, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(lbl_severity, 14, 0);
    lv_obj_set_style_pad_all(lbl_severity, 30, 0);
    lv_obj_set_style_text_color(lbl_severity, lv_color_hex(0x000000), 0);
    lv_obj_set_style_text_font(lbl_severity, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_align(lbl_severity, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_severity, LV_ALIGN_TOP_MID, 0, 30);

    lbl_ist_time = lv_label_create(scr);
    lv_label_set_text(lbl_ist_time, "IST: --:--:--");
    lv_obj_set_style_text_color(lbl_ist_time, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_ist_time, &lv_font_montserrat_48, 0);
    lv_obj_align(lbl_ist_time, LV_ALIGN_TOP_MID, 0, 165);

    lbl_ist_date = lv_label_create(scr);
    lv_label_set_text(lbl_ist_date, "---- -- -- --");
    lv_obj_set_style_text_color(lbl_ist_date, lv_color_hex(0xB0BEC5), 0);
    lv_obj_set_style_text_font(lbl_ist_date, &lv_font_montserrat_28, 0);
    lv_obj_align(lbl_ist_date, LV_ALIGN_TOP_MID, 0, 242);

    lbl_tremor_rms = lv_label_create(scr);
    lv_label_set_text(lbl_tremor_rms, "RMS: 0.0 dps");
    lv_obj_set_style_text_color(lbl_tremor_rms, lv_color_hex(0x00E676), 0);
    lv_obj_set_style_text_font(lbl_tremor_rms, &lv_font_montserrat_24, 0);
    lv_obj_align(lbl_tremor_rms, LV_ALIGN_TOP_MID, 0, 292);

    lbl_ble_status = lv_label_create(scr);
    lv_label_set_text(lbl_ble_status, "BLE: Disconnected");
    lv_obj_set_style_text_color(lbl_ble_status, lv_color_hex(0xFF4444), 0);
    lv_obj_set_style_text_font(lbl_ble_status, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_ble_status, LV_ALIGN_TOP_MID, 0, 330);

    lbl_rec_status = lv_label_create(scr);
    lv_label_set_text(lbl_rec_status, "Recording: OFF");
    lv_obj_set_style_text_color(lbl_rec_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_rec_status, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_rec_status, LV_ALIGN_TOP_MID, 0, 352);

    lbl_sample_count = lv_label_create(scr);
    lv_label_set_text(lbl_sample_count, "Samples: 0");
    lv_obj_set_style_text_color(lbl_sample_count, lv_color_hex(0x44AAFF), 0);
    lv_obj_set_style_text_font(lbl_sample_count, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_sample_count, LV_ALIGN_TOP_MID, 0, 374);

    lbl_battery = lv_label_create(scr);
    lv_label_set_text(lbl_battery, "Bat: --.--V --%");
    lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0xFFAA00), 0);
    lv_obj_set_style_text_font(lbl_battery, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_battery, LV_ALIGN_TOP_MID, 0, 396);

    lbl_lowbat = lv_label_create(scr);
    lv_label_set_text(lbl_lowbat, "LOW BATTERY");
    lv_obj_set_width(lbl_lowbat, LCD_WIDTH - 24);
    lv_obj_set_style_bg_color(lbl_lowbat, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_bg_opa(lbl_lowbat, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(lbl_lowbat, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_lowbat, &lv_font_montserrat_24, 0);
    lv_obj_set_style_radius(lbl_lowbat, 14, 0);
    lv_obj_set_style_pad_all(lbl_lowbat, 12, 0);
    lv_obj_set_style_text_align(lbl_lowbat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_lowbat, LV_ALIGN_TOP_MID, 0, 292);
    lv_obj_add_flag(lbl_lowbat, LV_OBJ_FLAG_HIDDEN);
}

void Lvgl_Update_Display(const char *ist_time, const char *ist_date,
                         const char *ble_status, const char *rec_status,
                         uint32_t samples, float battery_v, uint8_t battery_pct,
                         const char *sev_text, uint32_t sev_rgb, float tremor_rms_dps)
{
    if (lbl_severity && sev_text && sev_rgb) {
        lv_label_set_text(lbl_severity, sev_text);
        lv_obj_set_style_bg_color(lbl_severity, lv_color_hex(sev_rgb), 0);
    }

    if (lbl_ist_time && ist_time)
        lv_label_set_text(lbl_ist_time, ist_time);

    if (lbl_ist_date && ist_date)
        lv_label_set_text(lbl_ist_date, ist_date);

    if (lbl_tremor_rms) {
        static char buf[24];
        snprintf(buf, sizeof(buf), "RMS: %.1f dps", tremor_rms_dps);
        lv_label_set_text(lbl_tremor_rms, buf);
    }

    if (lbl_ble_status && ble_status) {
        lv_label_set_text(lbl_ble_status, ble_status);
        if (strstr(ble_status, "Connected"))
            lv_obj_set_style_text_color(lbl_ble_status, lv_color_hex(0x00FF00), 0);
        else
            lv_obj_set_style_text_color(lbl_ble_status, lv_color_hex(0xFF4444), 0);
    }

    if (lbl_rec_status && rec_status) {
        lv_label_set_text(lbl_rec_status, rec_status);
        if (strstr(rec_status, "ON"))
            lv_obj_set_style_text_color(lbl_rec_status, lv_color_hex(0xFF4444), 0);
        else
            lv_obj_set_style_text_color(lbl_rec_status, lv_color_hex(0x888888), 0);
    }

    if (lbl_sample_count) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Samples: %lu", (unsigned long)samples);
        lv_label_set_text(lbl_sample_count, buf);
    }

    if (lbl_battery) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Bat: %.2fV %u%%", battery_v, battery_pct);
        lv_label_set_text(lbl_battery, buf);
        if (battery_pct <= 20)
            lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0xFF4444), 0);
        else if (battery_pct <= 40)
            lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0xFFA726), 0);
        else
            lv_obj_set_style_text_color(lbl_battery, lv_color_hex(0x00E676), 0);
    }
}

void Lvgl_Set_LowBat(bool low, uint8_t pct)
{
    if (!lbl_lowbat) return;
    if (low) {
        char buf[32];
        snprintf(buf, sizeof(buf), "LOW BATTERY %u%%", pct);
        lv_label_set_text(lbl_lowbat, buf);
        lv_obj_clear_flag(lbl_lowbat, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(lbl_lowbat, LV_OBJ_FLAG_HIDDEN);
    }
}

void Lvgl_Loop(void)
{
    lv_timer_handler();
}