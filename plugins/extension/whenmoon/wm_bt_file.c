// botmanager — MIT
// wm_bt_file.c — `.wm` binary snapshot: serializer + mmap loader + inspector.

#define WHENMOON_INTERNAL
#include "wm_bt_file.h"

#include "backtest.h"
#include "market.h"
#include "whenmoon_strategy.h"

#include "alloc.h"
#include "clam.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define WM_BT_FILE_CTX  "whenmoon.bt.file"

// Round `v` up to the next multiple of `align`. `align` is a power of
// two — WM_BT_FILE_PAGE_ALIGN is 4096.
static inline uint64_t
wm_bt_align_up(uint64_t v, uint64_t align)
{
  return((v + align - 1u) & ~(align - 1u));
}

// Set *out_path to "<path>.tmp"; FAIL on overflow. The tmp form is
// what the writer creates + fsyncs; rename(2) flips it into place
// once the body is durable on disk.
static bool
wm_bt_file_tmp_path(const char *path, char *out_path, size_t cap)
{
  int n;

  if(path == NULL || out_path == NULL || cap == 0)
    return(FAIL);

  n = snprintf(out_path, cap, "%s.tmp", path);

  if(n < 0 || (size_t)n >= cap)
    return(FAIL);

  return(SUCCESS);
}

// Write all `len` bytes from `buf` at offset `off`. Loops on partial
// pwrite + retries EINTR. Returns SUCCESS on full delivery, FAIL on
// the first non-recoverable error (errno preserved).
static bool
wm_bt_pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
  const char *p = (const char *)buf;
  size_t      left = len;

  while(left > 0)
  {
    ssize_t n = pwrite(fd, p, left, off);

    if(n < 0)
    {
      if(errno == EINTR)
        continue;

      return(FAIL);
    }

    if(n == 0)
    {
      errno = EIO;
      return(FAIL);
    }

    p    += (size_t)n;
    off  += n;
    left -= (size_t)n;
  }

  return(SUCCESS);
}

// Write `len` zero bytes at offset `off`. Used to materialize the
// page padding between header end and first grain block, and between
// successive grain blocks. ftruncate would also work but pwrite-
// zeros keeps the file dense on filesystems that don't sparse-allocate
// holes (e.g. NFS) and matches the rest of the writer's pwrite path.
static bool
wm_bt_pwrite_zeros(int fd, size_t len, off_t off)
{
  static const char  zbuf[4096];
  size_t             left = len;

  while(left > 0)
  {
    size_t chunk = left > sizeof(zbuf) ? sizeof(zbuf) : left;

    if(wm_bt_pwrite_full(fd, zbuf, chunk, off) != SUCCESS)
      return(FAIL);

    off  += (off_t)chunk;
    left -= chunk;
  }

  return(SUCCESS);
}

// Header validation common to open + inspect. Magic / file_version /
// bar_size / n_grains failures populate err + return FAIL. Schema
// version mismatch is NOT checked here — open() rejects it; inspect()
// tolerates it and emits a NOTE line.
static bool
wm_bt_file_header_validate(const wm_bt_file_header_t *hdr,
    const char *path, char *err, size_t err_cap)
{
  if(hdr->magic != WM_BT_FILE_MAGIC)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: not a .wm file (bad magic 0x%08x; expected 0x%08x)",
          path, hdr->magic, WM_BT_FILE_MAGIC);
    return(FAIL);
  }

  if(hdr->file_version != WM_BT_FILE_VERSION)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: file_version=%u, daemon expects %u; recompile",
          path, hdr->file_version, WM_BT_FILE_VERSION);
    return(FAIL);
  }

  if(hdr->bar_size != (uint32_t)sizeof(wm_candle_full_t))
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: bar_size=%u, daemon expects %zu; struct layout drift,"
          " recompile",
          path, hdr->bar_size, sizeof(wm_candle_full_t));
    return(FAIL);
  }

  if(hdr->n_grains != (uint32_t)WM_GRAN_MAX)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: n_grains=%u, daemon expects %u",
          path, hdr->n_grains, (uint32_t)WM_GRAN_MAX);
    return(FAIL);
  }

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Writer                                                                  //
// ----------------------------------------------------------------------- //

bool
wm_bt_file_write(const char *path, const wm_backtest_snapshot_t *snap,
    char *err, size_t err_cap)
{
  wm_bt_file_header_t  hdr;
  char                 tmp_path[PATH_MAX];
  int                  fd = -1;
  uint64_t             cur_off;
  uint32_t             g;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(path == NULL || snap == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad write inputs");
    return(FAIL);
  }

  if(snap->is_mapped)
  {
    if(err != NULL)
      snprintf(err, err_cap, "cannot re-serialize an mmap'd snapshot");
    return(FAIL);
  }

  if(wm_bt_file_tmp_path(path, tmp_path, sizeof(tmp_path)) != SUCCESS)
  {
    if(err != NULL)
      snprintf(err, err_cap, "path too long: %s", path);
    return(FAIL);
  }

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic                    = WM_BT_FILE_MAGIC;
  hdr.file_version             = WM_BT_FILE_VERSION;
  hdr.indicator_schema_version = (uint32_t)WM_INDICATOR_SCHEMA_VERSION;
  hdr.bar_size                 = (uint32_t)sizeof(wm_candle_full_t);
  hdr.n_grains                 = (uint32_t)WM_GRAN_MAX;
  snprintf(hdr.source_market_id, sizeof(hdr.source_market_id),
      "%s", snap->source_market_id);
  snprintf(hdr.range_start, sizeof(hdr.range_start),
      "%s", snap->range_start);
  snprintf(hdr.range_end, sizeof(hdr.range_end),
      "%s", snap->range_end);
  hdr.range_start_ms = snap->range_start_ms;
  hdr.range_end_ms   = snap->range_end_ms;
  hdr.bars_loaded_1m = snap->bars_loaded_1m;

  // Two-pass plan: compute the on-disk layout (header padded to one
  // page, then each non-empty grain page-aligned, empty grains take
  // zero bytes), pre-allocate the file, write bodies, then patch the
  // header with the recorded offsets.
  cur_off = wm_bt_align_up((uint64_t)sizeof(wm_bt_file_header_t),
      WM_BT_FILE_PAGE_ALIGN);

  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    uint32_t  n   = snap->mkt.grain_n[g];
    uint64_t  sz;

    hdr.blocks[g].bar_size = (uint32_t)sizeof(wm_candle_full_t);

    if(n == 0)
    {
      hdr.blocks[g].off_bytes = 0;
      hdr.blocks[g].n_bars    = 0;
      continue;
    }

    sz = (uint64_t)n * sizeof(wm_candle_full_t);

    hdr.blocks[g].off_bytes = cur_off;
    hdr.blocks[g].n_bars    = n;

    cur_off = wm_bt_align_up(cur_off + sz, WM_BT_FILE_PAGE_ALIGN);
  }

  fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);

  if(fd < 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "open %s: %s", tmp_path, strerror(errno));
    return(FAIL);
  }

  // Pre-allocate the file to its final size so any disk-full failure
  // surfaces before the writer starts laying down body bytes.
  if(ftruncate(fd, (off_t)cur_off) != 0)
  {
    int saved = errno;

    close(fd);
    unlink(tmp_path);
    if(err != NULL)
      snprintf(err, err_cap, "ftruncate %s to %" PRIu64 ": %s",
          tmp_path, cur_off, strerror(saved));
    return(FAIL);
  }

  // Header first — placeholder, no body yet, but the layout is
  // already pinned. We could skip this and write only the final
  // header, but writing once up front means tools sniffing the
  // partial file in tmp_path can still see the magic.
  if(wm_bt_pwrite_full(fd, &hdr, sizeof(hdr), 0) != SUCCESS)
  {
    int saved = errno;

    close(fd);
    unlink(tmp_path);
    if(err != NULL)
      snprintf(err, err_cap, "write header to %s: %s",
          tmp_path, strerror(saved));
    return(FAIL);
  }

  // Header padding: zero bytes from end of struct to first grain
  // block offset (or to cur_off if no grain has any bars).
  {
    uint64_t  pad_start = (uint64_t)sizeof(hdr);
    uint64_t  pad_end   = wm_bt_align_up(pad_start, WM_BT_FILE_PAGE_ALIGN);

    if(pad_end > pad_start)
    {
      if(wm_bt_pwrite_zeros(fd, (size_t)(pad_end - pad_start),
            (off_t)pad_start) != SUCCESS)
      {
        int saved = errno;

        close(fd);
        unlink(tmp_path);
        if(err != NULL)
          snprintf(err, err_cap, "header padding write: %s",
              strerror(saved));
        return(FAIL);
      }
    }
  }

  // Grain bodies. Each non-empty grain writes its ring at its
  // computed offset; inter-grain padding is already covered by the
  // ftruncate-zeroed file.
  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    uint32_t  n     = hdr.blocks[g].n_bars;
    uint64_t  off   = hdr.blocks[g].off_bytes;
    size_t    body;

    if(n == 0)
      continue;

    if(snap->mkt.grain_arr[g] == NULL)
    {
      close(fd);
      unlink(tmp_path);
      if(err != NULL)
        snprintf(err, err_cap,
            "snapshot grain %u: n_bars=%u but grain_arr=NULL", g, n);
      return(FAIL);
    }

    body = (size_t)n * sizeof(wm_candle_full_t);

    if(wm_bt_pwrite_full(fd, snap->mkt.grain_arr[g], body, (off_t)off)
        != SUCCESS)
    {
      int saved = errno;

      close(fd);
      unlink(tmp_path);
      if(err != NULL)
        snprintf(err, err_cap,
            "write grain %u (%zu bytes at offset %" PRIu64 "): %s",
            g, body, off, strerror(saved));
      return(FAIL);
    }
  }

  // Flush data + metadata, close, then rename for atomicity.
  if(fsync(fd) != 0)
  {
    int saved = errno;

    close(fd);
    unlink(tmp_path);
    if(err != NULL)
      snprintf(err, err_cap, "fsync %s: %s", tmp_path, strerror(saved));
    return(FAIL);
  }

  if(close(fd) != 0)
  {
    int saved = errno;

    unlink(tmp_path);
    if(err != NULL)
      snprintf(err, err_cap, "close %s: %s", tmp_path, strerror(saved));
    return(FAIL);
  }

  if(rename(tmp_path, path) != 0)
  {
    int saved = errno;

    unlink(tmp_path);
    if(err != NULL)
      snprintf(err, err_cap, "rename %s -> %s: %s",
          tmp_path, path, strerror(saved));
    return(FAIL);
  }

  clam(CLAM_INFO, WM_BT_FILE_CTX,
      "wrote %s: %s [%s..%s] bars_1m=%u total=%" PRIu64 " bytes",
      path, hdr.source_market_id, hdr.range_start, hdr.range_end,
      hdr.bars_loaded_1m, cur_off);

  return(SUCCESS);
}

// ----------------------------------------------------------------------- //
// Loader                                                                  //
// ----------------------------------------------------------------------- //

wm_backtest_snapshot_t *
wm_bt_file_open(const char *path, char *err, size_t err_cap)
{
  wm_backtest_snapshot_t  *snap = NULL;
  struct stat              sb;
  void                    *base = MAP_FAILED;
  size_t                   sz   = 0;
  int                      fd   = -1;
  const wm_bt_file_header_t *hdr;
  uint32_t                 g;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(path == NULL)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad open inputs");
    return(NULL);
  }

  fd = open(path, O_RDONLY | O_CLOEXEC);

  if(fd < 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "open %s: %s", path, strerror(errno));
    return(NULL);
  }

  if(fstat(fd, &sb) != 0)
  {
    int saved = errno;

    close(fd);
    if(err != NULL)
      snprintf(err, err_cap, "fstat %s: %s", path, strerror(saved));
    return(NULL);
  }

  if(sb.st_size < (off_t)sizeof(wm_bt_file_header_t))
  {
    close(fd);
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: file too small (%lld bytes < header %zu)",
          path, (long long)sb.st_size, sizeof(wm_bt_file_header_t));
    return(NULL);
  }

  sz = (size_t)sb.st_size;

  base = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);

  if(base == MAP_FAILED)
  {
    int saved = errno;

    close(fd);
    if(err != NULL)
      snprintf(err, err_cap, "mmap %s (%zu bytes): %s",
          path, sz, strerror(saved));
    return(NULL);
  }

  hdr = (const wm_bt_file_header_t *)base;

  if(wm_bt_file_header_validate(hdr, path, err, err_cap) != SUCCESS)
  {
    munmap(base, sz);
    close(fd);
    return(NULL);
  }

  // Indicator schema mismatch on load is a hard reject. Recompile is
  // the only recovery path (per `feedback_no_deprecated_code`).
  if(hdr->indicator_schema_version != (uint32_t)WM_INDICATOR_SCHEMA_VERSION)
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: built with indicator schema v%u, daemon expects v%u;"
          " recompile the .wm file",
          path, hdr->indicator_schema_version,
          (uint32_t)WM_INDICATOR_SCHEMA_VERSION);
    munmap(base, sz);
    close(fd);
    return(NULL);
  }

  // Range-check every block against the mapped file size before we
  // hand out pointers. Catches a corrupt or truncated file before any
  // strategy reads off the end.
  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    const wm_bt_file_grain_block_t *blk = &hdr->blocks[g];
    uint64_t                        end;

    if(blk->n_bars == 0)
    {
      if(blk->off_bytes != 0)
      {
        if(err != NULL)
          snprintf(err, err_cap,
              "%s: grain %u has n_bars=0 but off_bytes=%" PRIu64,
              path, g, blk->off_bytes);
        munmap(base, sz);
        close(fd);
        return(NULL);
      }
      continue;
    }

    if(blk->bar_size != (uint32_t)sizeof(wm_candle_full_t))
    {
      if(err != NULL)
        snprintf(err, err_cap,
            "%s: grain %u bar_size=%u, daemon expects %zu",
            path, g, blk->bar_size, sizeof(wm_candle_full_t));
      munmap(base, sz);
      close(fd);
      return(NULL);
    }

    end = blk->off_bytes
        + (uint64_t)blk->n_bars * sizeof(wm_candle_full_t);

    if(end > (uint64_t)sz)
    {
      if(err != NULL)
        snprintf(err, err_cap,
            "%s: grain %u extends past EOF (end=%" PRIu64
            ", file=%zu)",
            path, g, end, sz);
      munmap(base, sz);
      close(fd);
      return(NULL);
    }
  }

  snap = mem_alloc(WM_BT_FILE_CTX, "snapshot", sizeof(*snap));

  memset(snap, 0, sizeof(*snap));

  // Identity fields — mirror the heap-build's stub-market plumbing so
  // downstream renderers (clam, /show whenmoon backtest) see the same
  // shape as a heap-built snapshot. product_id is the wire form
  // (WM_PRODUCT_ID_SZ=24); the canonical "<exch>-<base>-<quote>" is
  // silently truncated into it, mirroring the heap-build's
  // snprintf-truncate behaviour. The truncation is deliberate, so
  // strlcpy's return is not tested.
  snprintf(snap->mkt.market_id_str, sizeof(snap->mkt.market_id_str),
      "%s", hdr->source_market_id);
  strlcpy(snap->mkt.product_id, hdr->source_market_id,
      sizeof(snap->mkt.product_id));
  snap->mkt.market_id = -1;        // mmap'd snapshot has no DB row binding

  if(pthread_mutex_init(&snap->mkt.lock, NULL) != 0)
  {
    mem_free(snap);
    munmap(base, sz);
    close(fd);
    if(err != NULL)
      snprintf(err, err_cap, "%s: mmap snapshot lock init failed", path);
    return(NULL);
  }

  snap->market_id_db = -1;
  snprintf(snap->source_market_id, sizeof(snap->source_market_id),
      "%s", hdr->source_market_id);
  snprintf(snap->range_start, sizeof(snap->range_start),
      "%s", hdr->range_start);
  snprintf(snap->range_end, sizeof(snap->range_end),
      "%s", hdr->range_end);
  snap->range_start_ms = hdr->range_start_ms;
  snap->range_end_ms   = hdr->range_end_ms;
  snap->bars_loaded_1m = hdr->bars_loaded_1m;

  snap->map_base       = base;
  snap->map_size       = sz;
  snap->map_fd         = fd;
  snap->is_mapped      = true;

  // Wire up grain rings — pointers index into the mapped region.
  // grain_cap == grain_n: the mmap'd ring is "full" from the
  // backtest's perspective (no live ingest, no shift). Synth
  // markets borrow these pointers; no copy.
  for(g = 0; g < WM_GRAN_MAX; g++)
  {
    const wm_bt_file_grain_block_t *blk = &hdr->blocks[g];

    if(blk->n_bars == 0)
    {
      snap->mkt.grain_arr[g] = NULL;
      snap->mkt.grain_n[g]   = 0;
      snap->mkt.grain_cap[g] = 0;
      continue;
    }

    snap->mkt.grain_arr[g] = (wm_candle_full_t *)
        ((char *)base + blk->off_bytes);
    snap->mkt.grain_n[g]   = blk->n_bars;
    snap->mkt.grain_cap[g] = blk->n_bars;
  }

  snap->mkt.aggregator = NULL;     // mmap'd snapshot has no aggregator

  clam(CLAM_INFO, WM_BT_FILE_CTX,
      "opened %s: %s [%s..%s] bars_1m=%u"
      " (1m=%u 5m=%u 15m=%u 1h=%u 4h=%u 1d=%u)",
      path, snap->source_market_id, snap->range_start, snap->range_end,
      snap->bars_loaded_1m,
      snap->mkt.grain_n[WM_GRAN_1M], snap->mkt.grain_n[WM_GRAN_5M],
      snap->mkt.grain_n[WM_GRAN_15M], snap->mkt.grain_n[WM_GRAN_1H],
      snap->mkt.grain_n[WM_GRAN_4H], snap->mkt.grain_n[WM_GRAN_1D]);

  return(snap);
}

void
wm_bt_file_close(wm_backtest_snapshot_t *snap)
{
  if(snap == NULL)
    return;

  if(!snap->is_mapped)
    return;

  if(snap->map_base != NULL && snap->map_size > 0)
    munmap(snap->map_base, snap->map_size);

  if(snap->map_fd >= 0)
    close(snap->map_fd);

  pthread_mutex_destroy(&snap->mkt.lock);
  mem_free(snap);
}

// ----------------------------------------------------------------------- //
// Inspector                                                               //
// ----------------------------------------------------------------------- //

bool
wm_bt_file_inspect(const char *path, char *summary, size_t cap,
    char *err, size_t err_cap)
{
  wm_bt_file_header_t  hdr;
  int                  fd = -1;
  ssize_t              n;
  uint32_t             g;
  int                  hdr_written;
  size_t               off = 0;
  bool                 schema_stale;

  if(err != NULL && err_cap > 0)
    err[0] = '\0';

  if(summary != NULL && cap > 0)
    summary[0] = '\0';

  if(path == NULL || summary == NULL || cap == 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "bad inspect inputs");
    return(FAIL);
  }

  fd = open(path, O_RDONLY | O_CLOEXEC);

  if(fd < 0)
  {
    if(err != NULL)
      snprintf(err, err_cap, "open %s: %s", path, strerror(errno));
    return(FAIL);
  }

  n = pread(fd, &hdr, sizeof(hdr), 0);

  if(n < 0)
  {
    int saved = errno;

    close(fd);
    if(err != NULL)
      snprintf(err, err_cap, "pread %s: %s", path, strerror(saved));
    return(FAIL);
  }

  close(fd);

  if((size_t)n != sizeof(hdr))
  {
    if(err != NULL)
      snprintf(err, err_cap,
          "%s: short read (got %zd bytes, expected %zu)",
          path, n, sizeof(hdr));
    return(FAIL);
  }

  if(wm_bt_file_header_validate(&hdr, path, err, err_cap) != SUCCESS)
    return(FAIL);

  schema_stale = (hdr.indicator_schema_version
      != (uint32_t)WM_INDICATOR_SCHEMA_VERSION);

  hdr_written = snprintf(summary, cap,
      "path:             %s\n"
      "magic:            0x%08x\n"
      "file_version:     %u\n"
      "indicator_schema: v%u%s\n"
      "bar_size:         %u\n"
      "n_grains:         %u\n"
      "source_market_id: %s\n"
      "range_start:      %s\n"
      "range_end:        %s\n"
      "range_start_ms:   %" PRId64 "\n"
      "range_end_ms:     %" PRId64 "\n"
      "bars_loaded_1m:   %u\n",
      path, hdr.magic, hdr.file_version,
      hdr.indicator_schema_version,
      schema_stale ? " (STALE — recompile to run)" : "",
      hdr.bar_size, hdr.n_grains, hdr.source_market_id,
      hdr.range_start, hdr.range_end,
      hdr.range_start_ms, hdr.range_end_ms,
      hdr.bars_loaded_1m);

  if(hdr_written < 0)
    return(SUCCESS);

  off = (size_t)hdr_written;

  if(off >= cap)
    return(SUCCESS);

  for(g = 0; g < WM_GRAN_MAX && off < cap; g++)
  {
    int written = snprintf(summary + off, cap - off,
        "grain[%u]:         n_bars=%u off=%" PRIu64 " bar_size=%u\n",
        g, hdr.blocks[g].n_bars, hdr.blocks[g].off_bytes,
        hdr.blocks[g].bar_size);

    if(written < 0)
      break;

    off += (size_t)written;
  }

  if(schema_stale && err != NULL)
    snprintf(err, err_cap,
        "%s: indicator schema v%u, daemon at v%u — file cannot be run",
        path, hdr.indicator_schema_version,
        (uint32_t)WM_INDICATOR_SCHEMA_VERSION);

  return(SUCCESS);
}
