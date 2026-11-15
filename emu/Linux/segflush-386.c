#include <sys/types.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>

#include "dat.h"

/*
 * no cache flush is needed on x86, but the heap is not
 * executable on current systems, so make the pages holding
 * the newly compiled code executable.
 */
int
segflush(void *a, ulong n)
{
	uintptr_t s, e, pg;

	if(n == 0)
		return 0;
	pg = sysconf(_SC_PAGESIZE);
	s = (uintptr_t)a & ~(pg-1);
	e = ((uintptr_t)a + n + pg-1) & ~(pg-1);
	if(mprotect((void*)s, e-s, PROT_READ|PROT_WRITE|PROT_EXEC) < 0)
		return -1;
	return 0;
}
