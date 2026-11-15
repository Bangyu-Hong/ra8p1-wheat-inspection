#include "web_ui_asset.h"

#include <stddef.h>

#if defined(__arm__) || defined(__thumb__)
#include "crc32_ieee.h"
#endif

#define WEB_UI_OSPI_BASE          (0x80000000UL)
#define WEB_UI_OSPI_END           (0x82000000UL)
#define WEB_UI_PAGE_EXPECTED_CRC  (0xE3AF5867UL)

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
__attribute__((aligned(32), section(".web_asset_image")))
static uint8_t const g_web_ui_page[] =
#include "web_ui_page.inc"
;
#pragma GCC diagnostic pop

static uint8_t const g_recovery_page[] =
    "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
    "<title>RA8P1 Recovery</title><h1>RA8P1 网络恢复页</h1>"
    "<p>OSPI 网页资源校验失败。摄像头、AI 与本地显示仍可继续运行，请查看 SCI9 诊断。</p>";

static web_ui_asset_info_t g_info;
#if !defined(__arm__) && !defined(__thumb__)
static uint32_t web_ui_asset_software_crc32(uint8_t const * p_data, uint32_t data_bytes);
#endif

app_status_t web_ui_asset_init (void)
{
    g_info = (web_ui_asset_info_t)
    {
        .p_page = g_recovery_page,
        .page_bytes = sizeof(g_recovery_page) - 1U,
        .expected_crc32 = WEB_UI_PAGE_EXPECTED_CRC,
    };

#if defined(__arm__) || defined(__thumb__)
    if (((uintptr_t) g_web_ui_page < WEB_UI_OSPI_BASE) ||
        (((uintptr_t) g_web_ui_page + sizeof(g_web_ui_page)) > WEB_UI_OSPI_END))
    {
        return APP_STATUS_DATA;
    }
    if (APP_STATUS_OK != crc32_ieee_calculate(g_web_ui_page,
                                               sizeof(g_web_ui_page) - 1U,
                                               &g_info.actual_crc32))
    {
        return APP_STATUS_IO;
    }
    if (g_info.actual_crc32 != WEB_UI_PAGE_EXPECTED_CRC)
    {
        return APP_STATUS_DATA;
    }
#else
    g_info.actual_crc32 = web_ui_asset_software_crc32(g_web_ui_page,
                                                      sizeof(g_web_ui_page) - 1U);
    if (g_info.actual_crc32 != WEB_UI_PAGE_EXPECTED_CRC)
    {
        return APP_STATUS_DATA;
    }
#endif
    g_info.p_page = g_web_ui_page;
    g_info.page_bytes = sizeof(g_web_ui_page) - 1U;
    g_info.ospi_asset_valid = true;
    return APP_STATUS_OK;
}

#if !defined(__arm__) && !defined(__thumb__)
static uint32_t web_ui_asset_software_crc32 (uint8_t const * p_data,
                                             uint32_t data_bytes)
{
    uint32_t crc = UINT32_MAX;
    uint32_t index;

    for (index = 0U; index < data_bytes; index++)
    {
        uint32_t bit;

        crc ^= p_data[index];
        for (bit = 0U; bit < 8U; bit++)
        {
            uint32_t const mask = (uint32_t) (0U - (crc & 1U));

            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }
    return ~crc;
}
#endif

void web_ui_asset_get_info (web_ui_asset_info_t * p_info)
{
    if (NULL != p_info)
    {
        *p_info = g_info;
    }
}
