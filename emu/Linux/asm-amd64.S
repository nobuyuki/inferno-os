	.file	"asm-Linux-amd64.S"
	.text

/*
 * void FPsave(void *fpu)
 * void FPrestore(void *fpu)
 *	the floating-point state that matters is the SSE control
 *	and status register: the compiler does not use the x87
 */

	.type	FPsave,@function
	.global	FPsave
FPsave:
	stmxcsr	(%rdi)
	ret

	.type	FPrestore,@function
	.global	FPrestore
FPrestore:
	ldmxcsr	(%rdi)
	ret

/*
 * int _tas(int *lock)
 */
	.type	_tas,@function
	.globl	_tas
_tas:
	movl	$1, %eax
	xchgl	%eax, (%rdi)
	ret

	.section	.note.GNU-stack,"",@progbits
