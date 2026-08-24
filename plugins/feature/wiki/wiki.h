#ifndef BM_WIKI_H
#define BM_WIKI_H

// Command surface for /wiki. Every byte of presentation for the
// wikimedia service plugin lives here; the service itself owns the wire
// and the normalization and knows nothing about a command context.
//
// The grammar is deterministic and there is no model anywhere in it: a
// user gets savvy with the syntax the way they do with /s or /n, and
// the command has to keep working with the inference engine stopped.

#ifdef WIKI_INTERNAL

#include "alloc.h"
#include "clam.h"
#include "cmd.h"
#include "colors.h"
#include "common.h"
#include "display.h"
#include "method.h"
#include "plugin.h"

#include "wikimedia_api.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define WIKI_CTX  "wiki"

// One assembled reply line. Wider than any single card row because a
// dense line packs the whole answer into one.
#define WIKI_LINE_SZ  1792

// A body built up before it is framed — two of these plus their join
// still fit a line.
#define WIKI_TEXT_SZ  768

// One cell, bounded by display columns rather than by bytes: four bytes
// to a column and slack for the markers around it.
#define WIKI_CELL_SZ  384

// One property phrase — "date of birth", "cause of death".
#define WIKI_WORD_SZ  96

// A subject as the user wrote it. The service normalizes and bounds it
// again on the way in; this is our own storage and our own boundary.
#define WIKI_SUBJECT_SZ  128

// Trailing tokens the greedy split will try as a property.
#define WIKI_SPLIT_MAX  3

// Words one query may hold. A subject longer than this is not a subject.
#define WIKI_TOKENS_MAX 24

// What a sunk reply may weigh. The interpret cue fences a bridged
// command's output into METHOD_TEXT_SZ minus its own head and tail
// (plugins/bot/chat/interpret.c), and output past that is truncated
// with a marker. This sits under the fence with room to spare.
#define WIKI_DENSE_MAX  1200

// Card rows shown, plain and under -v. The service carries more
// (WM_FACTS_MAX); this is what fits a channel without being a wall.
#define WIKI_CARD_ROWS      7
#define WIKI_CARD_ROWS_FULL 16

// Reverse-query matches shown when -n says nothing.
#define WIKI_MATCHES_DEF  10

// Candidates kept from a resolution. The window the service returns is
// wider; what this command does with the tail is name three of them on
// an `also:` line, and a reverse query's own list is capped by -n.
#define WIKI_ALTS_MAX  10

// How long stop() waits for its own airborne closures. The same budget
// every plugin in this tree gives a drain.
#define WIKI_STOP_DRAIN_MS  3000

// Card geometry. The value column takes what is left of the house
// width, so widening the label column narrows the values and nothing
// silently runs past the rule.
#define WIKI_INDENT      2
#define WIKI_LABEL_COLS  20
#define WIKI_VALUE_COLS  (DISPLAY_COLS - WIKI_INDENT - WIKI_LABEL_COLS - 1)

// -l list geometry: id, label, then an elastic description.
#define WIKI_LIST_QID    10
#define WIKI_LIST_LABEL  26
#define WIKI_LIST_LINKS  5
#define WIKI_LIST_DESC   (DISPLAY_COLS - WIKI_INDENT - WIKI_LIST_QID \
                          - WIKI_LIST_LABEL - WIKI_LIST_LINKS - 3)

typedef enum
{
  WIKI_FORM_CARD,     // the subject alone
  WIKI_FORM_FACT,     // subject and property, both already known
  WIKI_FORM_SPLIT,    // subject and property still to be told apart
  WIKI_FORM_MENU,     // trailing "?" — what can be asked about this
  WIKI_FORM_REVERSE,  // property=value — who holds this statement
  WIKI_FORM_LIST      // -l — the candidate window, with ids
} wiki_form_t;

// One candidate reading of the line: a trailing run of tokens as the
// property, and the stem it leaves as the subject.
typedef struct
{
  char stem[WIKI_SUBJECT_SZ];
  char word[WIKI_WORD_SZ];
  bool viable;        // the word names a property; whether it has a
                      // value on the stem is a separate question
} wiki_split_t;

// Per-request heap closure. Deep-copies the command context, because
// every answer here arrives after the callback has returned — and the
// copied method_msg_t is what carries reply_route, without which a
// worker-thread reply reaches nobody.
typedef struct wiki_req
{
  cmd_ctx_t    ctx;
  method_msg_t msg;

  wiki_form_t  form;
  bool         verbose;

  // "age" is the one question Wikidata cannot be asked: nothing stores
  // it. It rides beside the form rather than being one, because a
  // split has to be able to give up on it like any other reading.
  bool         age;
  bool         dense;    // the answer is going to a model, not a person
  uint8_t      limit;    // -n, 0 = the form's own default

  char subject[WIKI_SUBJECT_SZ];
  char property[WIKI_WORD_SZ];
  char qid[WM_QID_SZ];   // pinned by the user, or filled by resolution

  wiki_split_t split[WIKI_SPLIT_MAX];
  uint8_t      n_split;
  uint8_t      at;

  wm_candidate_t subj;                 // the winner
  wm_candidate_t alt[WIKI_ALTS_MAX];   // the window, winner first
  uint8_t        n_alt;

  char lead[WIKI_LINE_SZ];  // -v: the article's first sentences

  // The birth date, carried across the second leg that asks whether
  // there is a death date to count to.
  wm_time_t born;

  struct wiki_req *next_active;
} wiki_req_t;

// wiki_render.c — all presentation, and the only place that knows what
// a channel looks like. Each takes the already-copied context because
// it runs on a curl worker, long after the dispatch returned.
void wiki_render_card(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_facts_res_t *f);
void wiki_render_fact(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_claims_res_t *c);
// What can be asked about this thing. Drawn from what the item
// actually HOLDS rather than from its class's P1963 menu: the menu is
// what a thing of that type MAY have, and Wikidata publishes one for
// broad classes only — Paris is a "big city" and has none. A word the
// menu offers and the item lacks answers nothing anyway.
void wiki_render_menu(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_facts_res_t *f);

// `death` is what the second leg learned: WM_OK and `died` is a real
// date, WM_NOT_FOUND and the subject is alive, anything else and we do
// not know — which is a THIRD answer and must not collapse into the
// second. Counting to today because a request was rate-limited is how
// you tell a channel that Einstein is 147.
void wiki_render_age(const cmd_ctx_t *ctx, const wiki_req_t *r,
    wm_status_t death, const wm_value_t *died);
void wiki_render_list(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_resolve_res_t *res);
void wiki_render_matches(const cmd_ctx_t *ctx, const wiki_req_t *r,
    const wm_resolve_res_t *res);

// A value as a reader would say it: a date at its declared precision, a
// recorded absence as words rather than as an empty cell.
void wiki_value_text(const wm_value_t *v, char *out, size_t cap);

// What went wrong, in one line addressed to whoever asked. `what` names
// the thing that was being looked for.
void wiki_render_status(const cmd_ctx_t *ctx, wm_status_t status,
    const char *message, const char *what);

#ifdef WIKI_CMD_UNIT
static void wiki_cmd(const cmd_ctx_t *ctx);
static bool wiki_init(void);
static bool wiki_stop(void);
static void wiki_deinit(void);
#endif

#endif // WIKI_INTERNAL

#endif // BM_WIKI_H
