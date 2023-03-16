# federprof - SQLite profiler

## Usage

```
% LD_PRELOAD=.../libfederprof.so sqlite3 ...
...
% python3 federprof.py
╭───────────┬─────────────────────────────────────┬─────────────────────────────────────────────────────────────────────────────────────────────────────────╮
│ sum(took… │ stats                               │ sql+eqp                                                                                                 │
├───────────┼─────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│           │                                     │ ┌─────────────────────────────────────────────────────────────────────────────────────────────────────┐ │
│           │                                     │ │                                                                                                     │ │
│           │                                     │ │                                                                                                     │ │
│           │                                     │ │ with rem as materialized (                                                                          │ │
│           │                                     │ │   select nfatrans.id                                                                                │ │
│           │                                     │ │   from json_each(?1) each                                                                           │ │
│           │                                     │ │   cross join nfastate on nfastate.state = each.value                                                │ │
│           │                                     │ │   cross join nfatrans on nfatrans.dst = nfastate.id                                                 │ │
│           │                                     │ │                                                                                                     │ │
│           │                                     │ │   union                                                                                             │ │
│           │                                     │ │                                                                                                     │ │
│           │                                     │ │   select nfatrans.id                                                                                │ │
│           │                                     │ │   from json_each(?2) each                                                                           │ │
│           │ ┌────────────────────┬────────────┐ │ │   cross join nfastate on nfastate.state = each.value                                                │ │
│           │ │ sum(vm_step)       │ 37,260     │ │ │   cross join nfatrans on nfatrans.src = nfastate.id                                                 │ │
│           │ │ sum(ncycle)        │ 20,757,261 │ │ │                                                                                                     │ │
│           │ │ sum(run)           │ 365        │ │ │   union                                                                                             │ │
│           │ │ sum(fullscan_step) │ 0.00       │ │ │                                                                                                     │ │
│           │ │ sum(sort)          │ 0.00       │ │ │   select nfatrans.id from nfatrans where dst is null                                                │ │
│           │ │ sum(autoindex)     │ 0.00       │ │ │ )                                                                                                   │ │
│           │ │ sum(reprepare)     │ 0.00       │ │ │ delete from nfatrans indexed by idx_nfatrans_id where nfatrans.id in (select id from rem)           │ │
│    0.01   │ │ sum(filter_miss)   │ 0.00       │ │ │                                                                                                     │ │
│           │ │ sum(filter_hit)    │ 0.00       │ │ │                                                                                                     │ │
│           │ │ avg(memused)       │ 15950.38   │ │ │                                                                                                     │ │
│           │ │ sum(nvisit)        │ -365       │ │ │      ncycle_p     nvisit_p   explain                                                                │ │
│           │ │ sum(est)           │ 373771.25  │ │ │  ─────────────────────────────────────────────────────────────────────────────────────────────────  │ │
│           │ │ sum(nloop)         │ 365        │ │ │          None         None   LIST SUBQUERY 4   est=365                                              │ │
│           │ │ sum(took_seconds)  │ 0.01       │ │ │        59.86%         None     MATERIALIZE rem   est=365                                            │ │
│           │ └────────────────────┴────────────┘ │ │          None         None       COMPOUND QUERY   est=365                                           │ │
│           │                                     │ │          None         None         LEFT-MOST SUBQUERY   est=365                                     │ │
│           │                                     │ │        10.28%       20.00%           SCAN each VIRTUAL TABLE INDEX 1:   nloop=365 nvisit=365 est…   │ │
│           │                                     │ │         6.32%       20.00%           SEARCH nfastate USING COVERING INDEX sqlite_autoindex_nfast…   │ │
│           │                                     │ │         3.46%       20.00%           SEARCH nfatrans USING COVERING INDEX idx_nfatrans_dst (dst=…   │ │
│           │                                     │ │          None         None         UNION USING TEMP B-TREE   est=365                                │ │
│           │                                     │ │         2.48%        0.00%           SCAN each VIRTUAL TABLE INDEX 1:   nloop=365 est=8,760         │ │
│           │                                     │ │         0.44%        0.00%           SEARCH nfastate USING COVERING INDEX sqlite_autoindex_nfast…   │ │
│           │                                     │ │         0.38%        0.00%           SEARCH nfatrans USING COVERING INDEX sqlite_autoindex_nfatr…   │ │
│           │                                     │ │          None         None         UNION USING TEMP B-TREE   est=365                                │ │
│           │                                     │ │         1.80%        0.00%           SEARCH nfatrans USING COVERING INDEX idx_nfatrans_dst (dst=…   │ │
│           │                                     │ │         8.05%       20.00%     SCAN rem   nloop=365 nvisit=365 est=321,208                          │ │
│           │                                     │ │         6.94%       20.00%   SEARCH nfatrans USING INDEX idx_nfatrans_id (id=?)   nloop=365 nvis…   │ │
│           │                                     │ │                                                                                                     │ │
│           │                                     │ └─────────────────────────────────────────────────────────────────────────────────────────────────────┘ │
╰───────────┴─────────────────────────────────────┴─────────────────────────────────────────────────────────────────────────────────────────────────────────╯
```

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
