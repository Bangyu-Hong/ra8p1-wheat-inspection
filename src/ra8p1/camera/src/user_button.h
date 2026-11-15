#ifndef USER_BUTTON_H_
#define USER_BUTTON_H_

#include <stdbool.h>
#include <stdint.h>

#define USER_BUTTON_DEBOUNCE_US    (30000ULL)

typedef enum e_user_button_event
{
    USER_BUTTON_EVENT_NONE = 0,
    USER_BUTTON_EVENT_PRESSED,
    USER_BUTTON_EVENT_RELEASED
} user_button_event_t;

typedef struct st_user_button
{
    uint64_t raw_changed_us;
    uint32_t press_events;
    uint32_t release_events;
    uint32_t consumed_events;
    uint32_t ignored_busy_events;
    volatile uint32_t irq_events;
    volatile uint32_t ignored_irq_events;
    volatile uint32_t pending_press_events;
    volatile uint32_t press_armed;
    bool     raw_pressed;
    bool     stable_pressed;
    bool     initialized;
} user_button_t;

typedef struct st_user_button_diagnostics
{
    uint32_t press_events;
    uint32_t release_events;
    uint32_t consumed_events;
    uint32_t ignored_busy_events;
    uint32_t irq_events;
    uint32_t ignored_irq_events;
    uint32_t pending_press_events;
    bool     press_armed;
    bool     raw_pressed;
    bool     stable_pressed;
} user_button_diagnostics_t;

void                user_button_init(user_button_t * p_button,
                                     bool initial_pressed,
                                     uint64_t now_us);
user_button_event_t user_button_update(user_button_t * p_button,
                                       bool raw_pressed,
                                       uint64_t now_us);
void                user_button_latch_press_from_isr(user_button_t * p_button);
user_button_event_t user_button_consume_press(user_button_t * p_button);
void                user_button_record_ignored_busy(user_button_t * p_button);
uint32_t            user_button_pending_press_events(user_button_t const * p_button);
void                user_button_get_diagnostics(user_button_t const * p_button,
                                                user_button_diagnostics_t * p_diagnostics);

#endif /* USER_BUTTON_H_ */
