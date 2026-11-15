#include "lib9.h"

/*
 * floating-point control and status on amd64, where the
 * compiler uses SSE2: map the x87-style bits of lib9.h
 * (exception enables in bits 0-5, rounding in bits 10-11)
 * onto MXCSR (flags in bits 0-5, masks in bits 7-12,
 * rounding in bits 13-14).  Precision control does not exist.
 */

static u32int
getmxcsr(void)
{
	u32int v;

	__asm__ __volatile__("stmxcsr %0" : "=m" (v));
	return v;
}

static void
setmxcsr(u32int v)
{
	__asm__ __volatile__("ldmxcsr %0" : : "m" (v));
}

void
setfcr(ulong fcr)
{
	u32int v;

	v = getmxcsr() & 0x3f;	/* keep the accrued flags */
	v |= (~fcr & 0x3f) << 7;	/* enabled -> not masked */
	v |= ((fcr >> 10) & 3) << 13;
	setmxcsr(v);
}

ulong
getfcr(void)
{
	u32int v;

	v = getmxcsr();
	return ((~v >> 7) & 0x3f) | (((v >> 13) & 3) << 10) | FPPDBL;
}

ulong
getfsr(void)
{
	return getmxcsr() & 0x3f;
}

void
setfsr(ulong fsr)
{
	setmxcsr((getmxcsr() & ~0x3f) | (fsr & 0x3f));
}
