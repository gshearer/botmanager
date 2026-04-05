#ifndef BM_MAIN_H
#define BM_MAIN_H

#ifdef MAIN_INTERNAL

#include "common.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

// Path to the bootstrap configuration file (NULL = default).
static const char *config_path = NULL;

// File descriptor for the instance lock (prevents concurrent daemons).
static int lock_fd = -1;

#endif // MAIN_INTERNAL

#endif // BM_MAIN_H
