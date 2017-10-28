/*
  core mavlink functionality
 */
#pragma once

#define MAVLINK_SEPARATE_HELPERS
#define MAVLINK_NO_CONVERSION_HELPERS

// arm processor does not handle misaligned accesses
#define MAVLINK_ALIGNED_FIELDS 0

#define MAVLINK_SEND_UART_BYTES(chan, buf, len) comm_send_buffer(chan, buf, len)

// allow two mavlink ports
#define MAVLINK_COMM_NUM_BUFFERS 1

#include <generated/mavlink/ardupilotmega/version.h>
#include <generated/mavlink/mavlink_types.h>

extern mavlink_system_t mavlink_system;

/*
  Send a byte to the nominated MAVLink channel
*/
void comm_send_ch(mavlink_channel_t chan, uint8_t ch);

#define MAVLINK_USE_CONVENIENCE_FUNCTIONS

/*
  the generated MAVLink code does its own alignment, so taking the
  address of packed members in its encode functions is fine
*/
#pragma GCC diagnostic push
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9)
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif
#include <generated/mavlink/ardupilotmega/mavlink.h>
#pragma GCC diagnostic pop

// alias for link to flight controller
#define MAVLINK_COMM_FC MAVLINK_COMM_0

#define MAVLINK_SYSTEM_ID 67
#define MAVLINK_TARGET_SYSTEM_ID 1

#define MAVLINK_COMPONENT_ID_REMOTE_LOG 72
