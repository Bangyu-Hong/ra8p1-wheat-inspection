#ifndef LCD_RGB565_H_
#define LCD_RGB565_H_

#include <stdint.h>

#define LCD_RGB565_PORT_B_MASK    (0x007FU)
#define LCD_RGB565_PORT_7_MASK    (0xE0E0U)
#define LCD_RGB565_PORT_8_MASK    (0x0007U)

static inline uint16_t lcd_rgb565_from_be_bytes (uint8_t high_byte, uint8_t low_byte)
{
    return (uint16_t) (((uint16_t) high_byte << 8U) | (uint16_t) low_byte);
}

static inline uint16_t lcd_rgb565_port_b_value (uint16_t pixel)
{
    return pixel & LCD_RGB565_PORT_B_MASK;
}

static inline uint16_t lcd_rgb565_port_7_value (uint16_t pixel)
{
    return (uint16_t) ((((pixel >> 7U) & 0x0007U) << 5U) |
                       (((pixel >> 10U) & 0x0007U) << 13U));
}

static inline uint16_t lcd_rgb565_port_8_value (uint16_t pixel)
{
    return (uint16_t) ((pixel >> 13U) & LCD_RGB565_PORT_8_MASK);
}

#endif /* LCD_RGB565_H_ */
