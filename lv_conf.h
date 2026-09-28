/**
 * LVGL Configuration for IMU BLE Streamer
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP    0

#define LV_MEM_CUSTOM      0
#define LV_MEM_SIZE        (48 * 1024)

#define LV_DISP_DEF_REFR_PERIOD  30
#define LV_INDEV_DEF_READ_PERIOD 30
#define LV_TICK_CUSTOM           0

#define LV_USE_LOG          0
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ           0

#define LV_SHADOW_CACHE_SIZE 0
#define LV_CIRCLE_CACHE_SIZE 4

#define LV_DPI_DEF 130

#define LV_DRAW_COMPLEX 1
#define LV_SHADOW_CACHE_SIZE 0

#define LV_IMG_CACHE_DEF_SIZE 0

#define LV_USE_GROUP        1
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR  0

#define LV_FONT_MONTSERRAT_8  0
#define LV_FONT_MONTSERRAT_10 0
#define LV_FONT_MONTSERRAT_12 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 0
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 0
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_26 0
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_30 0
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_34 0
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_38 0
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_42 0
#define LV_FONT_MONTSERRAT_44 0
#define LV_FONT_MONTSERRAT_46 0
#define LV_FONT_MONTSERRAT_48 1

#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK            0
#define LV_FONT_UNSCII_8                 0
#define LV_FONT_UNSCII_16               0

#define LV_FONT_DEFAULT &lv_font_montserrat_16

#define LV_FONT_FMT_TXT_LARGE  0
#define LV_USE_FONT_COMPRESSED  0
#define LV_USE_FONT_SUBPX      0
#if LV_USE_FONT_SUBPX
    #define LV_FONT_SUBPX_BGR  0
#endif
#define LV_USE_FONT_PLACEHOLDER 1

#define LV_TEXT_DEF_FONT_NAME ""
#define LV_TEXT_DEF_FONT_SIZE  12

#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BTN          1
#define LV_USE_BTNMATRIX    1
#define LV_USE_CANVAS       0
#define LV_USE_CHECKBOX     1
#define LV_USE_DROPDOWN     1
#define LV_USE_IMG          1
#define LV_USE_LABEL        1
#define LV_USE_LINE         1
#define LV_USE_ROLLER       1
#define LV_USE_SLIDER       1
#define LV_USE_SWITCH       1
#define LV_USE_TEXTAREA     1
#define LV_USE_TABLE        1

#define LV_USE_ANIMATION        0
#define LV_USE_SHADOW           0
#define LV_USE_BLEND_MODES      0
#define LV_USE_OPA_SCALE        0
#define LV_USE_IMG_TRANSFORM    0
#define LV_USE_GROUP            1
#define LV_USE_GPU_STM32_DMA2D  0

#define LV_USE_THEME_DEFAULT    1
#if LV_USE_THEME_DEFAULT
    #define LV_THEME_DEFAULT_DARK       1
    #define LV_THEME_DEFAULT_GROW       0
    #define LV_THEME_DEFAULT_TRANSITION_TIME 80
#endif
#define LV_USE_THEME_BASIC      1

#define LV_USE_MSG          1

#define LV_USE_DEMO_WIDGETS     0
#define LV_USE_DEMO_MUSIC       0

#endif
