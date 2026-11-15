#include "user_button.h"

#include <limits.h>
#include <stddef.h>

_Static_assert(sizeof(uint32_t) == sizeof(unsigned int),
               "USER button ISR latch requires a 32-bit unsigned int");
_Static_assert(2 == __GCC_ATOMIC_INT_LOCK_FREE,
               "USER button ISR latch requires lock-free 32-bit atomics");

static void user_button_increment_saturating(uint32_t * p_value);
static void user_button_increment_saturating_atomic(volatile uint32_t * p_value);

void user_button_init (user_button_t * p_button, bool initial_pressed, uint64_t now_us)
{
    if (NULL == p_button)
    {
        return;
    }

    *p_button = (user_button_t)
    {
        .raw_changed_us  = now_us,
        .raw_pressed     = initial_pressed,
        .stable_pressed  = initial_pressed,
        .press_armed     = initial_pressed ? 0U : 1U,
        .initialized     = true,
    };
}

user_button_event_t user_button_update (user_button_t * p_button,
                                        bool raw_pressed,
                                        uint64_t now_us)
{
    if ((NULL == p_button) || !p_button->initialized)
    {
        return USER_BUTTON_EVENT_NONE;
    }

    if (raw_pressed != p_button->raw_pressed)
    {
        p_button->raw_pressed = raw_pressed;
        p_button->raw_changed_us = now_us;
        return USER_BUTTON_EVENT_NONE;
    }

    if ((raw_pressed == p_button->stable_pressed) ||
        ((now_us - p_button->raw_changed_us) < USER_BUTTON_DEBOUNCE_US))
    {
        return USER_BUTTON_EVENT_NONE;
    }

    p_button->stable_pressed = raw_pressed;
    if (!raw_pressed)
    {
        __atomic_store_n(&p_button->press_armed, 1U, __ATOMIC_RELEASE);
        user_button_increment_saturating(&p_button->release_events);
        return USER_BUTTON_EVENT_RELEASED;
    }

    return USER_BUTTON_EVENT_NONE;
}

void user_button_latch_press_from_isr (user_button_t * p_button)
{
    if ((NULL == p_button) || !p_button->initialized)
    {
        return;
    }

    user_button_increment_saturating_atomic(&p_button->irq_events);

    if ((0U == __atomic_load_n(&p_button->press_armed, __ATOMIC_ACQUIRE)) ||
        (0U != __atomic_load_n(&p_button->pending_press_events, __ATOMIC_RELAXED)))
    {
        user_button_increment_saturating_atomic(&p_button->ignored_irq_events);
        return;
    }

    __atomic_store_n(&p_button->press_armed, 0U, __ATOMIC_RELEASE);
    __atomic_store_n(&p_button->pending_press_events, 1U, __ATOMIC_RELEASE);
}

user_button_event_t user_button_consume_press (user_button_t * p_button)
{
    if ((NULL == p_button) || !p_button->initialized)
    {
        return USER_BUTTON_EVENT_NONE;
    }

    if (0U == __atomic_exchange_n(&p_button->pending_press_events, 0U, __ATOMIC_ACQ_REL))
    {
        return USER_BUTTON_EVENT_NONE;
    }

    p_button->stable_pressed = true;
    user_button_increment_saturating(&p_button->press_events);
    user_button_increment_saturating(&p_button->consumed_events);
    return USER_BUTTON_EVENT_PRESSED;
}

void user_button_record_ignored_busy (user_button_t * p_button)
{
    if ((NULL != p_button) && p_button->initialized)
    {
        user_button_increment_saturating(&p_button->ignored_busy_events);
    }
}

uint32_t user_button_pending_press_events (user_button_t const * p_button)
{
    if ((NULL == p_button) || !p_button->initialized)
    {
        return 0U;
    }

    return __atomic_load_n(&p_button->pending_press_events, __ATOMIC_ACQUIRE);
}

void user_button_get_diagnostics (user_button_t const * p_button,
                                  user_button_diagnostics_t * p_diagnostics)
{
    if (NULL == p_diagnostics)
    {
        return;
    }

    *p_diagnostics = (user_button_diagnostics_t) {0};
    if ((NULL == p_button) || !p_button->initialized)
    {
        return;
    }

    p_diagnostics->press_events = p_button->press_events;
    p_diagnostics->release_events = p_button->release_events;
    p_diagnostics->consumed_events = p_button->consumed_events;
    p_diagnostics->ignored_busy_events = p_button->ignored_busy_events;
    p_diagnostics->irq_events = __atomic_load_n(&p_button->irq_events, __ATOMIC_ACQUIRE);
    p_diagnostics->ignored_irq_events = __atomic_load_n(&p_button->ignored_irq_events, __ATOMIC_ACQUIRE);
    p_diagnostics->pending_press_events =
        __atomic_load_n(&p_button->pending_press_events, __ATOMIC_ACQUIRE);
    p_diagnostics->press_armed =
        (0U != __atomic_load_n(&p_button->press_armed, __ATOMIC_ACQUIRE));
    p_diagnostics->raw_pressed = p_button->raw_pressed;
    p_diagnostics->stable_pressed = p_button->stable_pressed;
}

static void user_button_increment_saturating (uint32_t * p_value)
{
    if ((NULL != p_value) && (UINT32_MAX != *p_value))
    {
        (*p_value)++;
    }
}

static void user_button_increment_saturating_atomic (volatile uint32_t * p_value)
{
    uint32_t current;

    if (NULL == p_value)
    {
        return;
    }

    current = __atomic_load_n(p_value, __ATOMIC_RELAXED);
    while ((UINT32_MAX != current) &&
           !__atomic_compare_exchange_n(p_value,
                                        &current,
                                        current + 1U,
                                        true,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
    {
    }
}
