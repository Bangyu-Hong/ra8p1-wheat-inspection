#include <stdint.h>
#include <stdio.h>

#include "lcd_rgb565.h"

typedef enum e_test_port
{
    TEST_PORT_B = 0,
    TEST_PORT_7,
    TEST_PORT_8
} test_port_t;

typedef struct st_test_bit_mapping
{
    uint8_t     pixel_bit;
    test_port_t port;
    uint8_t     port_bit;
} test_bit_mapping_t;

static void expected_port_values(uint16_t pixel,
                                 uint16_t * p_port_b,
                                 uint16_t * p_port_7,
                                 uint16_t * p_port_8);
static uint16_t reconstruct_pixel(uint16_t port_b, uint16_t port_7, uint16_t port_8);

int main (void)
{
    static uint8_t const primary_color_bytes[][2] =
    {
        {0xF8U, 0x00U},
        {0x07U, 0xE0U},
        {0x00U, 0x1FU},
    };
    static uint16_t const primary_colors[] = {0xF800U, 0x07E0U, 0x001FU};

    for (uint32_t value = 0U; value <= UINT16_MAX; value++)
    {
        uint16_t const pixel = (uint16_t) value;
        uint8_t const high_byte = (uint8_t) (pixel >> 8U);
        uint8_t const low_byte = (uint8_t) pixel;
        uint16_t const port_b = lcd_rgb565_port_b_value(pixel);
        uint16_t const port_7 = lcd_rgb565_port_7_value(pixel);
        uint16_t const port_8 = lcd_rgb565_port_8_value(pixel);
        uint16_t expected_b;
        uint16_t expected_7;
        uint16_t expected_8;

        expected_port_values(pixel, &expected_b, &expected_7, &expected_8);

        if ((pixel != lcd_rgb565_from_be_bytes(high_byte, low_byte)) ||
            (port_b != expected_b) ||
            (port_7 != expected_7) ||
            (port_8 != expected_8) ||
            (0U != (port_b & (uint16_t) ~LCD_RGB565_PORT_B_MASK)) ||
            (0U != (port_7 & (uint16_t) ~LCD_RGB565_PORT_7_MASK)) ||
            (0U != (port_8 & (uint16_t) ~LCD_RGB565_PORT_8_MASK)) ||
            (pixel != reconstruct_pixel(port_b, port_7, port_8)))
        {
            (void) printf("rgb565 validation failed at 0x%04X\n", (unsigned int) pixel);
            return 1;
        }
    }

    for (uint32_t index = 0U;
         index < (uint32_t) (sizeof(primary_colors) / sizeof(primary_colors[0]));
         index++)
    {
        uint16_t const pixel = primary_colors[index];

        if (pixel != lcd_rgb565_from_be_bytes(primary_color_bytes[index][0],
                                              primary_color_bytes[index][1]))
        {
            (void) printf("primary-color byte-order validation failed at 0x%04X\n",
                          (unsigned int) pixel);
            return 1;
        }
    }

    (void) printf("lcd-rgb565 status=ok values=65536 primary_colors=3 byte_order=big\n");
    return 0;
}

static void expected_port_values (uint16_t pixel,
                                  uint16_t * p_port_b,
                                  uint16_t * p_port_7,
                                  uint16_t * p_port_8)
{
    static test_bit_mapping_t const mapping[] =
    {
        {0U,  TEST_PORT_B, 0U},
        {1U,  TEST_PORT_B, 1U},
        {2U,  TEST_PORT_B, 2U},
        {3U,  TEST_PORT_B, 3U},
        {4U,  TEST_PORT_B, 4U},
        {5U,  TEST_PORT_B, 5U},
        {6U,  TEST_PORT_B, 6U},
        {7U,  TEST_PORT_7, 5U},
        {8U,  TEST_PORT_7, 6U},
        {9U,  TEST_PORT_7, 7U},
        {10U, TEST_PORT_7, 13U},
        {11U, TEST_PORT_7, 14U},
        {12U, TEST_PORT_7, 15U},
        {13U, TEST_PORT_8, 0U},
        {14U, TEST_PORT_8, 1U},
        {15U, TEST_PORT_8, 2U},
    };

    *p_port_b = 0U;
    *p_port_7 = 0U;
    *p_port_8 = 0U;
    for (uint32_t index = 0U; index < (uint32_t) (sizeof(mapping) / sizeof(mapping[0])); index++)
    {
        if (0U == (pixel & (uint16_t) (1UL << mapping[index].pixel_bit)))
        {
            continue;
        }

        switch (mapping[index].port)
        {
            case TEST_PORT_B:
                *p_port_b |= (uint16_t) (1UL << mapping[index].port_bit);
                break;

            case TEST_PORT_7:
                *p_port_7 |= (uint16_t) (1UL << mapping[index].port_bit);
                break;

            case TEST_PORT_8:
                *p_port_8 |= (uint16_t) (1UL << mapping[index].port_bit);
                break;

            default:
                break;
        }
    }
}

static uint16_t reconstruct_pixel (uint16_t port_b, uint16_t port_7, uint16_t port_8)
{
    return (uint16_t) ((port_b & LCD_RGB565_PORT_B_MASK) |
                       (((port_7 >> 5U) & 0x0007U) << 7U) |
                       (((port_7 >> 13U) & 0x0007U) << 10U) |
                       ((port_8 & LCD_RGB565_PORT_8_MASK) << 13U));
}
