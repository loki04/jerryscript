/* Copyright JS Foundation and other contributors, http://js.foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "jerry-api.h"

#ifdef JERRY_DEBUGGER

#include "byte-code.h"
#include "jcontext.h"
#include "jerry-debugger.h"
#include "jerry-port.h"

/**
 * Type cast the debugger send buffer into a specific type.
 */
#define JERRY_DEBUGGER_SEND_BUFFER_AS(type, name_p) \
  type *name_p = (type *) (&JERRY_CONTEXT (debugger_send_buffer))

/**
 * Type cast the debugger receive buffer into a specific type.
 */
#define JERRY_DEBUGGER_RECEIVE_BUFFER_AS(type, name_p) \
  type *name_p = ((type *) recv_buffer_p)

/**
 * Free all unreferenced byte code structures which
 * were not acknowledged by the debugger client.
 */
void
jerry_debugger_free_unreferenced_byte_code (void)
{
  jerry_debugger_byte_code_free_t *byte_code_free_p;

  byte_code_free_p = JMEM_CP_GET_POINTER (jerry_debugger_byte_code_free_t,
                                          JERRY_CONTEXT (debugger_byte_code_free_head));

  while (byte_code_free_p != NULL)
  {
    jerry_debugger_byte_code_free_t *next_byte_code_free_p;
    next_byte_code_free_p = JMEM_CP_GET_POINTER (jerry_debugger_byte_code_free_t,
                                                 byte_code_free_p->next_cp);

    jmem_heap_free_block (byte_code_free_p,
                          ((size_t) byte_code_free_p->size) << JMEM_ALIGNMENT_LOG);

    byte_code_free_p = next_byte_code_free_p;
  }
} /* jerry_debugger_free_unreferenced_byte_code */

/**
 * Send backtrace.
 */
static void
jerry_debugger_send_backtrace (uint8_t *recv_buffer_p) /**< pointer the the received data */
{
  JERRY_DEBUGGER_RECEIVE_BUFFER_AS (jerry_debugger_receive_get_backtrace_t, get_backtrace_p);

  uint32_t max_depth;
  memcpy (&max_depth, get_backtrace_p->max_depth, sizeof (uint32_t));

  if (max_depth == 0)
  {
    max_depth = UINT32_MAX;
  }

  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_backtrace_t, backtrace_p);

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (backtrace_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE_FROM_TYPE (backtrace_p, jerry_debugger_send_backtrace_t);
  backtrace_p->type = JERRY_DEBUGGER_BACKTRACE;

  vm_frame_ctx_t *frame_ctx_p = JERRY_CONTEXT (vm_top_context_p);

  uint32_t current_frame = 0;

  while (frame_ctx_p != NULL && max_depth > 0)
  {
    if (current_frame >= JERRY_DEBUGGER_MAX_SIZE (jerry_debugger_frame_t))
    {
      if (!jerry_debugger_send (sizeof (jerry_debugger_send_backtrace_t)))
      {
        return;
      }
      current_frame = 0;
    }

    jerry_debugger_frame_t *frame_p = backtrace_p->frames + current_frame;

    jmem_cpointer_t byte_code_cp;
    JMEM_CP_SET_NON_NULL_POINTER (byte_code_cp, frame_ctx_p->bytecode_header_p);
    memcpy (frame_p->byte_code_cp, &byte_code_cp, sizeof (jmem_cpointer_t));

    uint32_t offset = (uint32_t) (frame_ctx_p->byte_code_p - (uint8_t *) frame_ctx_p->bytecode_header_p);
    memcpy (frame_p->offset, &offset, sizeof (uint32_t));

    frame_ctx_p = frame_ctx_p->prev_context_p;
    current_frame++;
    max_depth--;
  }

  size_t message_size = current_frame * sizeof (jerry_debugger_frame_t);

  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE (backtrace_p, 1 + message_size);
  backtrace_p->type = JERRY_DEBUGGER_BACKTRACE_END;

  jerry_debugger_send (sizeof (jerry_debugger_send_type_t) + message_size);
} /* jerry_debugger_send_backtrace */

/**
 * Check received packet size.
 */
#define JERRY_DEBUGGER_CHECK_PACKET_SIZE(type) \
  if (message_size != sizeof (type)) \
  { \
    jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Invalid message size\n"); \
    jerry_debugger_close_connection (); \
    return false; \
  }

/**
 * Receive message from the client.
 *
 * @return true - if message is processed successfully
 *         false - otherwise
 */
inline bool __attr_always_inline___
jerry_debugger_process_message (uint8_t *recv_buffer_p, /**< pointer the the received data */
                                uint32_t message_size, /**< message size */
                                bool *resume_exec_p) /**< pointer to the resume exec flag */
{
  /* Process the received message. */
  switch (recv_buffer_p[0])
  {
    case JERRY_DEBUGGER_FREE_BYTE_CODE_CP:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_byte_code_cp_t);

      JERRY_DEBUGGER_RECEIVE_BUFFER_AS (jerry_debugger_receive_byte_code_cp_t, byte_code_p);

      jmem_cpointer_t byte_code_free_cp;
      memcpy (&byte_code_free_cp, byte_code_p->byte_code_cp, sizeof (jmem_cpointer_t));

      jerry_debugger_byte_code_free_t *byte_code_free_p;
      byte_code_free_p = JMEM_CP_GET_NON_NULL_POINTER (jerry_debugger_byte_code_free_t,
                                                       byte_code_free_cp);

      if (JERRY_CONTEXT (debugger_byte_code_free_head) == byte_code_free_cp)
      {
        JERRY_CONTEXT (debugger_byte_code_free_head) = byte_code_free_p->next_cp;
      }

      if (byte_code_free_p->prev_cp != ECMA_NULL_POINTER)
      {
        jerry_debugger_byte_code_free_t *prev_byte_code_free_p;
        prev_byte_code_free_p = JMEM_CP_GET_NON_NULL_POINTER (jerry_debugger_byte_code_free_t,
                                                              byte_code_free_p->prev_cp);

        prev_byte_code_free_p->next_cp = byte_code_free_p->next_cp;
      }

      if (byte_code_free_p->next_cp != ECMA_NULL_POINTER)
      {
        jerry_debugger_byte_code_free_t *next_byte_code_free_p;
        next_byte_code_free_p = JMEM_CP_GET_NON_NULL_POINTER (jerry_debugger_byte_code_free_t,
                                                              byte_code_free_p->next_cp);

        next_byte_code_free_p->prev_cp = byte_code_free_p->prev_cp;
      }

      jmem_heap_free_block (byte_code_free_p,
                            ((size_t) byte_code_free_p->size) << JMEM_ALIGNMENT_LOG);
      return true;
    }

    case JERRY_DEBUGGER_UPDATE_BREAKPOINT:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_update_breakpoint_t);

      JERRY_DEBUGGER_RECEIVE_BUFFER_AS (jerry_debugger_receive_update_breakpoint_t, update_breakpoint_p);

      jmem_cpointer_t byte_code_cp;
      memcpy (&byte_code_cp, update_breakpoint_p->byte_code_cp, sizeof (jmem_cpointer_t));
      uint8_t *byte_code_p = JMEM_CP_GET_NON_NULL_POINTER (uint8_t, byte_code_cp);

      uint32_t offset;
      memcpy (&offset, update_breakpoint_p->offset, sizeof (uint32_t));
      byte_code_p += offset;

      JERRY_ASSERT (*byte_code_p == CBC_BREAKPOINT_ENABLED || *byte_code_p == CBC_BREAKPOINT_DISABLED);

      *byte_code_p = update_breakpoint_p->is_set_breakpoint ? CBC_BREAKPOINT_ENABLED : CBC_BREAKPOINT_DISABLED;
      return true;
    }

    case JERRY_DEBUGGER_STOP:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_type_t);

      JERRY_CONTEXT (debugger_stop_exec) = true;
      JERRY_CONTEXT (debugger_stop_context) = NULL;
      *resume_exec_p = false;
      return true;
    }

    case JERRY_DEBUGGER_CONTINUE:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_type_t);

      JERRY_CONTEXT (debugger_stop_exec) = false;
      JERRY_CONTEXT (debugger_stop_context) = NULL;

      *resume_exec_p = true;
      return true;
    }

    case JERRY_DEBUGGER_STEP:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_type_t);

      JERRY_CONTEXT (debugger_stop_exec) = true;
      JERRY_CONTEXT (debugger_stop_context) = NULL;
      *resume_exec_p = true;
      return true;
    }

    case JERRY_DEBUGGER_NEXT:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_type_t);

      JERRY_CONTEXT (debugger_stop_exec) = true;
      JERRY_CONTEXT (debugger_stop_context) = JERRY_CONTEXT (vm_top_context_p);
      *resume_exec_p = true;
      return true;
    }

    case JERRY_DEBUGGER_GET_BACKTRACE:
    {
      JERRY_DEBUGGER_CHECK_PACKET_SIZE (jerry_debugger_receive_get_backtrace_t);

      jerry_debugger_send_backtrace (recv_buffer_p);
      return true;
    }

    default:
    {
      jerry_port_log (JERRY_LOG_LEVEL_ERROR, "Unexpected message.");
      jerry_debugger_close_connection ();
      return false;
    }
  }
} /* jerry_debugger_process_message */

#undef JERRY_DEBUGGER_CHECK_PACKET_SIZE

/**
 * Tell the client that a breakpoint has been hit and wait for further debugger commands.
 */
void
jerry_debugger_breakpoint_hit (void)
{
  JERRY_ASSERT (JERRY_CONTEXT (jerry_init_flags) & JERRY_INIT_DEBUGGER);

  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_breakpoint_hit_t, breakpoint_hit_p);

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (breakpoint_hit_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE_FROM_TYPE (breakpoint_hit_p, jerry_debugger_send_breakpoint_hit_t);
  breakpoint_hit_p->type = (uint8_t) JERRY_DEBUGGER_BREAKPOINT_HIT;

  vm_frame_ctx_t *frame_ctx_p = JERRY_CONTEXT (vm_top_context_p);

  jmem_cpointer_t byte_code_header_cp;
  JMEM_CP_SET_NON_NULL_POINTER (byte_code_header_cp, frame_ctx_p->bytecode_header_p);
  memcpy (breakpoint_hit_p->byte_code_cp, &byte_code_header_cp, sizeof (jmem_cpointer_t));

  uint32_t offset = (uint32_t) (frame_ctx_p->byte_code_p - (uint8_t *) frame_ctx_p->bytecode_header_p);
  memcpy (breakpoint_hit_p->offset, &offset, sizeof (uint32_t));

  if (!jerry_debugger_send (sizeof (jerry_debugger_send_breakpoint_hit_t)))
  {
    return;
  }

  while (!jerry_debugger_receive ())
  {
  }

  JERRY_CONTEXT (debugger_message_delay) = JERRY_DEBUGGER_MESSAGE_FREQUENCY;
} /* jerry_debugger_breakpoint_hit */

/**
 * Send the type signal to the client.
 */
void
jerry_debugger_send_type (jerry_debugger_header_type_t type) /**< message type */
{
  JERRY_ASSERT (JERRY_CONTEXT (jerry_init_flags) & JERRY_INIT_DEBUGGER);

  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_type_t, message_type_p);

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (message_type_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE_FROM_TYPE (message_type_p, jerry_debugger_send_type_t)
  message_type_p->type = (uint8_t) type;

  jerry_debugger_send (sizeof (jerry_debugger_send_type_t));
} /* jerry_debugger_send_type */


/**
 * Send the type signal to the client.
 *
 * @return true - if the data sent successfully to the debugger client,
 *         false - otherwise
 */
bool
jerry_debugger_send_configuration (uint8_t max_message_size) /**< maximum message size */
{
  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_configuration_t, configuration_p);

  /* Helper structure for endianness check. */
  union
  {
    uint16_t uint16_value; /**< a 16-bit value */
    uint8_t uint8_value[2]; /**< lower and upper byte of a 16-bit value */
  } endian_data;

  endian_data.uint16_value = 1;

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (configuration_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE_FROM_TYPE (configuration_p, jerry_debugger_send_configuration_t);
  configuration_p->type = JERRY_DEBUGGER_CONFIGURATION;
  configuration_p->max_message_size = max_message_size;
  configuration_p->cpointer_size = sizeof (jmem_cpointer_t);
  configuration_p->little_endian = (endian_data.uint8_value[0] == 1);

  return jerry_debugger_send (sizeof (jerry_debugger_send_configuration_t));
} /* jerry_debugger_send_configuration */

/**
 * Send raw data to the debugger client.
 */
void
jerry_debugger_send_data (jerry_debugger_header_type_t type, /**< message type */
                          const void *data, /**< raw data */
                          size_t size) /**< size of data */
{
  JERRY_ASSERT (size < JERRY_DEBUGGER_MAX_SIZE (uint8_t));

  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_type_t, message_type_p);

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (message_type_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE (message_type_p, 1 + size);
  message_type_p->type = type;
  memcpy (message_type_p + 1, data, size);

  jerry_debugger_send (sizeof (jerry_debugger_send_type_t) + size);
} /* jerry_debugger_send_data */

/**
 * Send string to the debugger client.
 */
void
jerry_debugger_send_string (uint8_t message_type, /**< message type */
                            const jerry_char_t *string_p, /**< string data */
                            size_t string_length) /**< length of string */
{
  JERRY_ASSERT (JERRY_CONTEXT (jerry_init_flags) & JERRY_INIT_DEBUGGER);

  const size_t max_fragment_len = JERRY_DEBUGGER_MAX_SIZE (char);

  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_string_t, message_string_p);

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (message_string_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE_FROM_TYPE (message_string_p, jerry_debugger_send_string_t);
  message_string_p->type = message_type;

  while (string_length > max_fragment_len)
  {
    memcpy (message_string_p->string, string_p, max_fragment_len);

    if (!jerry_debugger_send (sizeof (jerry_debugger_send_string_t)))
    {
      return;
    }

    string_length -= max_fragment_len;
    string_p += max_fragment_len;
  }

  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE (message_string_p, 1 + string_length);
  message_string_p->type = (uint8_t) (message_type + 1);

  memcpy (message_string_p->string, string_p, string_length);

  jerry_debugger_send (sizeof (jerry_debugger_send_type_t) + string_length);
} /* jerry_debugger_send_string */

/**
 * Send the function name to the debugger client.
 */
void
jerry_debugger_send_function_name (const jerry_char_t *function_name_p, /**< function name */
                                   size_t function_name_length) /**< length of function name */
{
  JERRY_ASSERT (JERRY_CONTEXT (jerry_init_flags) & JERRY_INIT_DEBUGGER);

  jerry_debugger_send_string (JERRY_DEBUGGER_FUNCTION_NAME, function_name_p, function_name_length);
} /* jerry_debugger_send_function_name */

/**
 * Send the function compressed pointer to the debugger client.
 *
 * @return true - if the data was sent successfully to the debugger client,
 *         false - otherwise
 */
bool
jerry_debugger_send_function_cp (jerry_debugger_header_type_t type, /**< message type */
                                 ecma_compiled_code_t *compiled_code_p) /**< byte code pointer */
{
  JERRY_ASSERT (JERRY_CONTEXT (jerry_init_flags) & JERRY_INIT_DEBUGGER);

  JERRY_DEBUGGER_SEND_BUFFER_AS (jerry_debugger_send_byte_code_cp_t, byte_code_cp_p);

  JERRY_DEBUGGER_INIT_SEND_MESSAGE (byte_code_cp_p);
  JERRY_DEBUGGER_SET_SEND_MESSAGE_SIZE_FROM_TYPE (byte_code_cp_p, jerry_debugger_send_byte_code_cp_t);
  byte_code_cp_p->type = (uint8_t) type;

  jmem_cpointer_t compiled_code_cp;
  JMEM_CP_SET_NON_NULL_POINTER (compiled_code_cp, compiled_code_p);
  memcpy (byte_code_cp_p->byte_code_cp, &compiled_code_cp, sizeof (jmem_cpointer_t));

  return jerry_debugger_send (sizeof (jerry_debugger_send_byte_code_cp_t));
} /* jerry_debugger_send_function_cp */

#endif /* JERRY_DEBUGGER */
