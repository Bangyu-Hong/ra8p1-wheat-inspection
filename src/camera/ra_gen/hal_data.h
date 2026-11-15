/* generated HAL header file - do not edit */
#ifndef HAL_DATA_H_
#define HAL_DATA_H_
#include <stdint.h>
#include "bsp_api.h"
#include "common_data.h"
#include "r_dmac.h"
#include "r_transfer_api.h"
#include "r_sdhi.h"
#include "r_sdmmc_api.h"
#include "rm_block_media_sdmmc.h"
#include "rm_block_media_api.h"
#include "r_adc_b.h"
                      #include "r_adc_api.h"
#include "r_ospi_b.h"
#include "r_spi_flash_api.h"
#include "r_crc.h"
#include "r_crc_api.h"
#include "r_capture_api.h"
            #include "r_ceu.h"
#include "r_iic_master.h"
#include "r_i2c_master_api.h"
#include "r_sci_b_uart.h"
            #include "r_uart_api.h"
FSP_HEADER
/* Transfer on DMAC Instance. */
extern const transfer_instance_t g_sdhi_transfer;

/** Access the DMAC instance using these structures when calling API functions directly (::p_api is not used). */
extern dmac_instance_ctrl_t g_sdhi_transfer_ctrl;
extern const transfer_cfg_t g_sdhi_transfer_cfg;

#ifndef g_sdmmc0_dmac_callback
void g_sdmmc0_dmac_callback(transfer_callback_args_t * p_args);
#endif
/** SDMMC on SDMMC Instance. */
extern const sdmmc_instance_t g_sdmmc0;


/** Access the SDMMC instance using these structures when calling API functions directly (::p_api is not used). */
extern sdhi_instance_ctrl_t g_sdmmc0_ctrl;
extern sdmmc_cfg_t g_sdmmc0_cfg;

#ifndef rm_block_media_sdmmc_callback
void rm_block_media_sdmmc_callback(sdmmc_callback_args_t * p_args);
#endif
extern const rm_block_media_instance_t g_sd_block_media;

/** Access the SDMMC instance using these structures when calling API functions directly (::p_api is not used). */
extern rm_block_media_sdmmc_instance_ctrl_t g_sd_block_media_ctrl;
extern const rm_block_media_cfg_t g_sd_block_media_cfg;

#ifndef sd_media_callback
void sd_media_callback(rm_block_media_callback_args_t * p_args);
#endif
/** ADC on ADC_B instance. */
                    extern const adc_instance_t g_light_adc;

                    /** Access the ADC_B instance using these structures when calling API functions directly (::p_api is not used). */
                    extern adc_b_instance_ctrl_t g_light_adc_ctrl;
                    extern const adc_cfg_t g_light_adc_cfg;
                    extern const adc_b_scan_cfg_t g_light_adc_scan_cfg;

                    #ifndef environment_light_adc_callback
                    void environment_light_adc_callback(adc_callback_args_t * p_args);
                    #endif
#if OSPI_B_CFG_DMAC_SUPPORT_ENABLE
    #include "r_dmac.h"
#endif
#if OSPI_CFG_DOTF_SUPPORT_ENABLE
    #include "r_sce_if.h"
#endif

extern const spi_flash_instance_t g_ospi0;
extern ospi_b_instance_ctrl_t g_ospi0_ctrl;
extern const spi_flash_cfg_t g_ospi0_cfg;
extern const crc_instance_t g_crc0;
extern crc_instance_ctrl_t g_crc0_ctrl;
extern const crc_cfg_t g_crc0_cfg;
/* CEU on CAPTURE instance */
            extern const capture_instance_t g_ceu0;
            /* Access the CEU instance using these structures when calling API functions directly (::p_api is not used). */
            extern ceu_instance_ctrl_t g_ceu0_ctrl;
            extern const capture_cfg_t g_ceu0_cfg;
            #ifndef camera_ceu_callback
            void camera_ceu_callback(capture_callback_args_t * p_args);
            #endif
/* I2C Master on IIC Instance. */
extern const i2c_master_instance_t g_i2c_master0;

/** Access the I2C Master instance using these structures when calling API functions directly (::p_api is not used). */
extern iic_master_instance_ctrl_t g_i2c_master0_ctrl;
extern const i2c_master_cfg_t g_i2c_master0_cfg;

#ifndef camera_i2c0_callback
void camera_i2c0_callback(i2c_master_callback_args_t * p_args);
#endif
/** UART on SCI Instance. */
            extern const uart_instance_t      g_uart0;

            /** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
            extern sci_b_uart_instance_ctrl_t     g_uart0_ctrl;
            extern const uart_cfg_t g_uart0_cfg;
            extern const sci_b_uart_extended_cfg_t g_uart0_cfg_extend;

            #ifndef esp_at_uart0_callback
            void esp_at_uart0_callback(uart_callback_args_t * p_args);
            #endif
/** UART on SCI Instance. */
            extern const uart_instance_t      g_uart9;

            /** Access the UART instance using these structures when calling API functions directly (::p_api is not used). */
            extern sci_b_uart_instance_ctrl_t     g_uart9_ctrl;
            extern const uart_cfg_t g_uart9_cfg;
            extern const sci_b_uart_extended_cfg_t g_uart9_cfg_extend;

            #ifndef camera_uart9_callback
            void camera_uart9_callback(uart_callback_args_t * p_args);
            #endif
void hal_entry(void);
void g_hal_init(void);
FSP_FOOTER
#endif /* HAL_DATA_H_ */
