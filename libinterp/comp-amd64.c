#include "lib9.h"
#include "isa.h"
#include "interp.h"
#include "raise.h"

/*
 * no just-in-time compiler for amd64 yet: everything is interpreted
 */

void	(*comvec)(void);

int
compile(Module *m, int size, Modlink *ml)
{
	USED(m);
	USED(size);
	USED(ml);
	return 0;
}
