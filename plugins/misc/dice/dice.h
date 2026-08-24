#ifndef BM_DICE_H
#define BM_DICE_H

#ifdef DICE_INTERNAL

#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "common.h"
#include "kv.h"
#include "plugin.h"

#define DICE_CTX  "dice"

// The fixed half of the persona prompt. dice_render already prints the
// sides and the total, so the roll needs no facts alongside it.
#define DICE_FRAMING  "Announce the result, in your own voice."

// The die sizes this command rolls, and how many at once. Sides is an
// allowlist rather than a range: 4/6/8/12/20 are the dice people own,
// and a d7 is a different toy.
#define DICE_SIDES_DEFAULT  20
#define DICE_COUNT_DEFAULT  1
#define DICE_COUNT_MIN      1
#define DICE_COUNT_MAX      5

// A roll the arguments have already been checked against, and the only
// type that reaches the roller. Built by dice_parse and nowhere else:
// both fields are within their stated bounds by construction, so no
// site downstream re-checks them.
typedef struct
{
  uint8_t  sides;
  uint8_t  count;
} dice_spec_t;

// Every die's face, in roll order, plus what they came to. `all_max` /
// `all_min` are the two bands worth a line of their own, and they are
// mutually exclusive: the smallest die here has four faces.
typedef struct
{
  uint8_t  face[DICE_COUNT_MAX];
  uint8_t  sides;
  uint8_t  count;
  uint16_t total;
  bool     all_max;
  bool     all_min;
} dice_roll_t;

static bool  dice_arg_uint(const char *, unsigned long *);
static bool  dice_parse(const cmd_args_t *, dice_spec_t *);
static void  dice_roll(const dice_spec_t *, dice_roll_t *);
static void  dice_render(const dice_roll_t *, char *, size_t);
static void  dice_cmd(const cmd_ctx_t *);
static bool  dice_init(void);
static void  dice_deinit(void);

#endif // DICE_INTERNAL

#endif // BM_DICE_H
