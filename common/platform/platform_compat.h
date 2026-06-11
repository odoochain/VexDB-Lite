#ifndef PLATFORM_COMPAT_H
#define PLATFORM_COMPAT_H

#if defined(PG_VEXDB_TARGET_PG)
#include "pg_compat.h"
#endif

#if defined(PG_VEXDB_TARGET_DUCK)
#include "duck_compat.h"
#endif

#endif /* PLATFORM_COMPAT_H */
