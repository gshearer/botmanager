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

// The house width for strict-formatted output: a table, the rule under
// its header, a card's frame — anything whose columns have to line up
// down the page. Ninety columns.
//
// A renderer's total is DERIVED from this, never chosen for itself, so
// widening one column means naming which other column pays for it. That
// is the whole point of the constant: the number lives in one place and
// a grid that drifts off it drifts visibly.
//
// It is a default, not a law. A view with a real reason to sit
// elsewhere — a one-line status, a prose card meant to be read rather
// than scanned — says so where it defines its own total. What it must
// not do is pick a width silently.
//
// ⚠ The renderers that predate this rule agree with neither it nor each
// other: `!stock` draws 103, `!crypto` 85, `show attack` 73. They are
// **not** being swept. Bring one onto DISPLAY_COLS when you are already
// reworking its geometry for some other reason, never as an errand of
// its own — the same standing the tree gives its `snprintf`-copies and
// its inline cell reads.
#define DISPLAY_COLS  90

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
// ⚠ The mark is reserved out of `cap` — the BYTE budget — and not out
// of `cols`. A truncated result is therefore `cols + 1` columns wide,
// not `cols`. A padded cell absorbs that in its padding and never
// notices; the LAST cell of a row has no padding to absorb it, so a
// caller sizing a grid from `cols` puts one column past its own rule
// exactly when a value is long enough to be marked. Budget `cols` as
// the width you want MINUS the mark.
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
