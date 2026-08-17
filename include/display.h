#ifndef BOTMANAGER_DISPLAY_H
#define BOTMANAGER_DISPLAY_H

// Column geometry for the tables and cards plugins draw.
//
// Six renderers had grown their own copy of this arithmetic — stock,
// crypto, attack, whenmoon, rawg and tmdb (OBS-8) — and the copies had
// begun to disagree: crypto's width count never learned about UTF-8, so
// a multibyte glyph in one of its cells would have counted three columns
// instead of one. Nothing had put one there yet. The next renderer to
// hand-roll this arithmetic is the bug, so there is one copy now.
//
// Two things are invisible to strlen() and both are counted here:
//
//   - an abstract color marker from colors.h is "\x01" + one code
//     letter, two bytes of zero width. Pad BEFORE translation to ANSI;
//     a real escape sequence is opaque to this and will be counted.
//   - a UTF-8 code point is one column, however many bytes it took.
//     Every glyph these tables draw — block, skull, arrow, en-dash — is
//     single-width, so counting lead bytes is counting columns. An
//     East-Asian wide character would be counted as one and align
//     short; no caller draws one, and the day one does this is the one
//     place that has to learn wcwidth().
//
// Nothing here is thread-hostile: every function works on caller-owned
// storage and keeps no state.
//
// Text arriving from outside the process — a nickname, a provider's
// title — is the normal input, so every function tolerates a truncated
// UTF-8 sequence, an unpaired marker byte and a full buffer.

#include <stddef.h>

// Visible columns in `s`, per the two rules above.
size_t display_vis_len(const char *s);

// Pad `buf` to `width` visible columns in place, if it fits.
//
// A cell already at or past `width` is left alone: a renderer's grid
// gives way to the content rather than truncating it, which is what
// keeps a long value readable instead of merely aligned.
//
// display_align_right shifts the content up and fills the gap; it needs
// room for the whole padded cell and does nothing at all without it.
// display_align_left appends spaces and pads as far as `cap` allows.
// Both leave `buf` NUL-terminated.
void display_align_right(char *buf, size_t cap, int width);
void display_align_left (char *buf, size_t cap, int width);

// Copy at most `cols` display columns of `src` into `dst`, never
// splitting a UTF-8 sequence and always NUL-terminating.
//
// `mark` is written when — and only when — content was left behind:
// pass "…" for an ellipsis, NULL for a hard stop. Its own bytes are
// reserved out of `cap` before copying, so the mark can never be the
// thing that does not fit.
//
// `src` is raw text: no color markers are expected in it, and a marker
// there would be copied as two ordinary bytes and counted as two
// columns.
void display_fit(const char *src, int cols, char *dst, size_t cap,
    const char *mark);

// Append a finished cell to a line, stopping cleanly at capacity.
//
// `line` must already be a string — a renderer that starts an empty one
// writes `line[0] = '\0'` (or its first cell with snprintf) before the
// first call, because this finds the end by scanning for the NUL.
void display_cat(char *line, size_t cap, const char *cell);

#endif
