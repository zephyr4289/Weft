// test_xdp.c — CL-series: the XDP/eBPF engine gates (RFC-0019 §4.7).
//
// GATE MAP:
//   CL-X1  probe: unprivileged runner -> named PERMS refusal (sandbox leg)
//   CL-X2  caps report line carries the capability truth
//   CL-X3  WCF1 golden vectors: every validator rung + the accept case
//   CL-X4  schema hash double-entry (runtime FNV == frozen constant)
//   CL-X5  assembler: schema-any vs schema-42 differ by exactly 2 insns
//   CL-X6  assembler: all jump targets in range, tail is EXIT, helper
//          ids within {1, 44, 51}
//   CL-X7  assembler: protocol immediates present (dport LE, magic,
//          cluster id, version, IHL, frag mask)
//   CL-X8  assembler: two map-fd pseudo-loads patched
//   CL-X9  u31 law: oversize steering keys refused
//   CL-X10 driver init: capability refusal carries a named reason
//   CL-X11 placement law: non-page-multiple chunk0/chunk refused by name
//   CL-X12 ring-depth pow2 law refused by name

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_harness.h"

#include <stdio.h>
#include <string.h>

#include <linux/bpf.h>

#include "weft_wcr1.h"
#include "weft_xdp_driver.h"

// eBPF encoding checks (raw opcodes — linux/bpf.h's own BPF_* macros
// use the class|op split; these are the fully-combined bytes)
#define TBPF_JNE_K 0x55
#define TBPF_JEQ_K 0x15
#define TBPF_CALL   0x85
#define TBPF_EXIT   0x95

static int imm_count(const struct bpf_insn* insns, int cnt, int32_t imm) {
    int n = 0;
    for (int i = 0; i < cnt; i++) {
        if (insns[i].imm == imm) n++;
    }
    return n;
}

int main(void) {
    // -- CL-X1: the probe (unprivileged leg) --------------------------------
    {
        char d[256];
        const weft_xdp_cap_t cap = weft_xdp_probe(d, sizeof(d));
        if (cap == WEFT_XDP_CAP_NONE) {
            GATE("CL-X1 unprivileged probe -> named PERMS/caps refusal",
                 strstr(d, "CAP_") != NULL || strstr(d, "AF_XDP") != NULL);
        } else {
            SKIP("CL-X1 refusal leg", "caps present — hardware runner; "
                 "the LIVE legs run there (D-32 checklist)");
        }
    }

    // -- CL-X2: caps report ---------------------------------------------------
    {
        char buf[256];
        weft_caps_report(buf, sizeof(buf));
        GATE("CL-X2 caps report names net_admin/bpf",
             strstr(buf, "net_admin") != NULL &&
             strstr(buf, "bpf=") != NULL);
    }

    // -- CL-X3: WCF1 golden vectors (the shared mirror) ------------------------
    {
        uint8_t chunk[4096] __attribute__((aligned(64)));
        weft_wcf1_t* h = (weft_wcf1_t*)chunk;
        weft_wcf1_prepare(h, 0x2A4D, 0x11, 5, 512, 3, 4, 0, 4096, 12345);
        const weft_wcf1_t* oh = NULL;
        const void* pl = NULL;

        GATEI("CL-X3 accept", weft_wcf1_validate(chunk, 4096, 0x2A4D, 0x11,
                                                 4096, &oh, &pl),
              WEFT_WCF1_OK);
        GATE("CL-X3 payload pointer law", pl == chunk + 64);
        GATEI("CL-X3 cluster mismatch refused",
              weft_wcf1_validate(chunk, 4096, 0x9999, 0, 4096, NULL, NULL),
              WEFT_WCF1_REFUSE_CLUSTER);
        GATEI("CL-X3 schema mismatch refused",
              weft_wcf1_validate(chunk, 4096, 0x2A4D, 0x77, 4096, NULL,
                                 NULL),
              WEFT_WCF1_REFUSE_SCHEMA);
        GATEI("CL-X3 schema-any accepts foreign schema",
              weft_wcf1_validate(chunk, 4096, 0x2A4D, 0, 4096, NULL, NULL),
              WEFT_WCF1_OK);
        h->magic[2] = 'x';
        GATEI("CL-X3 magic refused",
              weft_wcf1_validate(chunk, 4096, 0x2A4D, 0, 4096, NULL, NULL),
              WEFT_WCF1_REFUSE_MAGIC);
        weft_wcf1_prepare(h, 1, 1, 6, 5000, 3, 4, 0, 4096, 0);  // > chunk-64
        GATEI("CL-X3 oversize payload refused",
              weft_wcf1_validate(chunk, 4096, 1, 0, 4096, NULL, NULL),
              WEFT_WCF1_REFUSE_LEN);
        weft_wcf1_prepare(h, 1, 1, 7, 128, 3, 4, 0, 100, 0);  // mid-chunk
        GATEI("CL-X3 mid-chunk placement refused",
              weft_wcf1_validate(chunk, 4096, 1, 0, 4096, NULL, NULL),
              WEFT_WCF1_REFUSE_PLACEMENT);
        weft_wcf1_prepare(h, 1, 1, 8, 128, 3, 4, 0, 4096, 0);
        h->reserved[3] = 7;
        GATEI("CL-X3 reserved-nonzero refused",
              weft_wcf1_validate(chunk, 4096, 1, 0, 4096, NULL, NULL),
              WEFT_WCF1_REFUSE_RESERVED);
        h->reserved[3] = 0;
        GATEI("CL-X3 short buffer refused",
              weft_wcf1_validate(chunk, 32, 1, 0, 4096, NULL, NULL),
              WEFT_WCF1_REFUSE_SHORT);
    }

    // -- CL-X4: schema hash double entry ---------------------------------------
    GATEI("CL-X4 WCF1 schema hash double-entry",
          (long long)weft_wcf1_schema_hash(),
          (long long)WEFT_WCF1_SCHEMA_HASH);

    // -- CL-X5..X9: the assembler (pure — runs everywhere) ----------------------
    {
        struct bpf_insn any[64], sch[64];
        weft_xdp_config_t cfg = weft_xdp_config_default();
        cfg.cluster_id = 0x2A4D;
        cfg.schema_id = 0;
        const int n_any = weft_xdp_build_filter(&cfg, any, 64, 77);
        cfg.schema_id = 0x4242;   // distinctive: 0x11 would collide with
                                  // the UDP-proto immediate (17)
        const int n_sch = weft_xdp_build_filter(&cfg, sch, 64, 77);

        GATE("CL-X5 assembly succeeded", n_any > 0 && n_sch > 0);
        GATEI("CL-X5 schema block adds exactly 2 insns", n_sch - n_any, 2);
        GATEI("CL-X5 program length sane", (n_any >= 30 && n_any <= 48),
              1);

        // CL-X6: structure — jumps bounded, tail EXIT, helpers known
        int jumps_ok = 1, tail_ok = 0, helpers_ok = 1;
        for (int i = 0; i < n_sch; i++) {
            const struct bpf_insn* in = &sch[i];
            if ((in->code & 0x07) == 0x05 &&
                (in->code & 0xf8) != 0x80 &&   // not CALL
                (in->code & 0xf8) != 0x90) {  // not EXIT (off==0 is not
                                             // a jump target)
                const int target = i + 1 + in->off;
                if (target < 0 || target >= n_sch) jumps_ok = 0;
            }
            if ((in->code & 0xf8) == 0x80 && (in->code & 0x07) == 0x05) {
                // CALL: helper id
                if (in->imm != 1 && in->imm != 44 && in->imm != 51) {
                    helpers_ok = 0;
                }
            }
        }
        tail_ok = (sch[n_sch - 1].code == TBPF_EXIT &&
                   sch[n_sch - 2].code == 0xb7);   // r0 = XDP_PASS
        GATE("CL-X6 all jump targets in range", jumps_ok);
        GATE("CL-X6 tail is MOV r0, PASS + EXIT", tail_ok);
        GATE("CL-X6 helper ids within {1,44,51}", helpers_ok);

        // CL-X7: immediates (the protocol constants)
        const uint32_t port = cfg.udp_port;
        const int32_t port_le = (int32_t)(((port & 0xff) << 8) | (port >> 8));
        GATEI("CL-X7 dport LE immediate present",
              imm_count(sch, n_sch, port_le), 1);
        GATEI("CL-X7 magic immediate present",
              imm_count(sch, n_sch, 0x7466772e), 1);
        GATEI("CL-X7 cluster id immediate present",
              imm_count(sch, n_sch, 0x2A4D), 1);
        GATEI("CL-X7 schema id immediate present (block emitted)",
              imm_count(sch, n_sch, 0x4242), 1);
        GATEI("CL-X7 schema-any omits the schema immediate",
              imm_count(any, n_any, 0x4242), 0);
        GATEI("CL-X7 version immediate present",
              imm_count(sch, n_sch, 1) >= 1, 1);   // wcf1 version == 1
        GATEI("CL-X7 IHL immediate present", imm_count(sch, n_sch, 5), 1);
        GATEI("CL-X7 frag mask immediate present",
              imm_count(sch, n_sch, (int32_t)0xFF3F), 1);
        GATEI("CL-X7 udp proto immediate present",
              imm_count(sch, n_sch, 17), 1);
        GATEI("CL-X7 head-strip constant present",
              imm_count(sch, n_sch, 42), 1);

        // CL-X8: two map-fd pseudo loads
        int pseudo_loads = 0;
        for (int i = 0; i + 1 < n_sch; i++) {
            if (sch[i].code == 0x18 && sch[i].src_reg == BPF_PSEUDO_MAP_FD
                && sch[i].imm == 77) {
                pseudo_loads++;
            }
        }
        GATEI("CL-X8 two patched map-fd pseudo loads", pseudo_loads, 2);

        // CL-X9: the u31 law
        cfg.cluster_id = 0x80000000u;
        GATEI("CL-X9 oversize cluster id refused",
              weft_xdp_build_filter(&cfg, any, 64, 77), -1);
        cfg.cluster_id = 1;
        cfg.schema_id = 0x90000000u;
        GATEI("CL-X9 oversize schema id refused",
              weft_xdp_build_filter(&cfg, any, 64, 77), -1);
    }

    // -- CL-X10: driver init refusal carries a named reason ---------------------
    {
        weft_wcr1_region_t r;
        weft_wcr1_create_anon(4, 4096, 1, &r);
        weft_xdp_config_t cfg = weft_xdp_config_default();
        weft_xdp_socket_t s;
        const weft_cluster_status_t st = weft_xdp_driver_init(&cfg, &r, &s);
        if (st == WEFT_CLUSTER_OK) {
            SKIP("CL-X10 refusal leg", "AF_XDP functional on this runner");
            weft_xdp_driver_shutdown(&s);
        } else {
            GATE("CL-X10 init refused with a named reason",
                 st == WEFT_CLUSTER_E_PERMS || st == WEFT_CLUSTER_E_SYS ||
                 st == WEFT_CLUSTER_E_IO);
            GATE("CL-X10 reason text is populated",
                 s.err[0] != '\0');
        }
        weft_wcr1_destroy(&r);
    }

    // -- CL-X11: the placement law (pure — runs everywhere) ----------------------
    {
        // non-page-multiple chunk_size
        weft_wcr1_region_t r;
        weft_wcr1_create_anon(4, 4096, 1, &r);
        r.chunk_size = 2048;   // 64-multiple but not page-multiple
        weft_xdp_config_t cfg = weft_xdp_config_default();
        weft_xdp_socket_t s;
        GATEI("CL-X11 non-page chunk refused",
              weft_xdp_driver_init(&cfg, &r, &s),
              WEFT_CLUSTER_E_INVALID_ARG);
        weft_wcr1_destroy(&r);
    }

    // -- CL-X12: ring-depth pow2 law --------------------------------------------
    {
        weft_wcr1_region_t r;
        weft_wcr1_create_anon(4, 4096, 1, &r);
        weft_xdp_config_t cfg = weft_xdp_config_default();
        cfg.fill_depth = 100;   // not a power of two
        weft_xdp_socket_t s;
        GATEI("CL-X12 non-pow2 depth refused",
              weft_xdp_driver_init(&cfg, &r, &s),
              WEFT_CLUSTER_E_INVALID_ARG);
        weft_wcr1_destroy(&r);
    }

    TEST_EXIT();
}
