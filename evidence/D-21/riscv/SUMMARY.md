# RISC-V RV64GC portability evidence (issue #18-2, D-21)
# env: riscv64-linux-gnu-gcc 14.2.0 (cross), qemu-riscv64 10.0.13 user-mode, host x86_64 Debian 13
L: L1-tear -> "pass":true
L: L2-writer-steps -> "pass":true
L: L3-reader-steps -> "pass":true
L: L4-freshness -> "pass":true
L: L5-progress -> "pass":true
L: L6-ownership -> "pass":true
L: L7-revocation -> "pass":true
L: L8-envelope -> "pass":true
F-series default regime: 396 checks ok
F-series seq_cst regime: 396 checks ok
audit: 1 amoswap.d.aqrl found
