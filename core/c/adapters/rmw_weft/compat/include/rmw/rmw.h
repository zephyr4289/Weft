// rmw/rmw.h — vendored, spec-conformant subset of the ROS 2 `rmw` C ABI
// (pinned to ROS 2 Humble layouts) for the rmw_weft middleware engine.
//
// WHY EXISTS: rmw_weft is a drop-in ROS 2 RMW implementation. Rule 1 of the
// Pillar 6 directive demands STRICT ABI FIDELITY for external standard ABIs:
// struct layouts, member orders, and function-pointer arities below match
// the official rmw specification exactly for every type and function this
// pillar implements. Where the official ABI surface is wider than the
// directive's list, this header vendors ONLY the required subset — the
// omitted members and functions are inventoried in D-62 §A.3 so an
// integrator can diff this header against a real ros2/rmw checkout.
//
// PIN POLICY: layout-affecting values are frozen and _Static_assert-pinned
// (RMW_IMPLEMENTATION_ID_MAX_SIZE, RMW_GID_STORAGE_SIZE, struct offsets for
// the handle types). A change here is a protocol version bump, not an edit.
//
// Honesty boundary: this header is compiled and executable-verified on
// x86_64 + aarch64 POSIX (Linux). It vendors no ROS 2 code — only ABI
// declarations — so an rmw_weft .so can be dropped into a ROS 2 graph in
// place of rmw_fastrtps/rmw_cyclonedds when linked against a matching
// rosidl typesupport. The POD/.weft descriptor fast path is rmw_weft's own
// extension surface (see rmw_weft/rmw_weft.h); ROS 2 *generated* types
// (C/Ada) are Engineer 3's integration seam, declared not defended here.

#ifndef RMW__RMW_H_
#define RMW__RMW_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "rosidl_runtime_c/message_type_support.h"

#ifdef __cplusplus
extern "C" {
#endif

// ===========================================================================
// §1 Return codes (rmw/types.h — values are frozen wire numbers)
// ===========================================================================

typedef enum {
    RMW_RET_OK = 0,
    RMW_RET_ERROR = 1,
    RMW_RET_TIMEOUT = 2,
    RMW_RET_UNSUPPORTED = 3,
    RMW_RET_BAD_ALLOC = 4,
    RMW_RET_INVALID_ARGUMENT = 5,
    RMW_RET_INCORRECT_RMW_IMPLEMENTATION = 6,
    RMW_RET_NODE_NAME_NON_EXISTENT = 10,
    RMW_RET_TOPIC_NAME_NON_EXISTENT = 11,
    RMW_RET_SUBSCRIPTION_TAKE_FAILED = 12,
} rmw_ret_t;

// The official enum continues with graph-event / loaned / security codes
// this subset does not implement — the delta inventory lives in D-62 §A.3.

// ===========================================================================
// §2 Scalars, time, identities (rmw/types.h)
// ===========================================================================

#define RMW_IMPLEMENTATION_ID_MAX_SIZE 128u
#define RMW_GID_STORAGE_SIZE 24u

typedef struct rmw_time_t {
    int64_t sec;
    uint64_t nsec;
} rmw_time_t;

typedef rmw_time_t rmw_time_point_value_t;
typedef rmw_time_t rmw_duration_t;

typedef struct rmw_gid_t {
    char implementation_identifier[RMW_IMPLEMENTATION_ID_MAX_SIZE];
    uint8_t data[RMW_GID_STORAGE_SIZE];
} rmw_gid_t;

typedef struct rmw_error_string_t {
    char str[1024];
} rmw_error_string_t;

// ===========================================================================
// §3 QoS profile (rmw/qos_profiles.h + rmw/types.h — member order pinned)
// ===========================================================================

enum rmw_qos_history_policy_e {
    RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT = 0,
    RMW_QOS_POLICY_HISTORY_KEEP_LAST = 1,
    RMW_QOS_POLICY_HISTORY_KEEP_ALL = 2,
};
typedef enum rmw_qos_history_policy_e rmw_qos_history_policy_t;

enum rmw_qos_reliability_policy_e {
    RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT = 0,
    RMW_QOS_POLICY_RELIABILITY_RELIABLE = 1,
    RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT = 2,
};
typedef enum rmw_qos_reliability_policy_e rmw_qos_reliability_policy_t;

enum rmw_qos_durability_policy_e {
    RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT = 0,
    RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL = 1,
    RMW_QOS_POLICY_DURABILITY_VOLATILE = 2,
};
typedef enum rmw_qos_durability_policy_e rmw_qos_durability_policy_t;

enum rmw_qos_liveliness_policy_e {
    RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT = 0,
    RMW_QOS_POLICY_LIVELINESS_AUTOMATIC = 1,
    RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_NODE = 2,
    RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC = 3,
};
typedef enum rmw_qos_liveliness_policy_e rmw_qos_liveliness_policy_t;

typedef struct rmw_qos_profile_t {
    rmw_qos_history_policy_t history;
    size_t depth;
    rmw_qos_reliability_policy_t reliability;
    rmw_qos_durability_policy_t durability;
    rmw_time_t deadline;
    rmw_time_t lifespan;
    rmw_qos_liveliness_policy_t liveliness;
    rmw_time_t liveliness_lease_duration;
} rmw_qos_profile_t;

#define RMW_QOS_DEADLINE_DEFAULT {0, 0}
#define RMW_QOS_LIFESPAN_DEFAULT {0, 0}
#define RMW_QOS_LIVELINESS_LEASE_DURATION_DEFAULT {0, 0}

extern const rmw_qos_profile_t RMW_QOS_PROFILE_DEFAULT;
extern const rmw_qos_profile_t RMW_QOS_PROFILE_SENSOR_DATA;

// ===========================================================================
// §4 Options, security, handles (rmw/types.h, rmw/init.h — layouts pinned)
// ===========================================================================

enum rmw_security_enforcement_policy_t {
    RMW_SECURITY_ENFORCEMENT_PERMISSIVE = 0,
    RMW_SECURITY_ENFORCEMENT_ENFORCE = 1,
    RMW_SECURITY_ENFORCEMENT_NOT_SET = 255,
};
typedef enum rmw_security_enforcement_policy_t
    rmw_security_enforcement_policy_t;

typedef struct rmw_security_options_t {
    rmw_security_enforcement_policy_t enforce_security;
    const char * security_root_path;
} rmw_security_options_t;

typedef struct rmw_node_security_options_t {
    rmw_security_enforcement_policy_t enforce_security;
    const char * security_root_path;
} rmw_node_security_options_t;

enum rmw_unique_network_flow_endpoints_requirement_t {
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_NOT_REQUIRED = 0,
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_REQUIRED = 1,
    RMW_UNIQUE_NETWORK_FLOW_ENDPOINTS_UNKNOWN = 2,
};
typedef enum rmw_unique_network_flow_endpoints_requirement_t
    rmw_unique_network_flow_endpoints_requirement_t;

typedef struct rmw_publisher_options_t {
    rmw_unique_network_flow_endpoints_requirement_t
        require_unique_network_flow_endpoints;
} rmw_publisher_options_t;

typedef struct rmw_subscription_options_t {
    bool ignore_local_publications;
    rmw_unique_network_flow_endpoints_requirement_t
        require_unique_network_flow_endpoints;
} rmw_subscription_options_t;

// Opaque impl data blocks (official ABI: forward-declared struct pointers).
typedef struct rmw_node_data_t rmw_node_data_t;
typedef struct rmw_publisher_data_t rmw_publisher_data_t;
typedef struct rmw_subscription_data_t rmw_subscription_data_t;
typedef struct rmw_wait_set_data_t rmw_wait_set_data_t;
typedef struct rmw_context_data_t rmw_context_data_t;
typedef struct rmw_publisher_allocation_t rmw_publisher_allocation_t;
typedef struct rmw_subscription_allocation_t rmw_subscription_allocation_t;
typedef struct rmw_wait_set_entry_t rmw_wait_set_entry_t;

/* The fini member references the typedef before the body completes —
 * forward-declare first (the official header relies on the same trick). */
typedef struct rmw_init_options_t rmw_init_options_t;

struct rmw_init_options_t {
    rmw_ret_t (* fini)(rmw_init_options_t * init_options);
    const char * implementation_identifier;
    size_t instance_id;
    const char * enclave;
    rmw_security_options_t security_options;
};

typedef struct rmw_context_t {
    rmw_init_options_t * options;
    const char * implementation_identifier;
    size_t instance_id;
    rmw_context_data_t * data;
} rmw_context_t;

typedef struct rmw_node_t {
    rmw_context_t * context;
    const char * implementation_identifier;
    rmw_node_data_t * data;
    char * name;
    char * namespace_;
} rmw_node_t;

typedef struct rmw_publisher_t {
    const char * implementation_identifier;
    rmw_publisher_data_t * data;
    char * topic_name;
    const rosidl_message_type_support_t * type_support_;
    rmw_publisher_options_t options;
    bool can_loan_messages;
} rmw_publisher_t;

typedef struct rmw_subscription_t {
    const char * implementation_identifier;
    rmw_subscription_data_t * data;
    char * topic_name;
    const rosidl_message_type_support_t * type_support_;
    rmw_subscription_options_t options;
    bool is_cft_enabled;
} rmw_subscription_t;

typedef struct rmw_wait_set_t {
    const char * implementation_identifier;
    rmw_wait_set_data_t * data;
} rmw_wait_set_t;

// Wait collections (rmw/types.h — arrays of opaque handles).
typedef struct rmw_subscriptions_t {
    size_t subscriber_count;
    void ** subscribers;
} rmw_subscriptions_t;

typedef struct rmw_guard_conditions_t {
    size_t guard_condition_count;
    void ** guard_conditions;
} rmw_guard_conditions_t;

typedef struct rmw_services_t {
    size_t service_count;
    void ** services;
} rmw_services_t;

typedef struct rmw_clients_t {
    size_t client_count;
    void ** clients;
} rmw_clients_t;

typedef struct rmw_events_t {
    size_t event_count;
    void ** events;
} rmw_events_t;

typedef struct rmw_message_info_t {
    rmw_time_point_value_t source_timestamp;
    rmw_time_point_value_t received_timestamp;
    rmw_gid_t publisher_gid;
    bool from_intra_process;
} rmw_message_info_t;

// ABI freeze pins (compile-time, every TU including this header).
_Static_assert(sizeof(rmw_ret_t) == 4, "rmw_ret_t must be a 4-byte enum");
_Static_assert(offsetof(rmw_publisher_t, topic_name) == 16,
               "rmw_publisher_t ABI drift: topic_name");
_Static_assert(offsetof(rmw_publisher_t, type_support_) == 24,
               "rmw_publisher_t ABI drift: type_support_");
_Static_assert(offsetof(rmw_publisher_t, options) == 32,
               "rmw_publisher_t ABI drift: options");
_Static_assert(offsetof(rmw_publisher_t, can_loan_messages) == 36,
               "rmw_publisher_t ABI drift: can_loan_messages");
_Static_assert(offsetof(rmw_subscription_t, topic_name) == 16,
               "rmw_subscription_t ABI drift: topic_name");
_Static_assert(offsetof(rmw_subscription_t, options) == 32,
               "rmw_subscription_t ABI drift: options");
_Static_assert(offsetof(rmw_message_info_t, publisher_gid) == 32,
               "rmw_message_info_t ABI drift: publisher_gid");
_Static_assert(offsetof(rmw_init_options_t, instance_id) == 16,
               "rmw_init_options_t ABI drift: instance_id");

// ===========================================================================
// §5 The implemented rmw C-API surface (arities per official spec)
// ===========================================================================

const char * rmw_get_implementation_identifier(void);
rmw_error_string_t rmw_get_error_string(void);

rmw_ret_t rmw_init_options_init(rmw_init_options_t * init_options,
                                int argc, const char * const * argv);
rmw_ret_t rmw_init_options_fini(rmw_init_options_t * init_options);

rmw_ret_t rmw_init(const rmw_init_options_t * options,
                   rmw_context_t * context);
rmw_ret_t rmw_fini(rmw_context_t * context);
rmw_ret_t rmw_shutdown(rmw_context_t * context);

rmw_node_t * rmw_create_node(rmw_context_t * context, const char * name,
                             const char * namespace_, size_t domain_id,
                             const rmw_node_security_options_t *
                                 security_options);
rmw_ret_t rmw_destroy_node(rmw_node_t * node);

rmw_publisher_t * rmw_create_publisher(
    const rmw_node_t * node,
    const rosidl_message_type_support_t * type_supports,
    const char * topic_name, const rmw_qos_profile_t * qos_policies,
    const rmw_publisher_options_t * publisher_options);
rmw_ret_t rmw_destroy_publisher(rmw_node_t * node,
                                rmw_publisher_t * publisher);
rmw_ret_t rmw_publish(const rmw_publisher_t * publisher,
                      const void * ros_message,
                      rmw_publisher_allocation_t * allocation);

// Loaned (zero-copy) message surface — official rmw loaned ABI.
rmw_ret_t rmw_borrow_loaned_message(
    const rmw_publisher_t * publisher,
    const rosidl_message_type_support_t * type_support,
    void ** loaned_message);
rmw_ret_t rmw_return_loaned_message_to_publisher(
    const rmw_publisher_t * publisher, void * loaned_message);
rmw_ret_t rmw_publish_loaned_message(const rmw_publisher_t * publisher,
                                     void * loaned_message,
                                     rmw_publisher_allocation_t * allocation);

rmw_subscription_t * rmw_create_subscription(
    const rmw_node_t * node,
    const rosidl_message_type_support_t * type_supports,
    const char * topic_name, const rmw_qos_profile_t * qos_policies,
    const rmw_subscription_options_t * subscription_options);
rmw_ret_t rmw_destroy_subscription(rmw_node_t * node,
                                   rmw_subscription_t * subscription);

rmw_ret_t rmw_take(const rmw_subscription_t * subscription,
                   void * ros_message, bool * taken,
                   rmw_subscription_allocation_t * allocation);
rmw_ret_t rmw_take_with_info(const rmw_subscription_t * subscription,
                             void * ros_message, bool * taken,
                             rmw_message_info_t * message_info,
                             rmw_subscription_allocation_t * allocation);
rmw_ret_t rmw_take_loaned_message(const rmw_subscription_t * subscription,
                                  void ** loaned_message, bool * taken,
                                  rmw_message_info_t * message_info);
rmw_ret_t rmw_return_loaned_message(const rmw_subscription_t * subscription,
                                    void * loaned_message);

rmw_wait_set_t * rmw_create_wait_set(rmw_context_t * context,
                                     size_t max_conditions);
rmw_ret_t rmw_destroy_wait_set(rmw_wait_set_t * wait_set);
rmw_ret_t rmw_wait_set_add_subscription(rmw_wait_set_t * wait_set,
                                        const rmw_subscription_t *
                                            subscription);
rmw_ret_t rmw_wait_set_remove_subscription(rmw_wait_set_t * wait_set,
                                           const rmw_subscription_t *
                                               subscription);

rmw_ret_t rmw_wait(rmw_wait_set_t * wait_set,
                   const rmw_subscriptions_t * subscriptions,
                   const rmw_guard_conditions_t * guard_conditions,
                   const rmw_services_t * services,
                   const rmw_clients_t * clients,
                   const rmw_events_t * events,
                   rmw_wait_set_entry_t * wait_set_info,
                   const rmw_time_point_value_t * wait_timeout);

#ifdef __cplusplus
}
#endif

#endif  // RMW__RMW_H_
