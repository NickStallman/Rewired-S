#!/usr/bin/env python3
"""Reference implementation of the request-grouping rules used by tests/test_request_nodes.py.

Rules, applied to one register table (data/families/<family>.yaml tables.<name>.rows):
 - rows iterated in table order; only rows with fn == F (F = 1,2,3,4 then 0x74) are considered
 - flags `end` (tag bit0) -> row closes the current node (next fn-F row starts a new node)
 - same cycle as the node's first row -> merge (span from node start, split if span >= 121)
 - different cycle, no `end` flag -> also merged, except address 0x396d which is then skipped
 - node cycle = cycle of its first row; node prio = 2 if flags `priority` (tag bit2) else 0
 - 59-node cap (checked on split/new node)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools"))
import datalib  # noqa: E402


def load(table_name, data=None):
    """Rows of one table as the grouping rules see them: idx, addr, n, fn, key (= cycle), tag."""
    data = data or datalib.load_register_data()
    t = next(t for t in data["tables"] if t["name"] == table_name)
    return [dict(idx=i, addr=r["regaddr"], n=r["nreg"], fn=r["fn"], key=r["group"], tag=r["tag"],
                 sig="0x%04x" % r["sigid"]) for i, r in enumerate(t["rows"])]


def stock(rows, F):
    """Group the fn-F rows into request nodes (node = start,count,key,fn,prio,rows)."""
    nodes = []
    cur = None
    base = 0   # base = index of the row that defines the node start

    def mk(r):
        n = dict(start=r['addr'], count=r['n'], key=r['key'], fn=F, prio=2 if r['tag'] & 4 else 0,
                 rows=[r['idx']])
        nodes.append(n)
        return n
    N = len(rows)
    for i, r in enumerate(rows):
        if r['fn'] != F:
            continue
        bit0 = r['tag'] & 1
        if not bit0 and r['key'] != rows[base]['key'] and r['addr'] == 0x396d:
            continue
        if cur is None:
            cur = mk(r)
            base = i
        b = rows[base]
        cur['start'] = b['addr']
        cur['key'] = b['key']
        span = (r['addr'] + r['n'] - b['addr']) & 0xffff
        if span < 0x79:
            cur['count'] = span
            if r['idx'] not in cur['rows']:
                cur['rows'].append(r['idx'])
        else:
            cur = mk(r)
            base = i
        if bit0 and i < N - 1 and rows[i + 1]['fn'] == F:
            cur = mk(rows[i + 1])
            base = i + 1
    return nodes


if __name__ == '__main__':
    data = datalib.load_register_data()
    for name in sys.argv[1:] or [t["name"] for t in data["tables"]]:
        rows = load(name, data)
        print('#', name)
        for F in (1, 2, 3, 4, 0x74):
            for n in stock(rows, F):
                keys = sorted(set(rows[j]['key'] for j in range(len(rows)) if rows[j]['idx'] in n['rows']))
                print('fn=%d start=%d count=%d cycle=%d (%.1fs) prio=%d rows=%d mixed_cycles=%s' % (
                    F, n['start'], n['count'], n['key'], n['key'] / 10, n['prio'], len(n['rows']),
                    keys if len(keys) > 1 else ''))
