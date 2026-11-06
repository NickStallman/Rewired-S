#!/usr/bin/env python3
"""Host unit tests for the pure helpers in tools/fake_iconfig_server.py (no sockets).

    python3 -I tests/test_fake_iconfig_server.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import fake_iconfig_server as fs


def check(name, cond):
    if not cond:
        raise AssertionError("FAILED: " + name)
    print("ok:", name)


def test_route_name():
    check("v2 path (P033 / P044)", fs.route_name("/v2/web/select_packages") == "select_packages")
    check("bare path (P016)", fs.route_name("/web/get_packages") == "get_packages")
    check("trailing slash", fs.route_name("/v2/web/get_time/") == "get_time")
    check("prefixed deployment", fs.route_name("/iconfig/v2/web/result") == "result")
    check("other path", fs.route_name("/v2/web/other") is None)
    check("not under /web/", fs.route_name("/select_packages") is None)
    check("substring is not enough", fs.route_name("/web/xselect_packages") is None)


def test_dev_model_from():
    check("query", fs.dev_model_from("dev_model=WiNet-S2&page=1") == "WiNet-S2")
    check("P016 array string", fs.dev_model_from("dev_model=%5B%22SH15T%22%5D") == "SH15T")
    check("form body", fs.dev_model_from("", b"page=1&dev_model=WiNet-S") == "WiNet-S")
    check("json body", fs.dev_model_from("", b'{"dev_model": "SH15T"}') == "SH15T")
    check("json array body", fs.dev_model_from("", b'{"dev_model": ["SH10RS"]}') == "SH10RS")
    check("query wins over body", fs.dev_model_from("dev_model=WiNet-S", b"dev_model=SH15T") == "WiNet-S")
    check("absent", fs.dev_model_from("page=1", b"") == "")
    check("garbage body", fs.dev_model_from("", b"\xff\xfe") == "")


def test_is_dongle():
    check("WiNet-S", fs.is_dongle("WiNet-S"))
    check("WiNet-S2", fs.is_dongle("WiNet-S2"))
    check("lower case", fs.is_dongle("winet"))
    check("inverter refused", not fs.is_dongle("SH15T"))
    check("empty refused", not fs.is_dongle("") and not fs.is_dongle(None))


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("\nALL fake_iconfig_server host tests passed")


if __name__ == "__main__":
    main()
