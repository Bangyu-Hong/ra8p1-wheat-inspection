/* generated configuration header file - do not edit */
#ifndef BSP_PIN_CFG_H_
#define BSP_PIN_CFG_H_
#include "r_ioport.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

#define DHT11_DATA (BSP_IO_PORT_00_PIN_10)
#define SD_DAT3 (BSP_IO_PORT_01_PIN_11)
#define USER_KEY (BSP_IO_PORT_03_PIN_03)
#define ESP8266_RESET_N (BSP_IO_PORT_03_PIN_06)
#define LCD_RESET (BSP_IO_PORT_03_PIN_07)
#define LCD_BACKLIGHT (BSP_IO_PORT_03_PIN_08)
#define LIGHT_ADC (BSP_IO_PORT_05_PIN_13)
#define LCD_CS (BSP_IO_PORT_06_PIN_01)
#define ESP8266_RXD0 (BSP_IO_PORT_06_PIN_02)
#define ESP8266_TXD0 (BSP_IO_PORT_06_PIN_03)
#define LCD_RS (BSP_IO_PORT_06_PIN_04)
#define LCD_WR (BSP_IO_PORT_06_PIN_05)
#define CAMERA_RESET (BSP_IO_PORT_07_PIN_09)
#define CAMERA_PWDN (BSP_IO_PORT_07_PIN_10)
#define LCD_RD (BSP_IO_PORT_08_PIN_04)
#define SD_DAT2 (BSP_IO_PORT_13_PIN_01)
#define SD_DAT1 (BSP_IO_PORT_13_PIN_02)
#define SD_DAT0 (BSP_IO_PORT_13_PIN_03)
#define SD_CMD (BSP_IO_PORT_13_PIN_04)
#define SD_CLK (BSP_IO_PORT_13_PIN_05)
#define SD_CARD_DETECT (BSP_IO_PORT_13_PIN_07)

extern const ioport_cfg_t g_bsp_pin_cfg; /* R7KA8P1KFLCAC.pincfg */

void BSP_PinConfigSecurityInit();

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER
#endif /* BSP_PIN_CFG_H_ */
