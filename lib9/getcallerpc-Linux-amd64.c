/*
 * getcallerpc is a macro using __builtin_return_address on amd64
 * (see lib9.h); the address of an argument tells us nothing there.
 */
int	__getcallerpc_dummy;
