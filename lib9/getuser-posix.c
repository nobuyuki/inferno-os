#include "lib9.h"
#pragma push_macro("long")
#undef long
#include <pwd.h>

#pragma pop_macro("long")
char*
getuser(void)
{
	struct passwd *p;

	static char *user = 0;

	if (!user) {
		p = getpwuid(getuid());
		if (p && p->pw_name) {
			user = malloc(strlen(p->pw_name)+1);
			if (user)
				strcpy(user, p->pw_name);
		}
	}
	if(!user)
		user = "unknown";
	return user;
}
