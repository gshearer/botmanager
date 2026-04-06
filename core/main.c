#define MAIN_INTERNAL
#include "main.h"
#include "admin.h"
#include "bconf.h"
#include "bot.h"
#include "clam.h"
#include "cmd.h"
#include "botmanctl.h"
#include "console.h"
#include "db.h"
#include "kv.h"
#include "mem.h"
#include "method.h"
#include "plugin.h"
#include "userns.h"
#include "pool.h"
#include "sig.h"
#include "curl.h"
#include "resolve.h"
#include "sock.h"
#include "task.h"
#include "util.h"
#include "version.h"

// Program start timestamp (global, extern declared in main.h).
time_t bm_start_time = 0;

// print the version string to stdout
static void
print_version(void)
{
  printf("%s\n", BM_VERSION_STR);
}

// print usage / help text to stdout
// prog: argv[0] program name
static void
print_usage(const char *prog)
{
  printf("Usage: %s [options]\n", prog);
  printf("  -c <file>   Specify configuration file\n");
  printf("  -v          Show version\n");
  printf("  -h          Show this help\n");
}

// returns: EXIT_SUCCESS or EXIT_FAILURE
// argc: argument count
// argv: argument vector
int
main(int argc, char *argv[])
{
  int opt;

  // --- 1. Parse command-line arguments ---

  while((opt = getopt(argc, argv, "c:vh")) != -1)
  {
    switch(opt)
    {
      case 'c':
        config_path = optarg;
        break;
      case 'v':
        print_version();
        return(EXIT_SUCCESS);
      case 'h':
        print_usage(argv[0]);
        return(EXIT_SUCCESS);
      default:
        print_usage(argv[0]);
        return(EXIT_FAILURE);
    }
  }

  // --- 2. Initialize core subsystems (DESIGN.md §Startup) ---

  bm_start_time = time(NULL);

  // CLAM first — logging must be available for everything else.
  clam_init();

  // Memory management.
  mem_init();

  // Shared utilities (PRNG seed, etc.).
  util_init();

  // Instance lock — prevent multiple copies of the daemon.
  {
    char lockpath[256];
    const char *home = getenv("HOME");

    if(home != NULL)
      snprintf(lockpath, sizeof(lockpath),
          "%s/.config/botmanager/botman.lock", home);
    else
      snprintf(lockpath, sizeof(lockpath), "/tmp/botman.lock");

    lock_fd = open(lockpath, O_CREAT | O_RDWR, 0600);

    if(lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0)
    {
      fprintf(stderr, "another instance is already running (lock: %s)\n",
          lockpath);

      if(lock_fd >= 0)
        close(lock_fd);

      mem_exit();
      clam_exit();
      return(EXIT_FAILURE);
    }
  }

  // Version banner (stdinout subscriber registered in clam_init).
  clam(CLAM_INFO, "main", "%s", BM_VERSION_STR);

  // Signal handling.
  sig_init();

  // Task system and thread pool.
  task_init();
  pool_init();

  // Socket service (epoll worker starts, config loaded later).
  sock_init();

  // DNS resolver (async lookups via task system).
  resolve_init();

  // Curl service (multi loop starts, config loaded later).
  curl_init();

  // Bootstrap configuration.
  bconf_init(config_path);

  // Method abstraction layer.
  method_init();

  // Bot subsystem.
  bot_init();

  // Command subsystem (registers built-in commands).
  cmd_init();

  // Administrative commands (set, show, status, quit, help).
  admin_init();

  // Task subsystem commands (show tasks).
  task_register_commands();

  // Memory subsystem commands (show memory).
  mem_register_commands();

  // Plugin subsystem commands (show plugin).
  plugin_register_commands();

  // Resolver user command (resolve).
  resolve_register_commands();

  // Plugin system: discover and resolve dependencies.
  plugin_init();

  const char *plugin_dir = bconf_get("PLUGIN_PATH");

  if(plugin_dir == NULL)
    plugin_dir = "./plugins";

  uint32_t n_loaded = plugin_discover(plugin_dir);
  clam(CLAM_INFO, "main", "discovered %u plugin(s)", n_loaded);

  if(plugin_resolve() != SUCCESS)
  {
    clam(CLAM_FATAL, "main", "plugin dependency resolution failed");
    goto shutdown;
  }

  // Database: extract driver from loaded DB plugin.
  const plugin_desc_t *db_plugin = plugin_find_type(PLUGIN_DB, NULL);

  if(db_plugin == NULL || db_plugin->ext == NULL)
  {
    clam(CLAM_FATAL, "main", "no database plugin found");
    goto shutdown;
  }

  if(db_init((const db_driver_t *)db_plugin->ext) != SUCCESS)
  {
    clam(CLAM_FATAL, "main", "database initialization failed");
    goto shutdown;
  }

  // KV configuration (phase 1: create in-memory store).
  kv_init();

  // Initialize plugins: registers KV schema, sets up internals.
  if(plugin_init_all() != SUCCESS)
  {
    clam(CLAM_FATAL, "main", "plugin initialization failed");
    goto shutdown;
  }

  // KV configuration (phase 2: load values from database).
  if(kv_load() != SUCCESS)
    clam(CLAM_WARN, "main", "kv_load failed, continuing with defaults");

  // CLAM stdinout KV registration (now that KV store is ready).
  clam_register_config();

  // Socket service KV registration (now that KV store is ready).
  sock_register_config();

  // DNS resolver KV registration.
  resolve_register_config();

  // Curl service KV registration.
  curl_register_config();

  // Thread pool KV registration.
  pool_register_config();

  // Bot subsystem KV registration.
  bot_register_config();

  // User namespace KV registration.
  userns_register_config();

  // Botmanctl method KV registration.
  botmanctl_register_config();

  // Admin command KV registration.
  admin_register_config();

  // Console KV registration (history settings).
  console_register_config();

  // Plugin subsystem KV registration.
  plugin_register_config();

  // Autoload: load any plugins listed in core.plugin.autoload that
  // were not found during initial discovery.
  {
    uint32_t n_autoload = plugin_load_autoload(plugin_dir);

    if(n_autoload > 0)
    {
      clam(CLAM_INFO, "main", "autoloaded %u additional plugin(s)",
          n_autoload);

      if(plugin_resolve() != SUCCESS)
      {
        clam(CLAM_FATAL, "main",
            "plugin dependency resolution failed after autoload");
        goto shutdown;
      }

      if(plugin_init_all() != SUCCESS)
      {
        clam(CLAM_FATAL, "main",
            "plugin initialization failed after autoload");
        goto shutdown;
      }
    }
  }

  // User namespaces (requires DB).
  if(userns_init() != SUCCESS)
  {
    clam(CLAM_FATAL, "main", "user namespace initialization failed");
    goto shutdown;
  }

  // Start plugins (begin active operation, dependency order).
  if(plugin_start_all() != SUCCESS)
  {
    clam(CLAM_FATAL, "main", "plugin startup failed");
    goto shutdown;
  }

  // Bot persistence: create tables and restore saved instances.
  if(bot_ensure_tables() != SUCCESS)
    clam(CLAM_WARN, "main", "bot table creation failed, continuing");

  bot_restore();

  // Claim any remaining DB entries not picked up by schema registration
  // (e.g., per-channel IRC config, dynamically added keys).
  kv_claim_pending();

  // Console method registration — must be after method_init and cmd_init
  // so console can register as a method and the input reader can dispatch.
  console_register_method(bm_start_time);

  // Botmanctl method registration — Unix domain socket for programmatic
  // operator control. Must be after method_init and cmd_init.
  botmanctl_register_method();

  clam(CLAM_INFO, "main", "startup complete");

  // First-run detection: if no bot instances exist, this is likely a
  // new installation. Print a welcome banner to guide the owner.
  {
    bot_stats_t bs;

    bot_get_stats(&bs);

    if(bs.instances == 0)
    {
      clam(CLAM_INFO, "main", "");
      clam(CLAM_INFO, "main",
          "Welcome to BotManager. No bot instances are configured.");
      clam(CLAM_INFO, "main",
          "Use the console to set up your first bot. Available commands:");
      clam(CLAM_INFO, "main", "  /help commands  — list all system commands");
      clam(CLAM_INFO, "main", "  /show     — show configuration values");
      clam(CLAM_INFO, "main", "  /set      — set a configuration value");
      clam(CLAM_INFO, "main", "  /status   — system health dashboard");
      clam(CLAM_INFO, "main", "  /quit     — graceful shutdown");
      clam(CLAM_INFO, "main", "");
    }
  }

  // --- 3. Main loop ---
  // Parent thread enters the task system as worker slot 0.
  // This blocks until shutdown is requested (signal or TASK_FATAL).
  pool_run_parent();

  // --- 4. Shutdown (DESIGN.md §Shutdown, reverse order) ---
shutdown:

  // Log shutdown reason.
  {
    int caught = sig_caught();

    if(caught != 0)
      clam(CLAM_INFO, "main", "shutdown: %s received", sig_name(caught));

    else
      clam(CLAM_INFO, "main", "shutdown: internal request");
  }

  // Log in-flight work before draining.
  {
    task_stats_t ts;

    task_get_stats(&ts);

    if(ts.total > 0)
      clam(CLAM_INFO, "main",
          "draining tasks (wait: %u run: %u sleep: %u link: %u)",
          ts.waiting, ts.running, ts.sleeping, ts.linked);
  }

  // 1. Bot instances (unsubscribe from methods).
  bot_exit();

  // 1b. Administrative commands.
  admin_exit();

  // 1c. Command subsystem (unregister all commands).
  cmd_exit();

  // 2. Stop plugins (signals persist tasks to shut down).
  plugin_stop_all();

  // 3. User namespaces.
  userns_exit();

  // 4. Thread pool: workers finish current tasks, join persist threads.
  pool_exit();

  // 5. Socket service (epoll worker joined by pool_exit above).
  sock_exit();

  // 5b. DNS resolver.
  resolve_exit();

  // 5c. Curl service (multi loop joined by pool_exit above).
  curl_exit();

  // 6. Deinitialize plugins (safe — persist threads already joined).
  //    Method plugins unregister their instances here.
  plugin_deinit_all();

  // 6b. Botmanctl method (persist task joined by pool_exit above).
  botmanctl_exit();

  // 6c. Method instances (catch-all for any not cleaned up by plugins).
  method_exit();

  // 7. Bootstrap configuration.
  bconf_exit();

  // 8. KV flush and exit (requires DB still alive).
  kv_exit();

  // 9. Database close.
  db_exit();

  // 10. Plugin system exit (dlclose all plugins).
  plugin_exit();

  // 11. Task system finalized.
  {
    task_stats_t ts;

    task_get_stats(&ts);

    if(ts.total > 0)
      clam(CLAM_WARN, "main", "%u task(s) abandoned", ts.total);
  }

  task_exit();

  // Signal handling restored.
  sig_exit();

  // Log uptime.
  {
    time_t uptime = time(NULL) - bm_start_time;

    clam(CLAM_INFO, "main", "shutdown complete (uptime: %lum %lus)",
        (unsigned long)(uptime / 60), (unsigned long)(uptime % 60));
  }

  // Console method stopped.
  // (Console input persist task is joined by pool_exit above.)
  console_exit();

  // Memory management: leak detection and shutdown.
  mem_exit();

  // CLAM shut down last.
  clam_exit();

  return(EXIT_SUCCESS);
}
