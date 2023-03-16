# federprof - SQLite profiler

## Usage

```
% LD_PRELOAD=.../libfederprof.so sqlite3 ...
...
% python3 federprof.py
```
![screenshot showing sql with query plan and statistics](screenshot.png?raw=true)

### Compile SQLite as shared library

```bash
gcc -g -O3 -I. -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS4 \
    -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 \
    -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_EXPLAIN_COMMENTS \
    -DHAVE_USLEEP -DHAVE_READLINE -DSQLITE_ENABLE_STMT_SCANSTATUS \
    -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_ENABLE_NORMALIZE \
    sqlite3.c -ldl -lm -lreadline -lncurses -fPIC -shared -o \
    libsqlite3-shared.so
```

Note: You can use `select * from pragma_compile_options` in your current
shell or application to determine suitable configuration options.

### Compile SQLite shell, linking against shared library

```bash
gcc -g -O3 -I. -DSQLITE_THREADSAFE=0 -DSQLITE_ENABLE_FTS4 \
    -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 \
    -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_EXPLAIN_COMMENTS \
    -DHAVE_USLEEP -DHAVE_READLINE -DSQLITE_ENABLE_STMT_SCANSTATUS \
    -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_ENABLE_NORMALIZE \
    shell.c -ldl -lm -lreadline -lncurses -L. -lsqlite3-shared \
   -Wl,-rpath="$(realpath .)" -fPIC -o sqlite3
```

### Compile `libfederprof.c` into a shared library

```bash
gcc -g -O3 -Wall -Wextra -Wno-comment -Wno-unused-variable \
    -Wno-unused-parameter \
    -fvisibility=hidden \
    -I.../sqlite-amalgamation-3410000/ -DSQLITE_THREADSAFE=0 \
    -DSQLITE_ENABLE_FTS4 \
    -DSQLITE_ENABLE_FTS5 -DSQLITE_ENABLE_JSON1 \
    -DSQLITE_ENABLE_RTREE -DSQLITE_ENABLE_EXPLAIN_COMMENTS \
    -DHAVE_USLEEP -DHAVE_READLINE -DSQLITE_ENABLE_STMT_SCANSTATUS \
    -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_ENABLE_NORMALIZE \
   libfederprof.c -ldl -lm -lreadline -lncurses -fPIC -shared -o \
   libfederprof.so
```

### Launch SQLite shell with `LD_PRELOAD`

When `libfederprof.so` is pre-loaded, it will append a bunch of ndjson lines
to the `log` file in the current directory containing extensive information
about all queries executed in all databases:

```bash
LD_PRELOAD=./libfederprof.so .../sqlite-amalgamation-3410000/sqlite3 ...
```

You can use the environment variable `SQLITE_PROFILE_COMMAND` to specify a
command that `libfederprof.so` will pipe its output to:

```bash
SQLITE_PROFILE_COMMAND="/usr/bin/gzip -9 > logfile.gz" LD_PRELOAD=... ...
```

### Generated output

Each line in the log file looks like this:

```json
  {
    "session": "60e2cd24-1696-4a08-ab63-0c1919c8efae",
    "counter": 29,
    "db_ptr": 94400446346568,
    "stmt_ptr": 94400446149176,
    "timestamp": 0,
    "fullscan_step": 0,
    "sort": 0,
    "autoindex": 0,
    "vm_step": 5,
    "reprepare": 0,
    "run": 1,
    "filter_miss": 0,
    "filter_hit": 0,
    "memused": 2656,
    "took_ns": 0,
    "unexpanded": "pragma journal_mode = memory",
    "expanded": "pragma journal_mode = memory",
    "scanstatus": [ ... ]
  }
```

### Interpreting the output

The Generated logfile can easily be processed with DuckDB which makes it easy to
process compressed ndjson files:

```
% duckdb
v0.7.1 b00b93f0b1
...
D select sum(vm_step), normalized from read_ndjson_auto('logfile.gz') group
    by normalized order by 1 desc limit 10;
...
┌──────────────┬────────────────────────────────────────────────────────────────┐
│ sum(vm_step) │                           normalized                           │
│    double    │                            varchar                             │
├──────────────┼────────────────────────────────────────────────────────────────┤
│  310719170.0 │ SELECT data.c1,data.c2,data.c3,?FROM meta,data USING(id)WHER…  │
│  155766770.0 │ SELECT*FROM view_reentrant_dfas_for_all_tokens;                │
│   26841669.0 │ INSERT INTO dfapipeline(src,via,dst,round)SELECT s.id AS src…  │
│   17115023.0 │ SELECT*FROM(SELECT*FROM view_tt1ch_token_nfa_transitions);     │
│   11600202.0 │ INSERT INTO nfapipeline(src,via,dst)SELECT EACH.value->>?AS …  │
│    7001364.0 │ SELECT*FROM(SELECT*FROM view_cloning_foo);                     │
│    6150572.0 │ INSERT OR IGNORE INTO nfatrans(src,via,dst)WITH RECURSIVE ba…  │
│    4480927.0 │ WITH s AS(SELECT dfastate.id,json_group_array(nfastate.state…  │
│    2993630.0 │ ANALYZE;                                                       │
│    2702345.0 │ SELECT*FROM(SELECT*FROM view_tt1ch_boundary_mandatory2);       │
├──────────────┴────────────────────────────────────────────────────────────────┤
│ 10 rows                                                             2 columns │
└───────────────────────────────────────────────────────────────────────────────┘
```

## federprof.py  

This repository includes a simple Python script that interprets the generated
logfile and produces some tabular data showing slow queries, their query plans,
and some statistics.
