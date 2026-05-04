// botmanager — MIT
// book_persist.c — durable trade-book state queue (WM-PT-3).
//
// Producers (paper fill apply, mode-set, reset, remove) hand off a
// fully-formed SQL statement keyed by (market_id, strategy_name).
// Coalescing: a second enqueue for the same key replaces the first
// and frees its prior SQL, so pending list size is bounded by the
// live book count.
//
// Drain runs on a plugin-global periodic task tick (1 s) AND inline
// at SIGTERM via wm_book_persist_flush_all so no snapshot is lost
// across restart. Statements run off-lock so DB latency never blocks
// enqueues from on-fill paths.

#define WHENMOON_INTERNAL
#include "book_persist.h"
#include "whenmoon.h"
#include "whenmoon_strategy.h"   // WM_STRATEGY_NAME_SZ

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "db.h"
#include "task.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Flush cadence (ms). Task system rounds sub-second values up to 1 s
// today, so finer cadences are no-ops; documented here so the value
// can be lowered later without re-discovering the floor.
#define WM_BP_FLUSH_INTERVAL_MS  1000

typedef struct wm_book_persist_entry
{
  int32_t                        market_id;
  char                           strategy_name[WM_STRATEGY_NAME_SZ];
  char                          *sql;       // owned; mem_free on drain
  struct wm_book_persist_entry  *next;
} wm_book_persist_entry_t;

static pthread_mutex_t           wm_bp_g_lock = PTHREAD_MUTEX_INITIALIZER;
static wm_book_persist_entry_t  *wm_bp_g_head = NULL;
static task_handle_t             wm_bp_g_task = TASK_HANDLE_NONE;

static void wm_bp_flush_task(task_t *t);
static void wm_bp_drain_locked(wm_book_persist_entry_t **head_out);
static void wm_bp_run_drained(wm_book_persist_entry_t *head);

// ------------------------------------------------------------------ //
// Plugin-global lifecycle                                             //
// ------------------------------------------------------------------ //

bool
wm_book_persist_global_init(void)
{
  if(wm_bp_g_task != TASK_HANDLE_NONE)
    return(SUCCESS);

  wm_bp_g_task = task_add_periodic("wm_book_flush", TASK_ANY, 200,
      WM_BP_FLUSH_INTERVAL_MS, wm_bp_flush_task, NULL);

  if(wm_bp_g_task == TASK_HANDLE_NONE)
  {
    clam(CLAM_WARN, WHENMOON_CTX, "book-persist flush task submit failed");
    return(FAIL);
  }

  return(SUCCESS);
}

void
wm_book_persist_global_destroy(void)
{
  // Flush any pending UPSERTs synchronously before canceling the
  // periodic task. The book state is the authoritative restart record
  // — losing the final upsert would diverge cash + position + fills
  // across SIGTERM.
  wm_book_persist_flush_all();

  if(wm_bp_g_task != TASK_HANDLE_NONE)
  {
    task_cancel(wm_bp_g_task);
    wm_bp_g_task = TASK_HANDLE_NONE;
  }
}

// ------------------------------------------------------------------ //
// Flush task                                                          //
// ------------------------------------------------------------------ //

static void
wm_bp_flush_task(task_t *t)
{
  wm_book_persist_entry_t *drained = NULL;

  if(t == NULL)
    return;

  // Detach the queue under its lock then run the SQL off-lock so DB
  // latency doesn't block enqueues from on-fill paths.
  pthread_mutex_lock(&wm_bp_g_lock);
  wm_bp_drain_locked(&drained);
  pthread_mutex_unlock(&wm_bp_g_lock);

  if(drained != NULL)
    wm_bp_run_drained(drained);

  t->state = TASK_ENDED;
}

// ------------------------------------------------------------------ //
// Queue ops                                                           //
// ------------------------------------------------------------------ //

static wm_book_persist_entry_t *
wm_bp_find_locked(int32_t market_id, const char *strategy_name)
{
  wm_book_persist_entry_t *e;

  for(e = wm_bp_g_head; e != NULL; e = e->next)
  {
    if(e->market_id != market_id)
      continue;

    if(strncmp(e->strategy_name, strategy_name,
           sizeof(e->strategy_name)) != 0)
      continue;

    return(e);
  }

  return(NULL);
}

bool
wm_book_persist_enqueue(int32_t market_id, const char *strategy_name,
    char *sql_owned)
{
  wm_book_persist_entry_t *e;

  if(strategy_name == NULL || sql_owned == NULL)
    return(FAIL);

  pthread_mutex_lock(&wm_bp_g_lock);

  e = wm_bp_find_locked(market_id, strategy_name);

  if(e != NULL)
  {
    // Coalesce: drop the prior pending statement, take the new one.
    if(e->sql != NULL)
      mem_free(e->sql);

    e->sql = sql_owned;
    pthread_mutex_unlock(&wm_bp_g_lock);
    return(SUCCESS);
  }

  e = mem_alloc("whenmoon", "book_persist_entry", sizeof(*e));

  if(e == NULL)
  {
    pthread_mutex_unlock(&wm_bp_g_lock);
    return(FAIL);
  }

  memset(e, 0, sizeof(*e));
  e->market_id = market_id;
  snprintf(e->strategy_name, sizeof(e->strategy_name), "%s", strategy_name);
  e->sql       = sql_owned;
  e->next      = wm_bp_g_head;
  wm_bp_g_head = e;

  pthread_mutex_unlock(&wm_bp_g_lock);
  return(SUCCESS);
}

bool
wm_book_persist_drop(int32_t market_id, const char *strategy_name)
{
  char  *sql;
  size_t cap = 256;
  char  *escaped;

  if(strategy_name == NULL)
    return(FAIL);

  escaped = db_escape(strategy_name);

  if(escaped == NULL)
    return(FAIL);

  sql = mem_alloc("whenmoon", "book_persist_drop_sql", cap);

  if(sql == NULL)
  {
    mem_free(escaped);
    return(FAIL);
  }

  snprintf(sql, cap,
      "DELETE FROM wm_trade_book_state"
      " WHERE market_id = %" PRId32
      "   AND strategy_name = '%s'",
      market_id, escaped);

  mem_free(escaped);

  if(wm_book_persist_enqueue(market_id, strategy_name, sql) != SUCCESS)
  {
    mem_free(sql);
    return(FAIL);
  }

  return(SUCCESS);
}

static void
wm_bp_drain_locked(wm_book_persist_entry_t **head_out)
{
  if(head_out == NULL)
    return;

  *head_out    = wm_bp_g_head;
  wm_bp_g_head = NULL;
}

static void
wm_bp_run_drained(wm_book_persist_entry_t *head)
{
  wm_book_persist_entry_t *e;
  wm_book_persist_entry_t *next;
  db_result_t             *res;

  for(e = head; e != NULL; e = next)
  {
    next = e->next;

    if(e->sql != NULL)
    {
      res = db_result_alloc();

      if(res != NULL)
      {
        if(db_query(e->sql, res) != SUCCESS || !res->ok)
          clam(CLAM_WARN, WHENMOON_CTX,
              "book-state persist failed (market_id=%" PRId32
              " strategy=%s): %s",
              e->market_id, e->strategy_name,
              res->error[0] != '\0' ? res->error : "(no driver error)");
        else
          clam(CLAM_DEBUG2, WHENMOON_CTX,
              "book-state persisted (market_id=%" PRId32
              " strategy=%s, affected=%u)",
              e->market_id, e->strategy_name, res->rows_affected);

        db_result_free(res);
      }

      mem_free(e->sql);
    }

    mem_free(e);
  }
}

void
wm_book_persist_flush_all(void)
{
  wm_book_persist_entry_t *drained = NULL;

  pthread_mutex_lock(&wm_bp_g_lock);
  wm_bp_drain_locked(&drained);
  pthread_mutex_unlock(&wm_bp_g_lock);

  if(drained != NULL)
    wm_bp_run_drained(drained);
}
