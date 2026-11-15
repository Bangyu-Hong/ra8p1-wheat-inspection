#include "ai_interaction.h"

#include <stddef.h>

void ai_interaction_init (ai_interaction_t * p_interaction)
{
    if (NULL != p_interaction)
    {
        *p_interaction = (ai_interaction_t)
        {
            .state         = AI_INTERACTION_LIVE_PREVIEW,
            .result_status = APP_STATUS_NOT_INITIALIZED,
        };
    }
}

ai_interaction_action_t ai_interaction_on_press (ai_interaction_t * p_interaction)
{
    if (NULL == p_interaction)
    {
        return AI_INTERACTION_ACTION_NONE;
    }

    if (AI_INTERACTION_LIVE_PREVIEW == p_interaction->state)
    {
        p_interaction->state = AI_INTERACTION_FREEZE_PENDING;
        p_interaction->freeze_requests++;
        return AI_INTERACTION_ACTION_REQUEST_FREEZE;
    }

    if (AI_INTERACTION_RESULT_HOLD == p_interaction->state)
    {
        p_interaction->resume_requests++;
        return AI_INTERACTION_ACTION_RESUME_PREVIEW;
    }

    p_interaction->busy_presses++;
    return AI_INTERACTION_ACTION_REPORT_BUSY;
}

ai_interaction_action_t ai_interaction_on_frame_held (ai_interaction_t * p_interaction)
{
    if ((NULL == p_interaction) ||
        (AI_INTERACTION_FREEZE_PENDING != p_interaction->state))
    {
        return AI_INTERACTION_ACTION_NONE;
    }

    p_interaction->state = AI_INTERACTION_INFERENCING;
    p_interaction->inference_requests++;
    return AI_INTERACTION_ACTION_RUN_INFERENCE;
}

ai_interaction_action_t ai_interaction_on_inference_complete (ai_interaction_t * p_interaction,
                                                               app_status_t result_status)
{
    if ((NULL == p_interaction) ||
        (AI_INTERACTION_INFERENCING != p_interaction->state))
    {
        return AI_INTERACTION_ACTION_NONE;
    }

    p_interaction->state = AI_INTERACTION_RESULT_HOLD;
    p_interaction->result_status = result_status;
    p_interaction->completed_results++;
    return AI_INTERACTION_ACTION_PRESENT_RESULT;
}

ai_interaction_action_t ai_interaction_on_transaction_failed (ai_interaction_t * p_interaction,
                                                               app_status_t result_status)
{
    if ((NULL == p_interaction) ||
        ((AI_INTERACTION_FREEZE_PENDING != p_interaction->state) &&
         (AI_INTERACTION_INFERENCING != p_interaction->state)) ||
        (APP_STATUS_OK == result_status) ||
        (APP_STATUS_BUSY == result_status))
    {
        return AI_INTERACTION_ACTION_NONE;
    }

    p_interaction->state = AI_INTERACTION_RESULT_HOLD;
    p_interaction->result_status = result_status;
    p_interaction->completed_results++;
    return AI_INTERACTION_ACTION_PRESENT_RESULT;
}

ai_interaction_action_t ai_interaction_on_resume_complete (ai_interaction_t * p_interaction)
{
    if ((NULL == p_interaction) ||
        (AI_INTERACTION_RESULT_HOLD != p_interaction->state))
    {
        return AI_INTERACTION_ACTION_NONE;
    }

    p_interaction->state = AI_INTERACTION_LIVE_PREVIEW;
    return AI_INTERACTION_ACTION_NONE;
}

ai_interaction_action_t ai_interaction_cancel_freeze (ai_interaction_t * p_interaction,
                                                       app_status_t result_status)
{
    if ((NULL == p_interaction) ||
        (AI_INTERACTION_FREEZE_PENDING != p_interaction->state) ||
        (APP_STATUS_OK == result_status))
    {
        return AI_INTERACTION_ACTION_NONE;
    }

    p_interaction->state = AI_INTERACTION_LIVE_PREVIEW;
    p_interaction->result_status = result_status;
    return AI_INTERACTION_ACTION_NONE;
}

char const * ai_interaction_state_name (ai_interaction_state_t state)
{
    static char const * const names[] =
    {
        "live-preview", "freeze-pending", "inferencing", "result-hold",
    };

    if ((uint32_t) state >= (uint32_t) (sizeof(names) / sizeof(names[0])))
    {
        return "unknown";
    }

    return names[state];
}
