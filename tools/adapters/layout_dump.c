/*
 * layout_dump.c - Weft Pillar 6 layout audit tool.
 *
 * Prints the compile-time-verified memory map of every projection
 * struct (the same offsets pinned by _Static_assert in the frozen
 * header). Output feeds D-61 section 3 (struct memory layout maps
 * and padding proofs).
 *
 * Build: cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
 *          -Icore/c/include tools/adapters/layout_dump.c \
 *          core/c/src/adapters/weft_adapter_checksum.c -o build/adapters/layout_dump
 */
#include "weft_adapters.h"

#include <stdio.h>

#define P(t, f)                                                        \
    printf("  %-28s %-24s off=%3zu size=%zu align=%zu\n", #t, #f,       \
           offsetof(t, f), sizeof(((t *)0)->f), _Alignof(t))

int main(void)
{
    printf("weft-adapters ABI version: %d (runtime %d)\n",
           WEFT_ADAPTERS_ABI_VERSION, weft_adapters_abi_version());

    printf("\n[itch header] weft_itch_hdr_t  sizeof=%zu align=%zu\n",
           sizeof(weft_itch_hdr_t), _Alignof(weft_itch_hdr_t));
    P(weft_itch_hdr_t, msg_type);
    P(weft_itch_hdr_t, stock_locate);
    P(weft_itch_hdr_t, tracking_number);
    P(weft_itch_hdr_t, timestamp_ns);

    printf("\n[itch bodies] all sizeof=48 (union slot 16..63)\n");
    P(weft_itch_stock_directory_body_t, round_lot_size);
    P(weft_itch_stock_directory_body_t, etp_leverage_factor);
    P(weft_itch_add_order_body_t, order_ref);
    P(weft_itch_add_order_body_t, shares);
    P(weft_itch_add_order_body_t, price_raw);
    P(weft_itch_add_order_mpid_body_t, mpid);
    P(weft_itch_order_executed_body_t, match_number);
    P(weft_itch_order_executed_body_t, executed_shares);
    P(weft_itch_order_exec_price_body_t, execution_price);
    P(weft_itch_order_replace_body_t, new_order_ref);
    P(weft_itch_order_replace_body_t, price_raw);
    P(weft_itch_trade_body_t, match_number);
    P(weft_itch_trade_body_t, price_raw);
    P(weft_itch_cross_trade_body_t, shares);
    P(weft_itch_cross_trade_body_t, cross_price_raw);
    P(weft_itch_noii_body_t, paired_shares);
    P(weft_itch_noii_body_t, current_ref_price_raw);
    P(weft_itch_mwcb_body_t, level3);
    P(weft_itch_ipo_body_t, ipo_price_raw);
    P(weft_itch_luld_body_t, reference_price);

    printf("\n[itch projection] weft_itch_msg_t  sizeof=%zu align=%zu stride=64\n",
           sizeof(weft_itch_msg_t), _Alignof(weft_itch_msg_t));
    P(weft_itch_msg_t, hdr);
    P(weft_itch_msg_t, u);

    printf("\n[ouch header] weft_ouch_hdr_t  sizeof=%zu align=%zu\n",
           sizeof(weft_ouch_hdr_t), _Alignof(weft_ouch_hdr_t));
    P(weft_ouch_hdr_t, timestamp_ns);
    P(weft_ouch_hdr_t, msg_type);

    printf("\n[ouch bodies] all sizeof=48\n");
    P(weft_ouch_accepted_body_t, order_ref);
    P(weft_ouch_accepted_body_t, price_raw);
    P(weft_ouch_accepted_body_t, firm);
    P(weft_ouch_executed_body_t, match_number);
    P(weft_ouch_exec_price_body_t, execution_price);
    P(weft_ouch_replaced_body_t, price_raw);

    printf("\n[ouch projection] weft_ouch_msg_t  sizeof=%zu align=%zu stride=64\n",
           sizeof(weft_ouch_msg_t), _Alignof(weft_ouch_msg_t));
    P(weft_ouch_msg_t, hdr);
    P(weft_ouch_msg_t, u);

    printf("\n[sbe descriptors]\n");
    printf("  weft_sbe_field_desc_t sizeof=%zu\n",
           sizeof(weft_sbe_field_desc_t));
    printf("  weft_sbe_group_desc_t sizeof=%zu\n",
           sizeof(weft_sbe_group_desc_t));
    printf("  weft_sbe_schema_t     sizeof=%zu\n", sizeof(weft_sbe_schema_t));
    printf("  weft_sbe_view_t       sizeof=%zu\n", sizeof(weft_sbe_view_t));
    P(weft_sbe_schema_t, fixed);
    P(weft_sbe_schema_t, groups);
    P(weft_sbe_schema_t, group_fields);

    printf("\n[checksum engine] crc32c impl=%s adler32 impl=%s\n",
           weft_adapter_crc32c_impl_name(),
           weft_adapter_adler32_impl_name());

    printf("\nLAYOUT DUMP COMPLETE\n");
    return 0;
}
