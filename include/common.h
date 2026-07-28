#ifndef BM_COMMON_H
#define BM_COMMON_H

#define _GNU_SOURCE

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SUCCESS false
#define FAIL    true

// Byte-copy a function pointer's value into an object pointer. ISO C
// defines no conversion between the two, but every registry that
// retains a callback must be range-testable against a plugin's mapping
// (see plugin_owns_ptr). Pass the ADDRESS of the function pointer:
//
//   cb(path, "cb", fn_addr(&def->cb), data);
static inline const void *
fn_addr(const void *pfn)
{
  const void *p;

  memcpy(&p, pfn, sizeof(p));
  return(p);
}

#endif
