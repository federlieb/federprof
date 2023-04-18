# TODO: error handling if no queries are executed.

import duckdb
import rich
from rich.table import Table
from rich.console import Console
from rich import box
from rich.text import Text
from rich.syntax import Syntax
import json
import itertools

typemap = {
    "DOUBLE": str,
    "HUGEINT": str,
    "VARCHAR": str,
}

db = duckdb.connect(":memory:")

db.execute(
    """

create or replace macro plan_agg(plans) as (
    with
    l1 as ( select unnest(plans) as plan ),
    l2 as ( select unnest(plan) as item from l1 ),
    l3 as (
        select
            {
                'idx':      item.idx,
                'selectid': item.selectid,
                'parentid': item.parentid,
                'name':     item.name,
                'explain':  item.explain,
                'nloop':    sum(case when item.nloop <> -1 then item.nloop else null end),
                'nvisit':   sum(case when item.nvisit <> -1 then item.nvisit else null end),
                -- TODO: does not really make sense to sum est?
                'est':      sum(case when item.est <> -1 then item.est else null end),
                'ncycle':   sum(case when item.ncycle <> -1 then item.ncycle else null end),
            } as x
        from
            l2
        group by
            item.idx,
            item.selectid,
            item.parentid,
            item.explain,
            item.name
    )
    select
        list(x order by x.idx)
    from
        l3
    group by
        null
)

"""
)


result = duckdb.sql(
    query="""

with
agg1 as (
    select
        sum(cast(vm_step as int128))       as "vm_step",
        sum(cast(run as int128))           as "run",
        sum(took_ns) * 1.0e-9              as "took_seconds",
        unexpanded                         as "unexpanded",
        plan_agg(list(scanstatus))         as "scanstatus",
        sum(cast(fullscan_step as int128)) as "fullscan_step",
        sum(cast(sort as int128))          as "sort",
        sum(cast(autoindex as int128))     as "autoindex",
        sum(cast(reprepare as int128))     as "reprepare",
        sum(cast(filter_miss as int128))   as "filter_miss",
        sum(cast(filter_hit as int128))    as "filter_hit",
    from
        read_ndjson_auto('logfile.gz', ignore_errors=true)
    group by
        unexpanded,
        plan
)
select
    agg1.*,
    list_aggregate(list_transform(agg1.scanstatus, x -> x.nloop), 'sum')  as "nloop",
    list_aggregate(list_transform(agg1.scanstatus, x -> x.nvisit), 'sum') as "nvisit",
    list_aggregate(list_transform(agg1.scanstatus, x -> x.ncycle), 'sum') as "ncycle",
    list_aggregate(list_transform(agg1.scanstatus, x -> x.est), 'sum')    as "est",
from
    agg1
order by
    vm_step desc nulls last
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


def scanstatus_to_table(data):

    if data is None:
        return None

    reorder_eqp(data, {"selectid": 0}, 0, 0)

    t = Table(box=box.SIMPLE, show_lines=False, min_width=120, highlight=True, show_footer=True)

    ncycle_sum = 0
    nvisit_sum = 0

    for cell in data:
        cell["explain"] = ("  " * (cell.get("posx") - 1)) + cell["explain"]
        cell["explain"] = Text(cell["explain"], overflow="ellipsis", no_wrap=True)
        details = "  "

        if cell.get("nloop"):
            details += f" nloop={to_rich(int(cell['nloop']))}"

        if cell.get("nvisit"):
            details += f" nvisit={to_rich(int(cell['nvisit']))}"
            nvisit_sum += int(cell['nvisit'])

        if cell.get("ncycle"):
            ncycle_sum += int(cell['ncycle'])

        if cell.get("est"):
            details += f" est={to_rich(int(cell['est']))}"

        cell["explain"].append(details, style="dim")

    for cell in data:
        cell["ncycle %"] = None
        if cell.get("ncycle") is not None:
            cell["ncycle %"] = 100.0 * int(cell.get("ncycle")) / ncycle_sum if ncycle_sum > 0 else None

        cell["nvisit %"] = None
        if cell.get("nvisit") is not None:
            cell["nvisit %"] = 100.0 * int(cell.get("nvisit")) / nvisit_sum if nvisit_sum > 0 else None

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
        "ncycle %",
        "nvisit %",
        "explain",
    ]

    t.add_column("ncycle %", to_rich(ncycle_sum))
    t.add_column("nvisit %", to_rich(nvisit_sum))
    t.add_column("explain", "")

    for node in sorted(data, key=lambda y: (y.get("posz"))):
        t.add_row(*[to_rich(node.get(x)) for x in order])

    t.columns[0].justify = "right"
    t.columns[1].justify = "right"

    t.columns[0].width = 10
    t.columns[1].width = 10
    t.columns[2].width = 74

    return t


t = Table(box=box.ROUNDED, show_lines=True, highlight=True)

selection = (
    "stats",
    "sql+eqp",
)

for col in selection:
    t.add_column(col)

for row in reversed(list(itertools.islice(result.fetchall(), 40))):

    data = dict(zip(result.columns, [(x) for x in row]))
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

    stats = Table(show_header=True, box=box.SIMPLE)
    stats.add_column("metric")
    stats.add_column("sum")

    sel2 = (
        'vm_step',
        'took_seconds',
        'run',
        'fullscan_step',
        'sort',
        'autoindex',
        'reprepare',
        'filter_miss',
        'filter_hit',
        'nloop',
        'nvisit',
        'ncycle',
        'est',
    )

    for h in sel2:
        stats.add_row(h, to_rich(data[h]))

    data["stats"] = stats

    t.add_row(*[data[k] for k in selection])

t.columns[1].style = rich.style.Style(bgcolor="#1e1e1e")
t.columns[1].vertical = "middle"
t.columns[0].vertical = "middle"
# t.columns[0].vertical = "middle"
# t.columns[0].justify = "center"

Console().print(t)
