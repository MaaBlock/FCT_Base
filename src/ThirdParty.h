#ifndef FCT_BASE_THIRDPARTY_H
#define FCT_BASE_THIRDPARTY_H

#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>

#undef ZoneScoped
#ifdef TRACY_ENABLE
#define ZoneScoped() SuppressVarShadowWarning( ZoneNamed( ___tracy_scoped_zone, true ) )
#else
#define ZoneScoped()
#endif

#endif // FCT_BASE_THIRDPARTY_H
