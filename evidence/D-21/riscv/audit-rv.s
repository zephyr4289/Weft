	.file	"audit_rv.c"
	.option pic
	.attribute arch, "rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_zicsr2p0_zifencei2p0"
	.attribute unaligned_access, 0
	.attribute stack_align, 16
	.text
	.align	1
	.globl	st_rel64
	.type	st_rel64, @function
st_rel64:
.LFB0:
	.cfi_startproc
	fence	rw,w
	sd	a1,0(a0)
	ret
	.cfi_endproc
.LFE0:
	.size	st_rel64, .-st_rel64
	.align	1
	.globl	ld_acq64
	.type	ld_acq64, @function
ld_acq64:
.LFB1:
	.cfi_startproc
	ld	a5,0(a0)
	fence	r,rw
	ret
	.cfi_endproc
.LFE1:
	.size	ld_acq64, .-ld_acq64
	.align	1
	.globl	xchg_acqrel
	.type	xchg_acqrel, @function
xchg_acqrel:
.LFB2:
	.cfi_startproc
	amoswap.d.aqrl	a1,a1,0(a0)
	mv	a0,a1
	ret
	.cfi_endproc
.LFE2:
	.size	xchg_acqrel, .-xchg_acqrel
	.align	1
	.globl	fence_sc
	.type	fence_sc, @function
fence_sc:
.LFB3:
	.cfi_startproc
	fence	rw,rw
	ret
	.cfi_endproc
.LFE3:
	.size	fence_sc, .-fence_sc
	.align	1
	.globl	st_rel32
	.type	st_rel32, @function
st_rel32:
.LFB4:
	.cfi_startproc
	fence	rw,w
	sw	a1,0(a0)
	ret
	.cfi_endproc
.LFE4:
	.size	st_rel32, .-st_rel32
	.align	1
	.globl	ld_relaxed32
	.type	ld_relaxed32, @function
ld_relaxed32:
.LFB5:
	.cfi_startproc
	lw	a0,0(a0)
	sext.w	a0,a0
	ret
	.cfi_endproc
.LFE5:
	.size	ld_relaxed32, .-ld_relaxed32
	.align	1
	.globl	st_relaxed32
	.type	st_relaxed32, @function
st_relaxed32:
.LFB6:
	.cfi_startproc
	sw	a1,0(a0)
	ret
	.cfi_endproc
.LFE6:
	.size	st_relaxed32, .-st_relaxed32
	.globl	g64
	.globl	g32
	.bss
	.align	3
	.type	g64, @object
	.size	g64, 8
g64:
	.zero	8
	.type	g32, @object
	.size	g32, 4
g32:
	.zero	4
	.ident	"GCC: (Debian 14.2.0-19) 14.2.0"
	.section	.note.GNU-stack,"",@progbits
