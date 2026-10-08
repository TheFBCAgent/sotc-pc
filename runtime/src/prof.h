/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Tracy zones for the runtime (session 18). With SOTC_TRACY (CMake -DSOTC_TRACY=ON) they map to
 * Tracy's C API (third_party/tracy, v0.14.1, on demand: free until a viewer connects); without it
 * they compile to nothing.
 *
 *   PROF_ZONE(z, "name");  ...  PROF_ZONE_END(z);    a zone in one function (z is a local)
 *   PROF_ZONE_IF(z, "name", cond)                    the same, recorded only when cond is true
 *   PROF_ZONE_TEXT(z, str)                           attach a NUL-terminated string to the zone */
#ifndef SOTC_PROF_H
#define SOTC_PROF_H

#ifdef SOTC_TRACY
#include <string.h>
#include <tracy/TracyC.h>
#define PROF_ZONE(z, name) TracyCZoneN(z, name, 1)
#define PROF_ZONE_IF(z, name, cond) TracyCZoneN(z, name, (cond))
#define PROF_ZONE_END(z) TracyCZoneEnd(z)
#define PROF_ZONE_TEXT(z, str)                                                                     \
    do {                                                                                           \
        const char *prof_s_ = (str);                                                               \
        if (prof_s_) TracyCZoneText(z, prof_s_, strlen(prof_s_));                                  \
    } while (0)
#define PROF_THREAD_NAME(name) TracyCSetThreadName(name)
#else
#define PROF_ZONE(z, name) ((void)0)
#define PROF_ZONE_IF(z, name, cond) ((void)0)
#define PROF_ZONE_END(z) ((void)0)
#define PROF_ZONE_TEXT(z, str) ((void)0)
#define PROF_THREAD_NAME(name) ((void)0)
#endif

#endif
