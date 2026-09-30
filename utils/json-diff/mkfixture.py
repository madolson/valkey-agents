#!/usr/bin/env python3
"""Write a module-written JSON RDB fixture and its expected observable state.

Usage: mkfixture.py <valkey-server> <libjson.so> <out.rdb> <out.expected>

The expected file has one line per key: the key name, a tab, then the raw
JSON.GET <key> $ reply from the module. JSON output escapes tabs and newlines,
so the tab is an unambiguous separator.
"""

import os
import shutil
import sys
import tempfile

import jsondiff
import resp

DEEP128 = "[" * 128 + "]" * 128
DEEPOBJ = '{"a":' * 127 + "0" + "}" * 127

KEYS = [
    ("scalar:true", "true"),
    ("scalar:false", "false"),
    ("scalar:null", "null"),
    ("scalar:int", "42"),
    ("scalar:negint", "-7"),
    ("scalar:float", "3.14159"),
    ("scalar:str", '"hello"'),
    ("scalar:empty-str", '""'),
    ("num:1E2", "1E2"),
    ("num:1e2", "1e2"),
    ("num:-0.0", "-0.0"),
    ("num:-0", "-0"),
    ("num:1.50", "1.50"),
    ("num:0.3", "0.30000000000000004"),
    ("num:u64max", "18446744073709551615"),
    ("num:i64min", "-9223372036854775808"),
    ("num:9e308", "9e308"),
    ("num:401-digits", "1" + "0" * 400),
    ("num:5e-324", "5e-324"),
    ("num:array", "[1E2,1e2,-0.0,1.0,0.1,2e22,1e-7,123.456e1,18446744073709551616,1.7976931348623157e308]"),
    ("empty:obj", "{}"),
    ("empty:arr", "[]"),
    ("nested", '{"a":{"b":[1,{"c":[true,false,null]},"x"]},"d":{"e":{"f":{}}}}'),
    ("mixed-array", '[1,"two",3.0,true,null,{"k":"v"},[[],[[]]]]'),
    ("deep:128-arrays", DEEP128),
    ("deep:127-objects", DEEPOBJ),
    ("obj:33-members", "{" + ",".join('"m%d":%d' % (i, i) for i in range(33)) + "}"),
    ("obj:300-members", "{" + ",".join('"f%d":"v%d"' % (i, i) for i in range(300)) + "}"),
    ("obj:duplicates", '{"a":1,"b":2,"a":3}'),
    ("arr:5000", "[" + ",".join(str(i) for i in range(5000)) + "]"),
    ("big:doc", "[" + ",".join('{"id":%d,"name":"item-%d","tags":["t%d","u"],"price":%d.%02d,'
                                 '"ok":%s}' % (i, i, i % 7, i, i % 100, "true" if i % 2 else "false")
                                 for i in range(1500)) + "]"),
    ("str:escapes", '"\\"q\\" \\\\ \\/ \\b\\f\\n\\r\\t \\u0000 \\u001f \\u007f"'),
    ("str:unicode-escaped", '"\\u00e9 \\u65e5\\u672c \\ud83d\\ude00 \\uFFFF"'),
    ("str:unicode-raw", '"\u00e9 \u65e5\u672c\u8a9e \U0001f600"'),
    ("str:long", '"' + "abcdefghij" * 2000 + '"'),
    ("members:odd-names", '{"":0,"a.b":1,"a b":2,"$":3,"*":4,"[0]":5,"~":6,"\u00e9":7,'
                          '"\\u0000":8,"\\"":9,"\U0001f600":10}'),
    ("\u00e9t\u00e9:\U0001f600", '{"unicode key name":true}'),
    ("ws:formatted", ' {\n  "a" : [ 1 , 2 ] ,\t"b" : { } \r\n} '),
]

# Keys built by commands, so the stored member order and number text come from
# mutations rather than the initial parse.
MUTATIONS = [
    ("mut:order", [["JSON.SET", "mut:order", "$", "{" + ",".join('"k%d":%d' % (i, i) for i in range(40)) + "}"],
                   ["JSON.DEL", "mut:order", "$.k3"], ["JSON.DEL", "mut:order", "$.k20"],
                   ["JSON.SET", "mut:order", "$.k3", '"re-added"'],
                   ["JSON.SET", "mut:order", "$.new", "[]"],
                   ["JSON.SET", "mut:order", "$.k0", '"updated"']]),
    ("mut:arith", [["JSON.SET", "mut:arith", "$", '{"i":1,"f":1.5,"e":1E2,"z":0}'],
                   ["JSON.NUMINCRBY", "mut:arith", "$.i", "2"],
                   ["JSON.NUMINCRBY", "mut:arith", "$.f", "0.1"],
                   ["JSON.NUMMULTBY", "mut:arith", "$.e", "3"],
                   ["JSON.NUMINCRBY", "mut:arith", "$.z", "0.30000000000000004"]]),
    ("mut:arrays", [["JSON.SET", "mut:arrays", "$", '{"a":[1,2,3],"b":["x"]}'],
                    ["JSON.ARRAPPEND", "mut:arrays", "$.a", "4", '"five"'],
                    ["JSON.ARRINSERT", "mut:arrays", "$.a", "0", "0"],
                    ["JSON.ARRPOP", "mut:arrays", "$.a", "2"],
                    ["JSON.ARRTRIM", "mut:arrays", "$.b", "0", "0"],
                    ["JSON.STRAPPEND", "mut:arrays", "$.b[0]", '"\\u00e9"'],
                    ["JSON.TOGGLE", "mut:arrays", "$.t"],
                    ["JSON.SET", "mut:arrays", "$.t", "true"],
                    ["JSON.TOGGLE", "mut:arrays", "$.t"]]),
    ("mut:cleared", [["JSON.SET", "mut:cleared", "$", '{"a":[1],"b":{"c":1},"n":5,"s":"x"}'],
                     ["JSON.CLEAR", "mut:cleared", "$.*"]]),
]


def check(ok, msg):
    if not ok:
        raise SystemExit("error: " + msg)


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    server, module, out_rdb, out_expected = sys.argv[1:]
    work = tempfile.mkdtemp(prefix="json-fixture-")
    s = jsondiff.Server(server, module, "A", work)
    try:
        c = resp.Conn(*s.addr)
        c.call([b"FLUSHALL"])
        names = []
        for key, doc in KEYS:
            r = c.call(["JSON.SET", key, "$", doc])
            check(r == b"+OK\r\n", "JSON.SET %s -> %r" % (key, r))
            names.append(key)
        for key, cmds in MUTATIONS:
            for args in cmds:
                r = c.call(args)
                check(not resp.is_error(r), "%r -> %r" % (args, r))
            names.append(key)

        encvers = set()
        for key in names:
            d = c.call([b"DUMP", key.encode()])
            payload = d[d.index(b"\r\n") + 2:]
            # RDB_TYPE_MODULE_2, then the 64-bit module id as a length (0x81 + 8 bytes BE).
            check(payload[0] == 7 and payload[1] == 0x81, "unexpected DUMP header for %s" % key)
            mid = int.from_bytes(payload[2:10], "big")
            encvers.add(mid & 1023)
        check(encvers == {3}, "module encoding versions %r, expected {3}" % encvers)

        lines = []
        for key in names:
            r = c.call([b"JSON.GET", key.encode(), b"$"])
            check(r.startswith(b"$"), "JSON.GET %s -> %r" % (key, r[:80]))
            body = r[r.index(b"\r\n") + 2:-2]
            check(b"\t" not in body and b"\n" not in body and b"\t" not in key.encode(),
                  "separator inside %s" % key)
            lines.append(key.encode() + b"\t" + body + b"\n")

        check(c.call([b"SAVE"]) == b"+OK\r\n", "SAVE failed")
        c.close()
        shutil.copyfile(os.path.join(s.dir, "dump.rdb"), out_rdb)
        with open(out_expected, "wb") as f:
            f.writelines(lines)
        sys.stdout.write("wrote %d keys (module encoding version 3) to %s and %s\n"
                         % (len(names), out_rdb, out_expected))
    finally:
        s.stop()
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
