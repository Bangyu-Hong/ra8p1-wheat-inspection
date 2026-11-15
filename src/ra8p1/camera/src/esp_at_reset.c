#include "esp_at_reset.h"

#include "hal_data.h"

#define ESP_AT_RESET_PIN  BSP_IO_PORT_03_PIN_06

app_status_t esp_at_reset_set_asserted (bool asserted)
{
    bsp_io_level_t const level = asserted ? BSP_IO_LEVEL_LOW : BSP_IO_LEVEL_HIGH;
    fsp_err_t const error = g_ioport.p_api->pinWrite(g_ioport.p_ctrl,
                                                     ESP_AT_RESET_PIN,
                                                     level);

    return (FSP_SUCCESS == error) ? APP_STATUS_OK : APP_STATUS_IO;
}
