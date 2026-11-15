#ifndef RA8P_WIRE_CONTRACT_H
#define RA8P_WIRE_CONTRACT_H

#include <stdint.h>

/* Canonical wire contract shared by the board WebSocket and internal services. */
#define RA8P_PROTOCOL_MAGIC_U32              (0x52413850u)
#define RA8P_PROTOCOL_VERSION                (1u)
#define RA8P_PROTOCOL_HEADER_BYTES           (24u)
#define RA8P_PROTOCOL_MAX_PAYLOAD_BYTES      (4096u)

#define RA8P_HEADER_MAGIC_OFFSET              (0u)
#define RA8P_HEADER_VERSION_OFFSET            (4u)
#define RA8P_HEADER_TYPE_OFFSET               (6u)
#define RA8P_HEADER_SEQUENCE_OFFSET           (8u)
#define RA8P_HEADER_PAYLOAD_BYTES_OFFSET      (12u)
#define RA8P_HEADER_PAYLOAD_CRC32_OFFSET      (16u)
#define RA8P_HEADER_FLAGS_OFFSET              (20u)

typedef enum e_ra8p_message_type
{
    RA8P_MESSAGE_HELLO             = 1u,
    RA8P_MESSAGE_HELLO_ACK         = 2u,
    RA8P_MESSAGE_PING              = 3u,
    RA8P_MESSAGE_PONG              = 4u,
    RA8P_MESSAGE_STATE_SNAPSHOT    = 16u,
    RA8P_MESSAGE_TELEMETRY         = 17u,
    RA8P_MESSAGE_EVENT             = 18u,
    RA8P_MESSAGE_FAULT             = 19u,
    RA8P_MESSAGE_COMMAND           = 32u,
    RA8P_MESSAGE_COMMAND_ACK       = 33u,
    RA8P_MESSAGE_MISSION_VALIDATE  = 48u,
    RA8P_MESSAGE_MISSION_APPLY     = 49u,
    RA8P_MESSAGE_MISSION_STATUS    = 50u,
    RA8P_MESSAGE_CONFIG_GET        = 64u,
    RA8P_MESSAGE_CONFIG_SET        = 65u,
    RA8P_MESSAGE_ERROR             = 255u
} ra8p_message_type_t;

typedef enum e_ra8p_message_flag
{
    RA8P_FLAG_NONE          = 0u,
    RA8P_FLAG_ACK_REQUIRED  = 1u,
    RA8P_FLAG_RESPONSE      = 2u,
    RA8P_FLAG_ERROR         = 4u,
    RA8P_FLAG_SNAPSHOT      = 8u
} ra8p_message_flag_t;

typedef enum e_ra8p_status
{
    RA8P_STATUS_OK                   = 0u,
    RA8P_STATUS_BUSY                 = 1u,
    RA8P_STATUS_INVALID              = 2u,
    RA8P_STATUS_CRC_ERROR            = 3u,
    RA8P_STATUS_UNSUPPORTED          = 4u,
    RA8P_STATUS_UNSUPPORTED_MOTION   = 5u,
    RA8P_STATUS_TIMEOUT              = 6u,
    RA8P_STATUS_CONFLICT             = 7u,
    RA8P_STATUS_NO_RESOURCE          = 8u,
    RA8P_STATUS_INTERNAL_ERROR       = 255u
} ra8p_status_t;

typedef enum e_ra8p_capability
{
    RA8P_CAPABILITY_ENVIRONMENT   = 1u,
    RA8P_CAPABILITY_VISION        = 2u,
    RA8P_CAPABILITY_SD_STORAGE    = 4u,
    RA8P_CAPABILITY_LAN_TCP       = 8u,
    RA8P_CAPABILITY_MISSION_PLAN  = 16u,
    RA8P_CAPABILITY_MOTION        = 32u,
    RA8P_CAPABILITY_SLIDE         = 64u,
    RA8P_CAPABILITY_AI_INFERENCE  = 128u
} ra8p_capability_t;

#endif /* RA8P_WIRE_CONTRACT_H */
