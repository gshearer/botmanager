#ifndef BM_MAIN_H
#define BM_MAIN_H

#include <time.h>

// Set once at startup. Used for uptime reporting in /show status.
extern time_t bm_start_time;

#ifdef MAIN_INTERNAL

#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

// NULL = default.
static const char *config_path = NULL;

// Prevents concurrent daemons.
static int lock_fd = -1;

#endif // MAIN_INTERNAL

#endif // BM_MAIN_H
