// botmanager — MIT
// shorturl: FastCGI entry point — resolve a token from QUERY_STRING, redirect.

#include <fcgiapp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "db.h"
#include "http.h"
#include "token.h"
#include "version_str.h"

static volatile sig_atomic_t shutdown_requested;

static void  on_signal(int);
static bool  install_signal_handlers(void);
static void  handle_request(FCGX_Request *);

static void
on_signal(int signal_number)
{
  (void)signal_number;

  shutdown_requested = 1;

  // Documented by libfcgi as signal-handler safe. It is what actually breaks
  // the accept loop: libfcgi restarts accept(2) internally on EINTR, so the
  // flag above would otherwise go unnoticed until the next request happened to
  // arrive — and a service manager asking this process to stop would wait out
  // its timeout and resort to SIGKILL.
  FCGX_ShutdownPending();
}

static bool
install_signal_handlers(void)
{
  struct sigaction action;

  memset(&action, 0, sizeof action);
  action.sa_handler = on_signal;
  sigemptyset(&action.sa_mask);

  // Deliberately no SA_RESTART: the handler must interrupt FCGX_Accept_r so
  // the loop can notice the flag and unwind, rather than blocking until the
  // next request happens to arrive.
  action.sa_flags = 0;

  return(sigaction(SIGTERM, &action, NULL) == 0 &&
         sigaction(SIGINT,  &action, NULL) == 0);
}

static void
handle_request(FCGX_Request *request)
{
  char target[SHORTURL_TARGET_MAX];
  const char *query_string;
  token_t token;

  // The path is nginx-side decoration — a vanity preface such as /p0ada — so
  // PATH_INFO and SCRIPT_NAME are deliberately never consulted.
  query_string = FCGX_GetParam("QUERY_STRING", request->envp);

  // Malformed tokens are the majority of traffic on a public host. Rejecting
  // them here costs a length test and eight range checks, and spares the
  // database a round trip it could never have satisfied.
  if(!token_parse(query_string, &token))
  {
    http_not_found(request);
    return;
  }

  switch(db_resolve(&token, target, sizeof target))
  {
    case DB_OK:
      http_redirect(request, target);
      break;

    case DB_NOT_FOUND:
      http_not_found(request);
      break;

    // db_resolve never reports a conflict, but naming it keeps the switch
    // exhaustive so -Wswitch catches a future addition to db_result_t.
    case DB_CONFLICT:
    case DB_ERROR:
      http_server_error(request);
      break;
  }
}

int
main(void)
{
  FCGX_Request request;

  if(!install_signal_handlers())
  {
    perror("shorturl: sigaction");
    return(1);
  }

  // Everything that can refuse to start does so here, before the first
  // request, so that no visitor ever meets a configuration error.
  if(!http_init() || !db_open())
    return(1);

  if(FCGX_Init() != 0)
  {
    fprintf(stderr, "shorturl: FCGX_Init failed\n");
    db_close();
    return(1);
  }

  if(FCGX_InitRequest(&request, 0, 0) != 0)
  {
    fprintf(stderr, "shorturl: FCGX_InitRequest failed\n");
    db_close();
    return(1);
  }

  fprintf(stderr, "%s ready\n", SHORTURL_VERSION_STRING);

  while(!shutdown_requested && FCGX_Accept_r(&request) >= 0)
  {
    handle_request(&request);
    FCGX_Finish_r(&request);
  }

  db_close();

  return(0);
}
