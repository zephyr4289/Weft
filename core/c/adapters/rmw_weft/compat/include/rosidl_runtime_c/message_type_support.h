// rosidl_runtime_c/message_type_support.h — vendored ABI declaration
// (pinned to ROS 2 Humble rosidl_runtime_c layout).
//
// WHY EXISTS: rmw_create_publisher/rmw_create_subscription receive message
// types through this exact struct; Rule 1 requires member order and the
// function-pointer arity to match the official specification. rmw_weft
// accepts any typesupport whose `typesupport_identifier` it recognizes
// (the POD/.weft descriptor fast path, constructed via rmw_weft.h) and
// fails closed with RMW_RET_UNSUPPORTED for identifiers it does not —
// no guessing, no silent reinterpretation.

#ifndef ROSIDL_RUNTIME_C__MESSAGE_TYPE_SUPPORT_H_
#define ROSIDL_RUNTIME_C__MESSAGE_TYPE_SUPPORT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rosidl_message_type_support_t rosidl_message_type_support_t;

typedef const rosidl_message_type_support_t * (*type_support_get_function)(
    const void * data, const char * identifier);

struct rosidl_message_type_support_t {
    const char * typesupport_identifier;
    const void * data;
    type_support_get_function function;
};

#ifdef __cplusplus
}
#endif

#endif  // ROSIDL_RUNTIME_C__MESSAGE_TYPE_SUPPORT_H_
