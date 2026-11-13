//
// Created by MaaBlock on 2026/10/18.
//
#ifndef FCT_TRACYIMPORTS_H
#define FCT_TRACYIMPORTS_H
#include <tracy/Tracy.hpp>
#include <tracy/TracyC.h>
#undef ZoneScoped
#ifdef TRACY_ENABLE
#define ZoneScoped() SuppressVarShadowWarning( ZoneNamed( ___tracy_scoped_zone, true ) )
#else
#define ZoneScoped()
#endif

namespace FCT
{
    using tracy::SetThreadName;
}
#endif // FCT_TRACYIMPORTS_H
