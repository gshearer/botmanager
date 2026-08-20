#ifndef SHORTURL_VERSION_STR_H
#define SHORTURL_VERSION_STR_H

#include "version.h"

// The daemon is copied to the web host by hand, so the one question its
// version string has to answer is which botmanager build produced the binary
// sitting over there. Only main.c and admin.c include this: botmanager's
// version.h is regenerated on every ninja run, and a TU that includes it
// recompiles every time.
#define SHORTURL_VERSION_STRING "shorturl — " BM_VERSION_STR

#endif
