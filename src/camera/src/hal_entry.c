#include "hal_data.h"
#include "app.h"

void hal_entry (void)
{
    (void) app_init();

    while (true)
    {
        (void) app_poll();
    }
}
