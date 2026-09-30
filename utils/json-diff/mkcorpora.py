#!/usr/bin/env python3
"""Writes the targeted corpora in corpora/ (all but generated-seed1.jsonl).

Usage: mkcorpora.py [OUTDIR]

Edit the cases here and rerun rather than editing the .jsonl files, so the
JSON-inside-JSON escaping stays correct.
"""
import json
import os
import sys

import corpus

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "corpora")


class F(object):
    def __init__(self, name, header):
        self.f = open(os.path.join(OUT, name), "w", encoding="utf-8")
        for h in header.strip().splitlines():
            self.f.write("# " + h + "\n")
        self.c(["FLUSHALL"])

    def c(self, *cmds):
        for args in cmds:
            self.f.write(corpus.dump_line(args) + "\n")

    def note(self, s):
        self.f.write("\n# " + s + "\n")

    def close(self):
        self.f.close()


ALL_READ = ["JSON.GET", "JSON.TYPE", "JSON.STRLEN", "JSON.ARRLEN", "JSON.OBJKEYS", "JSON.OBJLEN",
            "JSON.RESP", "JSON.DEBUG FIELDS", "JSON.DEBUG MEMORY"]
ALL_WRITE = ["JSON.DEL", "JSON.FORGET", "JSON.CLEAR", "JSON.TOGGLE", "JSON.ARRPOP"]


def each(key, path, which):
    out = []
    for c in which:
        parts = c.split()
        out.append(parts + [key, path])
    return out


# ---------------------------------------------------------------- numbers
f = F("numbers.jsonl", """
Number text: round trips through SET/GET/RESP/MGET, arithmetic typing and
overflow, and the RapidJSON exponent guard (9e308 accepted, 0e309 rejected).
""")
NUMS = ["0", "-0", "-0.0", "0.0", "1E2", "1e2", "1e+2", "1E-2", "1.0", "1.50", "0.1",
        "0.30000000000000004", "3.14159", "123.456e1", "2e22", "1e21", "1e-7",
        "9223372036854775807", "9223372036854775808", "-9223372036854775808",
        "-9223372036854775809", "18446744073709551615", "18446744073709551616",
        "123456789012345678901234567890", "9e308", "1.7976931348623157e308",
        "1.7976931348623159e308", "5e-324", "1e-400", "2.2250738585072014e-308",
        "1" + "0" * 308, "1" + "0" * 309, "1" + "0" * 400, "0." + "0" * 400 + "1",
        "1e0309", "0e309", "1e309", "-1e309", "0e-309", "0e-400", "1e-9999", "0e99999",
        "01", "-01", ".5", "1.", "+1", "1e", "1e+", "-", "NaN", "Infinity", "-Infinity", "0x10",
        "1_000", "--1", "1 ", " 1", "1 2"]
for i, n in enumerate(NUMS):
    f.c(["JSON.SET", "n", "$", n], ["JSON.GET", "n"], ["JSON.GET", "n", "$"],
        ["JSON.RESP", "n"], ["JSON.TYPE", "n"], ["JSON.TYPE", "n", "$"])
f.note("values that round to infinity; JSONPath arithmetic only, legacy-path NUMMULTBY by 0")
f.note("asserts in valkey-json 1.0.3")
for n in ["9e308", "1.7976931348623159e308", "1" + "0" * 309, "1" + "0" * 400, "-9e308"]:
    for cmd, op in [("JSON.NUMINCRBY", "0"), ("JSON.NUMINCRBY", "1"), ("JSON.NUMINCRBY", "-1e308"),
                    ("JSON.NUMMULTBY", "0"), ("JSON.NUMMULTBY", "1"), ("JSON.NUMMULTBY", "-1"),
                    ("JSON.NUMMULTBY", "0.5")]:
        f.c(["JSON.SET", "h", "$", "[%s]" % n], [cmd, "h", "$[0]", op], ["JSON.GET", "h"],
            ["JSON.RESP", "h"], ["JSON.SET", "h", "$", n], [cmd, "h", "$", op], ["JSON.GET", "h"])
    f.c(["JSON.SET", "h", "$", "[%s]" % n], ["JSON.NUMINCRBY", "h", "[0]", "1"],
        ["JSON.NUMMULTBY", "h", "[0]", "2"], ["JSON.GET", "h"])
f.note("numbers inside containers, re-emitted verbatim")
f.c(["JSON.SET", "c", "$", "[" + ",".join(NUMS[:32]) + "]"], ["JSON.GET", "c"],
    ["JSON.GET", "c", "$[*]"], ["JSON.RESP", "c"], ["JSON.MGET", "c", "n", "$[0]"],
    ["JSON.GET", "c", "INDENT", "  ", "NEWLINE", "\n", "SPACE", " "])
f.c(["JSON.SET", "o", "$", "{" + ",".join('"f%d":%s' % (i, n) for i, n in enumerate(NUMS[:32])) + "}"],
    ["JSON.GET", "o"], ["JSON.GET", "o", "$.*"], ["JSON.RESP", "o"])
f.note("arithmetic result typing follows the operand literal")
BASES = ["0", "1", "-1", "1.0", "1E2", "1.5", "-0.0", "9223372036854775807",
         "-9223372036854775808", "18446744073709551615", "1e308", "0.1", "3"]
OPS = ["1", "-1", "0", "1.0", "1E0", "0.5", "-0.5", "2", "1e2", "0.2", "1e308", "-1e308",
       "1e309", "9223372036854775807", "-9223372036854775808", "18446744073709551615",
       "1e-400", "abc", "\"1\"", "[1]", "null", "true", "", " 1", "1 "]
for b in BASES:
    for op in OPS:
        for cmd in ("JSON.NUMINCRBY", "JSON.NUMMULTBY"):
            f.c(["JSON.SET", "a", "$", '{"v":%s,"w":[%s,"s",%s]}' % (b, b, b)],
                [cmd, "a", "$.v", op], [cmd, "a", ".v", op], ["JSON.GET", "a"])
    f.c(["JSON.SET", "a", "$", '{"v":%s,"w":[%s,"s",%s]}' % (b, b, b)],
        ["JSON.NUMINCRBY", "a", "$.w[*]", "1"], ["JSON.NUMMULTBY", "a", "$..*", "2"],
        ["JSON.NUMINCRBY", "a", ".w[1]", "1"], ["JSON.GET", "a"], ["JSON.RESP", "a"])
f.note("repeated arithmetic drifting through float formatting")
f.c(["JSON.SET", "r", "$", "0"])
for _ in range(12):
    f.c(["JSON.NUMINCRBY", "r", "$", "0.1"])
f.c(["JSON.GET", "r"], ["JSON.SET", "r", "$", "1"])
for _ in range(70):
    f.c(["JSON.NUMMULTBY", "r", ".", "3"])
f.c(["JSON.GET", "r"], ["JSON.SET", "r", "$", "7"])
for op in ["1e300", "1e300", "1e300", "-1e300"]:
    f.c(["JSON.NUMMULTBY", "r", "$", op], ["JSON.GET", "r"])
f.c(["JSON.SET", "r", "$", "9223372036854775806"], ["JSON.NUMINCRBY", "r", "$", "1"],
    ["JSON.NUMINCRBY", "r", "$", "1"], ["JSON.GET", "r"], ["JSON.NUMINCRBY", "r", "$", "-1"],
    ["JSON.GET", "r"])
f.note("ARRINDEX number matching across representations")
f.c(["JSON.SET", "x", "$", "[1,1.0,1E0,10E-1,2,\"1\",true,null,-0,0,-0.0,18446744073709551615]"])
for v in ["1", "1.0", "1E0", "0.1E1", "2", "\"1\"", "true", "null", "0", "-0", "-0.0", "0.0",
          "18446744073709551615", "1.8446744073709552e19", "3"]:
    f.c(["JSON.ARRINDEX", "x", "$", v], ["JSON.ARRINDEX", "x", ".", v])
f.close()

# ---------------------------------------------------------------- paths
f = F("paths.jsonl", """
Legacy '.' paths vs '$' JSONPath: wildcards, recursive descent, slices, unions,
filters, wrong-typed steps, empty paths and unmatched paths, per command.
""")
DOC = json.dumps({
    "store": {"book": [
        {"category": "reference", "author": "Nigel Rees", "title": "Sayings", "price": 8.95},
        {"category": "fiction", "author": "Evelyn Waugh", "title": "Sword", "price": 12.99},
        {"category": "fiction", "author": "Herman Melville", "title": "Moby Dick",
         "isbn": "0-553-21311-3", "price": 8.99},
        {"category": "fiction", "author": "J. R. R. Tolkien", "title": "LOTR",
         "isbn": "0-395-19395-8", "price": 22.99}],
        "bicycle": {"color": "red", "price": 19.95, "inStock": True}},
    "a": {"a": {"a": 1}, "b": [1, 2, 3]}, "arr": [1, [2, [3, [4]]], {"x": 5}],
    "s": "str", "n": 1.5, "i": 42, "t": True, "f": False, "z": None,
    "e": {}, "ea": [], "a.b": "dotted", "a b": "space", "é": "accent", "": "empty",
    "q'": "squote", "q\"": "dquote", "~": "tilde", "*": "star", "$": "dollar",
})
PATHS = [
    "", ".", "$", "..", "$..", "$.", "..*", "$..*", "*", ".*", "$.*", "$[*]", "[*]",
    "store", ".store", "$.store", "store.book", ".store.book[0]", "$.store.book[0]",
    ".store.book[-1]", "$.store.book[-1]", ".store.book[4]", "$.store.book[4]", "$.store.book[-5]",
    ".store.book[0].price", "$.store.book[*].price", "$.store.book[*].author", "$..author",
    "$..price", "..price", "$..book[2]", "$..book[-1:]", "$..book[0,1]", "$..book[:2]",
    "$..book[1:3]", "$..book[::2]", "$..book[::-1]", "$..book[3:1:-1]", "$..book[1:2:0]",
    "$..book[-100:100]", "$..book[+1]", "$..book[ 1 ]", "$..book[ 0 , 1 ]",
    "$..book[?(@.isbn)]", "$..book[?(@.price<10)]", "$..book[?(@.price <= 8.99)]",
    "$..book[?(@.price>10 && @.category==\"fiction\")]",
    "$..book[?(@.price>20 || @.category==\"reference\")]", "$..book[?(@.category!=\"fiction\")]",
    "$..book[?(@[\"price\"]==8.95)]", "$..book[?(8.99==@.price)]", "$..book[?(@.price==$.n)]",
    "$..book[?(@.price > $.store.bicycle.price)]", "$..book[?((@.price<9) && (@.isbn))]",
    "$.store.*[?(@.price)]", "$.store.book[*][?(@.price>10)]", "$..*[?(@==1)]",
    "$.arr[*]", "$.arr[1][1][1][0]", ".arr[1][1][1][0]", "$..arr..*", "$.arr..x", "$..[0]",
    "$..[*]", "$.a..a", "$..a", "..a", ".a.a.a", "$.a.a.a", "$.a.a.a.a", ".a.a.a.a",
    "$.a.b[0,2]", "$.a.b[0,5]", "$.a.b[-1,0]", "$.a['a','b']", "$.a[\"a\",\"b\"]",
    "$['store']['book'][0]['title']", "['store']['book'][0]['title']",
    "$[\"a.b\"]", "[\"a.b\"]", "$['a b']", "['a b']", ".a b", "$.a b", "$.é", ".é",
    "$[\"é\"]", "$['']", "[\"\"]", "$[\"q'\"]", "$['q\"']", "$['q\\'']", "$[\"q\\\"\"]",
    "$.~", ".~", "$['*']", "$.$", "$['$']", "$.s", ".s", "$.s.x", ".s.x", "$.s[0]", ".s[0]",
    "$.i.x", ".i.x", "$.z.x", ".z.x", "$.e.x", ".e.x", "$.ea[0]", ".ea[0]", "$.ea[*]",
    "$.a[0]", ".a[0]", "$.arr.x", ".arr.x", "$.missing", ".missing", "missing", "$.missing.x",
    ".missing.x", "$[0]", "[0]", "$.s.*", "$.i[*]", "$..missing", "$.store.book[*].missing",
    "$[", "$.[0]", "$[a]", "$[?@.a]", "$[?(@.a==1]", "$[0", "$['a", "$.a[", "[", "]", "$$",
    ".a.", "a..", "$..*..*", "$[,]", "$[?(@.a=1)]", "$.a[9999999999999999999]", " $", "$ ",
    ". ", "$.store.book[?(@.price<10)].title", "$.store.book[?(@.author=~\"x\")]",
]
f.c(["JSON.SET", "d", "$", DOC])
for p in PATHS:
    for c in ALL_READ:
        f.c(c.split() + ["d", p])
    f.c(["JSON.GET", "d", p, "$.s"], ["JSON.MGET", "d", "missing", p])
f.note("mutations through every path shape; reset the document after each")
MUT = [
    lambda p: ["JSON.SET", "d", p, "\"X\""],
    lambda p: ["JSON.SET", "d", p, "{\"new\":1}", "NX"],
    lambda p: ["JSON.SET", "d", p, "[]", "XX"],
    lambda p: ["JSON.DEL", "d", p],
    lambda p: ["JSON.FORGET", "d", p],
    lambda p: ["JSON.CLEAR", "d", p],
    lambda p: ["JSON.TOGGLE", "d", p],
    lambda p: ["JSON.NUMINCRBY", "d", p, "1"],
    lambda p: ["JSON.NUMMULTBY", "d", p, "2"],
    lambda p: ["JSON.STRAPPEND", "d", p, "\"_x\""],
    lambda p: ["JSON.ARRAPPEND", "d", p, "1", "\"two\""],
    lambda p: ["JSON.ARRINSERT", "d", p, "0", "0"],
    lambda p: ["JSON.ARRINSERT", "d", p, "-1", "\"m\""],
    lambda p: ["JSON.ARRPOP", "d", p],
    lambda p: ["JSON.ARRPOP", "d", p, "0"],
    lambda p: ["JSON.ARRTRIM", "d", p, "1", "2"],
    lambda p: ["JSON.ARRINDEX", "d", p, "1"],
]
MUT_PATHS = ["", ".", "$", "$.*", ".*", "$..*", "$..a", "$.a.*", "$.s.*", "$.i.*", "$.s[*]",
             "$..price", "..price", "$.store.book[*].price", "$.store.book[*].title",
             "$..book[?(@.price<10)]", "$.store.book[0,2]", "$.store.book[1:3]", "$.arr[*]",
             "$.arr..*", "$.missing", ".missing", "$.missing.x", ".missing.x", "$.s.x", ".s.x",
             "$.i[0]", ".i[0]", "$.arr.x", ".arr.x", "$.a[0]", ".a[0]", "$.ea[0]", "$.e.x",
             "$.z.x", ".z", "$.z", "$.t", ".t", "$.f", "$..t", "$.arr[1][1][1]", ".arr[1][1]",
             "$.store.book[9]", ".store.book[9]", "$.store.book[-9]", "$.nomatch[*]",
             "$..nomatch", "$.*.nomatch", "$.e.*", "$.ea[*]", "$.s", ".s", "$.n", ".n",
             "$.store.bicycle", ".store.bicycle.color", "$[\"a.b\"]", "$['']", "$[", ".a."]
MDOC = json.dumps({
    "store": {"book": [{"title": "Sayings", "price": 8.95},
                       {"title": "Sword", "price": 12.99, "isbn": "0-553"}],
              "bicycle": {"color": "red", "price": 19.95}},
    "a": {"a": {"a": 1}, "b": [1, 2]}, "arr": [1, [2, [3, [4]]], {"x": 5}], "s": "str",
    "n": 1.5, "i": 42, "t": True, "f": False, "z": None, "e": {}, "ea": [], "a.b": "d", "": 0})
for p in MUT_PATHS:
    for m in MUT:
        f.c(["JSON.SET", "d", "$", MDOC], m(p), ["JSON.GET", "d"])
f.note("DEL with no path, explicit empty path, root paths")
f.c(["JSON.SET", "d", "$", DOC], ["JSON.DEL", "d", ""], ["JSON.GET", "d"], ["EXISTS", "d"],
    ["JSON.DEL", "d", "$"], ["EXISTS", "d"], ["JSON.SET", "d", "$", DOC], ["JSON.DEL", "d", "."],
    ["EXISTS", "d"], ["JSON.SET", "d", "$", DOC], ["JSON.DEL", "d"], ["EXISTS", "d"],
    ["JSON.DEL", "d"], ["JSON.DEL", "d", "$.x"], ["JSON.FORGET", "d", "."])
f.note("SET creating members, through wildcards that match nothing, new key needs root")
f.c(["JSON.SET", "w", "$", "{\"a\":{},\"b\":[1],\"c\":1}"],
    ["JSON.SET", "w", "$.a.new", "1"], ["JSON.SET", "w", ".a.new2", "2"],
    ["JSON.SET", "w", "$.x.y", "1"], ["JSON.SET", "w", ".x.y", "1"],
    ["JSON.SET", "w", "$.*.z", "3"], ["JSON.SET", "w", "$..q", "4"],
    ["JSON.SET", "w", "$.nomatch[*]", "5"], ["JSON.SET", "w", "$.b[5]", "6"],
    ["JSON.SET", "w", "$.b[-1]", "7"], ["JSON.SET", "w", ".b[1]", "8"],
    ["JSON.SET", "w", "$.c.x", "9"], ["JSON.SET", "w", ".c.x", "9"],
    ["JSON.SET", "w", "$.c[*]", "9"], ["JSON.SET", "w", "$.*", "0"], ["JSON.GET", "w"],
    ["JSON.SET", "nw", "$.a", "1"], ["JSON.SET", "nw", ".a", "1"], ["JSON.SET", "nw", "a", "1"],
    ["JSON.SET", "nw", "", "1"], ["JSON.SET", "nw", "$", "1", "XX"],
    ["JSON.SET", "nw", "$", "1", "NX"], ["JSON.SET", "nw", "$", "2", "NX"],
    ["JSON.SET", "nw", "$", "3", "XX"], ["JSON.GET", "nw"], ["JSON.SET", "nw", "$", "3", "ZZ"],
    ["JSON.SET", "nw", "$.a", "1", "XX"], ["JSON.SET", "nw", "$", "{}"],
    ["JSON.SET", "nw", "$.a", "1", "XX"], ["JSON.SET", "nw", "$.a", "1", "NX"],
    ["JSON.SET", "nw", "$.a", "2", "NX"], ["JSON.SET", "nw", "$.a", "3", "XX"],
    ["JSON.GET", "nw"])
f.note("missing key for every command")
for c in ALL_READ + ALL_WRITE:
    f.c(c.split() + ["nokey"], c.split() + ["nokey", "$"], c.split() + ["nokey", "."])
f.close()

# ---------------------------------------------------------------- limits
f = F("limits.jsonl", """
Nesting at and past 128, path length limits, objects past 32 members
(insertion order through updates, deletes and re-adds), large arrays.
""")
for d in [1, 2, 64, 126, 127, 128, 129, 130, 200, 1000]:
    f.c(["JSON.SET", "deep", "$", "[" * d + "]" * d], ["JSON.DEBUG", "DEPTH", "deep"],
        ["JSON.SET", "deepo", "$", '{"a":' * d + "0" + "}" * d],
        ["JSON.DEBUG", "DEPTH", "deepo"], ["JSON.GET", "deepo", "$" + ".a" * max(d - 1, 0)],
        ["JSON.GET", "deepo", "." + ".".join(["a"] * d)])
f.c(["JSON.SET", "grow", "$", "{}"])
p = "$"
for d in range(1, 132):
    f.c(["JSON.SET", "grow", p + ".a", "{}"])
    p += ".a"
f.c(["JSON.DEBUG", "DEPTH", "grow"], ["JSON.GET", "grow", "$..a" + ".a" * 125])
f.c(["JSON.SET", "grow2", "$", "[" * 127 + "]" * 127],
    ["JSON.ARRAPPEND", "grow2", "$" + "[0]" * 126, "[]"],
    ["JSON.ARRAPPEND", "grow2", "$" + "[0]" * 126, "[[]]"],
    ["JSON.ARRINSERT", "grow2", "$" + "[0]" * 126, "0", "[[]]"],
    ["JSON.SET", "grow2", "$" + "[0]" * 126, "[[1]]"],
    ["JSON.DEBUG", "DEPTH", "grow2"])
f.c(["JSON.GET", "deep", "$" + "[0]" * 200], ["JSON.GET", "deep", "$" + "..a" * 50],
    ["JSON.GET", "deep", "$" + "." * 300], ["JSON.GET", "deep", "$" + "[*]" * 130])
f.note("objects past the 32-member hash threshold keep insertion order")
for n in [31, 32, 33, 34, 64, 100, 300]:
    members = ",".join('"m%d":%d' % (i, i) for i in range(n))
    f.c(["JSON.SET", "big", "$", "{" + members + "}"], ["JSON.OBJKEYS", "big"],
        ["JSON.OBJLEN", "big"], ["JSON.GET", "big"], ["JSON.SET", "big", "$.m0", "\"u\""],
        ["JSON.DEL", "big", "$.m1"], ["JSON.SET", "big", "$.m1", "\"re\""],
        ["JSON.SET", "big", ".zz", "1"], ["JSON.SET", "big", "$.aa", "1"],
        ["JSON.OBJKEYS", "big"], ["JSON.GET", "big"], ["JSON.GET", "big", "$.m%d" % (n - 1)],
        ["JSON.GET", "big", "$.*"], ["JSON.DEBUG", "FIELDS", "big"])
f.c(["JSON.SET", "big", "$", "{}"])
for i in range(40):
    f.c(["JSON.SET", "big", "$.k%d" % (39 - i), str(i)])
for i in range(0, 40, 3):
    f.c(["JSON.DEL", "big", "$.k%d" % i])
for i in range(0, 40, 6):
    f.c(["JSON.SET", "big", "$.k%d" % i, "\"back\""])
f.c(["JSON.OBJKEYS", "big"], ["JSON.GET", "big"], ["JSON.CLEAR", "big"], ["JSON.GET", "big"])
f.note("duplicate member names in input")
f.c(["JSON.SET", "dup", "$", '{"a":1,"b":2,"a":3}'], ["JSON.GET", "dup"],
    ["JSON.OBJKEYS", "dup"], ["JSON.OBJLEN", "dup"], ["JSON.GET", "dup", "$.a"],
    ["JSON.DEL", "dup", "$.a"], ["JSON.GET", "dup"])
big = ",".join('"m%d":%d' % (i, i) for i in range(40))
f.c(["JSON.SET", "dup", "$", "{" + big + ',"m5":"again"}'], ["JSON.GET", "dup"],
    ["JSON.OBJLEN", "dup"])
f.note("large arrays")
f.c(["JSON.SET", "la", "$", "[" + ",".join(str(i) for i in range(5000)) + "]"],
    ["JSON.ARRLEN", "la"], ["JSON.ARRINDEX", "la", "$", "4999"],
    ["JSON.ARRINDEX", "la", "$", "10", "-100"], ["JSON.ARRINDEX", "la", "$", "4990", "-100"],
    ["JSON.ARRTRIM", "la", "$", "10", "-10"], ["JSON.ARRLEN", "la"],
    ["JSON.GET", "la", "$[-3:]"], ["JSON.ARRPOP", "la", "$", "-2"], ["JSON.GET", "la", "$[:5]"])
f.note("single-key debug scans are deterministic")
f.c(["FLUSHALL"], ["JSON.SET", "only", "$", '{"a":[{"b":[1]}]}'],
    ["JSON.DEBUG", "MAX-DEPTH-KEY"], ["JSON.DEBUG", "MAX-SIZE-KEY"],
    ["JSON.DEBUG", "DEPTH", "only"], ["JSON.DEBUG", "FIELDS", "only"],
    ["JSON.DEBUG", "FIELDS", "only", "$..*"], ["JSON.DEBUG", "FIELDS", "only", ".a"],
    ["JSON.DEBUG", "MEMORY", "only"], ["JSON.DEBUG", "MEMORY", "only", "$..*"],
    ["JSON.DEBUG", "MEMORY", "only", ".a"], ["JSON.DEBUG", "NOSUCH"], ["JSON.DEBUG"],
    ["JSON.DEBUG", "DEPTH"], ["JSON.DEBUG", "DEPTH", "only", "x"],
    ["JSON.DEBUG", "MAX-DEPTH-KEY", "x"])
f.close()

# ---------------------------------------------------------------- strings
f = F("strings.jsonl", """
Escapes, multi-byte UTF-8, invalid UTF-8 (hex arguments), member-name quoting,
STRLEN/STRAPPEND counting, and JSON.GET formatting options.
""")
STRS = ['""', '"a"', '"\\u00e9"', '"é"', '"\\ud83d\\ude00"', '"\U0001f600"', '"\\/"',
        '"/"', '"a\\u0000b"', '"\\b\\f\\n\\r\\t"', '"\\"q\\""', '"\\\\"', '"\\u001f"',
        '"\\u0001"', '"\\u007f"', '"\u007f"', '"\\u0080"', '"\\u2028\\u2029"', '" "',
        '"\\uFFFF"', '"\\uffff"', '"\\uFEFF"', '"\\u00E9"', '"日本語"', '"<>&\'"',
        '"\\ud800"', '"\\udc00"', '"\\ud800\\u0041"', '"\\ud83d"', '"\\x"', '"\\u12"',
        '"\\U0041"', '"a\tb"', '"a\nb"', '"unterminated', "'single'"]
for s in STRS:
    f.c(["JSON.SET", "s", "$", s], ["JSON.GET", "s"], ["JSON.GET", "s", "$"], ["JSON.RESP", "s"],
        ["JSON.STRLEN", "s"], ["JSON.STRAPPEND", "s", '"\\u00e9"'],
        ["JSON.STRAPPEND", "s", "$", s], ["JSON.STRLEN", "s", "$"], ["JSON.GET", "s"],
        ["JSON.SET", "o", "$", "{" + s + ":" + s + "}"], ["JSON.GET", "o"],
        ["JSON.OBJKEYS", "o"], ["JSON.GET", "o", "$.*"])
f.note("invalid UTF-8 in documents, member names and paths")
for h in ["22ff22", "22c322", "22c3a922", "22e282ac22", "22e28222", "22f09f988022", "22eda08022",
          "22c0af22", "22f4908080" + "22", "7b22ff223a317d", "2200", "22610062" + "22"]:
    f.c(["JSON.SET", "u", "$", {"hex": h}], ["JSON.GET", "u"], ["JSON.STRLEN", "u"])
f.c(["JSON.SET", "o", "$", '{"a":1}'], ["JSON.GET", "o", {"hex": "242eff"}],
    ["JSON.SET", "o", {"hex": "242eff"}, "1"], ["JSON.GET", "o"],
    ["JSON.GET", "o", {"hex": "242e6100"}], ["JSON.GET", "o", {"hex": "2e610062"}],
    ["JSON.SET", "o", {"hex": "242e610062"}, "2"], ["JSON.GET", "o"],
    ["JSON.SET", "o", "$.b", {"hex": "3100"}],
    ["JSON.GET", "o"], ["JSON.SET", {"hex": "6b00ff"}, "$", "1"], ["JSON.GET", {"hex": "6b00ff"}])
f.note("GET formatting")
DOC = '{"a":[1,{"b":"é","c":[]},{}],"d":{"e":null,"f":"\\u001f"},"g":[],"h":{}}'
f.c(["JSON.SET", "fmt", "$", DOC])
FMTS = [["INDENT", "\t"], ["INDENT", "  "], ["INDENT", ""], ["INDENT", "xx"],
        ["NEWLINE", "\n"], ["NEWLINE", "\r\n"], ["NEWLINE", ""], ["SPACE", " "], ["SPACE", ""],
        ["SPACE", "--"], ["NOESCAPE"], ["noescape"], ["indent", "\t", "newline", "\n"],
        ["INDENT", "\t", "NEWLINE", "\n", "SPACE", " "], ["INDENT", "\t", "INDENT", "  "],
        ["INDENT"], ["NEWLINE"], ["SPACE"], ["INDENT", "é"]]
for fm in FMTS:
    f.c(["JSON.GET", "fmt"] + fm, ["JSON.GET", "fmt"] + fm + ["$"],
        ["JSON.GET", "fmt"] + fm + ["$.a", ".d"], ["JSON.GET", "fmt", "$.a"] + fm + ["$.d"],
        ["JSON.GET", "fmt", "$.a"] + fm)
f.c(["JSON.GET", "fmt", ".a", ".d"], ["JSON.GET", "fmt", "$.a", "$.d"],
    ["JSON.GET", "fmt", ".a", "$.d"], ["JSON.GET", "fmt", "$.a", ".d"],
    ["JSON.GET", "fmt", ".a", ".nope"], ["JSON.GET", "fmt", "$.a", "$.nope"],
    ["JSON.GET", "fmt", ".a", ".a"], ["JSON.GET", "fmt", "$..*", "$.a[*]"])
f.close()

# ---------------------------------------------------------------- commands
f = F("commands.jsonl", """
Per-command behaviour: arity, wrong key type, CLEAR/TOGGLE per type, array
operations and their index edge cases, MSET/MGET, COPY/RENAME of JSON keys.
""")
f.c(["SET", "str", "x"], ["HSET", "hash", "f", "v"])
CMDS = ["JSON.SET", "JSON.GET", "JSON.MGET", "JSON.MSET", "JSON.DEL", "JSON.FORGET",
        "JSON.TYPE", "JSON.NUMINCRBY", "JSON.NUMMULTBY", "JSON.STRAPPEND", "JSON.STRLEN",
        "JSON.ARRAPPEND", "JSON.ARRINDEX", "JSON.ARRINSERT", "JSON.ARRLEN", "JSON.ARRPOP",
        "JSON.ARRTRIM", "JSON.OBJKEYS", "JSON.OBJLEN", "JSON.CLEAR", "JSON.TOGGLE", "JSON.RESP"]
f.c(["JSON.SET", "j", "$", '{"a":[1,2,3],"s":"x","n":1,"b":true,"o":{"k":1}}'])
for c in CMDS:
    for n in range(0, 8):
        f.c([c] + ["j", "$", "1", "2", "3", "4", "5"][:n])
    f.c([c, "str", "$", "1", "1"][:3], [c, "hash", "$"], [c, "str"], [c.lower(), "j"])
f.note("CLEAR and TOGGLE per type")
T = '{"o":{"a":1},"a":[1,2],"s":"str","n":3.5,"i":7,"t":true,"f":false,"z":null,"e":{},"ea":[]}'
for p in ["$.*", "$.o", "$.a", "$.s", "$.n", "$.i", "$.t", "$.f", "$.z", "$.e", "$.ea", "$",
          ".", ".o", ".a", ".s", ".n", ".i", ".t", ".f", ".z", "$..*", "$.nope", ".nope"]:
    for c in ["JSON.CLEAR", "JSON.TOGGLE"]:
        f.c(["JSON.SET", "t", "$", T], [c, "t", p], ["JSON.GET", "t"])
for v in ['"s"', "1", "1.5", "true", "false", "null", "[]", "{}", "[1]", '{"a":1}']:
    f.c(["JSON.SET", "t", "$", v], ["JSON.CLEAR", "t"], ["JSON.GET", "t"],
        ["JSON.SET", "t", "$", v], ["JSON.TOGGLE", "t"], ["JSON.GET", "t"],
        ["JSON.SET", "t", "$", v], ["JSON.TOGGLE", "t", "$"], ["JSON.GET", "t"])
f.note("array operations")
for idx in ["0", "1", "2", "3", "4", "-1", "-3", "-4", "100", "-100", "x", "1.5", "",
            "9223372036854775807", "-9223372036854775808", "99999999999999999999"]:
    f.c(["JSON.SET", "a", "$", "[1,2,3]"], ["JSON.ARRINSERT", "a", "$", idx, "\"v\""],
        ["JSON.ARRINSERT", "a", ".", idx, "\"w\""], ["JSON.GET", "a"],
        ["JSON.ARRPOP", "a", "$", idx], ["JSON.ARRPOP", "a", ".", idx], ["JSON.GET", "a"])
    for idx2 in ["0", "1", "-1", "2", "100", "-100"]:
        f.c(["JSON.SET", "a", "$", "[0,1,2,3,4]"], ["JSON.ARRTRIM", "a", "$", idx, idx2],
            ["JSON.GET", "a"], ["JSON.ARRINDEX", "a", "$", "3", idx, idx2],
            ["JSON.ARRINDEX", "a", ".", "3", idx, idx2])
f.c(["JSON.SET", "a", "$", "[]"], ["JSON.ARRPOP", "a"], ["JSON.ARRPOP", "a", "$"],
    ["JSON.ARRTRIM", "a", "$", "0", "0"], ["JSON.ARRINSERT", "a", "$", "0", "1"],
    ["JSON.ARRINSERT", "a", "$", "1", "2", "3"], ["JSON.ARRINSERT", "a", "$", "-1", "0"],
    ["JSON.ARRAPPEND", "a", "$", "{\"x\":[]}", "null", "[[]]"], ["JSON.GET", "a"],
    ["JSON.ARRAPPEND", "a", "$", "bad"], ["JSON.ARRAPPEND", "a", "$", "1", "bad"],
    ["JSON.GET", "a"], ["JSON.ARRPOP", "a", "$", "0"], ["JSON.ARRPOP", "a", ".", "-1"],
    ["JSON.ARRPOP", "a"], ["JSON.ARRPOP", "a"], ["JSON.ARRPOP", "a"], ["JSON.GET", "a"])
f.c(["JSON.SET", "a", "$", '{"x":[1],"y":{"x":[2,3]},"z":{"x":"s"}}'],
    ["JSON.ARRAPPEND", "a", "$..x", "9"], ["JSON.ARRINSERT", "a", "$..x", "0", "8"],
    ["JSON.ARRLEN", "a", "$..x"], ["JSON.ARRINDEX", "a", "$..x", "9"],
    ["JSON.ARRTRIM", "a", "$..x", "0", "1"], ["JSON.ARRPOP", "a", "$..x"],
    ["JSON.ARRAPPEND", "a", "..x", "9"], ["JSON.ARRLEN", "a", "..x"],
    ["JSON.ARRPOP", "a", "..x"], ["JSON.STRAPPEND", "a", "$..x", '"t"'],
    ["JSON.STRLEN", "a", "$..x"], ["JSON.STRLEN", "a", "..x"], ["JSON.GET", "a"])
f.note("MSET and MGET")
f.c(["JSON.MSET", "m1", "$", "1", "m2", "$", "[2]"], ["JSON.MGET", "m1", "m2", "m3", "$"],
    ["JSON.MGET", "m1", "m2", "m3", "."], ["JSON.MGET", "m1", "str", "$"],
    ["JSON.MSET", "m1", "$", "1", "m1", "$", "2"], ["JSON.GET", "m1"],
    ["JSON.MSET", "m1", "$", "1", "m9", "$.a", "2"], ["EXISTS", "m9"], ["JSON.GET", "m1"],
    ["JSON.MSET", "m1", "$", "5", "m2", "$", "bad"], ["JSON.GET", "m1"],
    ["JSON.MSET", "m2", "$[0]", "7", "m2", "$[5]", "8"], ["JSON.GET", "m2"],
    ["JSON.MSET", "m1", "$", "1", "str", "$", "2"], ["JSON.GET", "m1"],
    ["JSON.MSET", "m1", "$", "{\"a\":1}", "m1", "$.a", "2"], ["JSON.GET", "m1"],
    ["JSON.MGET", "m1", "m2", "$..a"], ["JSON.MGET", "m1", "m2", ".a"],
    ["JSON.MGET", "m1", "m2", "$["], ["JSON.MGET", "m1", "$"])
f.note("generic keyspace commands on JSON keys")
f.c(["JSON.SET", "g", "$", '{"a":[1,{"b":"c"}]}'], ["TYPE", "g"], ["OBJECT", "ENCODING", "g"],
    ["COPY", "g", "g2"], ["JSON.SET", "g2", "$.a[0]", "2"], ["JSON.GET", "g"],
    ["JSON.GET", "g2"], ["RENAME", "g2", "g3"], ["JSON.GET", "g3"], ["EXISTS", "g2"],
    ["COPY", "g", "g3"], ["COPY", "g", "g3", "REPLACE"], ["JSON.GET", "g3"],
    ["JSON.SET", "str", "$", "1"], ["GET", "str"], ["DEL", "g", "g3"], ["JSON.GET", "g"])
f.close()

# ---------------------------------------------------------------- resp3
f = F("resp3.jsonl", """
The same reads under RESP3 (HELLO 3), where nulls, doubles, maps and
nested arrays are encoded differently, then back to RESP2.
""")
DOC = ('{"a":[1,2.5,"s",true,null,{"x":1E2}],"o":{"k":-0.0,"l":[]},"s":"é","n":1.5,'
       '"i":18446744073709551615,"z":null,"b":false}')
READS = []
for p in [None, ".", "$", "$.*", "$..*", ".a", "$.a", "$.a[*]", ".o", "$.o", "$..x", ".nope",
          "$.nope", ".s", "$.s", ".n", "$.n", ".z", "$.z", "$.a[5]", ".i", "$.i"]:
    extra = [p] if p is not None else []
    for c in ALL_READ + ["JSON.GET NOESCAPE"]:
        READS.append(c.split() + ["r"] + extra)
    READS.append(["JSON.MGET", "r", "nokey", p or "."])
MUTS = [["JSON.NUMINCRBY", "r", "$.n", "1"], ["JSON.NUMINCRBY", "r", ".n", "1"],
        ["JSON.NUMMULTBY", "r", "$..*", "2"], ["JSON.STRAPPEND", "r", "$..*", '"x"'],
        ["JSON.ARRPOP", "r", "$..*"], ["JSON.ARRAPPEND", "r", "$..*", "1"],
        ["JSON.TOGGLE", "r", "$..*"], ["JSON.CLEAR", "r", "$..*"], ["JSON.DEL", "r", "$..x"],
        ["JSON.OBJLEN", "r", "$..*"], ["JSON.ARRINDEX", "r", "$..*", "1"],
        ["JSON.TYPE", "nokey"], ["JSON.GET", "nokey"], ["JSON.RESP", "nokey"],
        ["JSON.ARRPOP", "r", ".a"], ["JSON.ARRPOP", "r", "$.a", "0"]]
for proto in ["3", "2"]:
    f.c(["HELLO", proto], ["JSON.SET", "r", "$", DOC])
    f.c(*READS)
    for m in MUTS:
        f.c(["JSON.SET", "r", "$", DOC], m, ["JSON.GET", "r"])
f.close()

# ---------------------------------------------------------------- merge
f = F("merge.merge.jsonl", """
JSON.MERGE (RFC 7396). Only present in valkey-json unstable, so run.sh uses this
file only with --with-merge.
""")
BASE = '{"a":1,"b":{"c":2,"d":[1,2]},"e":null,"f":"s"}'
PATCHES = ['{"a":2}', '{"a":null}', '{"b":{"c":null}}', '{"b":{"d":[3]}}', '{"x":{"y":{"z":1}}}',
           '{"e":1}', '[1,2]', '1', 'null', '{}', '{"b":null,"f":{"g":1}}', 'bad', '{"a":1E2}']
for p in PATCHES:
    for path in ["$", ".", "$.b", ".b", "$.b.d", "$..c", "$.*", "$.nope", "$.f", "."]:
        f.c(["JSON.SET", "mg", "$", BASE], ["JSON.MERGE", "mg", path, p], ["JSON.GET", "mg"])
f.c(["JSON.MERGE", "newkey", "$", '{"a":1}'], ["JSON.GET", "newkey"],
    ["JSON.MERGE", "newkey2", "$.a", '{"a":1}'], ["JSON.MERGE", "mg"],
    ["JSON.MERGE", "mg", "$"], ["SET", "str", "x"], ["JSON.MERGE", "str", "$", "{}"])
f.close()
