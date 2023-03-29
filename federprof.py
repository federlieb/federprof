# TODO: error handling if no queries are executed.

import duckdb
import rich
from rich.table import Table
from rich.console import Console
from rich import box
from rich.text import Text
from rich.syntax import Syntax
import json

typemap = {
    "DOUBLE": str,
    "HUGEINT": str,
    "VARCHAR": str,
}

db = duckdb.connect(":memory:")

db.execute(
    """

create or replace macro plan_md5(scanstatus) as
md5(
    list_transform(
        scanstatus,
        x -> json_array(x.idx, x.selectid, x.parentid, x.name, x.explain)
    )
)

"""
)

db.execute(
    """

create or replace macro plan_agg(plans) as (
    with
    l1 as ( select unnest(plans) as plan ),
    l2 as ( select unnest(plan) as item from l1 ),
    l3 as (
        select {
            'idx':      item.idx,
            'selectid': item.selectid,
            'parentid': item.parentid,
            'name':     item.name,
            'explain':  item.explain,
            'nloop':    sum(case when item.nloop <> -1 then item.nloop else null end),
            'nvisit':   sum(case when item.nvisit <> -1 then item.nvisit else null end),
            'est':      sum(case when item.est <> -1 then item.est else null end),
            'ncycle':   sum(case when item.ncycle <> -1 then item.ncycle else null end),
        } as x
        from l2
        group by item.idx, item.selectid, item.parentid, item.explain, item.name
    ),
    l4 as (
        select {
            'idx':      x.idx,
            'selectid': x.selectid,
            'parentid': x.parentid,
            'name':     x.name,
            'explain':  x.explain,
            'nloop':    x.nloop,
            'nvisit':   x.nvisit,
            'est':      x.est,
            'ncycle':   x.ncycle,
            'ncycle_p': printf( '%2.2f%%', 100*((1.0 * x.ncycle) / sum(x.ncycle) over ()) ),
            'nvisit_p': printf( '%2.2f%%', 100*((1.0 * x.nvisit) / sum(x.nvisit) over ()) ),
        } as x
        from l3
    )
    select list(x order by x.idx) from l4 group by null
)

"""
)


result = duckdb.sql(
    query="""

with base as (
    select
        cast(sum(vm_step) as int128) as "sum(vm_step)",
        cast(sum(run) as int128) as "sum(run)",
        sum(took_ns) * 1.0e-9 as "sum(took_seconds)",
        sum(list_aggregate(list_transform(scanstatus, x -> x.nvisit), 'sum'))
            as "sum(nvisit)",
        sum(list_aggregate(list_transform(scanstatus, x -> x.ncycle), 'sum'))
            as "sum(ncycle)",
        sum(list_aggregate(list_transform(scanstatus, x -> x.est), 'sum'))
            as "sum(est)",
        sum(list_aggregate(list_transform(scanstatus, x -> x.nloop), 'sum'))
            as "sum(nloop)",
        plan_md5(scanstatus) as plan,
        unexpanded,
        to_json(plan_agg(list(scanstatus))) as scanstatus,
        sum(fullscan_step),
        sum(sort),
        sum(autoindex),
        sum(reprepare),
        sum(filter_miss),
        sum(filter_hit),
        avg(memused),
    from
        read_ndjson_auto('logfile.gz', ignore_errors=true)
    where
        event = 2
    group by
        unexpanded,
        plan
    order by
        "sum(vm_step)" desc nulls last
    limit 40
)
select
    *
from
    base
order by
    "sum(vm_step)" asc nulls first

    """,
    connection=db,
)


def reorder_eqp(x, node, depth, next_pos):

    children = sorted(
        filter(lambda y: y.get("parentid") == node.get("selectid"), x),
        key=lambda y: y.get("idx"),
    )

    for pos, child in enumerate(children):
        child["posy"] = pos
        child["posx"] = depth + 1
        child["posz"] = next_pos
        next_pos = reorder_eqp(x, child, depth + 1, next_pos + 1)

    return next_pos + 1


def to_rich(value):
    if isinstance(value, int):
        return f"{value:,}"

    if isinstance(value, float):
        return f"{value:.2f}"

    return value


def scanstatus_to_table(ss):

    if ss is None:
        return None

    data = json.loads(ss)

    reorder_eqp(data, {"selectid": 0}, 0, 0)

    t = Table(box=box.SIMPLE_HEAD, show_lines=False, min_width=120, highlight=True)

    for cell in data:
        cell["explain"] = ("  " * (cell.get("posx") - 1)) + cell["explain"]
        cell["explain"] = Text(cell["explain"], overflow="ellipsis", no_wrap=True)
        details = "  "

        if cell.get("nloop"):
            details += f" nloop={to_rich(int(cell['nloop']))}"

        if cell.get("nvisit"):
            details += f" nvisit={to_rich(int(cell['nvisit']))}"

        if cell.get("est"):
            details += f" est={to_rich(int(cell['est']))}"

        cell["explain"].append(details, style="dim")

        for k, v in cell.items():
            if v is None:
                cell[k] = 'None'

    order = [
        # "idx",
        # "nloop",
        # "nvisit",
        # "est",
        # "name",
        # "selectid",
        # "parentid",
        "ncycle_p",
        "nvisit_p",
        "explain",
    ]

    for col in order:
        t.add_column(col)

    for node in sorted(data, key=lambda y: (y.get("posz"))):
        t.add_row(*[to_rich(node.get(x)) for x in order])

    t.columns[0].justify = "right"
    t.columns[1].justify = "right"

    t.columns[0].width = 8
    t.columns[1].width = 8
    t.columns[2].width = 74

    return t


t = Table(box=box.ROUNDED, show_lines=True, highlight=True)

selection = (
    "sum(took_seconds)",
    "stats",
    "sql+eqp",
)

for col in selection:
    t.add_column(col)

for row in result.fetchall():

    data = dict(zip(result.columns, [to_rich(x) for x in row]))
    type = dict(zip(result.columns, result.dtypes))

    data["unexpanded"] = Syntax(
        data["unexpanded"],
        "sql",
        dedent=True,
        word_wrap=True,
        background_color="#1e1e1e",
    )

    data["scanstatus"] = scanstatus_to_table(
        data["scanstatus"]
    )

    sql_eqp = Table(show_header=False)
    sql_eqp.add_row(data["unexpanded"])
    sql_eqp.add_row(data["scanstatus"])

    data["sql+eqp"] = sql_eqp

    stats = Table(show_header=False)

    sel2 = (
        'sum(vm_step)',
        'sum(ncycle)',
        'sum(run)',
        'sum(fullscan_step)',
        'sum(sort)',
        'sum(autoindex)',
        'sum(reprepare)',
        'sum(filter_miss)',
        'sum(filter_hit)',
        'avg(memused)',
        'sum(nvisit)',
        'sum(est)',
        'sum(nloop)',
        'sum(took_seconds)'
    )

    for h in sel2:
        stats.add_row(h, data[h])

    data["stats"] = stats

    t.add_row(*[data[k] for k in selection])

t.columns[2].style = rich.style.Style(bgcolor="#1e1e1e")
t.columns[2].vertical = "middle"
t.columns[1].vertical = "middle"
t.columns[0].vertical = "middle"
t.columns[0].justify = "center"

Console().print(t)
