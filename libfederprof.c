
// TODO:
//
//   * could override `sqlite3_trace` and `sqlite3_trace_v2` so applications
//     do not disable this profiler. But that might confuse them.

#include <sqlite3.h>

#define _GNU_SOURCE 1
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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
}

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

    switch (*s) {
      case '\b':
        *out++ = '\\';
        *out++ = 'b';
        break;
      case '\t':
        *out++ = '\\';
        *out++ = 't';
        break;
      case '\n':
        *out++ = '\\';
        *out++ = 'n';
        break;
      case '\f':
        *out++ = '\\';
        *out++ = 'f';
        break;
      case '\r':
        *out++ = '\\';
        *out++ = 'r';
        break;
      case '\"':
        *out++ = '\\';
        *out++ = '"';
        break;
      case '\\':
        *out++ = '\\';
        *out++ = '\\';
        break;
      default:
        if (*s < 0x20) {
          *out++ = '\\';
          *out++ = 'u';
          *out++ = '0';
          *out++ = '0';
          *out++ = '0' + (*s >> 4);
          *out++ = "0123456789abcdef"[*s & 0xf];
        } else {
          *out++ = *s;
        }
        break;
    }

    ++s;
  }

  *out++ = 0;

  sqlite3_str_appendall(str, g_context.buffer);
}

int
record_scan_status(sqlite3_stmt* stmt, sqlite3_int64 took_ns)
{

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

  sqlite3_str_appendf(g_context.str, ",\"unexpanded\":\"");
  append_json_escaped(g_context.str, sqlite3_sql(stmt));
  sqlite3_str_appendf(g_context.str, "\",\"expanded\":\"");

  char* expanded = sqlite3_expanded_sql(stmt);
  if (expanded) {
    append_json_escaped(g_context.str, expanded);
    sqlite3_free(expanded);
  }

#ifdef SQLITE_ENABLE_NORMALIZE
  sqlite3_str_appendf(g_context.str, "\",\"normalized\":\"");
  append_json_escaped(g_context.str, sqlite3_normalized_sql(stmt));
#endif

  sqlite3_str_appendf(g_context.str, "\",\"scanstatus\":[");

  // printf("record_scan_status called\n");

#ifdef SQLITE_ENABLE_STMT_SCANSTATUS

  for (int idx = 0; /**/; ++idx) {

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
    int rc_explan = sqlite3_stmt_scanstatus_v2(stmt,
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

  sqlite3_str_appendf(g_context.str, "]}\n");

  if (SQLITE_OK != sqlite3_str_errcode(g_context.str)) {
    fprintf(stderr, "error writing profile log entry\n");
  }

  fprintf(g_context.log, "%s", sqlite3_str_value(g_context.str));
  fflush(g_context.log);

  sqlite3_str_reset(g_context.str);

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
      break;
    }
    case SQLITE_TRACE_PROFILE: {
      sqlite3_stmt* stmt = (sqlite3_stmt*)P;
      sqlite3_int64* took_ns = (sqlite3_int64*)X;
      record_scan_status(stmt, *took_ns);
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
}

int
xEntryPoint(sqlite3* db,
            const char** pzErrMsg,
            const struct sqlite3_api_routines* pThunk)
{

  int rc = sqlite3_trace_v2(db, SQLITE_TRACE_PROFILE, trace_callback, NULL);

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
