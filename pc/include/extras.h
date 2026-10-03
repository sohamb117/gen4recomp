/* PC port shadow of MSL's extras.h (metroskrew sdk msl/MSL_Extras/...).
 *
 * On the DS build mwcc's -stdinc resolves <extras.h> to Metrowerks' "MSL
 * Extras" header, a POSIX-compatibility grab bag the GameSpy nonport layer
 * (NitroDWC gs/nonport.h) includes for the underscore-less string functions.
 * The real header drags in a dozen more MSL-internal headers; the host libc
 * already has almost everything, so this shadow maps the names instead. */
#ifndef _MSL_EXTRAS_H
#define _MSL_EXTRAS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#ifndef stricmp
#define stricmp strcasecmp
#endif
#ifndef strnicmp
#define strnicmp strncasecmp
#endif
#ifndef strcmpi
#define strcmpi strcasecmp
#endif
#ifndef strncmpi
#define strncmpi strncasecmp
#endif

#endif /* _MSL_EXTRAS_H */
