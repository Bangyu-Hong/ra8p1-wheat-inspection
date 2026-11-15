#include "hmi_uart.h"

#include "board_port.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static app_status_t send_line(const char * line)
{
    if (line == NULL)
    {
        return APP_ERR_ARGUMENT;
    }
    return board_hmi_uart_write((const uint8_t *) line, strlen(line));
}

static bool valid_sample_id(const char * value)
{
    size_t index;
    size_t length = strlen(value);
    if ((length == 0U) || (length >= 24U))
    {
        return false;
    }
    for (index = 0U; index < length; ++index)
    {
        unsigned char ch = (unsigned char) value[index];
        if (!isalnum(ch) && (ch != '-') && (ch != '_'))
        {
            return false;
        }
    }
    return true;
}

app_status_t hmi_init(void)
{
    return send_line("PAGE,main\n");
}

app_status_t hmi_set_status(const char * text)
{
    char command[96];
    int length;
    if (text == NULL)
    {
        return APP_ERR_ARGUMENT;
    }
    length = snprintf(command, sizeof(command), "SET,status,%s\n", text);
    if ((length < 0) || ((size_t) length >= sizeof(command)))
    {
        return APP_ERR_NO_SPACE;
    }
    return send_line(command);
}

app_status_t hmi_show_result(const detection_record_t * record)
{
    char command[160];
    int length;
    if (record == NULL)
    {
        return APP_ERR_ARGUMENT;
    }
    length = snprintf(command,
                      sizeof(command),
                      "RESULT,%s,%u,%d,%u,%u\n",
                      record->sample_id,
                      (unsigned int) record->wheat_ear_count,
                      record->environment.valid ? record->environment.temperature_c : -99,
                      record->environment.valid ? record->environment.humidity_percent : 0U,
                      record->light_percent);
    if ((length < 0) || ((size_t) length >= sizeof(command)))
    {
        return APP_ERR_NO_SPACE;
    }
    return send_line(command);
}

hmi_event_t hmi_poll_event(char * sample_id, size_t sample_id_capacity)
{
    static char receive_line[64];
    static size_t receive_length;
    uint8_t input[16];
    size_t input_count = board_hmi_uart_read(input, sizeof(input));
    size_t index;

    for (index = 0U; index < input_count; ++index)
    {
        char ch = (char) input[index];
        if (ch == '\n')
        {
            hmi_event_t event = HMI_EVENT_NONE;
            receive_line[receive_length] = '\0';
            if (strcmp(receive_line, "EVT,CANCEL") == 0)
            {
                event = HMI_EVENT_CANCEL;
            }
            else if (strncmp(receive_line, "EVT,START,", 10U) == 0)
            {
                const char * value = receive_line + 10U;
                size_t value_length = strlen(value);
                if ((sample_id != NULL) && (sample_id_capacity > value_length) && valid_sample_id(value))
                {
                    memcpy(sample_id, value, value_length + 1U);
                    event = HMI_EVENT_START;
                }
            }
            receive_length = 0U;
            if (event != HMI_EVENT_NONE)
            {
                return event;
            }
        }
        else if ((ch >= 0x20) && (ch <= 0x7E) && (receive_length + 1U < sizeof(receive_line)))
        {
            receive_line[receive_length++] = ch;
        }
        else if (receive_length + 1U >= sizeof(receive_line))
        {
            receive_length = 0U;
        }
    }

    return HMI_EVENT_NONE;
}
