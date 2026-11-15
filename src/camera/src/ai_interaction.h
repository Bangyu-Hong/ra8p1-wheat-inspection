#ifndef AI_INTERACTION_H_
#define AI_INTERACTION_H_

#include <stdint.h>

#include "app_status.h"

typedef enum e_ai_interaction_state
{
    AI_INTERACTION_LIVE_PREVIEW = 0,
    AI_INTERACTION_FREEZE_PENDING,
    AI_INTERACTION_INFERENCING,
    AI_INTERACTION_RESULT_HOLD
} ai_interaction_state_t;

typedef enum e_ai_interaction_action
{
    AI_INTERACTION_ACTION_NONE = 0,
    AI_INTERACTION_ACTION_REQUEST_FREEZE,
    AI_INTERACTION_ACTION_RUN_INFERENCE,
    AI_INTERACTION_ACTION_PRESENT_RESULT,
    AI_INTERACTION_ACTION_RESUME_PREVIEW,
    AI_INTERACTION_ACTION_REPORT_BUSY
} ai_interaction_action_t;

typedef struct st_ai_interaction
{
    ai_interaction_state_t state;
    app_status_t           result_status;
    uint32_t               freeze_requests;
    uint32_t               inference_requests;
    uint32_t               completed_results;
    uint32_t               resume_requests;
    uint32_t               busy_presses;
} ai_interaction_t;

void                    ai_interaction_init(ai_interaction_t * p_interaction);
ai_interaction_action_t ai_interaction_on_press(ai_interaction_t * p_interaction);
ai_interaction_action_t ai_interaction_on_frame_held(ai_interaction_t * p_interaction);
ai_interaction_action_t ai_interaction_on_inference_complete(ai_interaction_t * p_interaction,
                                                              app_status_t result_status);
ai_interaction_action_t ai_interaction_on_transaction_failed(ai_interaction_t * p_interaction,
                                                              app_status_t result_status);
ai_interaction_action_t ai_interaction_on_resume_complete(ai_interaction_t * p_interaction);
ai_interaction_action_t ai_interaction_cancel_freeze(ai_interaction_t * p_interaction,
                                                      app_status_t result_status);
char const            * ai_interaction_state_name(ai_interaction_state_t state);

#endif /* AI_INTERACTION_H_ */
