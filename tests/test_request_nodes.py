#!/usr/bin/env python3
"""Request nodes vs the reference grouping: tests/fixtures/stock_request_nodes.py (the
request-grouping rules of the stock dongle) against gen_tables.build_requests_py (the Python
mirror of request_builder.cpp) on the poll tables of data/families/*.yaml, node by node
(fn, start, count, cycle). The C++ builder is checked against the same node counts in
tests/test_datamodel.cpp (request_stock_node_counts).
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import datalib  # noqa: E402
import gen_tables as g  # noqa: E402

spec = importlib.util.spec_from_file_location(
    "stock_request_nodes", os.path.join(HERE, "fixtures", "stock_request_nodes.py"))
srn = importlib.util.module_from_spec(spec)
spec.loader.exec_module(srn)

# Node counts (must match test_datamodel.cpp; inv_proto3 reads the full u32 at 13041, see the
# `fixes` of data/families/inverter.yaml).
EXPECTED = {"inv_proto2": 54, "inv_proto3": 54, "inv_proto1_4": 18}


def main():
    data = datalib.load_register_data()
    total = 0
    for t in data["tables"]:
        if t.get("export"):
            continue  # Modbus-TCP export maps, never polled
        rows = srn.load(t["name"], data)
        stock = [(fn, n["start"], n["count"], n["key"]) for fn in (3, 4)
                 for n in srn.stock(rows, fn)]
        ours = [(r["fn"], r["start"], r["count"], r["group"]) for r in g.build_requests_py(t["rows"])]
        assert stock == ours, "%s: reference %d nodes, ours %d; first diff %s" % (
            t["name"], len(stock), len(ours),
            next(((a, b) for a, b in zip(stock, ours) if a != b), None))
        if t["name"] in EXPECTED:
            assert len(ours) == EXPECTED[t["name"]], (t["name"], len(ours))
        total += len(ours)
    print("PASS request nodes identical to the reference grouping (%d nodes over the poll tables)" % total)


if __name__ == "__main__":
    main()
