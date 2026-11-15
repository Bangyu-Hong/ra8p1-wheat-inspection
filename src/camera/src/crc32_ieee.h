#ifndef CRC32_IEEE_H_
#define CRC32_IEEE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_status.h"

#define CRC32_IEEE_ALIGNMENT             (4U)
#define CRC32_IEEE_SELF_TEST_BYTES       (12U)
#define CRC32_IEEE_SELF_TEST_EXPECTED    (0x77D55834UL)

typedef struct st_crc32_ieee_diagnostics
{
    uint32_t     open_attempts;
    uint32_t     references;
    uint32_t     calculations;
    uint32_t     errors;
    uint32_t     self_tests;
    uint32_t     self_test_failures;
    uint32_t     last_crc32;
    int32_t      last_driver_error;
    app_status_t last_status;
    app_status_t last_error_status;
    bool         initialized;
} crc32_ieee_diagnostics_t;

app_status_t crc32_ieee_open(void);
app_status_t crc32_ieee_calculate(uint8_t const * p_data, uint32_t length, uint32_t * p_crc32);
app_status_t crc32_ieee_close(void);
void         crc32_ieee_get_diagnostics(crc32_ieee_diagnostics_t * p_diagnostics);

#endif /* CRC32_IEEE_H_ */
