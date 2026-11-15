#include "lib9.h"
#include <a.out.h>
#include <dynld.h>

/*
 * dynamically-loaded C modules are not supported on amd64
 */

long
dynmagic(void)
{
	return 0;
}

char*
dynreloc(uchar *b, ulong p, int m, Dynsym **tab, int ntab)
{
	USED(b);
	USED(p);
	USED(m);
	USED(tab);
	USED(ntab);
	return "dynamic modules not supported";
}
