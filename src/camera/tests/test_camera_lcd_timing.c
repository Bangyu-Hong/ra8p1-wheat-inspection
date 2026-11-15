#include <stdio.h>

#include "camera_sensor.h"
#include "lcd_i8080_bus.h"

#define TEST_CHECK(condition)                                                       \
    do                                                                              \
    {                                                                               \
        if (!(condition))                                                           \
        {                                                                           \
            (void) printf("check failed: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                               \
        }                                                                           \
    } while (0)

int main (void)
{
    TEST_CHECK(25U == CAMERA_SENSOR_VGA_FRAME_RATE);
    TEST_CHECK(40000U == CAMERA_SENSOR_VGA_FRAME_PERIOD_US);
    TEST_CHECK(0x1AU == CAMERA_SENSOR_PLL_CONTROL0);
    TEST_CHECK(0x21U == CAMERA_SENSOR_PLL_CONTROL1);
    TEST_CHECK(0x69U == CAMERA_SENSOR_PLL_CONTROL2);
    TEST_CHECK(0x13U == CAMERA_SENSOR_PLL_CONTROL3);
    TEST_CHECK(0x01U == CAMERA_SENSOR_SYSTEM_ROOT_DIVIDER);
    TEST_CHECK(1528U == CAMERA_SENSOR_VGA_HTS);
    TEST_CHECK(1100U == CAMERA_SENSOR_VGA_VTS);

    TEST_CHECK(12U == LCD_I8080_STROBE_DELAY_ITERATIONS);
    TEST_CHECK(66U == LCD_I8080_ST7796S_WRITE_CYCLE_MIN_NS);
    TEST_CHECK(15U == LCD_I8080_ST7796S_CONTROL_PULSE_MIN_NS);
    TEST_CHECK(10U == LCD_I8080_ST7796S_DATA_SETUP_HOLD_MIN_NS);

    (void) printf("timing-contract sensor_fps=%u period_us=%u hts=%u vts=%u lcd_nops=%u\n",
                  (unsigned int) CAMERA_SENSOR_VGA_FRAME_RATE,
                  (unsigned int) CAMERA_SENSOR_VGA_FRAME_PERIOD_US,
                  (unsigned int) CAMERA_SENSOR_VGA_HTS,
                  (unsigned int) CAMERA_SENSOR_VGA_VTS,
                  (unsigned int) LCD_I8080_STROBE_DELAY_ITERATIONS);

    return 0;
}
