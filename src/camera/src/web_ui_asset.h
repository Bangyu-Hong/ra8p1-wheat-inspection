#ifndef WEB_UI_ASSET_H_
#define WEB_UI_ASSET_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

typedef struct st_web_ui_asset_info
{
    uint8_t const * p_page;
    uint32_t page_bytes;
    uint32_t expected_crc32;
    uint32_t actual_crc32;
    bool ospi_asset_valid;
} web_ui_asset_info_t;

app_status_t web_ui_asset_init(void);
void web_ui_asset_get_info(web_ui_asset_info_t * p_info);

#endif /* WEB_UI_ASSET_H_ */
