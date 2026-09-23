// weft_rdma_abi.h — RFC-0019 §3.1: the libibverbs ABI mirror + the dlopen
// vtable (internal to the rdma backend; consumed by the driver and the
// CL-R mock battery).
//
// WHY A MIRROR: the house dlopen discipline (gpu_ring / weft_vk_bridge /
// the ORT table of RFC-0017 §5) forbids link-time dependencies on
// optional runtimes — but libibverbs passes STRUCTS BY POINTER, not
// opaque handles, so dlopen alone is not enough: the caller must lay out
// struct ibv_qp_attr byte-exactly or corrupt provider memory. The
// discipline therefore has TWO halves, both load-bearing:
//   1. this mirror: the minimal struct/enum surface the driver touches,
//      copied from rdma-core's infiniband/verbs.h layout (BSD-2/GPL-2
//      dual-licensed upstream; constants are stable provider ABI since
//      2005), and
//   2. WEFT_RDMA_VERIFY_ABI: on deployment machines WITH verbs.h
//      installed, this header #includes the real one instead and static
//      asserts burn ANY drift at compile time (the generated-mirror
//      stance of the ORT table: never hand-remembered, always asserted).
// The x86_64 CI/sandbox has no rdma-core — there the mirror compiles
// standalone and the mock battery drives the exact same vtable, so the
// LOGIC is fully gated while the HARDWARE leg stays DECLARED (Law 4).
//
// THE VTABLE: every symbol is resolved with dlsym at driver init; a
// missing symbol is a NAMED refusal (WEFT_CLUSTER_E_DRIVER), never a
// lazy null-deref. Tests inject their own table through
// weft_rdma_init_with_api() — the same seam the ORT mock used.

#ifndef WEFT_RDMA_ABI_H
#define WEFT_RDMA_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef WEFT_RDMA_VERIFY_ABI
// Deployment verification leg: the REAL header is the definition; every
// frozen offset the driver relies on is asserted below. A rdma-core
// release that changes layout fails THIS compile, not a customer cluster.
#include <infiniband/verbs.h>
#define WEFT_HAVE_REAL_VERBS 1
// verbs.h declares plain structs/unions/enums; alias them to the _t names
// the driver text uses (mirror mode defines these directly).
typedef struct ibv_qp_attr        ibv_qp_attr_t;
typedef struct ibv_qp_init_attr   ibv_qp_init_attr_t;
typedef struct ibv_port_attr      ibv_port_attr_t;
typedef struct ibv_ah_attr        ibv_ah_attr_t;
typedef struct ibv_global_route   ibv_global_route_t;
typedef struct ibv_qp_cap         ibv_qp_cap_t;
typedef struct ibv_sge            ibv_sge_t;
typedef struct ibv_send_wr        ibv_send_wr_t;
typedef struct ibv_wc             ibv_wc_t;
typedef struct ibv_mr             ibv_mr_t;
typedef struct ibv_qp             ibv_qp_t;
typedef union ibv_gid             ibv_gid_t;
typedef enum ibv_mtu              ibv_mtu_t;
typedef enum ibv_link_layer       ibv_link_layer_t;
#else

// ---- mirrored opaque handles (pointer-only — layout-free) -------------------

struct ibv_device;
struct ibv_context;
struct ibv_pd;
struct ibv_cq;
struct ibv_qp;
struct ibv_mr;
struct ibv_comp_channel;
struct ibv_srq;
struct ibv_ah;

// ---- mirrored enums (values frozen by provider ABI) --------------------------

typedef enum {
    IBV_MTU_256  = 1,
    IBV_MTU_512  = 2,
    IBV_MTU_1024 = 3,
    IBV_MTU_2048 = 4,
    IBV_MTU_4096 = 5,
} ibv_mtu_t;

typedef enum {
    IBV_LINK_LAYER_UNSPECIFIED = 0,
    IBV_LINK_LAYER_INFINIBAND  = 1,
    IBV_LINK_LAYER_ETHERNET    = 2,   // RoCE v1/v2
} ibv_link_layer_t;

typedef enum {
    IBV_QPS_RESET = 0,
    IBV_QPS_INIT  = 1,
    IBV_QPS_RTR   = 2,
    IBV_QPS_RTS   = 3,
    IBV_QPS_SQD   = 4,
    IBV_QPS_SQE   = 5,
    IBV_QPS_ERR   = 6,
} ibv_qp_state_t;

typedef enum {
    IBV_QPT_RC = 2,   // reliable connection — the one-sided-write lane
} ibv_qp_type_t;

enum {
    IBV_WR_RDMA_WRITE = 0,
};

enum {
    IBV_SEND_SIGNALED = 1,
    IBV_SEND_INLINE    = 1 << 1,
};

enum {
    IBV_ACCESS_LOCAL_WRITE  = 1,
    IBV_ACCESS_REMOTE_WRITE = 1 << 1,
    IBV_ACCESS_REMOTE_READ  = 1 << 2,
    IBV_ACCESS_ON_DEMAND    = 1 << 6,
};

// ibv_qp_attr mask bits (frozen ABI values).
enum {
    IBV_QP_STATE             = 1,
    IBV_QP_CUR_STATE         = 1 << 1,
    IBV_QP_ACCESS_FLAGS      = 1 << 3,
    IBV_QP_PKEY_INDEX        = 1 << 4,
    IBV_QP_PORT              = 1 << 5,
    IBV_QP_AV                = 1 << 7,
    IBV_QP_PATH_MTU          = 1 << 8,
    IBV_QP_TIMEOUT           = 1 << 9,
    IBV_QP_RETRY_CNT         = 1 << 10,
    IBV_QP_RNR_RETRY         = 1 << 11,
    IBV_QP_RQ_PSN            = 1 << 12,
    IBV_QP_MAX_QP_RD_ATOMIC  = 1 << 13,
    IBV_QP_MIN_RNR_TIMER     = 1 << 14,
    IBV_QP_SQ_PSN            = 1 << 15,
    IBV_QP_MAX_DEST_RD_ATOMIC = 1 << 16,
    IBV_QP_DEST_QPN          = 1 << 19,
};

typedef enum {
    IBV_WC_SUCCESS        = 0,
    IBV_WC_LOC_LEN_ERR    = 1,
    IBV_WC_LOC_QP_OP_ERR  = 2,
    IBV_WC_LOC_PROT_ERR   = 4,
    IBV_WC_WR_FLUSH_ERR   = 5,
    IBV_WC_LOC_ACCESS_ERR = 8,
    IBV_WC_REM_ACCESS_ERR = 10,
    IBV_WC_RETRY_EXC_ERR  = 12,
    IBV_WC_GENERAL_ERR    = 21,
} ibv_wc_status_t;

// ---- mirrored structs (field order frozen by provider ABI) ------------------

typedef union ibv_gid {
    uint8_t raw[16];
    struct {
        uint64_t subnet_prefix;
        uint64_t interface_id;
    } global;
} ibv_gid_t;

typedef struct ibv_port_attr {
    int          port_state;      // enum ibv_port_state
    int          max_mtu;         // enum ibv_mtu
    int          active_mtu;      // enum ibv_mtu
    int          gid_tbl_len;
    uint32_t     port_cap_flags;
    uint32_t     max_msg_sz;
    uint32_t     bad_pkey_cntr;
    uint32_t     qkey_viol_cntr;
    uint16_t     pkey_tbl_len;
    uint16_t     lid;
    uint16_t     sm_lid;
    uint8_t      lmc;
    uint8_t      max_vl_num;
    uint8_t      sm_sl;
    uint8_t      subnet_timeout;
    uint8_t      init_type_reply;
    uint8_t      active_width;
    uint8_t      active_speed;
    uint8_t      phys_state;
    uint8_t      link_layer;      // ibv_link_layer_t
} ibv_port_attr_t;

typedef struct ibv_global_route {
    ibv_gid_t dgid;
    uint32_t  flow_label;
    uint8_t   sgid_index;
    uint8_t   hop_limit;
    uint8_t   traffic_class;
} ibv_global_route_t;

typedef struct ibv_ah_attr {
    ibv_global_route_t grh;
    uint16_t           dlid;
    uint8_t            sl;
    uint8_t            src_path_bits;
    uint8_t            static_rate;
    uint8_t            is_global;
    uint8_t            port_num;
} ibv_ah_attr_t;

typedef struct ibv_qp_cap {
    uint32_t max_send_wr;
    uint32_t max_recv_wr;
    uint32_t max_send_sge;
    uint32_t max_recv_sge;
    uint32_t max_inline_data;
} ibv_qp_cap_t;

typedef struct ibv_qp_init_attr {
    void*             qp_context;
    struct ibv_cq*    send_cq;
    struct ibv_cq*    recv_cq;
    struct ibv_srq*   srq;
    ibv_qp_cap_t      cap;
    int               qp_type;    // ibv_qp_type_t
    int               sq_sig_all;
} ibv_qp_init_attr_t;

typedef struct ibv_qp_attr {
    int               qp_state;       // ibv_qp_state_t
    int               cur_qp_state;
    int               path_mtu;       // ibv_mtu_t
    int               path_mig_state;
    uint32_t          qkey;
    uint32_t          rq_psn;
    uint32_t          sq_psn;
    uint32_t          dest_qp_num;
    uint32_t          qp_access_flags;
    ibv_qp_cap_t      cap;
    ibv_ah_attr_t     ah_attr;
    ibv_ah_attr_t     alt_ah_attr;
    uint16_t          pkey_index;
    uint16_t          alt_pkey_index;
    uint8_t           en_sqd_async_notify;
    uint8_t           sq_draining;
    uint8_t           max_rd_atomic;
    uint8_t           max_dest_rd_atomic;
    uint8_t           min_rnr_timer;
    uint8_t           timeout;
    uint8_t           retry_cnt;
    uint8_t           rnr_retry;
    uint8_t           port;
} ibv_qp_attr_t;

typedef struct ibv_sge {
    uint64_t addr;
    uint32_t length;
    uint32_t lkey;
} ibv_sge_t;

typedef struct ibv_send_wr {
    uint64_t              wr_id;
    struct ibv_send_wr*   next;
    ibv_sge_t*            sg_list;
    int                   num_sge;
    int                   opcode;       // ibv_wr_opcode
    int                   send_flags;
    union {
        struct {
            uint64_t remote_addr;
            uint32_t rkey;
            uint32_t reserved;
        } rdma;
        uint8_t _opaque[40];
    } wr;
} ibv_send_wr_t;

typedef struct ibv_wc {
    uint64_t wr_id;
    int      status;       // ibv_wc_status_t
    int      opcode;
    uint32_t vendor_err;
    uint32_t byte_len;
    uint32_t imm_data;     // network order (unused here)
    uint32_t qp_num;
    uint32_t src_qp;
    uint32_t wc_flags;
    uint16_t pkey_index;
    uint16_t lid;
    uint8_t  sl;
    uint8_t  dlid_path_bits;
    uint8_t  port_num;
} ibv_wc_t;

typedef struct ibv_mr {
    struct ibv_context* context;
    struct ibv_pd*      pd;
    void*               addr;
    size_t              length;
    uint32_t            handle;
    uint32_t            lkey;
    uint32_t            rkey;
} ibv_mr_t;

/// struct ibv_qp — only the frozen head is mirrored (handle/qp_num sit
/// at offsets 40/44 and have not moved since 2005); the driver reads
/// qp_num after create_qp and never touches the tail.
typedef struct ibv_qp {
    struct ibv_context* context;
    struct ibv_pd*      pd;
    struct ibv_cq*      send_cq;
    struct ibv_cq*      recv_cq;
    struct ibv_srq*     srq;
    uint32_t            handle;
    uint32_t            qp_num;
} ibv_qp_t;

#endif // !WEFT_RDMA_VERIFY_ABI

// ---- frozen-offset asserts (both modes; the drift tripwire) -----------------
// These are the offsets the driver and the WRH1 handshake depend on.
// In verify mode they hold rdma-core to them; in mirror mode they hold
// the mirror to them. Same numbers, both sides, one truth.
_Static_assert(offsetof(ibv_qp_attr_t, dest_qp_num)     == 28, "abi: dest_qp_num@28");
_Static_assert(offsetof(ibv_qp_attr_t, cap)             == 36, "abi: cap@36");
_Static_assert(offsetof(ibv_qp_attr_t, ah_attr)         == 56, "abi: ah_attr@56");
_Static_assert(offsetof(ibv_qp_attr_t, max_dest_rd_atomic) == 127, "abi: max_dest_rd_atomic@127");
_Static_assert(offsetof(ibv_qp_attr_t, port)            == 132, "abi: port@132");
_Static_assert(sizeof(ibv_qp_attr_t)                    == 136, "abi: qp_attr 136B");
_Static_assert(offsetof(ibv_send_wr_t, opcode)          == 28, "abi: opcode@28");
_Static_assert(offsetof(ibv_send_wr_t, send_flags)      == 32, "abi: send_flags@32");
_Static_assert(offsetof(ibv_send_wr_t, wr)              == 40, "abi: wr@40");
_Static_assert(offsetof(ibv_wc_t, status)               == 8,  "abi: wc.status@8");
_Static_assert(offsetof(ibv_wc_t, byte_len)             == 20, "abi: wc.byte_len@20");
_Static_assert(sizeof(ibv_wc_t)                         == 48, "abi: wc 48B");
_Static_assert(offsetof(ibv_mr_t, addr)                 == 16, "abi: mr.addr@16");
_Static_assert(offsetof(ibv_mr_t, lkey)                 == 36, "abi: mr.lkey@36");
_Static_assert(offsetof(ibv_mr_t, rkey)                 == 40, "abi: mr.rkey@40");
_Static_assert(offsetof(ibv_qp_t, qp_num)               == 44, "abi: qp.qp_num@44");
_Static_assert(sizeof(ibv_ah_attr_t)                    == 32, "abi: ah_attr 32B");

// ---- the vtable (dlsym-resolved; mock-injectable) ---------------------------

typedef struct weft_ibv_api {
    // device lifecycle
    struct ibv_device** (*get_device_list)(int* num_devices);
    void                (*free_device_list)(struct ibv_device** list);
    const char*         (*get_device_name)(struct ibv_device* device);
    struct ibv_context* (*open_device)(struct ibv_device* device);
    int                 (*close_device)(struct ibv_context* context);
    int                 (*query_port)(struct ibv_context* context,
                                      uint8_t port_num,
                                      ibv_port_attr_t* attr);
    // protection domain + memory registration
    struct ibv_pd*      (*alloc_pd)(struct ibv_context* context);
    int                 (*dealloc_pd)(struct ibv_pd* pd);
    ibv_mr_t*           (*reg_mr)(struct ibv_pd* pd, void* addr,
                                  size_t length, int access);
    int                 (*dereg_mr)(ibv_mr_t* mr);
    // queue pair
    struct ibv_cq*      (*create_cq)(struct ibv_context* context, int cqe,
                                     void* cq_context,
                                     struct ibv_comp_channel* channel,
                                     int comp_vector);
    int                 (*destroy_cq)(struct ibv_cq* cq);
    struct ibv_qp*      (*create_qp)(struct ibv_pd* pd,
                                     ibv_qp_init_attr_t* attr);
    int                 (*destroy_qp)(struct ibv_qp* qp);
    int                 (*modify_qp)(struct ibv_qp* qp, ibv_qp_attr_t* attr,
                                     int attr_mask);
    // the data path (one-sided writes + bounded polling)
    int                 (*post_send)(struct ibv_qp* qp, ibv_send_wr_t* wr,
                                     ibv_send_wr_t** bad_wr);
    int                 (*poll_cq)(struct ibv_cq* cq, int num_wc,
                                   ibv_wc_t* wc);
} weft_ibv_api_t;

/// Fill `api` by dlsym from `handle`. Returns 0, or -1 with `missing`
/// naming the first absent symbol (an honest driver refusal, Law 4).
int weft_ibv_api_resolve(void* handle, weft_ibv_api_t* api,
                         const char** missing);

#endif // WEFT_RDMA_ABI_H
