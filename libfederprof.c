
// TODO:
//
//   * could override `sqlite3_trace` and `sqlite3_trace_v2` so applications
//     do not disable this profiler. But that might confuse them.
//
//   * use kernel crypto to sha1 the SQLs and (query plan - done) for easier
//     post-processing?
//
//   * Make the library executable so users would use it to spawn applications
//     hosting SQLite, instead of having to fiddle with LD_PRELOAD on their
//     own? So, `federprof ... -- sqlite3` would launch `sqlite3` with the
//     environment set up properly, probably through exec?
//
//   * Give this an option to filter out INSERT statements? Add documentation
//     how this could easily be done by adding a filter to the pipe command?

#include <sqlite3.h>

#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>

#include <sys/types.h>
#include <linux/if_alg.h>
#include <linux/socket.h>
#include <sys/socket.h>

struct sha1ctx {
  int sock_fd;
  int fd;
};

void
sha1_destroy(struct sha1ctx* ctx) {
  if (!ctx) {
    return;
  }

  if (ctx->fd >= 0) {
    close(ctx->fd);
  }

  if (ctx->sock_fd >= 0) {
    close(ctx->sock_fd);
  }

  free(ctx);
}

struct sha1ctx*
sha1_new(void) {

  struct sha1ctx *ctx = calloc(1, sizeof(*ctx));

  if (!ctx) {
    goto error;
  }

  struct sockaddr_alg sa_alg;
  memset(&sa_alg, 0, sizeof(sa_alg));

  sa_alg.salg_family = AF_ALG;

  memcpy(sa_alg.salg_type, "hash", 5);
  memcpy(sa_alg.salg_name, "sha1", 5);

  ctx->sock_fd = socket(AF_ALG, SOCK_SEQPACKET, 0);

  if (ctx->sock_fd < 0) {
    goto error;
  }

  int bind_rc = bind(ctx->sock_fd, (struct sockaddr*)&sa_alg, sizeof(sa_alg));

  if (bind_rc) {
    goto error;
  }

  ctx->fd = accept(ctx->sock_fd, NULL, 0);

  if (ctx->fd < 0) {
    goto error;
  }

  return ctx;

error:
  sha1_destroy(ctx);
  return NULL;
}

int
sha1_add(struct sha1ctx* ctx, void const* const data, ssize_t size) {

  if (size < 0) {
    goto error;
  }

  if (!ctx) {
    goto error;
  }

  ssize_t written = write(ctx->fd, data, (size_t)size);

  if (written != size) {
    // FIXME: error handling
    goto error;
  }

  return 0;

error:

  return 1;

}

char*
sha1_hash(struct sha1ctx* ctx) {
  static const int size = 20;

  char* hash = malloc(size);

  if (!ctx) {
    goto error;
  }

  if (!hash) {
    goto error;
  }

  size_t read_ = read(ctx->fd, hash, size);

  if (read_ != size) {
    goto error;
  }

  return hash;

error:
  if (hash) {
    free(hash);
  }

  return NULL;
}

char*
sha1_hex(struct sha1ctx* ctx) {
  char* hash = sha1_hash(ctx);

  char* hex = malloc(41);

  if (!hash) {
    goto error;
  }

  if (!hex) {
    goto error;
  }

  for (char* h = hash, *out = hex; (h - hash) < 20; ++h) {
    *out++ = "0123456789abcdef"[(unsigned char)(*h) >> 4];
    *out++ = "0123456789abcdef"[(unsigned char)(*h) & 0xf];
  }

  hex[40] = 0;

  free(hash);

  return hex;

error:

  if (hash) {
    free(hash);
  }

  return NULL;

}

struct context
{
  int initialized;
  int profiling;
  sqlite3_int64 counter;
  sqlite3_str* str;
  char* session;
  char* buffer;
  int buffer_size;
  FILE* log;
  struct timespec start_time;

  sqlite3_mutex* mutex;

  int want_expanded;
  int want_unexpanded;
  int want_normalized;
  int want_triggers;
};

static struct context g_context = { 0 };

void
clear_context(struct context* ctx)
{

  if (ctx->str) {
    sqlite3_str_finish(ctx->str);
    ctx->str = NULL;
  }

  if (ctx->session) {
    free(ctx->session);
    ctx->session = NULL;
  }

  if (ctx->buffer) {
    sqlite3_free(ctx->buffer);
    ctx->buffer = NULL;
  }

  if (ctx->log) {
    int rc = fclose(ctx->log);
    ctx->log = NULL;
  }

  if (ctx->mutex) {
    sqlite3_mutex_free(ctx->mutex);
    ctx->mutex = NULL;
  }
}

static char const json_escape[] = {
  1, 1, 1, 1, 1, 1, 1, 1, 'b', 't', 'n', 1, 'f', 'r', 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  0, 0, '"', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '\\', 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void
append_json_escaped(sqlite3_str* str, char const* s)
{
  char* out = g_context.buffer;

  if (!s) {
    sqlite3_str_appendf(str, "%s", NULL);
    return;
  }

  while (*s) {

    if ((out - g_context.buffer) > (g_context.buffer_size - 7)) {
      // FIXME: buffer might not have enough free space for this:
      *out++ = '[';
      *out++ = '.';
      *out++ = '.';
      *out++ = '.';
      *out++ = ']';
      break;
    }

    unsigned char ch = (unsigned char)*s;

    switch (json_escape[ch]) {
      case 0:
        *out++ = ch;
        break;
      case 1:
        *out++ = '\\';
        *out++ = 'u';
        *out++ = '0';
        *out++ = '0';
        *out++ = '0' + (ch >> 4);
        *out++ = "0123456789abcdef"[ch & 0xf];
        break;
      default:
        *out++ = '\\';
        *out++ = json_escape[ch];
        break;
    }

    ++s;
  }

  *out++ = 0;

  sqlite3_str_appendall(str, g_context.buffer);
}

int
record_scan_status(sqlite3_stmt* stmt, sqlite3_int64 took_ns, int event, int is_trigger)
{

  if (sqlite3_threadsafe()) {
    sqlite3_mutex_enter(g_context.mutex);
  }

  int reset = 1;

  struct timespec logged_time;

  clock_gettime(CLOCK_MONOTONIC, &logged_time);

  logged_time.tv_sec -= g_context.start_time.tv_sec;
  logged_time.tv_nsec -= g_context.start_time.tv_nsec;

  if (logged_time.tv_nsec < 0) {
    logged_time.tv_sec -= 1;
    logged_time.tv_nsec += 1E9L;
  }

  sqlite3_str_appendf(
    g_context.str,
    "{"
    "\"session\":\"%s\""
    ",\"counter\":%lld"
    ",\"event\":%u"
    ",\"db_ptr\":%llu"
    ",\"stmt_ptr\":%llu"
    ",\"timestamp\":%llu.%09llu"
    ",\"fullscan_step\":%llu"
    ",\"sort\":%llu"
    ",\"autoindex\":%llu"
    ",\"vm_step\":%llu"
    ",\"reprepare\":%llu"
    ",\"run\":%llu"
    ",\"filter_miss\":%llu"
    ",\"filter_hit\":%llu"
    ",\"memused\":%llu"
    ",\"took_ns\":%llu",
    g_context.session,
    g_context.counter++,
    event,
    (sqlite3_int64)(intptr_t)stmt,
    (sqlite3_int64)(intptr_t)sqlite3_db_handle(stmt),
    (sqlite3_int64)logged_time.tv_sec,
    (sqlite3_int64)logged_time.tv_nsec,
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FULLSCAN_STEP, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_SORT, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_AUTOINDEX, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_VM_STEP, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_REPREPARE, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_RUN, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FILTER_MISS, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_FILTER_HIT, reset),
    sqlite3_stmt_status(stmt, SQLITE_STMTSTATUS_MEMUSED, reset),
    took_ns);

  if (g_context.want_unexpanded) {
    sqlite3_str_appendf(g_context.str, ",\"unexpanded\":\"");
    append_json_escaped(g_context.str, sqlite3_sql(stmt));
    sqlite3_str_appendchar(g_context.str, 1, '"');
  }

  if (g_context.want_expanded) {
    sqlite3_str_appendf(g_context.str, ",\"expanded\":\"");

    char* expanded = sqlite3_expanded_sql(stmt);
    
    // TODO: ought to pass generate `null` instead of empty string
    if (expanded) {
      append_json_escaped(g_context.str, expanded);
      sqlite3_free(expanded);
    }
    sqlite3_str_appendchar(g_context.str, 1, '"');

  }

#ifdef SQLITE_ENABLE_NORMALIZE
  if (g_context.want_normalized) {
    sqlite3_str_appendf(g_context.str, ",\"normalized\":\"");
    append_json_escaped(g_context.str, sqlite3_normalized_sql(stmt));
    sqlite3_str_appendchar(g_context.str, 1, '"');
  }
#endif

  sqlite3_str_appendf(g_context.str, ",\"scanstatus\":[");

  // printf("record_scan_status called\n");

#ifdef SQLITE_ENABLE_STMT_SCANSTATUS

  struct sha1ctx *plan_digest = sha1_new();

  for (int idx = 0; /**/; ++idx) {

    if (is_trigger && sqlite3_libversion_number() < 3042000) {
      // https://sqlite.org/forum/forumpost/a6ffe28e5f
      break;
    }

    sqlite3_int64 nloop = -1;
    int rc_nloop = sqlite3_stmt_scanstatus_v2(
      stmt, idx, SQLITE_SCANSTAT_NLOOP, SQLITE_SCANSTAT_COMPLEX, (void*)&nloop);

    if (SQLITE_OK != rc_nloop) {
      break;
    }

    sqlite3_int64 nvisit = -1;
    int rc_nvisit = sqlite3_stmt_scanstatus_v2(stmt,
                                               idx,
                                               SQLITE_SCANSTAT_NVISIT,
                                               SQLITE_SCANSTAT_COMPLEX,
                                               (void*)&nvisit);

    double est = -1;
    int rc_est = sqlite3_stmt_scanstatus_v2(
      stmt, idx, SQLITE_SCANSTAT_EST, SQLITE_SCANSTAT_COMPLEX, (void*)&est);

    char* name = NULL;
    int rc_name = sqlite3_stmt_scanstatus_v2(
      stmt, idx, SQLITE_SCANSTAT_NAME, SQLITE_SCANSTAT_COMPLEX, (void*)&name);

    char* explain = NULL;
    int rc_explain = sqlite3_stmt_scanstatus_v2(stmt,
                                               idx,
                                               SQLITE_SCANSTAT_EXPLAIN,
                                               SQLITE_SCANSTAT_COMPLEX,
                                               (void*)&explain);

    int selectid = -1;
    int rc_selectid = sqlite3_stmt_scanstatus_v2(stmt,
                                                 idx,
                                                 SQLITE_SCANSTAT_SELECTID,
                                                 SQLITE_SCANSTAT_COMPLEX,
                                                 (void*)&selectid);

    int parentid = -1;
    int rc_parentid = sqlite3_stmt_scanstatus_v2(stmt,
                                                 idx,
                                                 SQLITE_SCANSTAT_PARENTID,
                                                 SQLITE_SCANSTAT_COMPLEX,
                                                 (void*)&parentid);

    sqlite3_int64 ncycle = -1;
    int rc_ncycle = sqlite3_stmt_scanstatus_v2(stmt,
                                               idx,
                                               SQLITE_SCANSTAT_NCYCLE,
                                               SQLITE_SCANSTAT_COMPLEX,
                                               (void*)&ncycle);

    sha1_add(plan_digest, &idx, sizeof(idx));
    sha1_add(plan_digest, &selectid, sizeof(selectid));
    sha1_add(plan_digest, &parentid, sizeof(parentid));
    sha1_add(plan_digest, name, name ? strlen(name) + 1 : 0);
    sha1_add(plan_digest, explain, explain ? strlen(explain) + 1 : 0);

    sqlite3_str_appendf(g_context.str,
                        "%s{"
                        "\"idx\":%d"
                        ",\"nloop\":%lld"
                        ",\"nvisit\":%lld"
                        ",\"est\":%f"
                        ",\"selectid\":%d"
                        ",\"parentid\":%d"
                        ",\"ncycle\":%lld",
                        (idx > 0 ? "," : ""),
                        idx,
                        nloop,
                        nvisit,
                        est,
                        selectid,
                        parentid,
                        ncycle);
    sqlite3_str_appendf(g_context.str, ",\"name\":\"");
    append_json_escaped(g_context.str, name);
    sqlite3_str_appendf(g_context.str, "\",\"explain\":\"");
    append_json_escaped(g_context.str, explain);
    sqlite3_str_appendf(g_context.str, "\"}");
  }

  if (reset) {
    sqlite3_stmt_scanstatus_reset(stmt);
  }

#endif

  sqlite3_str_appendf(g_context.str, "]");

#ifdef SQLITE_ENABLE_STMT_SCANSTATUS
  char* plan_hex = sha1_hex(plan_digest);
  sqlite3_str_appendf(g_context.str, ",\"plan\":\"%s\"", plan_hex);
  free(plan_hex);
  sha1_destroy(plan_digest);
#endif

  sqlite3_str_appendf(g_context.str, "}\n");

  if (SQLITE_OK != sqlite3_str_errcode(g_context.str)) {
    fprintf(stderr, "error writing profile log entry\n");
  }

  fprintf(g_context.log, "%s", sqlite3_str_value(g_context.str));
  fflush(g_context.log);

  sqlite3_str_reset(g_context.str);

  if (sqlite3_threadsafe()) {
    sqlite3_mutex_leave(g_context.mutex);
  }

  return SQLITE_OK;
}

int
trace_callback(unsigned T, void* C, void* P, void* X)
{

  // printf("trace_callback called\n");

  if (g_context.profiling) {
    return SQLITE_OK;
  }

  g_context.profiling = 1;

  switch (T) {
    case SQLITE_TRACE_STMT: {
      sqlite3_stmt* stmt = (sqlite3_stmt*)P;
      char* unexpanded = (char*)X;

      int is_trigger = 0;
      if (unexpanded && unexpanded[0] == '-' && unexpanded[1] == '-') {
        is_trigger = 1;
      }

      if (is_trigger && !g_context.want_triggers) {
        break;
      }

      record_scan_status(stmt, 0, SQLITE_TRACE_STMT, is_trigger);
      break;
    }
    case SQLITE_TRACE_PROFILE: {
      sqlite3_stmt* stmt = (sqlite3_stmt*)P;
      sqlite3_int64* took_ns = (sqlite3_int64*)X;
      record_scan_status(stmt, *took_ns, SQLITE_TRACE_PROFILE, 0);
      break;
    }
    case SQLITE_TRACE_ROW: {
      sqlite3_stmt* stmt = (sqlite3_stmt*)P;
      break;
    }
    case SQLITE_TRACE_CLOSE: {
      sqlite3* db = (sqlite3*)P;
      break;
    }
    default:
      break;
  }

  g_context.profiling = 0;

  return SQLITE_OK;
}

void
init_context(struct context* ctx)
{

  clock_gettime(CLOCK_MONOTONIC, &ctx->start_time);

  FILE* uuid = fopen("/proc/sys/kernel/random/uuid", "r");

  if (uuid) {
    char* line = NULL;
    size_t size = 0;
    ssize_t len = 0;

    if ((len = getline(&line, &size, uuid)) > 0) {
      if ('\n' == line[len - 1]) {
        line[len - 1] = 0;
        --len;
      }
      ctx->session = line;
    }

    fclose(uuid);
  }

  if (!ctx->session) {
    ctx->session = strdup("...");
  }

  // TODO: rename to FEDERPROF_PIPE or something like that.
  char* command = getenv("SQLITE_PROFILE_COMMAND");

  if (!command) {
    command = "/usr/bin/cat - > log";
  }

  ctx->log = popen(command, "we");

  if (NULL == ctx->log) {
    // TODO: error handling?
  }

  ctx->str = sqlite3_str_new(NULL);
  ctx->buffer_size = 4096;
  ctx->buffer = sqlite3_malloc(ctx->buffer_size);
  ctx->buffer[0] = 0;

  ctx->want_expanded = 1;
  ctx->want_unexpanded = 1;
  ctx->want_normalized = 1;
  ctx->want_triggers = 0;

  if (sqlite3_threadsafe()) {
    ctx->mutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
  }
}

int
xEntryPoint(sqlite3* db,
            const char** pzErrMsg,
            const struct sqlite3_api_routines* pThunk)
{

  int rc = sqlite3_trace_v2(db,
    SQLITE_TRACE_PROFILE|SQLITE_TRACE_STMT, trace_callback, NULL);

  // printf("xEntryPoint called\n");

  if (SQLITE_OK != rc) {
    return rc;
  }

  if (!g_context.log) {
    init_context(&g_context);
  }

  return SQLITE_OK;
}

void __attribute__((constructor)) init(void) {}

void __attribute__((destructor)) uninit(void)
{
  clear_context(&g_context);
}

__attribute__((visibility("default"))) int
sqlite3_initialize(void)
{
  if (g_context.initialized) {
    return SQLITE_OK;
  }

  int (*original_sqlite3_initialize)(void) =
    dlsym(RTLD_NEXT, "sqlite3_initialize");
  int init_rc = original_sqlite3_initialize();

  if (SQLITE_OK == init_rc) {
    g_context.initialized = 1;
    int auto_rc = sqlite3_auto_extension((void (*)(void))xEntryPoint);
    // printf("init_rc = %u auto_rc = %u\n", init_rc, auto_rc);
  }

  return init_rc;
}
