/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-message.c  DBusMessage object
 *
 * Copyright (C) 2002, 2003  Red Hat Inc.
 * Copyright (C) 2002, 2003  CodeFactory AB
 *
 * Licensed under the Academic Free License version 1.2
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-marshal.h"
#include "dbus-message.h"
#include "dbus-message-internal.h"
#include "dbus-memory.h"
#include "dbus-list.h"
#include "dbus-message-builder.h"
#include <string.h>

/**
 * @defgroup DBusMessageInternals DBusMessage implementation details
 * @ingroup DBusInternals
 * @brief DBusMessage private implementation details.
 *
 * The guts of DBusMessage and its methods.
 *
 * @{
 */

enum
{
  FIELD_HEADER_LENGTH,
  FIELD_BODY_LENGTH,
  FIELD_CLIENT_SERIAL,
  FIELD_NAME,
  FIELD_SERVICE,
  FIELD_SENDER,
  FIELD_REPLY_SERIAL,

  FIELD_LAST
};

static dbus_bool_t field_is_named[FIELD_LAST] =
{
  FALSE, /* FIELD_HEADER_LENGTH */
  FALSE, /* FIELD_BODY_LENGTH */
  FALSE, /* FIELD_CLIENT_SERIAL */
  TRUE,  /* FIELD_NAME */
  TRUE,  /* FIELD_SERVICE */
  TRUE,  /* FIELD_SENDER */
  TRUE   /* FIELD_REPLY_SERIAL */
};

typedef struct
{
  int offset; /**< Offset to start of field (location of name of field
               * for named fields)
               */
} HeaderField;

/**
 * @brief Internals of DBusMessage
 * 
 * Object representing a message received from or to be sent to
 * another application. This is an opaque object, all members
 * are private.
 */
struct DBusMessage
{
  dbus_atomic_t refcount; /**< Reference count */

  DBusString header; /**< Header network data, stored
                      * separately from body so we can
                      * independently realloc it.
                      */

  HeaderField header_fields[FIELD_LAST]; /**< Track the location
                                           * of each field in "header"
                                           */

  dbus_int32_t client_serial; /**< Cached client serial value for speed */
  dbus_int32_t reply_serial;  /**< Cached reply serial value for speed */
  
  int header_padding; /**< bytes of alignment in header */
  
  DBusString body;   /**< Body network data. */

  char byte_order; /**< Message byte order. */

  DBusCounter *size_counter; /**< Counter for the size of the message, or #NULL */
  long size_counter_delta;   /**< Size we incremented the size counter by. */
  
  unsigned int locked : 1; /**< Message being sent, no modifications allowed. */
};

/**
 * @brief Internals of DBusMessageIter
 * 
 * Object representing a position in a message. All fields are internal.
 */
struct DBusMessageIter
{
  int refcount; /**< Reference count */

  int pos; /**< Current position in the string */
  
  DBusMessage *message; /**< Message used */
};

/**
 * Gets the data to be sent over the network for this message.
 * The header and then the body should be written out.
 * This function is guaranteed to always return the same
 * data once a message is locked (with _dbus_message_lock()).
 *
 * @param message the message.
 * @param header return location for message header data.
 * @param body return location for message body data.
 */
void
_dbus_message_get_network_data (DBusMessage          *message,
                                const DBusString    **header,
                                const DBusString    **body)
{
  _dbus_assert (message->locked);
  
  *header = &message->header;
  *body = &message->body;
}

static void
clear_header_padding (DBusMessage *message)
{
  _dbus_string_shorten (&message->header,
                        message->header_padding);
  message->header_padding = 0;
}              

static dbus_bool_t
append_header_padding (DBusMessage *message)
{
  int old_len;
  old_len = _dbus_string_get_length (&message->header);
  if (!_dbus_string_align_length (&message->header, 8))
    return FALSE;

  message->header_padding = _dbus_string_get_length (&message->header) - old_len;

  return TRUE;
}

static void
adjust_field_offsets (DBusMessage *message,
                      int          offsets_after,
                      int          delta)
{
  int i;

  if (delta == 0)
    return;
  
  i = 0;
  while (i < FIELD_LAST)
    {
      if (message->header_fields[i].offset > offsets_after)
        message->header_fields[i].offset += delta;

      ++i;
    }
}

static const char*
get_string_field (DBusMessage *message,
                  int          field,
                  int         *len)
{
  int offset;
  const char *data;

  offset = message->header_fields[field].offset;

  _dbus_assert (field < FIELD_LAST);
  
  if (offset < 0)
    return NULL;

  /* offset points to string length, string data follows it */
  /* FIXME _dbus_demarshal_const_string() that returned
   * a reference to the string plus its len might be nice.
   */
  
  if (len)
    *len = _dbus_demarshal_uint32 (&message->header,
                                   message->byte_order,
                                   offset,
                                   NULL);

  _dbus_string_get_const_data (&message->header,
                               &data);
  
  return data + (offset + 4); 
}

static dbus_int32_t
get_int_field (DBusMessage *message,
               int          field)
{
  int offset;

  _dbus_assert (field < FIELD_LAST);
  
  offset = message->header_fields[field].offset;
  
  if (offset < 0)
    return -1; /* useless if -1 is a valid value of course */
  
  return _dbus_demarshal_int32 (&message->header,
                                message->byte_order,
                                offset,
                                NULL);
}

static dbus_bool_t
append_int_field (DBusMessage *message,
                  int          field,
                  const char  *name,
                  int          value)
{
  int orig_len;

  _dbus_assert (!message->locked);

  clear_header_padding (message);
  
  orig_len = _dbus_string_get_length (&message->header);
  
  if (!_dbus_string_align_length (&message->header, 4))
    goto failed;  
  
  if (!_dbus_string_append_len (&message->header, name, 4))
    goto failed;

  if (!_dbus_string_append_byte (&message->header, DBUS_TYPE_INT32))
    goto failed;

  if (!_dbus_string_align_length (&message->header, 4))
    goto failed;
  
  message->header_fields[FIELD_REPLY_SERIAL].offset =
    _dbus_string_get_length (&message->header);
  
  if (!_dbus_marshal_int32 (&message->header, message->byte_order,
                            value))
    goto failed;

  if (!append_header_padding (message))
    goto failed;
  
  return TRUE;
  
 failed:
  message->header_fields[field].offset = -1;
  _dbus_string_set_length (&message->header, orig_len);

  /* this must succeed because it was allocated on function entry and
   * DBusString doesn't ever realloc smaller
   */
  if (!append_header_padding (message))
    _dbus_assert_not_reached ("failed to reappend header padding");
  return FALSE;
}

static dbus_bool_t
append_string_field (DBusMessage *message,
                     int          field,
                     const char  *name,
                     const char  *value)
{
  int orig_len;

  _dbus_assert (!message->locked);

  clear_header_padding (message);
  
  orig_len = _dbus_string_get_length (&message->header);

  if (!_dbus_string_align_length (&message->header, 4))
    goto failed;
  
  if (!_dbus_string_append_len (&message->header, name, 4))
    goto failed;
  
  if (!_dbus_string_append_byte (&message->header, DBUS_TYPE_STRING))
    goto failed;

  if (!_dbus_string_align_length (&message->header, 4))
    goto failed;
  
  message->header_fields[field].offset =
    _dbus_string_get_length (&message->header);
  
  if (!_dbus_marshal_string (&message->header, message->byte_order,
                             value))
    goto failed;

  if (!append_header_padding (message))
    goto failed;
  
  return TRUE;
  
 failed:
  message->header_fields[field].offset = -1;
  _dbus_string_set_length (&message->header, orig_len);

  /* this must succeed because it was allocated on function entry and
   * DBusString doesn't ever realloc smaller
   */
  if (!append_header_padding (message))
    _dbus_assert_not_reached ("failed to reappend header padding");
  
  return FALSE;
}

static void
delete_int_field (DBusMessage *message,
                  int          field)
{
  int offset = message->header_fields[field].offset;

  _dbus_assert (!message->locked);
  _dbus_assert (field_is_named[field]);
  
  if (offset < 0)
    return;  

  clear_header_padding (message);
  
  /* The field typecode and name take up 8 bytes */
  _dbus_string_delete (&message->header,
                       offset - 8,
                       12);

  message->header_fields[field].offset = -1;
  
  adjust_field_offsets (message,
                        offset - 8,
                        - 12);

  append_header_padding (message);
}

static void
delete_string_field (DBusMessage *message,
                     int          field)
{
  int offset = message->header_fields[field].offset;
  int len;
  int delete_len;
  
  _dbus_assert (!message->locked);
  _dbus_assert (field_is_named[field]);
  
  if (offset < 0)
    return;

  clear_header_padding (message);
  
  get_string_field (message, field, &len);
  
  /* The field typecode and name take up 8 bytes, and the nul
   * termination is 1 bytes, string length integer is 4 bytes
   */
  delete_len = 8 + 4 + 1 + len;
  
  _dbus_string_delete (&message->header,
                       offset - 8,
                       delete_len);

  message->header_fields[field].offset = -1;
  
  adjust_field_offsets (message,
                        offset - 8,
                        - delete_len);

  append_header_padding (message);
}

static dbus_bool_t
set_int_field (DBusMessage *message,
               int          field,
               int          value)
{
  int offset = message->header_fields[field].offset;

  _dbus_assert (!message->locked);
  
  if (offset < 0)
    {
      /* need to append the field */

      switch (field)
        {
        case FIELD_REPLY_SERIAL:
          return append_int_field (message, field,
                                   DBUS_HEADER_FIELD_REPLY,
                                   value);
        default:
          _dbus_assert_not_reached ("appending an int field we don't support appending");
          return FALSE;
        }
    }
  else
    {
      _dbus_marshal_set_int32 (&message->header,
                               message->byte_order,
                               offset, value);

      return TRUE;
    }
}

static dbus_bool_t
set_uint_field (DBusMessage  *message,
                int           field,
                dbus_uint32_t value)
{
  int offset = message->header_fields[field].offset;

  _dbus_assert (!message->locked);
  
  if (offset < 0)
    {
      /* need to append the field */

      switch (field)
        {
        default:
          _dbus_assert_not_reached ("appending a uint field we don't support appending");
          return FALSE;
        }
    }
  else
    {
      _dbus_marshal_set_uint32 (&message->header,
                                message->byte_order,
                                offset, value);

      return TRUE;
    }
}

static dbus_bool_t
set_string_field (DBusMessage *message,
                  int          field,
                  const char  *value)
{
  int offset = message->header_fields[field].offset;

  _dbus_assert (!message->locked);
  _dbus_assert (value != NULL);
  
  if (offset < 0)
    {      
      /* need to append the field */

      switch (field)
        {
        case FIELD_SENDER:
          return append_string_field (message, field,
                                      DBUS_HEADER_FIELD_SENDER,
                                      value);
        default:
          _dbus_assert_not_reached ("appending a string field we don't support appending");
          return FALSE;
        }
    }
  else
    {
      DBusString v;
      int old_len;
      int new_len;
      int len;
      
      clear_header_padding (message);
      
      old_len = _dbus_string_get_length (&message->header);

      len = strlen (value);
      
      _dbus_string_init_const_len (&v, value,
				   len + 1); /* include nul */
      if (!_dbus_marshal_set_string (&message->header,
                                     message->byte_order,
                                     offset, &v,
				     len))
        goto failed;
      
      new_len = _dbus_string_get_length (&message->header);

      adjust_field_offsets (message,
                            offset,
                            new_len - old_len);

      if (!append_header_padding (message))
	goto failed;
      
      return TRUE;

    failed:
      /* this must succeed because it was allocated on function entry and
       * DBusString doesn't ever realloc smaller
       */
      if (!append_header_padding (message))
	_dbus_assert_not_reached ("failed to reappend header padding");

      return FALSE;
    }
}

/**
 * Sets the serial number of a message. 
 * This can only be done once on a message.
 * 
 * @param message the message
 * @param serial the serial
 */
void
_dbus_message_set_serial (DBusMessage  *message,
                          dbus_int32_t  serial)
{
  _dbus_assert (!message->locked);
  _dbus_assert (dbus_message_get_serial (message) < 0);
  
  set_int_field (message, FIELD_CLIENT_SERIAL,
                 serial);
  message->client_serial = serial;
}

/**
 * Sets the reply serial of a message (the client serial
 * of the message this is a reply to).
 *
 * @param message the message
 * @param reply_serial the client serial
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_reply_serial (DBusMessage  *message,
                                dbus_int32_t  reply_serial)
{
  _dbus_assert (!message->locked);

  if (set_int_field (message, FIELD_REPLY_SERIAL,
                     reply_serial))
    {
      message->reply_serial = reply_serial;
      return TRUE;
    }
  else
    return FALSE;
}

/**
 * Returns the serial of a message or -1 if none has been specified.
 * The message's serial number is provided by the application sending
 * the message and is used to identify replies to this message.
 *
 * @param message the message
 * @returns the client serial
 */
dbus_int32_t
dbus_message_get_serial (DBusMessage *message)
{
  return message->client_serial;
}

/**
 * Returns the serial that the message is
 * a reply to or -1 if none.
 *
 * @param message the message
 * @returns the reply serial
 */
dbus_int32_t
dbus_message_get_reply_serial  (DBusMessage *message)
{
  return message->reply_serial;
}

/**
 * Adds a counter to be incremented immediately with the
 * size of this message, and decremented by the size
 * of this message when this message if finalized.
 *
 * @param message the message
 * @param counter the counter
 */
void
_dbus_message_add_size_counter (DBusMessage *message,
                                DBusCounter *counter)
{
  _dbus_assert (message->size_counter == NULL); /* If this fails we may need to keep a list of
                                                 * counters instead of just one
                                                 */

  message->size_counter = counter;
  _dbus_counter_ref (message->size_counter);

  /* When we can change message size, we may want to
   * update this each time we do so, or we may want to
   * just KISS like this.
   */
  message->size_counter_delta =
    _dbus_string_get_length (&message->header) +
    _dbus_string_get_length (&message->body);

#if 0
  _dbus_verbose ("message has size %ld\n",
                 message->size_counter_delta);
#endif
  
  _dbus_counter_adjust (message->size_counter, message->size_counter_delta);
}

static dbus_bool_t
dbus_message_create_header (DBusMessage *message,
                            const char  *service,
                            const char  *name)
{
  unsigned int flags;
  
  if (!_dbus_string_append_byte (&message->header, message->byte_order))
    return FALSE;

  flags = 0;
  if (!_dbus_string_append_byte (&message->header, flags))
    return FALSE;

  if (!_dbus_string_append_byte (&message->header, DBUS_MAJOR_PROTOCOL_VERSION))
    return FALSE;

  if (!_dbus_string_append_byte (&message->header, 0))
    return FALSE;

  message->header_fields[FIELD_HEADER_LENGTH].offset = 4;
  if (!_dbus_marshal_uint32 (&message->header, message->byte_order, 0))
    return FALSE;

  message->header_fields[FIELD_BODY_LENGTH].offset = 8;
  if (!_dbus_marshal_uint32 (&message->header, message->byte_order, 0))
    return FALSE;

  message->header_fields[FIELD_CLIENT_SERIAL].offset = 12;
  if (!_dbus_marshal_int32 (&message->header, message->byte_order, -1))
    return FALSE;
  
  /* Marshal message service */
  if (service != NULL)
    {
      if (!append_string_field (message,
                                FIELD_SERVICE,
                                DBUS_HEADER_FIELD_SERVICE,
                                service))
        return FALSE;
    }

  _dbus_assert (name != NULL);
  if (!append_string_field (message,
                            FIELD_NAME,
                            DBUS_HEADER_FIELD_NAME,
                            name))
    return FALSE;
  
  return TRUE;
}

/**
 * Locks a message. Allows checking that applications don't keep a
 * reference to a message in the outgoing queue and change it
 * underneath us. Messages are locked when they enter the outgoing
 * queue (dbus_connection_send_message()), and the library complains
 * if the message is modified while locked.
 *
 * @param message the message to lock.
 */
void
_dbus_message_lock (DBusMessage  *message)
{
  if (!message->locked)
    {
      /* Fill in our lengths */
      set_uint_field (message,
                      FIELD_HEADER_LENGTH,
                      _dbus_string_get_length (&message->header));

      set_uint_field (message,
                      FIELD_BODY_LENGTH,
                      _dbus_string_get_length (&message->body));

      message->locked = TRUE;
    }
}

/** @} */

/**
 * @defgroup DBusMessage DBusMessage
 * @ingroup  DBus
 * @brief Message to be sent or received over a DBusConnection.
 *
 * A DBusMessage is the most basic unit of communication over a
 * DBusConnection. A DBusConnection represents a stream of messages
 * received from a remote application, and a stream of messages
 * sent to a remote application.
 *
 * @{
 */

/**
 * @typedef DBusMessage
 *
 * Opaque data type representing a message received from or to be
 * sent to another application.
 */

static DBusMessage*
dbus_message_new_empty_header (void)
{
  DBusMessage *message;
  int i;
  
  message = dbus_new0 (DBusMessage, 1);
  if (message == NULL)
    return NULL;
  
  message->refcount = 1;
  message->byte_order = DBUS_COMPILER_BYTE_ORDER;
  message->client_serial = -1;
  message->reply_serial = -1;
  
  i = 0;
  while (i < FIELD_LAST)
    {
      message->header_fields[i].offset = -1;
      ++i;
    }
  
  if (!_dbus_string_init (&message->header, _DBUS_INT_MAX))
    {
      dbus_free (message);
      return NULL;
    }
  
  if (!_dbus_string_init (&message->body, _DBUS_INT_MAX))
    {
      _dbus_string_free (&message->header);
      dbus_free (message);
      return NULL;
    }
  
  return message;
}


/**
 * Constructs a new message. Returns #NULL if memory can't be
 * allocated for the message. The service may be #NULL in which case
 * no service is set; this is appropriate when using D-BUS in a
 * peer-to-peer context (no message bus).
 *
 * @todo reverse the arguments, first 'name' then 'service'
 * as 'name' is more fundamental
 *
 * @param service service that the message should be sent to or #NULL
 * @param name name of the message
 * @returns a new DBusMessage, free with dbus_message_unref()
 * @see dbus_message_unref()
 */
DBusMessage*
dbus_message_new (const char *service,
		  const char *name)
{
  DBusMessage *message;

  message = dbus_message_new_empty_header ();
  if (message == NULL)
    return NULL;
  
  if (!dbus_message_create_header (message, service, name))
    {
      dbus_message_unref (message);
      return NULL;
    }
  
  return message;
}

/**
 * Constructs a message that is a reply to some other
 * message. Returns #NULL if memory can't be allocated
 * for the message.
 *
 * @param original_message the message which the created
 * message is a reply to.
 * @returns a new DBusMessage, free with dbus_message_unref()
 * @see dbus_message_new(), dbus_message_unref()
 */ 
DBusMessage*
dbus_message_new_reply (DBusMessage *original_message)
{
  DBusMessage *message;
  const char *sender, *name;

  sender = get_string_field (original_message,
                             FIELD_SENDER, NULL);
  name = get_string_field (original_message,
			   FIELD_NAME, NULL);

  /* sender is allowed to be null here in peer-to-peer case */
  
  message = dbus_message_new (sender, name);
  
  if (message == NULL)
    return NULL;

  if (!dbus_message_set_reply_serial (message,
                                      dbus_message_get_serial (original_message)))
    {
      dbus_message_unref (message);
      return NULL;
    }

  return message;
}

/**
 * Creates a new message that is an error reply to a certain message.
 *
 * @param original_message the original message
 * @param error_name the error name
 * @param error_message the error message string
 * @returns a new error message
 */
DBusMessage*
dbus_message_new_error_reply (DBusMessage *original_message,
			      const char  *error_name,
			      const char  *error_message)
{
  DBusMessage *message;
  const char *sender;

  sender = get_string_field (original_message,
                             FIELD_SENDER, NULL);
  
  _dbus_assert (sender != NULL);
  
  message = dbus_message_new (sender, error_name);
  
  if (message == NULL)
    return NULL;

  if (!dbus_message_set_reply_serial (message,
                                      dbus_message_get_serial (original_message)))
    {
      dbus_message_unref (message);
      return NULL;
    }

  if (!dbus_message_append_string (message, error_message))
    {
      dbus_message_unref (message);
      return NULL;
    }

  dbus_message_set_is_error (message, TRUE);
  
  return message;
}

/**
 * Creates a new message that is an exact replica of the message
 * specified, except that its refcount is set to 1.
 *
 * @param message the message.
 * @returns the new message.
 */
DBusMessage *
dbus_message_copy (const DBusMessage *message)
{
  DBusMessage *retval;
  int i;
  
  retval = dbus_new0 (DBusMessage, 1);
  if (retval == NULL)
    return NULL;
  
  retval->refcount = 1;
  retval->byte_order = message->byte_order;
  retval->client_serial = message->client_serial;
  retval->reply_serial = message->reply_serial;
  retval->header_padding = message->header_padding;
  retval->locked = FALSE;
  
  if (!_dbus_string_init (&retval->header, _DBUS_INT_MAX))
    {
      dbus_free (retval);
      return NULL;
    }
  
  if (!_dbus_string_init (&retval->body, _DBUS_INT_MAX))
    {
      _dbus_string_free (&retval->header);
      dbus_free (retval);
      return NULL;
    }

  if (!_dbus_string_copy (&message->header, 0,
			  &retval->header, 0))
    {
      _dbus_string_free (&retval->header);
      _dbus_string_free (&retval->body);
      dbus_free (retval);

      return NULL;
    }

  if (!_dbus_string_copy (&message->body, 0,
			  &retval->body, 0))
    {
      _dbus_string_free (&retval->header);
      _dbus_string_free (&retval->body);
      dbus_free (retval);

      return NULL;
    }

  for (i = 0; i < FIELD_LAST; i++)
    {
      retval->header_fields[i].offset = message->header_fields[i].offset;
    }
  
  return retval;
}


/**
 * Increments the reference count of a DBusMessage.
 *
 * @param message The message
 * @see dbus_message_unref
 */
void
dbus_message_ref (DBusMessage *message)
{
  dbus_atomic_t refcount;

  refcount = _dbus_atomic_inc (&message->refcount);
  _dbus_assert (refcount > 1);
}

/**
 * Decrements the reference count of a DBusMessage.
 *
 * @param message The message
 * @see dbus_message_ref
 */
void
dbus_message_unref (DBusMessage *message)
{
  dbus_atomic_t refcount;

  refcount = _dbus_atomic_dec (&message->refcount);
  
  _dbus_assert (refcount >= 0);

  if (refcount == 0)
    {
      if (message->size_counter != NULL)
        {
          _dbus_counter_adjust (message->size_counter,
                                - message->size_counter_delta);
          _dbus_counter_unref (message->size_counter);
        }
      
      _dbus_string_free (&message->header);
      _dbus_string_free (&message->body);
      
      dbus_free (message);
    }
}

/**
 * Gets the name of a message.
 *
 * @param message the message
 * @returns the message name (should not be freed)
 */
const char*
dbus_message_get_name (DBusMessage *message)
{
  return get_string_field (message, FIELD_NAME, NULL);
}

/**
 * Gets the destination service of a message.
 *
 * @todo I think if we have set_sender/get_sender,
 * this function might be better named set_destination/
 * get_destination for clarity, as the sender
 * is also a service name.
 * 
 * @param message the message
 * @returns the message destination service (should not be freed)
 */
const char*
dbus_message_get_service (DBusMessage *message)
{
  return get_string_field (message, FIELD_SERVICE, NULL);
}

/**
 * Appends fields to a message given a variable argument
 * list. The variable argument list should contain the type
 * of the argument followed by the value to add. Array values
 * are specified by a pointer to the array followed by an int
 * giving the length of the array. The list is terminated
 * with 0.
 *
 * @param message the message
 * @param first_arg_type type of the first argument
 * @param ... value of first argument, list of additional type-value pairs
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_args (DBusMessage *message,
			  int first_arg_type,
			  ...)
{
  dbus_bool_t retval;
  va_list var_args;

  va_start (var_args, first_arg_type);
  retval = dbus_message_append_args_valist (message,
					    first_arg_type,
					    var_args);
  va_end (var_args);

  return retval;
}

/**
 * This function takes a va_list for use by language bindings
 *
 * @see dbus_message_append_args.  
 * @param message the message
 * @param first_arg_type type of first argument
 * @param var_args value of first argument, then list of type/value pairs
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_args_valist (DBusMessage *message,
				 int          first_arg_type,
				 va_list      var_args)
{
  int type, old_len;

  old_len = _dbus_string_get_length (&message->body);
  
  type = first_arg_type;

  while (type != 0)
    {
      switch (type)
	{
	case DBUS_TYPE_NIL:
	  if (!dbus_message_append_nil (message))
	    goto enomem;
	case DBUS_TYPE_BOOLEAN:
	  if (!dbus_message_append_boolean (message, va_arg (var_args, dbus_bool_t)))
	    goto enomem;
	  break;
	case DBUS_TYPE_INT32:
	  if (!dbus_message_append_int32 (message, va_arg (var_args, dbus_int32_t)))
	    goto enomem;
	  break;
	case DBUS_TYPE_UINT32:
	  if (!dbus_message_append_uint32 (message, va_arg (var_args, dbus_uint32_t)))
	    goto enomem;	    
	  break;
	case DBUS_TYPE_DOUBLE:
	  if (!dbus_message_append_double (message, va_arg (var_args, double)))
	    goto enomem;
	  break;
	case DBUS_TYPE_STRING:
	  if (!dbus_message_append_string (message, va_arg (var_args, const char *)))
	    goto enomem;
	  break;
	case DBUS_TYPE_BOOLEAN_ARRAY:
	  {
	    int len;
	    unsigned char *data;

	    data = va_arg (var_args, unsigned char *);
	    len = va_arg (var_args, int);

	    if (!dbus_message_append_boolean_array (message, data, len))
	      goto enomem;
	  }
	  break;
	case DBUS_TYPE_INT32_ARRAY:
	  {
	    int len;
	    dbus_int32_t *data;

	    data = va_arg (var_args, dbus_int32_t *);
	    len = va_arg (var_args, int);

	    if (!dbus_message_append_int32_array (message, data, len))
	      goto enomem;
	  }
	  break;
	case DBUS_TYPE_UINT32_ARRAY:
	  {
	    int len;
	    dbus_uint32_t *data;

	    data = va_arg (var_args, dbus_uint32_t *);
	    len = va_arg (var_args, int);

	    if (!dbus_message_append_uint32_array (message, data, len))
	      goto enomem;
	  }
	  break;
	case DBUS_TYPE_DOUBLE_ARRAY:
	  {
	    int len;
	    double *data;

	    data = va_arg (var_args, double *);
	    len = va_arg (var_args, int);

	    if (!dbus_message_append_double_array (message, data, len))
	      goto enomem;
	  }
	  break;
	case DBUS_TYPE_BYTE_ARRAY:
	  {
	    int len;
	    unsigned char *data;

	    data = va_arg (var_args, unsigned char *);
	    len = va_arg (var_args, int);

	    if (!dbus_message_append_byte_array (message, data, len))
	      goto enomem;
	  }
          break;
	case DBUS_TYPE_STRING_ARRAY:
	  {
	    int len;
	    const char **data;
	    
	    data = va_arg (var_args, const char **);
	    len = va_arg (var_args, int);

	    if (!dbus_message_append_string_array (message, data, len))
	      goto enomem;
	  }
	  break;
	case DBUS_TYPE_DICT:
	  {
	    DBusDict *dict;

	    dict = va_arg (var_args, DBusDict *);

	    if (!dbus_message_append_dict (message, dict))
	      goto enomem;
	    break;
	  }
	default:
	  _dbus_warn ("Unknown field type %d\n", type);
	}

      type = va_arg (var_args, int);
    }

  return TRUE;

 enomem:
  return FALSE;
}

/**
 * Appends a nil value to the message
 *
 * @param message the message
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_nil (DBusMessage *message)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_NIL))
      return FALSE;
  else
    return TRUE;
}

/**
 * Appends a boolean value to the message
 *
 * @param message the message
 * @param value the boolean value
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_boolean (DBusMessage  *message,
			     dbus_bool_t   value)
{
  _dbus_assert (!message->locked);
  
  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_BOOLEAN))
    return FALSE;

  if (!_dbus_string_append_byte (&message->body, (value != FALSE)))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a 32 bit signed integer to the message.
 *
 * @param message the message
 * @param value the integer value
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_int32 (DBusMessage  *message,
			   dbus_int32_t  value)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_INT32))
      return FALSE;
  
  if (!_dbus_marshal_int32 (&message->body, message->byte_order, value))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a 32 bit unsigned integer to the message.
 *
 * @param message the message
 * @param value the integer value
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_uint32 (DBusMessage   *message,
			    dbus_uint32_t  value)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_UINT32))
      return FALSE;
  
  if (!_dbus_marshal_uint32 (&message->body, message->byte_order, value))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;      
}

/**
 * Appends a double value to the message.
 *
 * @param message the message
 * @param value the double value
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_double (DBusMessage *message,
			    double       value)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_DOUBLE))
    return FALSE;

  if (!_dbus_marshal_double (&message->body, message->byte_order, value))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }
  
  return TRUE;
}

/**
 * Appends a UTF-8 string to the message.
 *
 * @param message the message
 * @param value the string
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_string (DBusMessage *message,
			    const char  *value)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_STRING))
      return FALSE;
  
  if (!_dbus_marshal_string (&message->body, message->byte_order, value))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;      
    }

  return TRUE;
}

/**
 * Appends a boolean array to the message.
 *
 * @param message the message
 * @param value the array
 * @param len the length of the array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_boolean_array (DBusMessage         *message,
				   unsigned const char *value,
				   int                  len)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_BOOLEAN_ARRAY))
    return FALSE;

  if (!_dbus_marshal_byte_array (&message->body, message->byte_order, value, len))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a 32 bit signed integer array to the message.
 *
 * @param message the message
 * @param value the array
 * @param len the length of the array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_int32_array (DBusMessage        *message,
				 const dbus_int32_t *value,
				 int                 len)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_INT32_ARRAY))
    return FALSE;

  if (!_dbus_marshal_int32_array (&message->body, message->byte_order,
				  value, len))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a 32 bit unsigned integer array to the message.
 *
 * @param message the message
 * @param value the array
 * @param len the length of the array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_uint32_array (DBusMessage         *message,
				  const dbus_uint32_t *value,
				  int                  len)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_UINT32_ARRAY))
    return FALSE;

  if (!_dbus_marshal_uint32_array (&message->body, message->byte_order,
				  value, len))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a double array to the message.
 *
 * @param message the message
 * @param value the array
 * @param len the length of the array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_double_array (DBusMessage  *message,
				  const double *value,
				  int           len)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_DOUBLE_ARRAY))
    return FALSE;

  if (!_dbus_marshal_double_array (&message->body, message->byte_order,
				   value, len))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a byte array to the message.
 *
 * @param message the message
 * @param value the array
 * @param len the length of the array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_byte_array (DBusMessage         *message,
				unsigned const char *value,
				int                 len)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_BYTE_ARRAY))
    return FALSE;
  
  if (!_dbus_marshal_byte_array (&message->body, message->byte_order, value, len))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }
      
  return TRUE;
}

/**
 * Appends a string array to the message.
 *
 * @param message the message
 * @param value the array
 * @param len the length of the array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_string_array (DBusMessage  *message,
				  const char  **value,
				  int           len)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_STRING_ARRAY))
    return FALSE;

  if (!_dbus_marshal_string_array (&message->body, message->byte_order,
				   (const char **)value, len))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Appends a dict to the message.
 *
 * @param message the message
 * @param dict the dict
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_append_dict (DBusMessage *message,
			  DBusDict    *dict)
{
  _dbus_assert (!message->locked);

  if (!_dbus_string_append_byte (&message->body, DBUS_TYPE_DICT))
    return FALSE;

  if (!_dbus_marshal_dict (&message->body, message->byte_order, dict))
    {
      _dbus_string_shorten (&message->body, 1);
      return FALSE;
    }

  return TRUE;
}

/**
 * Gets arguments from a message given a variable argument list.
 * The variable argument list should contain the type of the
 * argumen followed by a pointer to where the value should be
 * stored. The list is terminated with 0.
 *
 * @param message the message
 * @param error error to be filled in on failure
 * @param first_arg_type the first argument type
 * @param ... location for first argument value, then list of type-location pairs
 * @returns #FALSE if the error was set
 */
dbus_bool_t
dbus_message_get_args (DBusMessage *message,
                       DBusError   *error,
		       int          first_arg_type,
		       ...)
{
  DBusResultCode retval;
  va_list var_args;

  va_start (var_args, first_arg_type);
  retval = dbus_message_get_args_valist (message, error, first_arg_type, var_args);
  va_end (var_args);

  return retval;
}

/**
 * This function takes a va_list for use by language bindings
 *
 * @todo this function (or some lower-level non-convenience function)
 * needs better error handling; should allow the application to
 * distinguish between out of memory, and bad data from the remote
 * app. It also needs to not leak a bunch of args when it gets
 * to the arg that's bad, as that would be a security hole
 * (allow one app to force another to leak memory)
 *
 * @todo We need to free the argument data when an error occurs.
 *
 * @see dbus_message_get_args
 * @param message the message
 * @param error error to be filled in
 * @param first_arg_type type of the first argument
 * @param var_args return location for first argument, followed by list of type/location pairs
 * @returns #FALSE if error was set
 */
dbus_bool_t
dbus_message_get_args_valist (DBusMessage *message,
                              DBusError   *error,
			      int          first_arg_type,
			      va_list      var_args)
{
  int spec_type, msg_type, i;
  DBusMessageIter *iter;
  dbus_bool_t retval;
  
  iter = dbus_message_get_args_iter (message);

  if (iter == NULL)
    {
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                      "No memory to get message arguments");
      return FALSE;
    }

  retval = FALSE;
  
  spec_type = first_arg_type;
  i = 0;
  
  while (spec_type != 0)
    {
      msg_type = dbus_message_iter_get_arg_type (iter);      
      
      if (msg_type != spec_type)
	{
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Argument %d is specified to be of type \"%s\", but "
                          "is actually of type \"%s\"\n", i,
                          _dbus_type_to_string (spec_type),
                          _dbus_type_to_string (msg_type));

          goto out;
	}

      switch (spec_type)
	{
	case DBUS_TYPE_NIL:
	  break;
	case DBUS_TYPE_BOOLEAN:
	  {
	    dbus_bool_t *ptr;

	    ptr = va_arg (var_args, dbus_bool_t *);

	    *ptr = dbus_message_iter_get_boolean (iter);
	    break;
	  }
	case DBUS_TYPE_INT32:
	  {
	    dbus_int32_t *ptr;

	    ptr = va_arg (var_args, dbus_int32_t *);

	    *ptr = dbus_message_iter_get_int32 (iter);
	    break;
	  }
	case DBUS_TYPE_UINT32:
	  {
	    dbus_uint32_t *ptr;

	    ptr = va_arg (var_args, dbus_uint32_t *);

	    *ptr = dbus_message_iter_get_uint32 (iter);
	    break;
	  }

	case DBUS_TYPE_DOUBLE:
	  {
	    double *ptr;

	    ptr = va_arg (var_args, double *);

	    *ptr = dbus_message_iter_get_double (iter);
	    break;
	  }

	case DBUS_TYPE_STRING:
	  {
	    char **ptr;

	    ptr = va_arg (var_args, char **);

	    *ptr = dbus_message_iter_get_string (iter);

	    if (!*ptr)
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    
	    break;
	  }

	case DBUS_TYPE_BOOLEAN_ARRAY:
	  {
	    unsigned char **ptr;
	    int *len;

	    ptr = va_arg (var_args, unsigned char **);
	    len = va_arg (var_args, int *);

	    if (!dbus_message_iter_get_boolean_array (iter, ptr, len))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    break;
	  }
	  
	case DBUS_TYPE_INT32_ARRAY:
	  {
	    dbus_int32_t **ptr;
	    int *len;

	    ptr = va_arg (var_args, dbus_int32_t **);
	    len = va_arg (var_args, int *);

	    if (!dbus_message_iter_get_int32_array (iter, ptr, len))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    
	    break;
	  }

	case DBUS_TYPE_UINT32_ARRAY:
	  {
	    dbus_uint32_t **ptr;
	    int *len;

	    ptr = va_arg (var_args, dbus_uint32_t **);
	    len = va_arg (var_args, int *);

	    if (!dbus_message_iter_get_uint32_array (iter, ptr, len))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    
	    break;
	  }

	case DBUS_TYPE_DOUBLE_ARRAY:
	  {
	    double **ptr;
	    int *len;

	    ptr = va_arg (var_args, double **);
	    len = va_arg (var_args, int *);

	    if (!dbus_message_iter_get_double_array (iter, ptr, len))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    break;
	  }
	  
	case DBUS_TYPE_BYTE_ARRAY:
	  {
	    unsigned char **ptr;
	    int *len;

	    ptr = va_arg (var_args, unsigned char **);
	    len = va_arg (var_args, int *);

	    if (!dbus_message_iter_get_byte_array (iter, ptr, len))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    break;
	  }
	case DBUS_TYPE_STRING_ARRAY:
	  {
	    char ***ptr;
	    int *len;

	    ptr = va_arg (var_args, char ***);
	    len = va_arg (var_args, int *);

	    if (!dbus_message_iter_get_string_array (iter, ptr, len))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    break;
	  }
	case DBUS_TYPE_DICT:
	  {
	    DBusDict **dict;

	    dict = va_arg (var_args, DBusDict **);

	    if (!dbus_message_iter_get_dict (iter, dict))
              {
                dbus_set_error (error, DBUS_ERROR_NO_MEMORY,
                                "No memory for argument %d", i);
                goto out;
              }
	    break;
	  }
	default:	  
	  _dbus_warn ("Unknown field type %d\n", spec_type);
	}
      
      spec_type = va_arg (var_args, int);
      if (spec_type != 0 && !dbus_message_iter_next (iter))
        {
          dbus_set_error (error, DBUS_ERROR_INVALID_ARGS,
                          "Message has only %d arguments, but more were expected", i);
          goto out;
        }

      i++;
    }
  
  retval = TRUE;
  
 out:
  dbus_message_iter_unref (iter);
  return retval;
}

/**
 * Returns a DBusMessageIter representing the arguments of the
 * message passed in.
 *
 * @todo IMO the message iter should follow the GtkTextIter pattern,
 * a static object with a "stamp" value used to detect invalid
 * iter uses (uninitialized or after changing the message).
 * ref/unref is kind of annoying to deal with, and slower too.
 * This implies not ref'ing the message from the iter.
 *
 * @todo I'd also name this dbus_message_iter_new() or
 * for the static object dbus_message_iter_init() rather
 * than making it a method on the message
 *
 * @param message the message
 * @returns a new iter.
 */
DBusMessageIter *
dbus_message_get_args_iter (DBusMessage *message)
{
  DBusMessageIter *iter;
  
  iter = dbus_new (DBusMessageIter, 1);

  if (iter != NULL)
    {
      dbus_message_ref (message);
  
      iter->refcount = 1;
      iter->message = message;
      iter->pos = 0;
    }
  
  return iter;
}

/**
 * Increments the reference count of a DBusMessageIter.
 *
 * @param iter the message iter
 * @see dbus_message_iter_unref
 */
void
dbus_message_iter_ref (DBusMessageIter *iter)
{
  _dbus_assert (iter->refcount > 0);
  
  iter->refcount += 1;
}

/**
 * Decrements the reference count of a DBusMessageIter.
 *
 * @param iter The message iter
 * @see dbus_message_iter_ref
 */
void
dbus_message_iter_unref (DBusMessageIter *iter)
{
  _dbus_assert (iter->refcount > 0);

  iter->refcount -= 1;

  if (iter->refcount == 0)
    {
      dbus_message_unref (iter->message);

      dbus_free (iter);
    }
}

/**
 * Checks if an iterator has any more fields.
 *
 * @param iter the message iter
 * @returns #TRUE if there are more fields
 * following
 */
dbus_bool_t
dbus_message_iter_has_next (DBusMessageIter *iter)
{
  int end_pos;
  
  if (!_dbus_marshal_get_arg_end_pos (&iter->message->body,
                                      iter->message->byte_order,
                                      iter->pos, &end_pos))
    return FALSE;
  
  if (end_pos >= _dbus_string_get_length (&iter->message->body))
    return FALSE;
  
  return TRUE;  
}

/**
 * Moves the iterator to the next field.
 *
 * @param iter The message iter
 * @returns #TRUE if the iterator was moved to the next field
 */
dbus_bool_t
dbus_message_iter_next (DBusMessageIter *iter)
{
  int end_pos;
  
  if (!_dbus_marshal_get_arg_end_pos (&iter->message->body,
                                      iter->message->byte_order,
                                      iter->pos, &end_pos))
    return FALSE;

  if (end_pos >= _dbus_string_get_length (&iter->message->body))
    return FALSE;

  iter->pos = end_pos;

  return TRUE;
}

/**
 * Returns the argument type of the argument that the
 * message iterator points at.
 *
 * @param iter the message iter
 * @returns the field type
 */
int
dbus_message_iter_get_arg_type (DBusMessageIter *iter)
{
  const char *data;

  if (iter->pos >= _dbus_string_get_length (&iter->message->body))
    return DBUS_TYPE_INVALID;

  _dbus_string_get_const_data_len (&iter->message->body, &data, iter->pos, 1);

  if (*data > DBUS_TYPE_INVALID && *data <= DBUS_TYPE_DICT)
    return *data;

  return DBUS_TYPE_INVALID;
}

/**
 * Returns the string value that an iterator may point to.
 * Note that you need to check that the iterator points to
 * a string value before using this function.
 *
 * @see dbus_message_iter_get_arg_type
 * @param iter the message iter
 * @returns the string
 */
char *
dbus_message_iter_get_string (DBusMessageIter *iter)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_STRING);

  return _dbus_demarshal_string (&iter->message->body, iter->message->byte_order,
                                 iter->pos + 1, NULL);
}

/**
 * Returns the boolean value that an iterator may point to.
 * Note that you need to check that the iterator points to
 * a boolean value before using this function.
 *
 * @see dbus_message_iter_get_arg_type
 * @param iter the message iter
 * @returns the string
 */
dbus_bool_t
dbus_message_iter_get_boolean (DBusMessageIter *iter)
{
  unsigned char value;
  
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_BOOLEAN);

  value = _dbus_string_get_byte (&iter->message->body, iter->pos + 1);
  
  return value;
}

/**
 * Returns the 32 bit signed integer value that an iterator may point to.
 * Note that you need to check that the iterator points to
 * an integer value before using this function.
 *
 * @see dbus_message_iter_get_arg_type
 * @param iter the message iter
 * @returns the integer
 */
int
dbus_message_iter_get_int32 (DBusMessageIter *iter)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_INT32);
  
  return _dbus_demarshal_int32 (&iter->message->body, iter->message->byte_order,
				iter->pos + 1, NULL);
}

/**
 * Returns the 32 bit unsigned integer value that an iterator may point to.
 * Note that you need to check that the iterator points to
 * an unsigned integer value before using this function.
 *
 * @see dbus_message_iter_get_arg_type
 * @param iter the message iter
 * @returns the integer
 */
int
dbus_message_iter_get_uint32 (DBusMessageIter *iter)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_UINT32);
  
  return _dbus_demarshal_uint32 (&iter->message->body, iter->message->byte_order,
				 iter->pos + 1, NULL);
}

/**
 * Returns the double value that an iterator may point to.
 * Note that you need to check that the iterator points to
 * a string value before using this function.
 *
 * @see dbus_message_iter_get_arg_type
 * @param iter the message iter
 * @returns the double
 */
double
dbus_message_iter_get_double (DBusMessageIter *iter)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_DOUBLE);
  
  return _dbus_demarshal_double (&iter->message->body, iter->message->byte_order,
				 iter->pos + 1, NULL);
}

/**
 * Returns the boolean array that the iterator may point to. Note that
 * you need to check that the iterator points to an array of the
 * correct type prior to using this function.
 *
 * @param iter the iterator
 * @param value return location for the array
 * @param len return location for the array length
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_boolean_array (DBusMessageIter   *iter,
				     unsigned char    **value,
				     int               *len)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_BOOLEAN_ARRAY);

  if (!_dbus_demarshal_byte_array (&iter->message->body, iter->message->byte_order,
				   iter->pos + 1, NULL, value, len))
    return FALSE;
  else
    return TRUE;
}

/**
 * Returns the 32 bit signed integer array that the iterator may point
 * to. Note that you need to check that the iterator points to an
 * array of the correct type prior to using this function.
 *
 * @param iter the iterator
 * @param value return location for the array
 * @param len return location for the array length
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_int32_array  (DBusMessageIter *iter,
				    dbus_int32_t   **value,
				    int             *len)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_INT32_ARRAY);

  if (!_dbus_demarshal_int32_array (&iter->message->body, iter->message->byte_order,
				    iter->pos + 1, NULL, value, len))
    return FALSE;
  else
    return TRUE;
}

/**
 * Returns the 32 bit unsigned integer array that the iterator may point
 * to. Note that you need to check that the iterator points to an
 * array of the correct type prior to using this function.
 *
 * @param iter the iterator
 * @param value return location for the array
 * @param len return location for the array length
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_uint32_array  (DBusMessageIter *iter,
				     dbus_uint32_t  **value,
				     int             *len)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_UINT32_ARRAY);

  if (!_dbus_demarshal_uint32_array (&iter->message->body, iter->message->byte_order,
				     iter->pos + 1, NULL, value, len))
    return FALSE;
  else
    return TRUE;
}

/**
 * Returns the double array that the iterator may point to. Note that
 * you need to check that the iterator points to an array of the
 * correct type prior to using this function.
 *
 * @param iter the iterator
 * @param value return location for the array
 * @param len return location for the array length
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_double_array  (DBusMessageIter *iter,
				     double         **value,
				     int             *len)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_DOUBLE_ARRAY);

  if (!_dbus_demarshal_double_array (&iter->message->body, iter->message->byte_order,
				     iter->pos + 1, NULL, value, len))
    return FALSE;
  else
    return TRUE;
}

/**
 * Returns the byte array that the iterator may point to.
 * Note that you need to check that the iterator points
 * to a byte array prior to using this function.
 *
 * @param iter the iterator
 * @param value return location for array values
 * @param len return location for length of byte array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_byte_array (DBusMessageIter  *iter,
				  unsigned char   **value,
                                  int              *len)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_BYTE_ARRAY);

  if (!_dbus_demarshal_byte_array (&iter->message->body, iter->message->byte_order,
				   iter->pos + 1, NULL, value, len))
    return FALSE;
  else
    return TRUE;
}

/**
 * Returns the string array that the iterator may point to.
 * Note that you need to check that the iterator points
 * to a byte array prior to using this function.
 *
 * The returned value is a #NULL-terminated array of strings.
 * Each string is a separate malloc block, and the array
 * itself is a malloc block. You can free this type of
 * string array with dbus_free_string_array().
 *
 * @param iter the iterator
 * @param value return location for string values
 * @param len return location for length of byte array
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_string_array (DBusMessageIter *iter,
				    char          ***value,
				    int             *len)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_STRING_ARRAY);

  if (!_dbus_demarshal_string_array (&iter->message->body, iter->message->byte_order,
				     iter->pos + 1, NULL, value, len))
    return FALSE;
  else
    return TRUE;
}

/**
 * Returns the dict that the iterator may point to.
 * Note that you need to check that the iterator points
 * to a dict prior to using this function.
 *
 * @param iter the iterator
 * @param dict return location for dict
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_message_iter_get_dict (DBusMessageIter *iter,
			    DBusDict       **dict)
{
  _dbus_assert (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_DICT);

  if (!_dbus_demarshal_dict (&iter->message->body, iter->message->byte_order,
			     iter->pos + 1, NULL, dict))
    return FALSE;
  else
    return TRUE;
}

/**
 * Sets the message sender.
 *
 * @param message the message
 * @param sender the sender
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_message_set_sender (DBusMessage  *message,
                         const char   *sender)
{
  _dbus_assert (!message->locked);

  if (sender == NULL)
    {
      delete_string_field (message, FIELD_SENDER);
      return TRUE;
    }
  else
    {
      return set_string_field (message,
                               FIELD_SENDER,
                               sender);
    }
}

/**
 * Sets a flag indicating that the message is an error reply
 * message, i.e. an "exception" rather than a normal response.
 *
 * @param message the message
 * @param is_error_reply #TRUE if this is an error message.
 */
void
dbus_message_set_is_error (DBusMessage *message,
                           dbus_bool_t  is_error_reply)
{
  char *header;
  
  _dbus_assert (!message->locked);
  
  _dbus_string_get_data_len (&message->header, &header, 1, 1);
  
  if (is_error_reply)
    *header |= DBUS_HEADER_FLAG_ERROR;
  else
    *header &= ~DBUS_HEADER_FLAG_ERROR;    
}

/**
 * Returns #TRUE if the message is an error
 * reply to some previous message we sent.
 *
 * @param message the message
 * @returns #TRUE if the message is an error
 */
dbus_bool_t
dbus_message_get_is_error (DBusMessage *message)
{
  const char *header;

  _dbus_string_get_const_data_len (&message->header, &header, 1, 1);

  return (*header & DBUS_HEADER_FLAG_ERROR) != 0;
}

/**
 * Gets the service which originated this message,
 * or #NULL if unknown or inapplicable.
 *
 * @param message the message
 * @returns the service name or #NULL
 */
const char*
dbus_message_get_sender (DBusMessage *message)
{
  return get_string_field (message, FIELD_SENDER, NULL);
}

/**
 * Checks whether the message has the given name.
 * If the message has no name or has a different
 * name, returns #FALSE.
 *
 * @param message the message
 * @param name the name to check (must not be #NULL)
 * 
 * @returns #TRUE if the message has the given name
 */
dbus_bool_t
dbus_message_name_is (DBusMessage *message,
		      const char  *name)
{
  const char *n;

  _dbus_assert (name != NULL);
  
  n = dbus_message_get_name (message);

  if (n && strcmp (n, name) == 0)
    return TRUE;
  else
    return FALSE;
}

/**
 * Checks whether the message was sent to the given service.  If the
 * message has no service specified or has a different name, returns
 * #FALSE.
 *
 * @param message the message
 * @param service the service to check (must not be #NULL)
 * 
 * @returns #TRUE if the message has the given destination service
 */
dbus_bool_t
dbus_message_service_is (DBusMessage  *message,
                         const char   *service)
{
  const char *s;

  _dbus_assert (service != NULL);
  
  s = dbus_message_get_service (message);

  if (s && strcmp (s, service) == 0)
    return TRUE;
  else
    return FALSE;
}

/**
 * Checks whether the message has the given service as its sender.  If
 * the message has no sender specified or has a different sender,
 * returns #FALSE. Note that if a peer application owns multiple
 * services, its messages will have only one of those services as the
 * sender (usually the base service). So you can't use this
 * function to prove the sender didn't own service Foo, you can
 * only use it to prove that it did.
 *
 * @param message the message
 * @param service the service to check (must not be #NULL)
 * 
 * @returns #TRUE if the message has the given origin service
 */
dbus_bool_t
dbus_message_sender_is (DBusMessage  *message,
                        const char   *service)
{
  const char *s;

  _dbus_assert (service != NULL);
  
  s = dbus_message_get_sender (message);

  if (s && strcmp (s, service) == 0)
    return TRUE;
  else
    return FALSE;
}

/** @} */

/**
 * @addtogroup DBusMessageInternals
 *
 * @{
 */
/**
 * @typedef DBusMessageLoader
 *
 * The DBusMessageLoader object encapsulates the process of converting
 * a byte stream into a series of DBusMessage. It buffers the incoming
 * bytes as efficiently as possible, and generates a queue of
 * messages. DBusMessageLoader is typically used as part of a
 * DBusTransport implementation. The DBusTransport then hands off
 * the loaded messages to a DBusConnection, making the messages
 * visible to the application.
 *
 * @todo write tests for break-loader that a) randomly delete header
 * fields and b) set string fields to zero-length and other funky
 * values.
 * 
 */

/* we definitely use signed ints for sizes, so don't exceed
 * _DBUS_INT_MAX; and add 16 for paranoia, since a message
 * over 128M is pretty nuts anyhow.
 */

/**
 * The maximum sane message size.
 */
#define MAX_SANE_MESSAGE_SIZE (_DBUS_INT_MAX/16)

/**
 * Implementation details of DBusMessageLoader.
 * All members are private.
 */
struct DBusMessageLoader
{
  int refcount;        /**< Reference count. */

  DBusString data;     /**< Buffered data */
  
  DBusList *messages;  /**< Complete messages. */

  long max_message_size; /**< Maximum size of a message */
  
  unsigned int buffer_outstanding : 1; /**< Someone is using the buffer to read */

  unsigned int corrupted : 1; /**< We got broken data, and are no longer working */
};

/**
 * The initial buffer size of the message loader.
 * 
 * @todo this should be based on min header size plus some average
 * body size, or something. Or rather, the min header size only, if we
 * want to try to read only the header, store that in a DBusMessage,
 * then read only the body and store that, etc., depends on
 * how we optimize _dbus_message_loader_get_buffer() and what
 * the exact message format is.
 */
#define INITIAL_LOADER_DATA_LEN 32

/**
 * Creates a new message loader. Returns #NULL if memory can't
 * be allocated.
 *
 * @returns new loader, or #NULL.
 */
DBusMessageLoader*
_dbus_message_loader_new (void)
{
  DBusMessageLoader *loader;

  loader = dbus_new0 (DBusMessageLoader, 1);
  if (loader == NULL)
    return NULL;
  
  loader->refcount = 1;

  /* Try to cap message size at something that won't *totally* hose
   * the system if we have a couple of them.
   */
  loader->max_message_size = _DBUS_ONE_MEGABYTE * 32;
  
  if (!_dbus_string_init (&loader->data, _DBUS_INT_MAX))
    {
      dbus_free (loader);
      return NULL;
    }

  /* preallocate the buffer for speed, ignore failure */
  _dbus_string_set_length (&loader->data, INITIAL_LOADER_DATA_LEN);
  _dbus_string_set_length (&loader->data, 0);
  
  return loader;
}

/**
 * Increments the reference count of the loader.
 *
 * @param loader the loader.
 */
void
_dbus_message_loader_ref (DBusMessageLoader *loader)
{
  loader->refcount += 1;
}

/**
 * Decrements the reference count of the loader and finalizes the
 * loader when the count reaches zero.
 *
 * @param loader the loader.
 */
void
_dbus_message_loader_unref (DBusMessageLoader *loader)
{
  loader->refcount -= 1;
  if (loader->refcount == 0)
    {
      _dbus_list_foreach (&loader->messages,
                          (DBusForeachFunction) dbus_message_unref,
                          NULL);
      _dbus_list_clear (&loader->messages);
      _dbus_string_free (&loader->data);
      dbus_free (loader);
    }
}

/**
 * Gets the buffer to use for reading data from the network.  Network
 * data is read directly into an allocated buffer, which is then used
 * in the DBusMessage, to avoid as many extra memcpy's as possible.
 * The buffer must always be returned immediately using
 * _dbus_message_loader_return_buffer(), even if no bytes are
 * successfully read.
 *
 * @todo this function can be a lot more clever. For example
 * it can probably always return a buffer size to read exactly
 * the body of the next message, thus avoiding any memory wastage
 * or reallocs.
 *
 * @todo we need to enforce a max length on strings in header fields.
 * 
 * @param loader the message loader.
 * @param buffer the buffer
 */
void
_dbus_message_loader_get_buffer (DBusMessageLoader  *loader,
                                 DBusString        **buffer)
{
  _dbus_assert (!loader->buffer_outstanding);

  *buffer = &loader->data;
  
  loader->buffer_outstanding = TRUE;
}

/**
 * The smallest header size that can occur. 
 * (It won't be valid)
 */
#define DBUS_MINIMUM_HEADER_SIZE 16

/** Pack four characters as in "abcd" into a uint32 */
#define FOUR_CHARS_TO_UINT32(a, b, c, d)                \
                      ((((dbus_uint32_t)a) << 24) |     \
                       (((dbus_uint32_t)b) << 16) |     \
                       (((dbus_uint32_t)c) << 8)  |     \
                       ((dbus_uint32_t)d))

/** DBUS_HEADER_FIELD_NAME packed into a dbus_uint32_t */
#define DBUS_HEADER_FIELD_NAME_AS_UINT32    \
  FOUR_CHARS_TO_UINT32 ('n', 'a', 'm', 'e')

/** DBUS_HEADER_FIELD_SERVICE packed into a dbus_uint32_t */
#define DBUS_HEADER_FIELD_SERVICE_AS_UINT32 \
  FOUR_CHARS_TO_UINT32 ('s', 'r', 'v', 'c')

/** DBUS_HEADER_FIELD_REPLY packed into a dbus_uint32_t */
#define DBUS_HEADER_FIELD_REPLY_AS_UINT32   \
  FOUR_CHARS_TO_UINT32 ('r', 'p', 'l', 'y')

/** DBUS_HEADER_FIELD_SENDER Packed into a dbus_uint32_t */
#define DBUS_HEADER_FIELD_SENDER_AS_UINT32  \
  FOUR_CHARS_TO_UINT32 ('s', 'n', 'd', 'r')

/* FIXME impose max length on name, srvc, sndr */
static dbus_bool_t
decode_header_data (const DBusString   *data,
		    int		        header_len,
		    int                 byte_order,
                    HeaderField         fields[FIELD_LAST],
		    int                *message_padding)
{
  const char *field;
  int pos, new_pos;
  int i;
  
  if (header_len < 16)
    return FALSE;
  
  i = 0;
  while (i < FIELD_LAST)
    {
      fields[i].offset = -1;
      ++i;
    }
  
  fields[FIELD_HEADER_LENGTH].offset = 4;
  fields[FIELD_BODY_LENGTH].offset = 8;   
  fields[FIELD_CLIENT_SERIAL].offset = 12;
  
  /* Now handle the named fields. A real named field is at least 4
   * bytes for the name, plus a type code (1 byte) plus padding.  So
   * if we have less than 8 bytes left, it must be alignment padding,
   * not a field. While >= 8 bytes can't be entirely alignment
   * padding.
   */  
  pos = 16;
  while ((pos + 7) < header_len)
    {
      pos = _DBUS_ALIGN_VALUE (pos, 4);
      
      if ((pos + 4) > header_len)
        return FALSE;      
      
      _dbus_string_get_const_data_len (data, &field, pos, 4);
      pos += 4;

      _dbus_assert (_DBUS_ALIGN_ADDRESS (field, 4) == field);

      /* I believe FROM_BE is right, but if not we'll find out
       * I guess. ;-)
       */
      switch (DBUS_UINT32_FROM_BE (*(int*)field))
        {
        case DBUS_HEADER_FIELD_SERVICE_AS_UINT32:
          if (fields[FIELD_SERVICE].offset >= 0)
            {
              _dbus_verbose ("%s field provided twice\n",
                             DBUS_HEADER_FIELD_SERVICE);
              return FALSE;
            }
          
          fields[FIELD_SERVICE].offset = _DBUS_ALIGN_VALUE (pos + 1, 4);
#if 0
          _dbus_verbose ("Found service name at offset %d\n",
                         fields[FIELD_SERVICE].offset);
#endif
          break;

        case DBUS_HEADER_FIELD_NAME_AS_UINT32:
          if (fields[FIELD_NAME].offset >= 0)
            {              
              _dbus_verbose ("%s field provided twice\n",
                             DBUS_HEADER_FIELD_NAME);
              return FALSE;
            }
          
          fields[FIELD_NAME].offset = _DBUS_ALIGN_VALUE (pos + 1, 4);

#if 0
          _dbus_verbose ("Found message name at offset %d\n",
                         fields[FIELD_NAME].offset);
#endif
          break;
	case DBUS_HEADER_FIELD_SENDER_AS_UINT32:
          if (fields[FIELD_SENDER].offset >= 0)
            {
              _dbus_verbose ("%s field provided twice\n",
                             DBUS_HEADER_FIELD_SENDER);
              return FALSE;
            }
          
          fields[FIELD_SENDER].offset = _DBUS_ALIGN_VALUE (pos + 1, 4);

          _dbus_verbose ("Found sender name at offset %d\n",
                         fields[FIELD_NAME].offset);
	  break;
          
	case DBUS_HEADER_FIELD_REPLY_AS_UINT32:
          if (fields[FIELD_REPLY_SERIAL].offset >= 0)
            {
              _dbus_verbose ("%s field provided twice\n",
                             DBUS_HEADER_FIELD_REPLY);
              return FALSE;
            }
          
          fields[FIELD_REPLY_SERIAL].offset = _DBUS_ALIGN_VALUE (pos + 1, 4);

          _dbus_verbose ("Found reply serial at offset %d\n",
                         fields[FIELD_REPLY_SERIAL].offset);
	  break;

        default:
	  _dbus_verbose ("Ignoring an unknown header field: %c%c%c%c at offset %d\n",
			 field[0], field[1], field[2], field[3], pos);
	}

      if (!_dbus_marshal_validate_arg (data, byte_order, pos, &new_pos))
        {
          _dbus_verbose ("Failed to validate argument to named header field\n");
          return FALSE;
        }

      if (new_pos > header_len)
        {
          _dbus_verbose ("Named header field tries to extend beyond header length\n");
          return FALSE;
        }
      
      pos = new_pos;
    }

  if (pos < header_len)
    {
      /* Alignment padding, verify that it's nul */
      _dbus_assert ((header_len - pos) < 8);

      if (!_dbus_string_validate_nul (data,
                                      pos, (header_len - pos)))
        {
          _dbus_verbose ("header alignment padding is not nul\n");
          return FALSE;
        }
    }

 if (fields[FIELD_NAME].offset < 0)
   {
     _dbus_verbose ("No %s field provided\n",
                    DBUS_HEADER_FIELD_NAME);
     return FALSE;
   }
  
  if (message_padding)
    *message_padding = header_len - pos;  
  
  return TRUE;
}

/**
 * Returns a buffer obtained from _dbus_message_loader_get_buffer(),
 * indicating to the loader how many bytes of the buffer were filled
 * in. This function must always be called, even if no bytes were
 * successfully read.
 *
 * @param loader the loader.
 * @param buffer the buffer.
 * @param bytes_read number of bytes that were read into the buffer.
 */
void
_dbus_message_loader_return_buffer (DBusMessageLoader  *loader,
                                    DBusString         *buffer,
                                    int                 bytes_read)
{
  _dbus_assert (loader->buffer_outstanding);
  _dbus_assert (buffer == &loader->data);

  loader->buffer_outstanding = FALSE;
}

/**
 * Converts buffered data into messages.
 *
 * @param loader the loader.
 * @returns #TRUE if we had enough memory to finish.
 */
dbus_bool_t
_dbus_message_loader_queue_messages (DBusMessageLoader *loader)
{
  if (loader->corrupted)
    return TRUE;

  while (_dbus_string_get_length (&loader->data) >= 16)
    {
      DBusMessage *message;      
      const char *header_data;
      int byte_order, header_len, body_len, header_padding;
      dbus_uint32_t header_len_unsigned, body_len_unsigned;
      
      _dbus_string_get_const_data_len (&loader->data, &header_data, 0, 16);

      _dbus_assert (_DBUS_ALIGN_ADDRESS (header_data, 4) == header_data);

      if (header_data[2] != DBUS_MAJOR_PROTOCOL_VERSION)
        {
          _dbus_verbose ("Message has protocol version %d ours is %d\n",
                         (int) header_data[2], DBUS_MAJOR_PROTOCOL_VERSION);
          loader->corrupted = TRUE;
          return TRUE;
        }
      
      byte_order = header_data[0];

      if (byte_order != DBUS_LITTLE_ENDIAN &&
	  byte_order != DBUS_BIG_ENDIAN)
	{
          _dbus_verbose ("Message with bad byte order '%c' received\n",
                         byte_order);
	  loader->corrupted = TRUE;
	  return TRUE;
	}

      header_len_unsigned = _dbus_unpack_uint32 (byte_order, header_data + 4);
      body_len_unsigned = _dbus_unpack_uint32 (byte_order, header_data + 8);

      if (header_len_unsigned < 16)
        {
          _dbus_verbose ("Message had broken too-small header length %u\n",
                         header_len_unsigned);
          loader->corrupted = TRUE;
          return TRUE;
        }

      if (header_len_unsigned > (unsigned) MAX_SANE_MESSAGE_SIZE ||
          body_len_unsigned > (unsigned) MAX_SANE_MESSAGE_SIZE)
        {
          _dbus_verbose ("Header or body length too large (%u %u)\n",
                         header_len_unsigned,
                         body_len_unsigned);
          loader->corrupted = TRUE;
          return TRUE;
        }

      /* Now that we know the values are in signed range, get
       * rid of stupid unsigned, just causes bugs
       */
      header_len = header_len_unsigned;
      body_len = body_len_unsigned;

      if (_DBUS_ALIGN_VALUE (header_len, 8) != header_len_unsigned)
        {
	  
          _dbus_verbose ("header length %d is not aligned to 8 bytes\n",
                         header_len);
          loader->corrupted = TRUE;
          return TRUE;
        }
      
      if (header_len + body_len > loader->max_message_size)
	{
          _dbus_verbose ("Message claimed length header = %d body = %d exceeds max message length %ld\n",
                         header_len, body_len, loader->max_message_size);
	  loader->corrupted = TRUE;
	  return TRUE;
	}

      if (_dbus_string_get_length (&loader->data) >= (header_len + body_len))
	{
          HeaderField fields[FIELD_LAST];
          int i;
          int next_arg;          

#if 0
	  _dbus_verbose_bytes_of_string (&loader->data, 0, header_len + body_len);
#endif	  
 	  if (!decode_header_data (&loader->data, header_len, byte_order,
                                   fields, &header_padding))
	    {
              _dbus_verbose ("Header was invalid\n");
	      loader->corrupted = TRUE;
	      return TRUE;
	    }
          
          next_arg = header_len;
          while (next_arg < (header_len + body_len))
            {
              int prev = next_arg;

              if (!_dbus_marshal_validate_arg (&loader->data,
                                               byte_order,
                                               next_arg,
                                               &next_arg))
                {
                  loader->corrupted = TRUE;
                  return TRUE;
                }

              _dbus_assert (next_arg > prev);
            }
          
          if (next_arg > (header_len + body_len))
            {
              _dbus_verbose ("end of last arg at %d but message has len %d+%d=%d\n",
                             next_arg, header_len, body_len,
                             header_len + body_len);
              loader->corrupted = TRUE;
              return TRUE;
            }

  	  message = dbus_message_new_empty_header ();
	  if (message == NULL)
            return FALSE;

          message->byte_order = byte_order;
          message->header_padding = header_padding;
	  
          /* Copy in the offsets we found */
          i = 0;
          while (i < FIELD_LAST)
            {
              message->header_fields[i] = fields[i];
              ++i;
            }
          
	  if (!_dbus_list_append (&loader->messages, message))
            {
              dbus_message_unref (message);
              return FALSE;
            }

          _dbus_assert (_dbus_string_get_length (&message->header) == 0);
          _dbus_assert (_dbus_string_get_length (&message->body) == 0);

          _dbus_assert (_dbus_string_get_length (&loader->data) >=
                        (header_len + body_len));
          
	  if (!_dbus_string_move_len (&loader->data, 0, header_len, &message->header, 0))
            {
              _dbus_list_remove_last (&loader->messages, message);
              dbus_message_unref (message);
              return FALSE;
            }
          
	  if (!_dbus_string_move_len (&loader->data, 0, body_len, &message->body, 0))
            {
              dbus_bool_t result;

              /* put the header back, we'll try again later */
              result = _dbus_string_copy_len (&message->header, 0, header_len,
                                              &loader->data, 0);
              _dbus_assert (result); /* because DBusString never reallocs smaller */

              _dbus_list_remove_last (&loader->messages, message);
              dbus_message_unref (message);
              return FALSE;
            }

          _dbus_assert (_dbus_string_get_length (&message->header) == header_len);
          _dbus_assert (_dbus_string_get_length (&message->body) == body_len);

          /* Fill in caches */
          message->reply_serial = get_int_field (message,
                                                 FIELD_REPLY_SERIAL);
          message->client_serial = get_int_field (message,
                                                  FIELD_CLIENT_SERIAL);
          
	  _dbus_verbose ("Loaded message %p\n", message);
	}
      else
        return TRUE;
    }

  return TRUE;
}

/**
 * Peeks at first loaded message, returns #NULL if no messages have
 * been queued.
 *
 * @param loader the loader.
 * @returns the next message, or #NULL if none.
 */
DBusMessage*
_dbus_message_loader_peek_message (DBusMessageLoader *loader)
{
  if (loader->messages)
    return loader->messages->data;
  else
    return NULL;
}

/**
 * Pops a loaded message (passing ownership of the message
 * to the caller). Returns #NULL if no messages have been
 * queued.
 *
 * @param loader the loader.
 * @returns the next message, or #NULL if none.
 */
DBusMessage*
_dbus_message_loader_pop_message (DBusMessageLoader *loader)
{
  return _dbus_list_pop_first (&loader->messages);
}

/**
 * Pops a loaded message inside a list link (passing ownership of the
 * message and link to the caller). Returns #NULL if no messages have
 * been loaded.
 *
 * @param loader the loader.
 * @returns the next message link, or #NULL if none.
 */
DBusList*
_dbus_message_loader_pop_message_link (DBusMessageLoader *loader)
{
  return _dbus_list_pop_first_link (&loader->messages);
}

/**
 * Checks whether the loader is confused due to bad data.
 * If messages are received that are invalid, the
 * loader gets confused and gives up permanently.
 * This state is called "corrupted."
 *
 * @param loader the loader
 * @returns #TRUE if the loader is hosed.
 */
dbus_bool_t
_dbus_message_loader_get_is_corrupted (DBusMessageLoader *loader)
{
  return loader->corrupted;
}

/**
 * Sets the maximum size message we allow.
 *
 * @param loader the loader
 * @param size the max message size in bytes
 */
void
_dbus_message_loader_set_max_message_size (DBusMessageLoader  *loader,
                                           long                size)
{
  if (size > MAX_SANE_MESSAGE_SIZE)
    {
      _dbus_verbose ("clamping requested max message size %ld to %d\n",
                     size, MAX_SANE_MESSAGE_SIZE);
      size = MAX_SANE_MESSAGE_SIZE;
    }
  loader->max_message_size = size;
}

/**
 * Gets the maximum allowed message size in bytes.
 *
 * @param loader the loader
 * @returns max size in bytes
 */
long
_dbus_message_loader_get_max_message_size (DBusMessageLoader  *loader)
{
  return loader->max_message_size;
}

/** @} */
#ifdef DBUS_BUILD_TESTS
#include "dbus-test.h"
#include <stdio.h>

static void
message_iter_test (DBusMessage *message)
{
  DBusMessageIter *iter;
  char *str;
  
  iter = dbus_message_get_args_iter (message);

  /* String tests */
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_STRING)
    _dbus_assert_not_reached ("Argument type isn't string");

  str = dbus_message_iter_get_string (iter);
  if (strcmp (str, "Test string") != 0)
    _dbus_assert_not_reached ("Strings differ");
  dbus_free (str);

  if (!dbus_message_iter_next (iter))
    _dbus_assert_not_reached ("Reached end of arguments");

  /* Signed integer tests */
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_INT32)
    _dbus_assert_not_reached ("Argument type isn't int32");

  if (dbus_message_iter_get_int32 (iter) != -0x12345678)
    _dbus_assert_not_reached ("Signed integers differ");

  if (!dbus_message_iter_next (iter))
    _dbus_assert_not_reached ("Reached end of fields");
  
  /* Unsigned integer tests */
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_UINT32)
    _dbus_assert_not_reached ("Argument type isn't int32");

  if (dbus_message_iter_get_uint32 (iter) != 0xedd1e)
    _dbus_assert_not_reached ("Unsigned integers differ");

  if (!dbus_message_iter_next (iter))
    _dbus_assert_not_reached ("Reached end of arguments");

  /* Double tests */
  if (dbus_message_iter_get_arg_type (iter) != DBUS_TYPE_DOUBLE)
    _dbus_assert_not_reached ("Argument type isn't double");

  if (dbus_message_iter_get_double (iter) != 3.14159)
    _dbus_assert_not_reached ("Doubles differ");

  if (dbus_message_iter_next (iter))
    _dbus_assert_not_reached ("Didn't reach end of arguments");
  
  dbus_message_iter_unref (iter);
}

static dbus_bool_t
check_message_handling (DBusMessage *message)
{
  DBusMessageIter *iter;
  int type;
  dbus_bool_t retval;
  dbus_int32_t client_serial;
  
  retval = FALSE;
  iter = NULL;
  
  client_serial = dbus_message_get_serial (message);

  /* can't use set_serial due to the assertions at the start of it */
  set_int_field (message, FIELD_CLIENT_SERIAL,
                 client_serial);
  
  if (client_serial != dbus_message_get_serial (message))
    {
      _dbus_warn ("get/set cycle for client_serial did not succeed\n");
      goto failed;
    }
  
  /* If we implement message_set_arg (message, n, value)
   * then we would want to test it here
   */

  iter = dbus_message_get_args_iter (message);
  while ((type = dbus_message_iter_get_arg_type (iter)) != DBUS_TYPE_INVALID)
    {
      switch (type)
        {
	case DBUS_TYPE_NIL:
	  break;
	case DBUS_TYPE_INT32:
	  dbus_message_iter_get_int32 (iter);
	  break;
	case DBUS_TYPE_UINT32:
	  dbus_message_iter_get_uint32 (iter);
	  break;
	case DBUS_TYPE_DOUBLE:
	  dbus_message_iter_get_double (iter);
	  break;
        case DBUS_TYPE_STRING:
          {
            char *str;
            str = dbus_message_iter_get_string (iter);
            dbus_free (str);
          }
          break;
        case DBUS_TYPE_BOOLEAN_ARRAY:
          {
	    unsigned char *values;
	    int len;
	    
            if (!dbus_message_iter_get_boolean_array (iter, &values, &len))
	      return FALSE;

	    dbus_free (values);
          }
          break;
        case DBUS_TYPE_INT32_ARRAY:
          {
	    dbus_int32_t *values;
	    int len;
	    
            if (!dbus_message_iter_get_int32_array (iter, &values, &len))
	      return FALSE;

	    dbus_free (values);
          }
          break;
        case DBUS_TYPE_UINT32_ARRAY:
          {
	    dbus_uint32_t *values;
	    int len;
	    
            if (!dbus_message_iter_get_uint32_array (iter, &values, &len))
	      return FALSE;

	    dbus_free (values);
          }
          break;
        case DBUS_TYPE_DOUBLE_ARRAY:
          {
	    double *values;
	    int len;
	    
            if (!dbus_message_iter_get_double_array (iter, &values, &len))
	      return FALSE;

	    dbus_free (values);
          }
	  break;
	case DBUS_TYPE_STRING_ARRAY:
          {
	    char **values;
	    int len;
	    
            if (!dbus_message_iter_get_string_array (iter, &values, &len))
	      return FALSE;

	    dbus_free_string_array (values);
          }
          break;

	case DBUS_TYPE_DICT:
	  {
	    DBusDict *dict;

	    if (!dbus_message_iter_get_dict (iter, &dict))
	      return FALSE;
	    dbus_dict_unref (dict);
	  }
	  break;

	default:
	  break;
        }
      
      if (!dbus_message_iter_next (iter))
        break;
    }

  retval = TRUE;
  
 failed:
  if (iter)
    dbus_message_iter_unref (iter);

  return retval;
}

static dbus_bool_t
check_have_valid_message (DBusMessageLoader *loader)
{
  DBusMessage *message;
  dbus_bool_t retval;

  message = NULL;
  retval = FALSE;

  if (!_dbus_message_loader_queue_messages (loader))
    _dbus_assert_not_reached ("no memory to queue messages");
  
  if (_dbus_message_loader_get_is_corrupted (loader))
    {
      _dbus_warn ("loader corrupted on message that was expected to be valid\n");
      goto failed;
    }
  
  message = _dbus_message_loader_pop_message (loader);
  if (message == NULL)
    {
      _dbus_warn ("didn't load message that was expected to be valid (message not popped)\n");
      goto failed;
    }
  
  if (_dbus_string_get_length (&loader->data) > 0)
    {
      _dbus_warn ("had leftover bytes from expected-to-be-valid single message\n");
      goto failed;
    }

  /* Verify that we're able to properly deal with the message.
   * For example, this would detect improper handling of messages
   * in nonstandard byte order.
   */
  if (!check_message_handling (message))
    goto failed;  
  
  retval = TRUE;

 failed:
  if (message)
    dbus_message_unref (message);

  return retval;
}

static dbus_bool_t
check_invalid_message (DBusMessageLoader *loader)
{
  dbus_bool_t retval;

  retval = FALSE;

  if (!_dbus_message_loader_queue_messages (loader))
    _dbus_assert_not_reached ("no memory to queue messages");
  
  if (!_dbus_message_loader_get_is_corrupted (loader))
    {
      _dbus_warn ("loader not corrupted on message that was expected to be invalid\n");
      goto failed;
    }

  retval = TRUE;

 failed:
  return retval;
}

static dbus_bool_t
check_incomplete_message (DBusMessageLoader *loader)
{
  DBusMessage *message;
  dbus_bool_t retval;

  message = NULL;
  retval = FALSE;

  if (!_dbus_message_loader_queue_messages (loader))
    _dbus_assert_not_reached ("no memory to queue messages");
  
  if (_dbus_message_loader_get_is_corrupted (loader))
    {
      _dbus_warn ("loader corrupted on message that was expected to be valid (but incomplete)\n");
      goto failed;
    }
  
  message = _dbus_message_loader_pop_message (loader);
  if (message != NULL)
    {
      _dbus_warn ("loaded message that was expected to be incomplete\n");
      goto failed;
    }

  retval = TRUE;

 failed:
  if (message)
    dbus_message_unref (message);
  return retval;
}

static dbus_bool_t
check_loader_results (DBusMessageLoader      *loader,
                      DBusMessageValidity     validity)
{
  if (!_dbus_message_loader_queue_messages (loader))
    _dbus_assert_not_reached ("no memory to queue messages");
  
  switch (validity)
    {
    case _DBUS_MESSAGE_VALID:
      return check_have_valid_message (loader);
    case _DBUS_MESSAGE_INVALID:
      return check_invalid_message (loader);
    case _DBUS_MESSAGE_INCOMPLETE:
      return check_incomplete_message (loader);
    case _DBUS_MESSAGE_UNKNOWN:
      return TRUE;
    }

  _dbus_assert_not_reached ("bad DBusMessageValidity");
  return FALSE;
}


/**
 * Loads the message in the given message file.
 *
 * @param filename filename to load
 * @param is_raw if #TRUE load as binary data, if #FALSE as message builder language
 * @param data string to load message into
 * @returns #TRUE if the message was loaded
 */
dbus_bool_t
dbus_internal_do_not_use_load_message_file (const DBusString    *filename,
                                            dbus_bool_t          is_raw,
                                            DBusString          *data)
{
  dbus_bool_t retval;

  retval = FALSE;  

  if (is_raw)
    {
      DBusError error;

      dbus_error_init (&error);
      if (!_dbus_file_get_contents (data, filename, &error))
        {
          const char *s;      
          _dbus_string_get_const_data (filename, &s);
          _dbus_warn ("Could not load message file %s: %s\n", s, error.message);
          dbus_error_free (&error);
          goto failed;
        }
    }
  else
    {
      if (!_dbus_message_data_load (data, filename))
        {
          const char *s;      
          _dbus_string_get_const_data (filename, &s);
          _dbus_warn ("Could not load message file %s\n", s);
          goto failed;
        }
    }

  retval = TRUE;
  
 failed:

  return retval;
}

/**
 * Tries loading the message in the given message file
 * and verifies that DBusMessageLoader can handle it.
 *
 * @param filename filename to load
 * @param is_raw if #TRUE load as binary data, if #FALSE as message builder language
 * @param expected_validity what the message has to be like to return #TRUE
 * @returns #TRUE if the message has the expected validity
 */
dbus_bool_t
dbus_internal_do_not_use_try_message_file (const DBusString    *filename,
                                           dbus_bool_t          is_raw,
                                           DBusMessageValidity  expected_validity)
{
  DBusString data;
  dbus_bool_t retval;

  retval = FALSE;
  
  if (!_dbus_string_init (&data, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("could not allocate string\n");

  if (!dbus_internal_do_not_use_load_message_file (filename, is_raw,
                                                   &data))
    goto failed;

  retval = dbus_internal_do_not_use_try_message_data (&data, expected_validity);

 failed:

  if (!retval)
    {
      const char *s;

      if (_dbus_string_get_length (&data) > 0)
        _dbus_verbose_bytes_of_string (&data, 0,
                                       _dbus_string_get_length (&data));
      
      _dbus_string_get_const_data (filename, &s);
      _dbus_warn ("Failed message loader test on %s\n",
                  s);
    }
  
  _dbus_string_free (&data);

  return retval;
}

/**
 * Tries loading the given message data.
 *
 *
 * @param data the message data
 * @param expected_validity what the message has to be like to return #TRUE
 * @returns #TRUE if the message has the expected validity
 */
dbus_bool_t
dbus_internal_do_not_use_try_message_data (const DBusString    *data,
                                           DBusMessageValidity  expected_validity)
{
  DBusMessageLoader *loader;
  dbus_bool_t retval;
  int len;
  int i;

  loader = NULL;
  retval = FALSE;

  /* Write the data one byte at a time */
  
  loader = _dbus_message_loader_new ();

  len = _dbus_string_get_length (data);
  for (i = 0; i < len; i++)
    {
      DBusString *buffer;

      _dbus_message_loader_get_buffer (loader, &buffer);
      _dbus_string_append_byte (buffer,
                                _dbus_string_get_byte (data, i));
      _dbus_message_loader_return_buffer (loader, buffer, 1);
    }
  
  if (!check_loader_results (loader, expected_validity))
    goto failed;

  _dbus_message_loader_unref (loader);
  loader = NULL;

  /* Write the data all at once */
  
  loader = _dbus_message_loader_new ();

  {
    DBusString *buffer;
    
    _dbus_message_loader_get_buffer (loader, &buffer);
    _dbus_string_copy (data, 0, buffer,
                       _dbus_string_get_length (buffer));
    _dbus_message_loader_return_buffer (loader, buffer, 1);
  }
  
  if (!check_loader_results (loader, expected_validity))
    goto failed;

  _dbus_message_loader_unref (loader);
  loader = NULL;  

  /* Write the data 2 bytes at a time */
  
  loader = _dbus_message_loader_new ();

  len = _dbus_string_get_length (data);
  for (i = 0; i < len; i += 2)
    {
      DBusString *buffer;

      _dbus_message_loader_get_buffer (loader, &buffer);
      _dbus_string_append_byte (buffer,
                                _dbus_string_get_byte (data, i));
      if ((i+1) < len)
        _dbus_string_append_byte (buffer,
                                  _dbus_string_get_byte (data, i+1));
      _dbus_message_loader_return_buffer (loader, buffer, 1);
    }
  
  if (!check_loader_results (loader, expected_validity))
    goto failed;

  _dbus_message_loader_unref (loader);
  loader = NULL;
  
  retval = TRUE;
  
 failed:
  
  if (loader)
    _dbus_message_loader_unref (loader);
  
  return retval;
}

static dbus_bool_t
process_test_subdir (const DBusString          *test_base_dir,
                     const char                *subdir,
                     DBusMessageValidity        validity,
                     DBusForeachMessageFileFunc function,
                     void                      *user_data)
{
  DBusString test_directory;
  DBusString filename;
  DBusDirIter *dir;
  dbus_bool_t retval;
  DBusError error;

  retval = FALSE;
  dir = NULL;
  
  if (!_dbus_string_init (&test_directory, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("didn't allocate test_directory\n");

  _dbus_string_init_const (&filename, subdir);
  
  if (!_dbus_string_copy (test_base_dir, 0,
                          &test_directory, 0))
    _dbus_assert_not_reached ("couldn't copy test_base_dir to test_directory");
  
  if (!_dbus_concat_dir_and_file (&test_directory, &filename))    
    _dbus_assert_not_reached ("couldn't allocate full path");

  _dbus_string_free (&filename);
  if (!_dbus_string_init (&filename, _DBUS_INT_MAX))
    _dbus_assert_not_reached ("didn't allocate filename string\n");

  dbus_error_init (&error);
  dir = _dbus_directory_open (&test_directory, &error);
  if (dir == NULL)
    {
      const char *s;
      _dbus_string_get_const_data (&test_directory, &s);
      _dbus_warn ("Could not open %s: %s\n", s,
                  error.message);
      dbus_error_free (&error);
      goto failed;
    }

  printf ("Testing:\n");
  
 next:
  while (_dbus_directory_get_next_file (dir, &filename, &error))
    {
      DBusString full_path;
      dbus_bool_t is_raw;
      
      if (!_dbus_string_init (&full_path, _DBUS_INT_MAX))
        _dbus_assert_not_reached ("couldn't init string");

      if (!_dbus_string_copy (&test_directory, 0, &full_path, 0))
        _dbus_assert_not_reached ("couldn't copy dir to full_path");

      if (!_dbus_concat_dir_and_file (&full_path, &filename))
        _dbus_assert_not_reached ("couldn't concat file to dir");

      if (_dbus_string_ends_with_c_str (&filename, ".message"))
        is_raw = FALSE;
      else if (_dbus_string_ends_with_c_str (&filename, ".message-raw"))
        is_raw = TRUE;
      else
        {
          const char *filename_c;
          _dbus_string_get_const_data (&filename, &filename_c);
          _dbus_verbose ("Skipping non-.message file %s\n",
                         filename_c);
	  _dbus_string_free (&full_path);
          goto next;
        }

      {
        const char *s;
        _dbus_string_get_const_data (&filename, &s);
        printf ("    %s\n", s);
      }
      
      _dbus_verbose (" expecting %s\n",
                     validity == _DBUS_MESSAGE_VALID ? "valid" :
                     (validity == _DBUS_MESSAGE_INVALID ? "invalid" :
                      (validity == _DBUS_MESSAGE_INCOMPLETE ? "incomplete" : "unknown")));
      
      if (! (*function) (&full_path, is_raw, validity, user_data))
        {
          _dbus_string_free (&full_path);
          goto failed;
        }
      else
        _dbus_string_free (&full_path);
    }

  if (dbus_error_is_set (&error))
    {
      const char *s;
      _dbus_string_get_const_data (&test_directory, &s);
      _dbus_warn ("Could not get next file in %s: %s\n",
                  s, error.message);
      dbus_error_free (&error);
      goto failed;
    }
    
  retval = TRUE;
  
 failed:

  if (dir)
    _dbus_directory_close (dir);
  _dbus_string_free (&test_directory);
  _dbus_string_free (&filename);

  return retval;
}
                     
/**
 * Runs the given function on every message file in the test suite.
 * The function should return #FALSE on test failure or fatal error.
 *
 * @param test_data_dir root dir of the test suite data files (top_srcdir/test/data)
 * @param func the function to run
 * @param user_data data for function
 * @returns #FALSE if there's a failure
 */
dbus_bool_t
dbus_internal_do_not_use_foreach_message_file (const char                *test_data_dir,
                                               DBusForeachMessageFileFunc func,
                                               void                      *user_data)
{
  DBusString test_directory;
  dbus_bool_t retval;

  retval = FALSE;
  
  _dbus_string_init_const (&test_directory, test_data_dir);

  if (!process_test_subdir (&test_directory, "valid-messages",
                            _DBUS_MESSAGE_VALID, func, user_data))
    goto failed;

  if (!process_test_subdir (&test_directory, "invalid-messages",
                            _DBUS_MESSAGE_INVALID, func, user_data))
    goto failed;
  
  if (!process_test_subdir (&test_directory, "incomplete-messages",
                            _DBUS_MESSAGE_INCOMPLETE, func, user_data))
    goto failed;

  retval = TRUE;
  
 failed:

  _dbus_string_free (&test_directory);
  
  return retval;
}

static void
verify_test_message (DBusMessage *message)
{
  dbus_int32_t our_int;
  char *our_str;
  double our_double;
  dbus_bool_t our_bool;
  
  if (!dbus_message_get_args (message, NULL,
                              DBUS_TYPE_INT32, &our_int,
                              DBUS_TYPE_STRING, &our_str,
                              DBUS_TYPE_DOUBLE, &our_double,
                              DBUS_TYPE_BOOLEAN, &our_bool,
                              0))
    _dbus_assert_not_reached ("Could not get arguments");

  if (our_int != -0x12345678)
    _dbus_assert_not_reached ("integers differ!");

  if (our_double != 3.14159)
    _dbus_assert_not_reached ("doubles differ!");

  if (strcmp (our_str, "Test string") != 0)
    _dbus_assert_not_reached ("strings differ!");

  if (!our_bool)
    _dbus_assert_not_reached ("booleans differ");
  
  dbus_free (our_str);
}

/**
 * @ingroup DBusMessageInternals
 * Unit test for DBusMessage.
 *
 * @returns #TRUE on success.
 */
dbus_bool_t
_dbus_message_test (const char *test_data_dir)
{
  DBusMessage *message;
  DBusMessageLoader *loader;
  int i;
  const char *data;
  DBusMessage *copy;
  const char *name1;
  const char *name2;
  
  /* Test the vararg functions */
  message = dbus_message_new ("org.freedesktop.DBus.Test", "testMessage");
  _dbus_message_set_serial (message, 1);
  dbus_message_append_args (message,
			    DBUS_TYPE_INT32, -0x12345678,
			    DBUS_TYPE_STRING, "Test string",
			    DBUS_TYPE_DOUBLE, 3.14159,
			    DBUS_TYPE_BOOLEAN, TRUE,
			    0);
  _dbus_verbose_bytes_of_string (&message->header, 0,
                                 _dbus_string_get_length (&message->header));
  _dbus_verbose_bytes_of_string (&message->body, 0,
                                 _dbus_string_get_length (&message->body));

  verify_test_message (message);

  copy = dbus_message_copy (message);
  
  _dbus_assert (message->client_serial == copy->client_serial);
  _dbus_assert (message->reply_serial == copy->reply_serial);
  _dbus_assert (message->header_padding == copy->header_padding);
  
  _dbus_assert (_dbus_string_get_length (&message->header) ==
                _dbus_string_get_length (&copy->header));

  _dbus_assert (_dbus_string_get_length (&message->body) ==
                _dbus_string_get_length (&copy->body));

  verify_test_message (copy);

  name1 = dbus_message_get_name (message);
  name2 = dbus_message_get_name (copy);

  _dbus_assert (strcmp (name1, name2) == 0);
  
  dbus_message_unref (message);
  dbus_message_unref (copy);
  
  message = dbus_message_new ("org.freedesktop.DBus.Test", "testMessage");
  _dbus_message_set_serial (message, 1);
  dbus_message_set_reply_serial (message, 0x12345678);

  dbus_message_append_string (message, "Test string");
  dbus_message_append_int32 (message, -0x12345678);
  dbus_message_append_uint32 (message, 0xedd1e);
  dbus_message_append_double (message, 3.14159);

  message_iter_test (message);

  /* Message loader test */
  _dbus_message_lock (message);
  loader = _dbus_message_loader_new ();

  /* Write the header data one byte at a time */
  _dbus_string_get_const_data (&message->header, &data);
  for (i = 0; i < _dbus_string_get_length (&message->header); i++)
    {
      DBusString *buffer;

      _dbus_message_loader_get_buffer (loader, &buffer);
      _dbus_string_append_byte (buffer, data[i]);
      _dbus_message_loader_return_buffer (loader, buffer, 1);
    }

  /* Write the body data one byte at a time */
  _dbus_string_get_const_data (&message->body, &data);
  for (i = 0; i < _dbus_string_get_length (&message->body); i++)
    {
      DBusString *buffer;

      _dbus_message_loader_get_buffer (loader, &buffer);
      _dbus_string_append_byte (buffer, data[i]);
      _dbus_message_loader_return_buffer (loader, buffer, 1);
    }

  dbus_message_unref (message);

  /* Now pop back the message */
  if (!_dbus_message_loader_queue_messages (loader))
    _dbus_assert_not_reached ("no memory to queue messages");
  
  if (_dbus_message_loader_get_is_corrupted (loader))
    _dbus_assert_not_reached ("message loader corrupted");
  
  message = _dbus_message_loader_pop_message (loader);
  if (!message)
    _dbus_assert_not_reached ("received a NULL message");

  if (dbus_message_get_reply_serial (message) != 0x12345678)
    _dbus_assert_not_reached ("reply serial fields differ");
  
  message_iter_test (message);
  
  dbus_message_unref (message);
  _dbus_message_loader_unref (loader);

  /* Now load every message in test_data_dir if we have one */
  if (test_data_dir == NULL)
    return TRUE;

  return dbus_internal_do_not_use_foreach_message_file (test_data_dir,
                                                        (DBusForeachMessageFileFunc)
                                                        dbus_internal_do_not_use_try_message_file,
                                                        NULL);
}

#endif /* DBUS_BUILD_TESTS */
