#ifndef FCT_BASE_THIRDPARTY_H
#define FCT_BASE_THIRDPARTY_H

#include "TracyImports.h"
#ifdef __ANDROID__
#define FCT_USE_ANDROID
#define FCT_ANDROID
#endif

#if defined(_DEBUG) || defined(DEBUG)
#define FCT_DEBUG_MODE
#define FCT_DEBUG
#endif

#endif // FCT_BASE_THIRDPARTY_H
