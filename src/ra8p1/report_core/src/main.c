#include "app_config.h"
#include "app_state.h"

#if defined(__GNUC__)
#define FRAME_ALIGN __attribute__((aligned(32)))
#else
#define FRAME_ALIGN
#endif

static uint8_t g_frame_buffer[CAMERA_FRAME_STRIDE * CAMERA_FRAME_HEIGHT] FRAME_ALIGN;
static app_context_t g_application;

int main(void)
{
    /* FSP-generated hal_entry() may call this initialization sequence instead. */
    app_state_init(&g_application, g_frame_buffer, sizeof(g_frame_buffer));
    for (;;)
    {
        app_state_step(&g_application);
    }
}
