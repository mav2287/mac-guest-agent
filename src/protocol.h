#ifndef MGA_PROTOCOL_H
#define MGA_PROTOCOL_H

#include "third_party/cJSON.h"

/* Parse a QMP request JSON string. Returns cJSON object or NULL. Caller frees with cJSON_Delete. */
cJSON *protocol_parse_request(const char *data);

/* Extract the "execute" command name from a parsed request. Returns internal pointer (do not free). */
const char *protocol_get_command(const cJSON *request);

/* Extract the "arguments" object from a parsed request. Returns internal pointer or NULL. */
cJSON *protocol_get_arguments(const cJSON *request);

/* Extract the "id" value from a parsed request. Returns internal pointer or NULL. */
cJSON *protocol_get_id(const cJSON *request);

/* Build a success response JSON string. Caller frees. */
char *protocol_build_response(cJSON *return_value, const cJSON *id);

/* Build an error response JSON string. Caller frees. */
char *protocol_build_error(const char *error_class, const char *desc, const cJSON *id);

/* Build an empty success response (return: {}). Caller frees. */
char *protocol_build_empty_response(const cJSON *id);

#endif
