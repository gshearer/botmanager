// botmanager — MIT
// Cases for the method instance traffic counters (include/method.h).
//
// msg_out is incremented by method_send / method_send_emote from
// whatever thread replies — two command tasks replying at once is
// routine traffic — while msg_in and both readers sit under
// method_mutex. A mutex only one side takes orders nothing, so a plain
// increment outside the lock loses updates silently and permanently:
// /show methods under-reports and nothing ever reconciles it (root
// OBS-17; OBS-15's TSan gate reported the pair six times).
// These rows state the contract as exact arithmetic over the public
// surface only, so the suite is red under a plain -O2 build (lost
// increments at four contending threads) and red under
// -fsanitize=thread (unlocked write vs locked read) — no daemon, no
// network, no dlopen.

#include "test.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"
#include "method.h"

#include <pthread.h>
#include <string.h>

#define SUITE "method_stats"

#define SENDERS           4
#define SENDS_PER_THREAD  100000
#define EMOTES_PER_THREAD 25000

static method_inst_t *inst;
static bool           readers_stop;   // __atomic access only

static bool
stub_send(void *handle, const char *target, const char *text)
{
  (void)handle;
  (void)target;
  (void)text;
  return(SUCCESS);
}

static const method_driver_t stub_driver = {
  .name = "tstats",
  .send = stub_send,
};

static void *
sender_entry(void *arg)
{
  (void)arg;

  for(uint32_t i = 0; i < SENDS_PER_THREAD; i++)
    method_send(inst, "#t", "x");

  for(uint32_t i = 0; i < EMOTES_PER_THREAD; i++)
    method_send_emote(inst, "#t", "x");

  return(NULL);
}

// A live reader while the senders hammer: method_get_stats takes
// method_mutex, the senders take nothing — this is the exact
// unlocked-write / locked-read pair the TSan arm exists to see.
static void *
reader_entry(void *arg)
{
  method_stats_t st;

  (void)arg;

  while(!__atomic_load_n(&readers_stop, __ATOMIC_RELAXED))
    method_get_stats(&st);

  return(NULL);
}

typedef struct
{
  uint64_t in;
  uint64_t out;
  bool     found;
} inst_probe_t;

static void
inst_probe_cb(const char *name, const char *kind, method_state_t state,
    uint64_t msg_in, uint64_t msg_out, uint32_t sub_count,
    time_t connected_at, void *data)
{
  inst_probe_t *p = data;

  (void)kind;
  (void)state;
  (void)sub_count;
  (void)connected_at;

  if(strcmp(name, "tstats") == 0)
  {
    p->in    = msg_in;
    p->out   = msg_out;
    p->found = true;
  }
}

static void
probe(inst_probe_t *p)
{
  memset(p, 0, sizeof(*p));
  method_iterate_instances(inst_probe_cb, p);
}

// Single-threaded semantics first: what lands is what counts, on both
// write sites, and a delivery moves the other counter.
static void
case_counts_what_landed(void)
{
  method_msg_t msg;
  inst_probe_t p;

  test_check_bool(SUITE, "a send lands", SUCCESS,
      method_send(inst, "#t", "hello"));

  test_check_bool(SUITE, "an emote lands via the send fallback", SUCCESS,
      method_send_emote(inst, "#t", "waves"));

  for(uint32_t i = 0; i < 4; i++)
    method_send(inst, "#t", "x");

  method_send_emote(inst, "#t", "x");

  for(uint32_t i = 0; i < 3; i++)
  {
    memset(&msg, 0, sizeof(msg));
    method_deliver(inst, &msg);
  }

  probe(&p);

  test_check_bool(SUITE, "the instance is visible", true, p.found);

  test_check_sz(SUITE, "five sends and two emotes are seven out",
      7, (size_t)p.out);

  test_check_sz(SUITE, "three deliveries are three in",
      3, (size_t)p.in);
}

// A refused send counts nothing: msg_out is a count of what LANDED.
static void
case_refusal_counts_nothing(void)
{
  inst_probe_t p;

  method_set_state(inst, METHOD_RUNNING);

  test_check_bool(SUITE, "a send to a non-AVAILABLE instance refuses",
      FAIL, method_send(inst, "#t", "x"));

  method_set_state(inst, METHOD_AVAILABLE);
  probe(&p);

  test_check_sz(SUITE, "and the refusal counted nothing",
      7, (size_t)p.out);
}

// The row: four threads inside method_send at once, a reader under
// method_mutex the whole time. The joins give happens-before, so the
// final counts are EXACT — one lost increment anywhere is red here.
static void
case_concurrent_exact(void)
{
  pthread_t      senders[SENDERS];
  pthread_t      reader;
  method_stats_t st;
  inst_probe_t   p;
  const uint64_t expect = 7
      + (uint64_t)SENDERS * (SENDS_PER_THREAD + EMOTES_PER_THREAD);

  readers_stop = false;
  pthread_create(&reader, NULL, reader_entry, NULL);

  for(uint32_t i = 0; i < SENDERS; i++)
    pthread_create(&senders[i], NULL, sender_entry, NULL);

  for(uint32_t i = 0; i < SENDERS; i++)
    pthread_join(senders[i], NULL);

  __atomic_store_n(&readers_stop, true, __ATOMIC_RELAXED);
  pthread_join(reader, NULL);

  probe(&p);
  method_get_stats(&st);

  test_check_sz(SUITE, "the instance count is exact",
      (size_t)expect, (size_t)p.out);

  test_check_sz(SUITE, "the stats total agrees",
      (size_t)expect, (size_t)st.total_msg_out);

  test_check_sz(SUITE, "msg_in did not move",
      3, (size_t)p.in);
}

int
main(void)
{
  mem_init();
  clam_init();
  method_init();

  inst = method_register(&stub_driver, "tstats");
  method_set_state(inst, METHOD_AVAILABLE);

  case_counts_what_landed();
  case_refusal_counts_nothing();
  case_concurrent_exact();

  method_unregister("tstats");
  method_release(inst);

  return(test_report(SUITE));
}
