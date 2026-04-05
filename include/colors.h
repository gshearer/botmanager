#ifndef BM_COLORS_H
#define BM_COLORS_H

#include <stddef.h>

// Console (ANSI)
#define CON_RED     "\033[0;31m"
#define CON_GREEN   "\033[0;32m"
#define CON_YELLOW  "\033[0;33m"
#define CON_BLUE    "\033[0;34m"
#define CON_PURPLE  "\033[0;35m"
#define CON_CYAN    "\033[0;36m"
#define CON_WHITE   "\033[0;37m"
#define CON_ORANGE  "\033[0;33m"   // closest ANSI: yellow/brown
#define CON_GRAY    "\033[0;90m"
#define CON_BOLD    "\033[1m"
#define CON_BLINK   "\033[4m"
#define CON_RESET   "\033[0m"

// Internet Relay Chat
#define IRC_WHITE   "\00300"
#define IRC_BLACK   "\00301"
#define IRC_BLUE    "\00302"
#define IRC_GREEN   "\00303"
#define IRC_RED     "\00304"
#define IRC_MAROON  "\00305"
#define IRC_PURPLE  "\00306"
#define IRC_ORANGE  "\00307"
#define IRC_YELLOW  "\00308"
#define IRC_LGREEN  "\00309"
#define IRC_TEAL    "\00310"
#define IRC_CYAN    "\00311"
#define IRC_RBLUE   "\00312"
#define IRC_MAGENTA "\00313"
#define IRC_GRAY    "\00314"
#define IRC_LGRAY   "\00315"
#define IRC_RESET   "\017"

// Abstract color markers (method-agnostic).
// These embed 2-byte tokens (\x01 + identifier) into strings.
// method_send() translates them to the driver's native codes.
#define CLR_RED     "\x01" "R"
#define CLR_GREEN   "\x01" "G"
#define CLR_YELLOW  "\x01" "Y"
#define CLR_BLUE    "\x01" "B"
#define CLR_PURPLE  "\x01" "P"
#define CLR_CYAN    "\x01" "C"
#define CLR_WHITE   "\x01" "W"
#define CLR_ORANGE  "\x01" "O"
#define CLR_GRAY    "\x01" "A"
#define CLR_BOLD    "\x01" "b"
#define CLR_RESET   "\x01" "X"

// Color table: maps abstract markers to method-specific color strings.
// Each method driver provides a static instance of this struct.
typedef struct
{
  const char *red;
  const char *green;
  const char *yellow;
  const char *blue;
  const char *purple;
  const char *cyan;
  const char *white;
  const char *orange;
  const char *gray;
  const char *bold;
  const char *reset;
} color_table_t;

// Translate abstract color markers in src to native color strings
// using the given color table. If ct is NULL, markers are stripped.
// Always NUL-terminates dst. Returns bytes written (excluding NUL).
size_t color_translate(char *dst, size_t dst_sz,
    const char *src, const color_table_t *ct);

#endif // BM_COLORS_H
