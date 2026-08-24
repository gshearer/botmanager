// botmanager — MIT
// LLM registry admin commands: /llm (add/del service|model, service
// refresh, probe, test) and /show llm [models|service].

#include "llm_priv.h"

#include "cmd.h"
#include "colors.h"
#include "db.h"
#include "display.h"
#include "json.h"
#include "method.h"
#include "userns.h"
#include "util.h"

#include <json-c/json.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Commands: /llm *, /show llm

// The canonical wire id for a model, given whatever an operator typed or a
// provider listed.
//
// Google namespaces its /models listing — "models/gemini-3.5-flash" — but
// the id itself is the bare tail. Both forms reach chat completions, so the
// prefix looks harmless right up until someone registers an image model
// with it: /images/generations accepts ONLY the bare form and answers the
// other with a 404 that blames the model for not existing. Worse, the
// listing is what the registration warning checks against, so the prefixed
// id — the broken one — is the form that registers quietly.
//
// Canonicalising on the way in, into the cache and into a registration,
// means every kind gets an id that works, `show llm models` reads
// uniformly, and the cached listing is something an operator can paste
// straight into `llm add model`.
//
// The test is the exact literal prefix, so a provider whose ids genuinely
// contain a slash (vLLM's "nvidia/Gemma-4-31B-IT-NVFP4") is untouched.
static const char *
llm_model_id_canon(const char *model_id)
{
  static const char prefix[] = "models/";

  if(model_id != NULL && strncmp(model_id, prefix, sizeof(prefix) - 1) == 0)
    return(model_id + sizeof(prefix) - 1);

  return(model_id);
}

// -----------------------------------------------------------------------
// The model table
//
// One grid, two layouts. `show llm models` is a survey across every kind
// and leads with `kind`; `show llm models chat` drops that column — the
// command already said it — and spends what it saved on `ctx`. The two
// layouts cost exactly the same, which is why one elastic budget serves
// both.
//
// The rule behind every column here: a column whose value is identical on
// every row carries no information, so it belongs in the header or in the
// command. It is also why the per-service settings — reasoning_effort,
// priority — are not in this table. They would repeat down the page; they
// live on llm_service_line instead.
// -----------------------------------------------------------------------

#define LLM_LIST_MAX_ROWS  64
#define LLM_EFFORTS_SZ     128

// Column widths, measured against the live registry. The lead gutter is
// two glyph slots — the default star and the disabled cross — plus the
// space that separates it from the first cell.
#define LLM_TBL_LEAD    3
#define LLM_TBL_SVC    13     // "hiigara-image"
#define LLM_TBL_NAME    9     // "gpt56luna"
#define LLM_TBL_MID     6     // `kind` in the survey, `ctx`/`dim` when filtered
#define LLM_TBL_REQ     5
#define LLM_TBL_ERR     4
#define LLM_TBL_AVG     6     // "123.4s"
#define LLM_TBL_SEP     2

// The model id is elastic and takes whatever is left of DISPLAY_COLS —
// thirty-one columns when every cell before it fits its own, comfortably
// past the twenty-six of "RadixArk/Qwen3.8-27B-NVFP4".

typedef struct
{
  char       name[LLM_MODEL_NAME_SZ];
  char       service[LLM_MODEL_NAME_SZ];
  char       model_id[LLM_MODEL_ID_SZ];
  char       efforts[LLM_EFFORTS_SZ];   // the declared set; "" is UNDECLARED
  llm_kind_t kind;
  uint32_t   embed_dim;
  uint32_t   max_context;
  bool       enabled;
  bool       is_default;                // an engine default names this model
  bool       thinking_only;             // a declared set that refuses "none"
  uint64_t   requests;
  uint64_t   errors;
  uint64_t   ok_latency_ms;
} llm_list_row_t;

typedef struct
{
  bool           filtered;
  llm_kind_t     want;
  uint32_t       count;                 // matched, including rows not stored
  uint32_t       n_rows;
  llm_list_row_t rows[LLM_LIST_MAX_ROWS];
} llm_list_state_t;

// Snapshot only. llm_model_iterate runs this under the models rwlock, and
// everything else a row needs — the KV declarations, the per-model
// counters — takes a lock of its own, so those wait for llm_list_annotate
// below. One walk, never two: a second one retakes the rwlock and can see
// a different set of models.
static void
llm_list_iter_cb(const char *name, llm_kind_t kind,
    const char *service_name, const char *model_id, uint32_t embed_dim,
    uint32_t max_context, float default_temp, bool enabled, void *user)
{
  llm_list_state_t *st = user;
  llm_list_row_t   *row;
  (void)default_temp;

  if(st->filtered && kind != st->want)
    return;

  st->count++;

  if(st->n_rows >= LLM_LIST_MAX_ROWS)
    return;

  row = &st->rows[st->n_rows++];
  memset(row, 0, sizeof(*row));

  strlcpy(row->name,     name,         sizeof row->name);
  strlcpy(row->service,  service_name, sizeof row->service);
  strlcpy(row->model_id, model_id,     sizeof row->model_id);

  row->kind        = kind;
  row->embed_dim   = embed_dim;
  row->max_context = max_context;
  row->enabled     = enabled;
}

static void
llm_list_annotate(llm_list_state_t *st)
{
  // Interned, so both pointers outlive the loop (include/kv.h).
  const char *def_chat  = kv_get_str("llm.default_chat_model");
  const char *def_embed = kv_get_str("llm.default_embed_model");

  for(uint32_t i = 0; i < st->n_rows; i++)
  {
    llm_list_row_t   *row = &st->rows[i];
    llm_model_stats_t stats;
    char              key[LLM_KV_KEY_SZ];
    const char       *csv;

    // ⛔ Only the engine's own two knobs may star a row. plugin.imagine
    // .default looks like the missing third and is not ours: it belongs to
    // a plugin above this one (PLUGIN.md §Layer Rules), and `show imagine`
    // stars it from inside that plugin, where it belongs.
    if(row->kind == LLM_KIND_CHAT)
      row->is_default = def_chat != NULL && strcmp(def_chat, row->name) == 0;

    else if(row->kind == LLM_KIND_EMBED)
      row->is_default = def_embed != NULL && strcmp(def_embed, row->name) == 0;

    if(row->kind == LLM_KIND_CHAT)
    {
      snprintf(key, sizeof(key), "llm.model.%s.efforts", row->name);
      csv = kv_get_str(key);

      if(csv != NULL)
        strlcpy(row->efforts, csv, sizeof row->efforts);

      // An empty set is UNDECLARED and admits everything, so it says
      // nothing about `none` either way. Only a populated set that refuses
      // it means the provider will not take the field omitted — which is
      // the 200-with-empty-content failure the engine warns about after
      // the fact, and this is where a reader sees it coming.
      row->thinking_only = row->efforts[0] != '\0'
          && !llm_effort_set_admits(row->efforts, LLM_EFFORT_NONE);
    }

    if(llm_model_stats(row->name, &stats) == SUCCESS)
    {
      row->requests      = stats.requests;
      row->errors        = stats.errors;
      row->ok_latency_ms = stats.ok_latency_ms;
    }
  }
}

// One fixed-width cell plus the grid's separator. The cell arrives already
// coloured: markers count no columns, so padding sees through them
// (include/display.h).
static void
llm_tbl_cell(char *line, size_t cap, const char *text, int width, bool right)
{
  char cell[256];

  strlcpy(cell, text, sizeof cell);

  if(right)
    display_align_right(cell, sizeof cell, width);
  else
    display_align_left(cell, sizeof cell, width);

  display_cat(line, cap, cell);
  display_cat(line, cap, "  ");
}

// `llm  ·  chat models` on the left, the counter window on the right. The
// window is not decoration: a /plugin reload inference zeroes every number
// under it, and a table that does not say so reads as history.
static void
llm_list_emit_title(const cmd_ctx_t *ctx, const llm_list_state_t *st,
    time_t since)
{
  char      left[128];
  char      when[48];
  char      line[256];
  struct tm tm;
  size_t    pad;

  snprintf(left, sizeof(left), CLR_BOLD "llm" CLR_RESET "  ·  %s models",
      st->filtered ? llm_kind_to_str(st->want) : "registered");

  when[0] = '\0';

  if(since > 0 && localtime_r(&since, &tm) != NULL)
    strftime(when, sizeof(when), "counters since %H:%M", &tm);

  if(when[0] == '\0')
  {
    cmd_reply(ctx, left);
    return;
  }

  pad = DISPLAY_COLS - display_vis_len(left) - strlen(when);

  // Unsigned, so a title wider than the house width comes out enormous
  // rather than negative. One space is the floor either way.
  if(pad == 0 || pad > DISPLAY_COLS)
    pad = 1;

  snprintf(line, sizeof(line), "%s%*s" CLR_GRAY "%s" CLR_RESET,
      left, (int)pad, "", when);
  cmd_reply(ctx, line);
}

static void
llm_list_emit_head(const cmd_ctx_t *ctx, const llm_list_state_t *st)
{
  char line[512];

  // Indent first, emphasis second: cmd_reply_table_head reads the margin
  // off the head with strspn, and a leading colour marker would hide it.
  snprintf(line, sizeof(line), "%*s" CLR_BOLD, LLM_TBL_LEAD, "");

  if(!st->filtered)
    llm_tbl_cell(line, sizeof(line), "kind", LLM_TBL_MID, false);

  llm_tbl_cell(line, sizeof(line), "service", LLM_TBL_SVC,  false);
  llm_tbl_cell(line, sizeof(line), "model",   LLM_TBL_NAME, false);

  if(st->filtered)
    llm_tbl_cell(line, sizeof(line),
        st->want == LLM_KIND_EMBED ? "dim" : "ctx", LLM_TBL_MID, true);

  llm_tbl_cell(line, sizeof(line), "req", LLM_TBL_REQ, true);
  llm_tbl_cell(line, sizeof(line), "err", LLM_TBL_ERR, true);
  llm_tbl_cell(line, sizeof(line), "avg", LLM_TBL_AVG, true);
  display_cat(line, sizeof(line), "model id" CLR_RESET);
  cmd_reply_table_head(ctx, line);
}

// The declared effort set, under its model's row.
//
// At twenty-three columns a set costs more than the three counters together
// and is empty for four of the five kinds, so it is not a column. Truncating
// it into a column is worse than a second line: `low medium high minimal`
// cut to twelve reads `low medium…`, which is nothing a caller can type
// into `!ask -e`.
static void
llm_list_emit_thinking(const cmd_ctx_t *ctx, const llm_list_state_t *st,
    const llm_list_row_t *row)
{
  char set[LLM_EFFORTS_SZ];
  char line[256];
  int  indent = LLM_TBL_LEAD + LLM_TBL_SVC + LLM_TBL_SEP
      + (st->filtered ? 0 : LLM_TBL_MID + LLM_TBL_SEP);

  // Space-separated so it reads as a set rather than a CSV to paste back,
  // and otherwise verbatim: an unrecognised token is the operator's typo,
  // and swallowing it here hides the refusal `!ask -e` will hand back.
  strlcpy(set, row->efforts, sizeof set);

  for(char *p = set; *p != '\0'; p++)
    if(*p == ',')
      *p = ' ';

  snprintf(line, sizeof(line), "%*s" CLR_GRAY "thinking:" CLR_RESET " %s%s",
      indent, "", set,
      row->thinking_only ? "  " CLR_YELLOW "⚠" CLR_RESET : "");
  cmd_reply(ctx, line);
}

static void
llm_list_emit_row(const cmd_ctx_t *ctx, const llm_list_state_t *st,
    const llm_list_row_t *row)
{
  char line[512];
  char cell[256];
  int  left;

  // The lead gutter carries two independent facts: a model an engine
  // default points at, and a model an operator has switched off. Both at
  // once is a broken config, and it shows as both.
  snprintf(line, sizeof(line), "%s%s ",
      row->is_default ? CLR_YELLOW "★" CLR_RESET : " ",
      row->enabled    ? " " : CLR_RED "✗" CLR_RESET);

  if(!st->filtered)
  {
    snprintf(cell, sizeof(cell), CLR_GRAY "%s" CLR_RESET,
        llm_kind_to_str(row->kind));
    llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_MID, false);
  }

  snprintf(cell, sizeof(cell), CLR_CYAN "%s" CLR_RESET, row->service);
  llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_SVC, false);

  snprintf(cell, sizeof(cell), CLR_WHITE "%s" CLR_RESET, row->name);
  llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_NAME, false);

  // An embed model's vector width is the fact worth this column; every
  // other kind wants its context size.
  if(st->filtered)
  {
    snprintf(cell, sizeof(cell), "%u",
        row->kind == LLM_KIND_EMBED ? row->embed_dim : row->max_context);
    llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_MID, true);
  }

  if(row->requests > 0)
    snprintf(cell, sizeof(cell), "%" PRIu64, row->requests);
  else
    strlcpy(cell, CLR_GRAY "—" CLR_RESET, sizeof cell);

  llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_REQ, true);

  if(row->errors > 0)
    snprintf(cell, sizeof(cell), CLR_RED "%" PRIu64 CLR_RESET, row->errors);
  else
    strlcpy(cell, row->requests > 0 ? "0" : CLR_GRAY "—" CLR_RESET,
        sizeof cell);

  llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_ERR, true);

  // The mean excludes failures, so a model that has only ever failed has no
  // mean to report — and that is the divide-by-zero guard as well.
  //
  // Milliseconds below a second: an embed round trip is tens of them, and
  // one decimal of seconds renders the whole embed table as "0.0s".
  if(row->requests > row->errors)
  {
    uint64_t mean = row->ok_latency_ms / (row->requests - row->errors);

    if(mean < 1000)
      snprintf(cell, sizeof(cell), "%" PRIu64 "ms", mean);
    else
      snprintf(cell, sizeof(cell), "%.1fs", (double)mean / 1000.0);
  }

  else
    strlcpy(cell, CLR_GRAY "—" CLR_RESET, sizeof cell);

  llm_tbl_cell(line, sizeof(line), cell, LLM_TBL_AVG, true);

  // Measured rather than assumed: display_align_left lets a cell wider than
  // its column win, by design (include/display.h), so a long model name
  // shifts everything right of it. The id is the column that pays for that.
  // ⚠ One column off the budget for the ellipsis: display_fit reserves its
  // mark out of the BYTE budget, not the column budget, and a last cell has
  // no padding to absorb the extra.
  left = DISPLAY_COLS - (int)display_vis_len(line) - 1;

  display_fit(row->model_id, left > 1 ? left : 1, cell, sizeof(cell), "…");
  display_cat(line, sizeof(line), CLR_GRAY);
  display_cat(line, sizeof(line), cell);
  display_cat(line, sizeof(line), CLR_RESET);
  cmd_reply(ctx, line);

  if(row->efforts[0] != '\0')
    llm_list_emit_thinking(ctx, st, row);
}

// What a ★ actually names. "default" alone was read as "the model I will
// get", which is the one thing it does not mean: llm.c registers both knobs
// and reads neither, and every consumer above resolves its own chain before
// it ever falls back to one.
//
// Only CHAT and EMBED rows can be starred (llm_list_annotate), so a filtered
// view that reaches here is one of those two kinds.
static const char *
llm_list_star_knob(const llm_list_state_t *st)
{
  if(!st->filtered)
    return("the engine default for its kind");

  if(st->want == LLM_KIND_CHAT)
    return("llm.default_chat_model");

  return("llm.default_embed_model");
}

// Join one legend clause onto `line`, with the separator every clause but
// the first carries. `line` must already be a string.
static void
llm_legend_part(char *line, size_t cap, bool *first, const char *text)
{
  if(!*first)
    display_cat(line, cap, " · ");

  display_cat(line, cap, text);
  *first = false;
}

static void
llm_list_emit_legend(const cmd_ctx_t *ctx, const llm_list_state_t *st)
{
  bool starred  = false;
  bool disabled = false;
  bool warn     = false;
  bool undecl   = false;
  bool first    = true;
  char line[256];

  for(uint32_t i = 0; i < st->n_rows; i++)
  {
    const llm_list_row_t *row = &st->rows[i];

    // Separate flags, not one `marks`: a table with a disabled row and no
    // default was captioning a star nothing on it wore.
    starred  = starred  || row->is_default;
    disabled = disabled || !row->enabled;
    warn     = warn     || row->thinking_only;
    undecl   = undecl
        || (row->kind == LLM_KIND_CHAT && row->efforts[0] == '\0');
  }

  if(starred || disabled || warn)
  {
    strlcpy(line, "  " CLR_GRAY, sizeof line);

    if(starred)
      llm_legend_part(line, sizeof(line), &first, "★ default");

    if(disabled)
      llm_legend_part(line, sizeof(line), &first, "✗ disabled");

    if(warn)
      llm_legend_part(line, sizeof(line), &first,
          "⚠ thinking-only: omitting the field returns empty");

    display_cat(line, sizeof(line), CLR_RESET);
    cmd_reply(ctx, line);
  }

  // ⚠ The star names a knob, not an outcome, and the knob is the fact worth
  // printing: !ask never reads either one, chat reaches one only when
  // bot.<n>.chat_model is empty, and !imagine has a third of its own. A
  // reader who stops at the glyph has the right answer for no surface in
  // particular. Its own line — folded into the key above, the three-clause
  // case runs past DISPLAY_COLS.
  if(starred)
  {
    snprintf(line, sizeof(line), "  " CLR_GRAY "★ = %s — a fallback, not a"
        " promise" CLR_RESET, llm_list_star_knob(st));
    cmd_reply(ctx, line);

    if(!st->filtered || st->want == LLM_KIND_CHAT)
      cmd_reply(ctx, "  " CLR_GRAY "what a surface really uses: !show ask"
          " · !show imagine · !show bot <n> model" CLR_RESET);
  }

  // ⚠ Absent means UNDECLARED, not "accepts nothing". Without this line a
  // reader draws the opposite conclusion from a blank.
  if(undecl)
    cmd_reply(ctx, "  " CLR_GRAY "a model with no thinking line has no"
        " declared effort set — not \"none accepted\"" CLR_RESET);
}

static void
llm_render_models(const cmd_ctx_t *ctx, bool filtered, llm_kind_t want)
{
  llm_list_state_t st;
  llm_stats_t      window;
  char             line[256];

  st = (llm_list_state_t){ .filtered = filtered, .want = want };

  llm_model_iterate(llm_list_iter_cb, &st);
  llm_list_annotate(&st);

  llm_get_stats(&window);
  llm_list_emit_title(ctx, &st, window.since);

  if(st.n_rows == 0)
  {
    if(filtered)
      snprintf(line, sizeof(line), "  " CLR_GRAY "(no %s models registered)"
          CLR_RESET, llm_kind_to_str(want));
    else
      strlcpy(line, "  " CLR_GRAY "(none)" CLR_RESET, sizeof line);

    cmd_reply(ctx, line);
    return;
  }

  llm_list_emit_head(ctx, &st);

  for(uint32_t i = 0; i < st.n_rows; i++)
    llm_list_emit_row(ctx, &st, &st.rows[i]);

  // ⛔ Never silently: a cut-off table reads as "that is all of them".
  if(st.count > st.n_rows)
  {
    snprintf(line, sizeof(line), "  " CLR_GRAY "… +%u more" CLR_RESET,
        st.count - st.n_rows);
    cmd_reply(ctx, line);
  }

  llm_list_emit_legend(ctx, &st);
}

static void
cmd_llm_list(const cmd_ctx_t *ctx)
{
  llm_kind_t want     = LLM_KIND_CHAT;
  bool       filtered = false;

  if(ctx->parsed != NULL && ctx->parsed->argc > 0)
  {
    // Same closed vocabulary, refused in the same words as `llm add model`.
    if(llm_kind_from_str(ctx->parsed->argv[0], &want) != SUCCESS)
    {
      cmd_reply(ctx, "error: type must be 'chat', 'embed', 'image', 'stt' or 'tts'");
      return;
    }

    filtered = true;
  }

  llm_render_models(ctx, filtered, want);
}

// -----------------------------------------------------------------------
// Service /models cache refresh
//
// A service's /models list is fetched asynchronously and cached in the
// llm_service_models table so `show llm service <name> models` is a fast
// DB read. Fired on `llm add service`, `llm service <name> refresh`, and
// once per service at startup (llm_services_refresh_all). No auth header
// is attached — a service whose /models needs a key simply caches nothing
// and models are added by hand (the add path warns, never blocks).
// -----------------------------------------------------------------------

typedef struct
{
  char name[LLM_MODEL_NAME_SZ];
} llm_refresh_ctx_t;

// Replace the cached model list for one service from a /models JSON body:
// clear the service's rows, then upsert one per data[] item (id + optional
// max_model_len), and stamp llm_services.refreshed. Returns the number of
// models cached, or -1 on a DB error.
static int
llm_service_models_store(const char *service, struct json_object *root)
{
  struct json_object *data;
  char        *e_svc;
  db_result_t *res;
  char         sql[512];
  int          n;
  int          stored = 0;

  data = json_get_array(root, "data");
  if(data == NULL)
    return(0);

  n     = (int)json_object_array_length(data);
  e_svc = db_escape(service);

  // db_escape returns NULL when the pool has no connection to escape
  // against — the query would fail anyway, and mem_free aborts on NULL.
  if(e_svc == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: database unavailable", service);
    return(-1);
  }

  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_service_models WHERE service_name='%s'", e_svc);
  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, "llm", "refresh %s: cache clear failed: %s",
        service, res->error);
    db_result_free(res);
    mem_free(e_svc);
    return(-1);
  }

  db_result_free(res);

  for(int i = 0; i < n; i++)
  {
    struct json_object *item = json_object_array_get_idx(data, i);
    char    model_id[LLM_MODEL_ID_SZ];
    char   *e_mid;
    int64_t max_len = 0;
    if(item == NULL)
      continue;

    memset(model_id, 0, sizeof(model_id));
    json_get_str(item, "id", model_id, sizeof(model_id));

    if(model_id[0] == '\0')
      continue;

    e_mid = db_escape(llm_model_id_canon(model_id));

    if(e_mid == NULL)
      continue;

    if(json_get_int64(item, "max_model_len", &max_len) && max_len > 0)
      snprintf(sql, sizeof(sql),
          "INSERT INTO llm_service_models (service_name, model_id, "
          "max_model_len) VALUES ('%s', '%s', %" PRId64 ") "
          "ON CONFLICT (service_name, model_id) DO UPDATE "
          "SET max_model_len=EXCLUDED.max_model_len, fetched=NOW()",
          e_svc, e_mid, max_len);
    else
      snprintf(sql, sizeof(sql),
          "INSERT INTO llm_service_models (service_name, model_id) "
          "VALUES ('%s', '%s') "
          "ON CONFLICT (service_name, model_id) DO UPDATE SET fetched=NOW()",
          e_svc, e_mid);

    mem_free(e_mid);

    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok)
      stored++;

    db_result_free(res);
  }

  snprintf(sql, sizeof(sql),
      "UPDATE llm_services SET refreshed=NOW() WHERE name='%s'", e_svc);
  res = db_result_alloc();
  db_query(sql, res);
  db_result_free(res);

  mem_free(e_svc);
  return(stored);
}

// Stamp llm_services.probe_http with the most recent /models probe status
// (0 on transport error) so `show llm service` can surface auth failures
// without the value ever being printed as a secret.
static void
llm_service_set_probe_http(const char *name, long status)
{
  char        *e_name;
  db_result_t *res;
  char         sql[256];

  e_name = db_escape(name);

  if(e_name == NULL)
    return;

  snprintf(sql, sizeof(sql),
      "UPDATE llm_services SET probe_http=%ld WHERE name='%s'",
      status, e_name);
  mem_free(e_name);

  res = db_result_alloc();
  db_query(sql, res);
  db_result_free(res);
}

static void
llm_service_refresh_done_cb(const curl_response_t *resp)
{
  llm_refresh_ctx_t  *rctx = resp->user_data;
  struct json_object *root;
  int                 stored;

  // Record the probe outcome regardless of success so the show surface can
  // report "needs API key" (401/403) or a failed probe (non-200 / 0).
  llm_service_set_probe_http(rctx->name, resp->status);

  if(resp->status != 200 || resp->body == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: http=%ld curl=%d %s",
        rctx->name, resp->status, resp->curl_code,
        resp->error ? resp->error : "");

    // Auth-gated /models: nudge the operator toward the key. Setting it
    // fires llm_apikey_kv_cb, which re-probes automatically.
    if(resp->status == 401 || resp->status == 403)
      clam(CLAM_INFO, "llm",
          "service %s requires an API key: "
          "set kv llm.service.%s.creds.apikey <key>", rctx->name, rctx->name);

    mem_free(rctx);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, "llm:refresh");
  if(root == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: response was not valid JSON",
        rctx->name);
    mem_free(rctx);
    return;
  }

  stored = llm_service_models_store(rctx->name, root);
  json_object_put(root);

  if(stored >= 0)
    clam(CLAM_INFO, "llm", "service %s: cached %d models", rctx->name, stored);

  mem_free(rctx);
}

// Fire an async GET <base_url>/models for one service. The probe runs
// keyless by default (local providers don't gate /models); an Authorization
// header is attached only when llm.service.<name>.creds.apikey holds a value, so a
// key-gated /models succeeds once the operator sets the key. Returns SUCCESS
// if the request was submitted (curl worker owns the heap ctx thereafter).
bool
llm_service_refresh(const char *name)
{
  llm_refresh_ctx_t *rctx;
  curl_request_t    *cr;
  char base[LLM_ENDPOINT_SZ];
  char url[LLM_ENDPOINT_SZ + 16];
  char kvkey[LLM_KV_KEY_SZ];
  const char *apikey;
  if(name == NULL || name[0] == '\0')
    return(FAIL);

  if(llm_service_base_url(name, base, sizeof(base)) != SUCCESS)
    return(FAIL);

  if(llm_build_url(base, "models", url, sizeof(url)) != SUCCESS)
    return(FAIL);

  rctx = mem_alloc("llm", "refresh_ctx", sizeof(*rctx));
  snprintf(rctx->name, sizeof(rctx->name), "%s", name);

  cr = curl_request_create(CURL_METHOD_GET, url,
      llm_service_refresh_done_cb, rctx);
  if(cr == NULL)
  {
    clam(CLAM_WARN, "llm", "refresh %s: request create failed", name);
    mem_free(rctx);
    return(FAIL);
  }

  // Bearer token only when configured — keyless providers probe fine
  // without it, and an empty "Bearer " would break some gateways.
  snprintf(kvkey, sizeof(kvkey), "llm.service.%s.creds.apikey", name);
  apikey = kv_get_creds(kvkey);

  if(apikey != NULL && apikey[0] != '\0')
  {
    char hdr[LLM_KV_KEY_SZ + 512];
    snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", apikey);
    curl_request_add_header(cr, hdr);
  }

  // curl_request_submit uses SUCCESS=false / FAIL=true and releases cr
  // internally on failure — but never frees user_data, so rctx is ours to
  // free here. On success the done callback frees it.
  if(curl_request_submit(cr) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "refresh %s: curl submit failed for %s", name, url);
    mem_free(rctx);
    return(FAIL);
  }

  return(SUCCESS);
}

// KV change hook on llm.service.<name>.creds.apikey. Parses the service
// name out of the key and re-probes /models when the value becomes
// non-empty. Fired outside the KV lock (core/kv.c), so reading kv_get_str
// and submitting curl from here is deadlock-safe.
void
llm_apikey_kv_cb(const char *key, void *data)
{
  static const char pfx[] = "llm.service.";
  const char *tail;
  const char *dot;
  const char *val;
  char        name[LLM_MODEL_NAME_SZ];
  size_t      n;
  (void)data;
  if(key == NULL || strncmp(key, pfx, sizeof(pfx) - 1) != 0)
    return;

  tail = key + (sizeof(pfx) - 1);
  dot  = strstr(tail, ".creds.apikey");

  if(dot == NULL || dot == tail)
    return;

  n = (size_t)(dot - tail);

  if(n >= sizeof(name))
    return;

  memcpy(name, tail, n);
  name[n] = '\0';

  // Empty value = key cleared; nothing to re-probe.
  val = kv_get_str(key);

  if(val == NULL || val[0] == '\0')
    return;

  clam(CLAM_INFO, "llm",
      "api key set for service %s — re-probing /models", name);
  llm_service_refresh(name);
}

// Startup seed: fire one refresh per known service so the /models cache is
// warm. Names are snapshotted under the lock, then curl is submitted after
// the lock is dropped (never hold a lock across a submit).
void
llm_services_refresh_all(void)
{
  char   names[64][LLM_MODEL_NAME_SZ];
  size_t count = 0;

  pthread_rwlock_rdlock(&llm_services_lock);

  for(llm_service_t *s = llm_services_head; s != NULL && count < 64;
      s = s->next)
  {
    snprintf(names[count], LLM_MODEL_NAME_SZ, "%s", s->name);
    count++;
  }

  pthread_rwlock_unlock(&llm_services_lock);

  for(size_t i = 0; i < count; i++)
    llm_service_refresh(names[i]);
}

// -----------------------------------------------------------------------
// embed_dim probe
//
// POST <base_url>/embeddings with a one-byte input and count the returned
// vector. Runs only for embed models, on `llm add model … embed` and on
// demand via `llm probe <name>`. The stored dim cannot be changed after
// it's first set (the pgvector column on conversation_embeddings is typed
// to it): a mismatched probe logs a WARN and is ignored. max_context is
// NOT probed here — it comes from the service /models cache.
// -----------------------------------------------------------------------

typedef struct
{
  char     name[LLM_MODEL_NAME_SZ];
  char     model_id[LLM_MODEL_ID_SZ];
  uint32_t expected_dim;   // 0 = no prior dim, accept whatever comes back
} llm_probe_dim_ctx_t;

// Count the float entries in data[0].embedding from an /embeddings
// response root. Returns 0 if the shape doesn't match expectations.
static size_t
llm_probe_count_embedding(struct json_object *root)
{
  struct json_object *data = json_get_array(root, "data");
  struct json_object *first;
  struct json_object *emb;
  int n;
  if(data == NULL || json_object_array_length(data) <= 0)
    return(0);

  first = json_object_array_get_idx(data, 0);
  if(first == NULL)
    return(0);

  emb = json_get_array(first, "embedding");
  if(emb == NULL)
    return(0);

  n = (int)json_object_array_length(emb);
  return(n > 0 ? (size_t)n : 0);
}

static void
llm_probe_embed_dim_done_cb(const curl_response_t *resp)
{
  llm_probe_dim_ctx_t *pctx = resp->user_data;

  struct json_object *root;
  size_t       dim;
  char        *e_name;
  db_result_t *res;
  char         sql[512];
  if(resp->status != 200 || resp->body == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim http=%ld curl=%d %s",
        pctx->name, resp->status, resp->curl_code,
        resp->error ? resp->error : "");
    mem_free(pctx);
    return;
  }

  root = json_parse_buf(resp->body, resp->body_len, "llm:probe_dim");
  if(root == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed response was not valid JSON", pctx->name);
    mem_free(pctx);
    return;
  }

  dim = llm_probe_count_embedding(root);
  json_object_put(root);

  if(dim == 0 || dim > 65536)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: could not count embedding (got %zu)", pctx->name, dim);
    mem_free(pctx);
    return;
  }

  if(pctx->expected_dim != 0 && (uint32_t)dim != pctx->expected_dim)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: endpoint reports embed_dim=%zu but %u is stored; "
        "refusing to change — use `llm del model` + `llm add model` to switch",
        pctx->name, dim, pctx->expected_dim);
    mem_free(pctx);
    return;
  }

  if(pctx->expected_dim == (uint32_t)dim)
  {
    // Idempotent re-probe — no DB write needed.
    clam(CLAM_INFO, "llm",
        "probe %s: embed_dim=%zu (confirmed)", pctx->name, dim);
    mem_free(pctx);
    return;
  }

  e_name = db_escape(pctx->name);

  if(e_name == NULL)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim not stored (database unavailable)", pctx->name);
    mem_free(pctx);
    return;
  }

  snprintf(sql, sizeof(sql),
      "UPDATE llm_models SET embed_dim=%zu WHERE name='%s'", dim, e_name);
  mem_free(e_name);

  res = db_result_alloc();
  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim update failed: %s", pctx->name, res->error);
    db_result_free(res);
    mem_free(pctx);
    return;
  }

  db_result_free(res);
  llm_models_reload();

  clam(CLAM_INFO, "llm",
      "probe %s: embed_dim=%zu (auto-detected)", pctx->name, dim);

  mem_free(pctx);
}

static bool
llm_probe_embed_dim_submit(const char *name)
{
  llm_probe_dim_ctx_t *pctx;
  char base[LLM_ENDPOINT_SZ];
  char url[LLM_ENDPOINT_SZ + 16];
  char escaped_id[LLM_MODEL_ID_SZ * 2];
  char body[LLM_MODEL_ID_SZ * 2 + 64];
  bool have_model;
  int  body_len;
  if(name == NULL || name[0] == '\0')
    return(false);

  pctx = mem_alloc("llm", "probe_dim_ctx", sizeof(*pctx));

  memset(base, 0, sizeof(base));
  have_model = false;

  pthread_rwlock_rdlock(&llm_models_lock);
  for(llm_model_t *m = llm_models_head; m != NULL; m = m->next)
  {
    if(strcmp(m->name, name) == 0 && m->kind == LLM_KIND_EMBED)
    {
      snprintf(base, sizeof(base), "%s", m->base_url);
      snprintf(pctx->name, sizeof(pctx->name), "%s", m->name);
      snprintf(pctx->model_id, sizeof(pctx->model_id), "%s", m->model_id);
      pctx->expected_dim = m->embed_dim;
      have_model = true;
      break;
    }
  }
  pthread_rwlock_unlock(&llm_models_lock);

  if(!have_model)
  {
    mem_free(pctx);
    return(false);
  }

  if(llm_build_url(base, "embeddings", url, sizeof(url)) != SUCCESS)
  {
    clam(CLAM_WARN, "llm", "probe %s: cannot build embeddings URL", name);
    mem_free(pctx);
    return(false);
  }

  // Build the POST body. Use "x" as the input — shortest payload that
  // still returns a full-dim vector. JSON-escape model_id defensively in
  // case it contains a slash or quote (it usually doesn't).
  json_escape(pctx->model_id, escaped_id, sizeof(escaped_id));

  body_len = snprintf(body, sizeof(body),
      "{\"model\":\"%s\",\"input\":\"x\"}", escaped_id);
  if(body_len <= 0 || (size_t)body_len >= sizeof(body))
  {
    mem_free(pctx);
    return(false);
  }

  if(curl_post(url, "application/json", body, (size_t)body_len,
      llm_probe_embed_dim_done_cb, pctx) != SUCCESS)
  {
    clam(CLAM_WARN, "llm",
        "probe %s: embed_dim curl submit failed for %s", name, url);
    mem_free(pctx);
    return(false);
  }

  return(true);
}

static void
cmd_llm_probe(const cmd_ctx_t *ctx)
{
  const char *name;
  llm_kind_t  k;
  char msg[256];
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm probe <name>");
    return;
  }

  name = ctx->parsed->argv[0];

  if(llm_model_kind(name, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: no such model");
    return;
  }

  if(k != LLM_KIND_EMBED)
  {
    cmd_reply(ctx, "nothing to probe: max_context comes from the service "
        "model cache — run `llm service <service> refresh`");
    return;
  }

  if(!llm_probe_embed_dim_submit(name))
  {
    cmd_reply(ctx, "probe: failed to submit (see CLAM log)");
    return;
  }

  snprintf(msg, sizeof(msg),
      "embed_dim probe submitted for %s — result will appear in CLAM log",
      name);
  cmd_reply(ctx, msg);
}

// -----------------------------------------------------------------------
// Service CRUD
// -----------------------------------------------------------------------

static void
cmd_llm_add_service(const cmd_ctx_t *ctx)
{
  const char  *name;
  const char  *base_url;
  char        *e_name;
  char        *e_url;
  db_result_t *res;
  char         sql[2048];
  char         msg[512];
  if(ctx->parsed == NULL || ctx->parsed->argc < 2)
  {
    cmd_reply(ctx, "usage: llm add service <name> <base_url>");
    return;
  }

  name     = ctx->parsed->argv[0];
  base_url = ctx->parsed->argv[1];

  e_name = db_escape(name);
  e_url  = db_escape(base_url);

  if(e_name == NULL || e_url == NULL)
  {
    if(e_name != NULL)
      mem_free(e_name);

    if(e_url != NULL)
      mem_free(e_url);

    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "INSERT INTO llm_services (name, base_url) VALUES ('%s', '%s')",
      e_name, e_url);
  mem_free(e_name);
  mem_free(e_url);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    snprintf(msg, sizeof(msg), "insert failed: %s", res->error);
    cmd_reply(ctx, msg);
    db_result_free(res);
    return;
  }

  db_result_free(res);

  // Reload registers the llm.service.<name>.creds.apikey KV slot; the refresh
  // seeds the /models cache asynchronously and runs keyless. If the
  // provider gates /models behind auth, the probe records 401/403 and
  // `show llm service` will flag "needs API key" — set it then and the
  // KV hook re-probes automatically.
  llm_services_reload();
  llm_service_refresh(name);

  snprintf(msg, sizeof(msg),
      "ok — probing /models; check `show llm service %s`", name);
  cmd_reply(ctx, msg);
}

static void
cmd_llm_del_service(const cmd_ctx_t *ctx)
{
  const char  *name;
  char        *e_name;
  db_result_t *res;
  char         sql[512];
  long         refs;
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm del service <name>");
    return;
  }

  name   = ctx->parsed->argv[0];
  e_name = db_escape(name);

  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  // Block deletion while any defined model still references the service.
  snprintf(sql, sizeof(sql),
      "SELECT count(*) FROM llm_models WHERE service_name='%s'", e_name);
  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "error: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    mem_free(e_name);
    return;
  }

  refs = (res->rows > 0 && db_result_get(res, 0, 0) != NULL)
      ? strtol(db_result_get(res, 0, 0), NULL, 10) : 0;
  db_result_free(res);

  if(refs > 0)
  {
    char emsg[128];
    snprintf(emsg, sizeof(emsg),
        "error: %ld model(s) still reference this service", refs);
    cmd_reply(ctx, emsg);
    mem_free(e_name);
    return;
  }

  // Cache rows (llm_service_models) cascade on the FK.
  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_services WHERE name='%s'", e_name);
  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "delete failed: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  if(res->rows_affected == 0)
  {
    char emsg[128];
    snprintf(emsg, sizeof(emsg), "no such service: %s", name);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_services_reload();
  cmd_reply(ctx, "ok");
}

static void
cmd_llm_service(const cmd_ctx_t *ctx)
{
  const char *name;
  const char *action;
  if(ctx->parsed == NULL || ctx->parsed->argc < 2)
  {
    cmd_reply(ctx, "usage: llm service <name> refresh");
    return;
  }

  name   = ctx->parsed->argv[0];
  action = ctx->parsed->argv[1];

  if(strcmp(action, "refresh") == 0)
  {
    if(llm_service_refresh(name) != SUCCESS)
    {
      cmd_reply(ctx, "error: no such service, or refresh could not start");
      return;
    }

    cmd_reply(ctx, "ok (refreshing model list — count appears in CLAM log)");
    return;
  }

  cmd_reply(ctx, "usage: llm service <name> refresh");
}

// -----------------------------------------------------------------------
// Model CRUD
// -----------------------------------------------------------------------

static void
cmd_llm_add_model(const cmd_ctx_t *ctx)
{
  const char  *name;
  const char  *service;
  const char  *model_id;
  const char  *kind_s;
  llm_kind_t   k;
  char        *e_name;
  char        *e_svc;
  char        *e_mid;
  db_result_t *res;
  uint32_t     max_ctx;
  bool         in_cache;
  char         sql[2048];
  if(ctx->parsed == NULL || ctx->parsed->argc < 4)
  {
    cmd_reply(ctx,
        "usage: llm add model <chat|embed|image|stt|tts> <name> <service> <model_id>");
    return;
  }

  kind_s   = ctx->parsed->argv[0];
  name     = ctx->parsed->argv[1];
  service  = ctx->parsed->argv[2];
  model_id = llm_model_id_canon(ctx->parsed->argv[3]);

  if(llm_kind_from_str(kind_s, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: type must be 'chat', 'embed', 'image', 'stt' or 'tts'");
    return;
  }

  e_svc = db_escape(service);

  if(e_svc == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  // Service must exist.
  snprintf(sql, sizeof(sql),
      "SELECT 1 FROM llm_services WHERE name='%s'", e_svc);
  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok || res->rows == 0)
  {
    cmd_reply(ctx, "error: no such service (add it with `llm add service`)");
    db_result_free(res);
    mem_free(e_svc);
    return;
  }

  db_result_free(res);

  // Seed max_context from the cached /models entry when present; warn
  // (non-fatal) if the model_id isn't in the service's cache yet.
  e_mid    = db_escape(model_id);
  max_ctx  = llm_cfg.max_context_tokens;
  in_cache = false;

  if(e_mid == NULL)
  {
    mem_free(e_svc);
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT max_model_len FROM llm_service_models "
      "WHERE service_name='%s' AND model_id='%s'", e_svc, e_mid);
  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
  {
    const char *ml = db_result_get(res, 0, 0);
    in_cache = true;

    if(ml != NULL && ml[0] != '\0')
      max_ctx = (uint32_t)strtoul(ml, NULL, 10);
  }

  db_result_free(res);

  // A miss means one of two very different things, and saying "warning"
  // for both taught operators to read a benign one as a rejection. A
  // service's listing is fetched asynchronously when it is added, so
  // registering a model in the first seconds of a new service's life
  // always misses — there was nothing to check against. Only a miss
  // against a listing we actually hold says anything about the id.
  if(!in_cache)
  {
    bool have_listing = false;

    snprintf(sql, sizeof(sql),
        "SELECT 1 FROM llm_service_models WHERE service_name='%s' LIMIT 1",
        e_svc);
    res = db_result_alloc();

    if(db_query(sql, res) == SUCCESS && res->ok && res->rows > 0)
      have_listing = true;

    db_result_free(res);

    if(have_listing)
      cmd_reply(ctx, "warning: this service's /models list does not advertise "
          "that model_id — adding anyway (check the spelling against "
          "`show llm service <name> models`)");
    else
      cmd_reply(ctx, "note: this service has no cached /models list yet, so "
          "the model_id could not be verified — it is fetched in the "
          "background when a service is added (`llm service <name> refresh`)");
  }

  e_name = db_escape(name);

  if(e_name == NULL)
  {
    mem_free(e_svc);
    mem_free(e_mid);
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "INSERT INTO llm_models (name, kind, service_name, model_id, "
      "embed_dim, max_context) VALUES ('%s', '%s', '%s', '%s', 0, %u)",
      e_name, llm_kind_to_str(k), e_svc, e_mid, max_ctx);

  mem_free(e_name);
  mem_free(e_svc);
  mem_free(e_mid);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "insert failed: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();

  // Embed models need their output dimension probed once.
  if(k == LLM_KIND_EMBED)
  {
    if(llm_probe_embed_dim_submit(name))
      cmd_reply(ctx, "ok (probing endpoint for embed_dim)");
    else
      cmd_reply(ctx, "ok (embed_dim probe could not start — see CLAM log)");
  }

  else
    cmd_reply(ctx, "ok");
}

static void
cmd_llm_del_model(const cmd_ctx_t *ctx)
{
  const char  *name;
  char        *e_name;
  db_result_t *res;
  char         sql[256];
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm del model <name>");
    return;
  }

  name   = ctx->parsed->argv[0];
  e_name = db_escape(name);

  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "DELETE FROM llm_models WHERE name='%s'", e_name);
  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) != SUCCESS || !res->ok)
  {
    char emsg[512];
    snprintf(emsg, sizeof(emsg), "delete failed: %s", res->error);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  // A DELETE that matched nothing still "succeeds" — report it instead of
  // a misleading "ok" (e.g. `llm del model chat g4nv` deletes name='chat',
  // not the model, because del takes just <name> unlike add's typed form).
  if(res->rows_affected == 0)
  {
    char emsg[128];
    snprintf(emsg, sizeof(emsg), "no such model: %s", name);
    cmd_reply(ctx, emsg);
    db_result_free(res);
    return;
  }

  db_result_free(res);
  llm_models_reload();
  cmd_reply(ctx, "ok");
}

// /llm test: a bounded synchronous probe.
//
// This is the documented exception to the daemon's non-blocking rule
// (see DESIGN.md): an operator asked "does this model answer?" and wants
// the answer in the same breath. The wait is therefore confined to the
// command thread and bounded — but the request outlives the bound. A
// model slower than its window keeps running, and its callback fires on
// a curl worker after the command has already replied and returned.
//
// The waiter is consequently heap-owned and reference-counted, never a
// stack local: one reference for the command, one for the in-flight
// request, and whoever drops the last one frees it. Abandoning a stack
// waiter would hand the curl worker a dead frame to write 512 bytes
// into — silent corruption that surfaces as a crash somewhere else
// entirely, minutes later.
//
// The windows below are deliberately code, not KV. The wait is served by
// whatever thread dispatched the command, and over botmanctl that thread
// is the control socket's own poll task — cmd_dispatch_as() invokes the
// handler inline — so a generous operator-tunable knob here is an
// invitation to take the control surface offline for minutes while
// diagnosing a slow model. Image generation is inherently slower than a
// chat ping and gets its own window; no other kind needs one, and a
// model slower than its window is not a failure, it is a log entry.
#define LLM_TEST_WAIT_SECS        15
#define LLM_TEST_WAIT_IMAGE_SECS  30

typedef struct
{
  pthread_mutex_t mu;
  pthread_cond_t  cv;
  bool            done;
  bool            ok;
  long            status;
  char            content[512];
  char            err[256];

  uint32_t        refs;        // guarded by mu; 0 = free it
  bool            abandoned;   // command gave up: report to the log instead
  char            model[LLM_MODEL_NAME_SZ];
  struct timespec t0;          // probe start, for the late-result latency
} llm_test_sync_t;

// Two references: the command's own, and the one the request carries as
// user_data. A failed submit means the second never materialises, so the
// caller releases twice.
static llm_test_sync_t *
llm_test_sync_create(const char *model)
{
  llm_test_sync_t *s;

  s = mem_alloc("llm", "test", sizeof(*s));
  memset(s, 0, sizeof(*s));

  pthread_mutex_init(&s->mu, NULL);
  pthread_cond_init(&s->cv, NULL);

  s->refs = 2;
  snprintf(s->model, sizeof(s->model), "%s", model);
  clock_gettime(CLOCK_MONOTONIC, &s->t0);

  return(s);
}

static void
llm_test_sync_release(llm_test_sync_t *s)
{
  uint32_t remaining;

  pthread_mutex_lock(&s->mu);
  remaining = --s->refs;
  pthread_mutex_unlock(&s->mu);

  if(remaining > 0)
    return;

  pthread_mutex_destroy(&s->mu);
  pthread_cond_destroy(&s->cv);
  mem_free(s);
}

// Every kind's callback ends here with the result fields already filled:
// wake a waiting command, or — when the command timed out and left — put
// the result the operator asked for in the log, where it is still worth
// having. Consumes the request's reference, so `s` is unsafe to touch on
// return.
static void
llm_test_sync_complete(llm_test_sync_t *s)
{
  char     line[832];
  bool     abandoned;
  uint64_t ms;

  pthread_mutex_lock(&s->mu);

  s->done   = true;
  abandoned = s->abandoned;
  ms        = util_ms_since(&s->t0);

  // Format under the lock, emit outside it: clam() takes a lock of its
  // own and nothing here needs to hold two at once.
  if(abandoned)
    snprintf(line, sizeof(line), "test %s: late result after %lums, http %ld: %s",
        s->model, (unsigned long)ms, s->status,
        s->ok ? s->content : s->err);

  pthread_cond_broadcast(&s->cv);
  pthread_mutex_unlock(&s->mu);

  if(abandoned)
    clam(CLAM_INFO, "llm", "%s", line);

  llm_test_sync_release(s);
}

static void
cmd_llm_test_done(const llm_chat_response_t *resp)
{
  llm_test_sync_t *s = resp->user_data;

  pthread_mutex_lock(&s->mu);

  s->ok     = resp->ok;
  s->status = resp->http_status;

  if(resp->content != NULL)
  {
    size_t n = resp->content_len < sizeof(s->content) - 1
        ? resp->content_len : sizeof(s->content) - 1;
    memcpy(s->content, resp->content, n);
    s->content[n] = '\0';
  }

  if(resp->error != NULL)
    snprintf(s->err, sizeof(s->err), "%s", resp->error);

  pthread_mutex_unlock(&s->mu);

  llm_test_sync_complete(s);
}

static void
cmd_llm_test_embed_done(const llm_embed_response_t *resp)
{
  llm_test_sync_t *s = resp->user_data;

  pthread_mutex_lock(&s->mu);

  s->ok     = resp->ok;
  s->status = resp->http_status;

  if(resp->ok)
    snprintf(s->content, sizeof(s->content),
        "dim=%u vectors=%zu", resp->dim, resp->n_vectors);

  if(resp->error != NULL)
    snprintf(s->err, sizeof(s->err), "%s", resp->error);

  pthread_mutex_unlock(&s->mu);

  llm_test_sync_complete(s);
}

static void
cmd_llm_test_image_done(const llm_image_response_t *resp)
{
  llm_test_sync_t *s = resp->user_data;

  pthread_mutex_lock(&s->mu);

  s->ok     = resp->ok;
  s->status = resp->http_status;

  if(resp->ok)
    snprintf(s->content, sizeof(s->content),
        "%s b64_len=%zu", resp->mime, resp->b64_len);

  if(resp->error != NULL)
    snprintf(s->err, sizeof(s->err), "%s", resp->error);

  pthread_mutex_unlock(&s->mu);

  llm_test_sync_complete(s);
}

static void
cmd_llm_test(const cmd_ctx_t *ctx)
{
  llm_test_sync_t *s;
  const char      *name;
  const char      *prompt;
  struct timespec  until;
  char             line[832];
  uint64_t         ms;
  uint32_t         wait_secs;
  llm_kind_t       k;
  bool             submitted;
  bool             done;

  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, "usage: llm test <name> [prompt...]");
    return;
  }

  name   = ctx->parsed->argv[0];
  prompt = ctx->parsed->argc > 1 ? ctx->parsed->argv[1] : "ping";

  if(llm_model_kind(name, &k) != SUCCESS)
  {
    cmd_reply(ctx, "error: unknown model");
    return;
  }

  // The speech kinds are deliberately not testable from here: one wants
  // a WAV this command has no way to hold and the other answers with
  // one. Say so plainly rather than fall through to the embed arm and
  // fail against its kind guard with a baffling message.
  if(k != LLM_KIND_CHAT && k != LLM_KIND_IMAGE && k != LLM_KIND_EMBED)
  {
    cmd_reply(ctx, "error: `llm test` does not cover the speech kinds — "
        "try `/bot <name> say <text>` for a tts model, and speak to the robot "
        "for an stt one");
    return;
  }

  s = llm_test_sync_create(name);

  if(k == LLM_KIND_CHAT)
  {
    llm_chat_params_t params = { 0 };
    llm_message_t     msgs[1];

    memset(msgs, 0, sizeof(msgs));
    // Wide enough that a thinking model can reason and still answer. At
    // 32 the reasoning pass consumed the whole budget and the probe came
    // back "no content in response" — a healthy model reported as broken,
    // which is the opposite of what a diagnostic is for. The extra tokens
    // cost nothing next to being lied to.
    params.max_tokens = 512;

    msgs[0].role    = LLM_ROLE_USER;
    msgs[0].content = prompt;

    submitted = (llm_chat_submit(name, &params, msgs, 1,
        cmd_llm_test_done, NULL, s) == SUCCESS);
  }

  else if(k == LLM_KIND_IMAGE)
  {
    llm_image_params_t params = { 0 };

    params.n = 1;

    submitted = (llm_image_submit(name, &params, prompt,
        cmd_llm_test_image_done, s) == SUCCESS);
  }

  else
  {
    const char *inputs[1] = { prompt };

    submitted = (llm_embed_submit(name, inputs, 1,
        cmd_llm_test_embed_done, s) == SUCCESS);
  }

  if(!submitted)
  {
    cmd_reply(ctx, "error: submit failed");

    // No callback will ever fire, so the request's reference is ours to
    // drop as well as our own.
    llm_test_sync_release(s);
    llm_test_sync_release(s);
    return;
  }

  wait_secs = (k == LLM_KIND_IMAGE)
      ? LLM_TEST_WAIT_IMAGE_SECS : LLM_TEST_WAIT_SECS;

  clock_gettime(CLOCK_REALTIME, &until);
  until.tv_sec += wait_secs;

  pthread_mutex_lock(&s->mu);

  while(!s->done)
    if(pthread_cond_timedwait(&s->cv, &s->mu, &until) != 0)
      break;

  done = s->done;
  ms   = util_ms_since(&s->t0);

  // Claim the result while still holding the lock the callback needs, so
  // there is no window in which both sides believe they own the reply.
  if(done)
    snprintf(line, sizeof(line), "%s (%lums, http %ld): %s",
        s->ok ? "ok" : "failed", (unsigned long)ms, s->status,
        s->ok ? s->content : s->err);
  else
    s->abandoned = true;

  pthread_mutex_unlock(&s->mu);

  if(!done)
    snprintf(line, sizeof(line),
        "still running after %us — this model is slower than the probe "
        "window; the result will be logged when it lands", wait_secs);

  cmd_reply(ctx, line);
  llm_test_sync_release(s);
}

// -----------------------------------------------------------------------
// /show llm [models|service]
// -----------------------------------------------------------------------

static void
llm_show_iter_cb(const char *model_name, llm_kind_t kind, bool streaming,
    uint32_t elapsed_secs, void *data)
{
  const cmd_ctx_t *ctx = data;
  char line[256];

  snprintf(line, sizeof(line),
      "    " CLR_WHITE "%-*s" CLR_RESET " " CLR_GRAY "%-*s" CLR_RESET
      "  %s  elapsed %us",
      LLM_TBL_NAME, model_name,
      LLM_TBL_MID, llm_kind_to_str(kind),
      streaming ? "streaming" : "buffered",
      elapsed_secs);

  cmd_reply(ctx, line);
}

// Both registries are short lists read under their own rwlock, so these are
// snapshots rather than promises — which is all the header line claims.
static uint32_t
llm_service_count(void)
{
  uint32_t n = 0;

  pthread_rwlock_rdlock(&llm_services_lock);

  for(const llm_service_t *s = llm_services_head; s != NULL; s = s->next)
    n++;

  pthread_rwlock_unlock(&llm_services_lock);

  return(n);
}

static uint32_t
llm_model_count(void)
{
  uint32_t n = 0;

  pthread_rwlock_rdlock(&llm_models_lock);

  for(const llm_model_t *m = llm_models_head; m != NULL; m = m->next)
    n++;

  pthread_rwlock_unlock(&llm_models_lock);

  return(n);
}

static void
cmd_show_llm(const cmd_ctx_t *ctx)
{
  llm_stats_t s;
  char        buf[512];
  char        when[48];
  struct tm   tm;
  uint64_t    avg_ms;

  llm_get_stats(&s);

  avg_ms = s.total_requests > 0
      ? s.total_latency_ms / s.total_requests : 0;

  when[0] = '\0';

  if(s.since > 0 && localtime_r(&s.since, &tm) != NULL)
    strftime(when, sizeof(when), "   since %H:%M", &tm);

  snprintf(buf, sizeof(buf),
      CLR_BOLD "llm" CLR_RESET "  ·  %u services, %u models, %u in flight",
      llm_service_count(), llm_model_count(), s.active);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  traffic : %" PRIu64 " requests · %" PRIu64 " errors · %" PRIu64
      " retries · avg %" PRIu64 " ms" CLR_GRAY "%s" CLR_RESET,
      s.total_requests, s.total_errors, s.total_retries, avg_ms, when);
  cmd_reply(ctx, buf);

  snprintf(buf, sizeof(buf),
      "  tokens  : %" PRIu64 " prompt · %" PRIu64 " completion",
      s.total_prompt_tokens, s.total_completion_tokens);
  cmd_reply(ctx, buf);

  llm_render_models(ctx, false, LLM_KIND_CHAT);

  if(s.active > 0)
  {
    cmd_reply(ctx, CLR_BOLD "  in flight" CLR_RESET);
    llm_iterate_active(llm_show_iter_cb, (void *)ctx);
  }
}

// Render one colorized service summary line. The API token is checked for
// presence only — its value is NEVER printed.
static void
llm_service_line(const cmd_ctx_t *ctx, const char *name, const char *base,
    const char *refreshed, const char *cached, const char *defined,
    const char *probe_http)
{
  char        key[LLM_KV_KEY_SZ];
  const char *token;
  const char *effort;
  bool        key_set;
  long        probe;
  char        note[96];
  char        eff[64];
  char        line[768];
  if(name == NULL)
    return;

  snprintf(key, sizeof(key), "llm.service.%s.creds.apikey", name);
  token   = kv_get_str(key);
  key_set = token != NULL && token[0] != '\0';

  // reasoning_effort is per SERVICE, so it belongs here and not in the
  // model table, where it would repeat down the page. This line is
  // free-form and deliberately not on DISPLAY_COLS; one short token is
  // what it can afford.
  snprintf(key, sizeof(key), "llm.service.%s.reasoning_effort", name);
  effort = kv_get_str(key);
  eff[0] = '\0';

  if(effort != NULL && effort[0] != '\0')
    snprintf(eff, sizeof(eff), "  effort=%s", effort);

  // Translate the last /models probe status into an at-a-glance note.
  // -1 = never probed; 0 = transport failure; 401/403 = auth wall.
  probe   = (probe_http && probe_http[0]) ? strtol(probe_http, NULL, 10) : -1;
  note[0] = '\0';

  if(probe == 401 || probe == 403)
    snprintf(note, sizeof(note), "  " CLR_RED "needs API key" CLR_RESET);
  else if(probe == 0)
    snprintf(note, sizeof(note), "  " CLR_RED "probe failed" CLR_RESET);
  else if(probe > 0 && probe != 200)
    snprintf(note, sizeof(note),
        "  " CLR_RED "probe http=%ld" CLR_RESET, probe);

  snprintf(line, sizeof(line),
      CLR_BOLD "%s" CLR_RESET "  " CLR_GRAY "%s" CLR_RESET
      "  " CLR_CYAN "models=%s" CLR_RESET "  defined=%s  key=%s%s" CLR_RESET
      "%s  " CLR_GRAY "refreshed=%s" CLR_RESET "%s",
      name,
      base ? base : "",
      cached ? cached : "0",
      defined ? defined : "0",
      key_set ? CLR_GREEN : CLR_RED,
      key_set ? "set" : "unset",
      eff,
      (refreshed && refreshed[0]) ? refreshed : "never",
      note);

  cmd_reply(ctx, line);
}

static void
cmd_show_llm_service_models(const cmd_ctx_t *ctx, const char *name)
{
  char        *e_name;
  db_result_t *res;
  char         sql[512];
  uint32_t     shown = 0;

  e_name = db_escape(name);

  if(e_name == NULL)
  {
    cmd_reply(ctx, "error: database unavailable");
    return;
  }

  snprintf(sql, sizeof(sql),
      "SELECT model_id, max_model_len FROM llm_service_models "
      "WHERE service_name='%s' ORDER BY model_id", e_name);
  mem_free(e_name);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    char line[256];

    for(uint32_t r = 0; r < res->rows; r++)
    {
      const char *mid = db_result_get(res, r, 0);
      const char *ml  = db_result_get(res, r, 1);

      if(ml != NULL && ml[0] != '\0')
        snprintf(line, sizeof(line),
            "  %s  " CLR_GRAY "ctx=%s" CLR_RESET, mid ? mid : "?", ml);
      else
        snprintf(line, sizeof(line), "  %s", mid ? mid : "?");

      cmd_reply(ctx, line);
      shown++;
    }
  }

  db_result_free(res);

  if(shown == 0)
  {
    char msg[256];
    snprintf(msg, sizeof(msg),
        "  (none — run `llm service %s refresh`)", name);
    cmd_reply(ctx, msg);
  }
}

// Shared SELECT for the list (0-arg) and detail (<name>) surfaces; the
// WHERE clause is appended by the caller (empty for the full list).
static void
cmd_show_llm_service_summary(const cmd_ctx_t *ctx, const char *where_name)
{
  db_result_t *res;
  char         sql[1024];
  char         where[128];
  uint32_t     shown = 0;

  where[0] = '\0';

  if(where_name != NULL)
  {
    char *e_name = db_escape(where_name);

    // An unescapable filter must not widen into "list everything".
    if(e_name == NULL)
    {
      cmd_reply(ctx, "error: database unavailable");
      return;
    }

    snprintf(where, sizeof(where), " WHERE s.name='%s'", e_name);
    mem_free(e_name);
  }

  snprintf(sql, sizeof(sql),
      "SELECT s.name, s.base_url, s.refreshed, "
      "(SELECT count(*) FROM llm_service_models c "
      "WHERE c.service_name=s.name), "
      "(SELECT count(*) FROM llm_models m WHERE m.service_name=s.name), "
      "s.probe_http "
      "FROM llm_services s%s ORDER BY s.name", where);

  res = db_result_alloc();

  if(db_query(sql, res) == SUCCESS && res->ok)
  {
    for(uint32_t r = 0; r < res->rows; r++)
    {
      llm_service_line(ctx,
          db_result_get(res, r, 0), db_result_get(res, r, 1),
          db_result_get(res, r, 2), db_result_get(res, r, 3),
          db_result_get(res, r, 4), db_result_get(res, r, 5));
      shown++;
    }
  }

  db_result_free(res);

  if(shown == 0)
  {
    if(where_name != NULL)
      cmd_reply(ctx, "error: no such service");
    else
      cmd_reply(ctx,
          "  (none — add one with `llm add service <name> <base_url>`)");
  }
}

static void
cmd_show_llm_service(const cmd_ctx_t *ctx)
{
  const char *name;

  // 0 args → colorized list of all services.
  if(ctx->parsed == NULL || ctx->parsed->argc < 1)
  {
    cmd_reply(ctx, CLR_BOLD "llm services" CLR_RESET);
    cmd_show_llm_service_summary(ctx, NULL);
    return;
  }

  name = ctx->parsed->argv[0];

  // `<name> models` → cached /models list.
  if(ctx->parsed->argc >= 2 && strcmp(ctx->parsed->argv[1], "models") == 0)
  {
    cmd_show_llm_service_models(ctx, name);
    return;
  }

  // `<name>` → one-service detail.
  cmd_show_llm_service_summary(ctx, name);
}

// -----------------------------------------------------------------------
// Registration
// -----------------------------------------------------------------------

static const cmd_arg_desc_t ad_add_service[] = {
  { "name",     CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "base_url", CMD_ARG_NONE, CMD_ARG_REQUIRED, 0,                     NULL },
};

static const cmd_arg_desc_t ad_add_model[] = {
  { "type",     CMD_ARG_NONE, CMD_ARG_REQUIRED, 16,                    NULL },
  { "name",     CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "service",  CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "model_id", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_ID_SZ - 1,   NULL },
};

static const cmd_arg_desc_t ad_del_one[] = {
  { "name", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_llm_service[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
  { "action", CMD_ARG_NONE, CMD_ARG_REQUIRED, 16,                    NULL },
};

static const cmd_arg_desc_t ad_llm_probe[] = {
  { "name", CMD_ARG_NONE, CMD_ARG_REQUIRED, LLM_MODEL_NAME_SZ - 1, NULL },
};

static const cmd_arg_desc_t ad_llm_test[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_REQUIRED,                LLM_MODEL_NAME_SZ - 1, NULL },
  { "prompt", CMD_ARG_NONE, CMD_ARG_OPTIONAL | CMD_ARG_REST, 0,                     NULL },
};

static const cmd_arg_desc_t ad_show_models[] = {
  { "type", CMD_ARG_NONE, CMD_ARG_OPTIONAL, 16, NULL },
};

static const cmd_arg_desc_t ad_show_service[] = {
  { "name",   CMD_ARG_NONE, CMD_ARG_OPTIONAL, LLM_MODEL_NAME_SZ - 1, NULL },
  { "action", CMD_ARG_NONE, CMD_ARG_OPTIONAL, 16,                    NULL },
};

static void
cmd_llm_root(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: /llm <add|del|service|probe|test> ...");
}

static void
cmd_llm_add_usage(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: llm add service <name> <base_url>");
  cmd_reply(ctx, "       llm add model <chat|embed|image|stt|tts> <name> <service> <model_id>");
}

static void
cmd_llm_del_usage(const cmd_ctx_t *ctx)
{
  cmd_reply(ctx, "usage: llm del service <name>  |  llm del model <name>");
}

static const cmd_decl_t llm_decl = {
  .module      = "llm",
  .name        = "llm",
  .usage       = "llm",
  .description = "LLM model registry",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_root,
};

static const cmd_decl_t llm_add_decl = {
  .module      = "llm",
  .name        = "add",
  .usage       = "llm add <service|model> ...",
  .description = "Create an LLM service or model",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_add_usage,
  .parent_path = "llm",
  .abbrev      = "a",
};

static const cmd_decl_t llm_add_service_decl = {
  .module      = "llm",
  .name        = "service",
  .usage       = "llm add service <name> <base_url>",
  .description = "Register an OpenAI-compatible provider (base URL)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_add_service,
  .parent_path = "llm/add",
  .abbrev      = "s",
  .arg_desc    = ad_add_service,
  .arg_count   = (uint8_t)(sizeof(ad_add_service) / sizeof(ad_add_service[0])),
};

static const cmd_decl_t llm_add_model_decl = {
  .module      = "llm",
  .name        = "model",
  .usage       = "llm add model <chat|embed|image|stt|tts> <name> <service>"
                 " <model_id>",
  .description = "Register a model against a service",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_add_model,
  .parent_path = "llm/add",
  .abbrev      = "m",
  .arg_desc    = ad_add_model,
  .arg_count   = (uint8_t)(sizeof(ad_add_model) / sizeof(ad_add_model[0])),
};

static const cmd_decl_t llm_del_decl = {
  .module      = "llm",
  .name        = "del",
  .usage       = "llm del <service|model> <name>",
  .description = "Delete an LLM service or model",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_del_usage,
  .parent_path = "llm",
  .abbrev      = "d",
};

static const cmd_decl_t llm_del_service_decl = {
  .module      = "llm",
  .name        = "service",
  .usage       = "llm del service <name>",
  .description = "Delete a service (blocked while models reference it)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_del_service,
  .parent_path = "llm/del",
  .abbrev      = "s",
  .arg_desc    = ad_del_one,
  .arg_count   = 1,
};

static const cmd_decl_t llm_del_model_decl = {
  .module      = "llm",
  .name        = "model",
  .usage       = "llm del model <name>",
  .description = "Delete a defined model",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_del_model,
  .parent_path = "llm/del",
  .abbrev      = "m",
  .arg_desc    = ad_del_one,
  .arg_count   = 1,
};

static const cmd_decl_t llm_service_decl = {
  .module      = "llm",
  .name        = "service",
  .usage       = "llm service <name> refresh",
  .description = "Refresh a service's cached /models list",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_service,
  .parent_path = "llm",
  .abbrev      = "sv",
  .arg_desc    = ad_llm_service,
  .arg_count   = (uint8_t)(sizeof(ad_llm_service) / sizeof(ad_llm_service[0])),
};

static const cmd_decl_t llm_probe_decl = {
  .module      = "llm",
  .name        = "probe",
  .usage       = "llm probe <name>",
  .description = "Re-probe an embed model's output dimension",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_probe,
  .parent_path = "llm",
  .abbrev      = "pb",
  .arg_desc    = ad_llm_probe,
  .arg_count   = (uint8_t)(sizeof(ad_llm_probe) / sizeof(ad_llm_probe[0])),
};

static const cmd_decl_t llm_test_decl = {
  .module      = "llm",
  .name        = "test",
  .usage       = "llm test <name> [prompt]",
  .description = "Probe an LLM model synchronously",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_test,
  .parent_path = "llm",
  .abbrev      = "t",
  .arg_desc    = ad_llm_test,
  .arg_count   = (uint8_t)(sizeof(ad_llm_test) / sizeof(ad_llm_test[0])),
};

static const cmd_decl_t show_llm_decl = {
  .module      = "llm",
  .name        = "llm",
  .usage       = "show llm",
  .description = "Show LLM subsystem state",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_llm,
  .parent_path = "show",
  .abbrev      = "llm",
};

static const cmd_decl_t show_llm_models_decl = {
  .module      = "llm",
  .name        = "models",
  .usage       = "show llm models [type]",
  .description = "List registered LLM models, optionally one kind",
  .group       = USERNS_GROUP_USER,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_llm_list,
  .parent_path = "show/llm",
  .abbrev      = "m",
  .arg_desc    = ad_show_models,
  .arg_count   = (uint8_t)(sizeof(ad_show_models) / sizeof(ad_show_models[0])),
};

static const cmd_decl_t show_llm_service_decl = {
  .module      = "llm",
  .name        = "service",
  .usage       = "show llm service [<name> [models]]",
  .description = "Show LLM services (list, detail, or cached /models)",
  .group       = USERNS_GROUP_ADMIN,
  .level       = 100,
  .scope       = CMD_SCOPE_ANY,
  .methods     = METHOD_T_ANY,
  .cb          = cmd_show_llm_service,
  .parent_path = "show/llm",
  .abbrev      = "s",
  .arg_desc    = ad_show_service,
  .arg_count   = (uint8_t)(sizeof(ad_show_service)
                 / sizeof(ad_show_service[0])),
};

void
llm_register_commands(void)
{
  cmd_register(&llm_decl);

  // add → { service, model }
  cmd_register(&llm_add_decl);
  cmd_register(&llm_add_service_decl);
  cmd_register(&llm_add_model_decl);

  // del → { service, model }
  cmd_register(&llm_del_decl);
  cmd_register(&llm_del_service_decl);
  cmd_register(&llm_del_model_decl);

  // service <name> <action> (refresh)
  cmd_register(&llm_service_decl);

  // Abbrev is "pb" not "p": the llm-bot plugin claims "p" for
  // /llm personality later in init, and a collision aborts plugin load.
  cmd_register(&llm_probe_decl);
  cmd_register(&llm_test_decl);
  cmd_register(&show_llm_decl);

  // ⭐ The one verb in this file that is not admin (operator's ruling,
  // 2026-08-23). It prints service names, model names, model ids and
  // context sizes — no URL and no key, both of which live on
  // llm_service_line, which is not this renderer. Every other command
  // registered here stays USERNS_GROUP_ADMIN, `show llm` included: a
  // parent's group does not gate a child's, since dispatch and
  // cmd_permits both read the resolved leaf (core/cmd.c) and neither
  // walks ancestors. ⚠ `user` is not `everyone` — an unidentified caller
  // is refused with not_authenticated, which is a named refusal telling
  // them to identify rather than a missing surface.
  cmd_register(&show_llm_models_decl);
  cmd_register(&show_llm_service_decl);
}
