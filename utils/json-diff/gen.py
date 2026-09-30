#!/usr/bin/env python3
"""Seeded random corpus generator for the JSON differential harness.

Usage: gen.py --seed N --count N [--merge] [-o FILE]

The generator keeps a rough model of each key's document so that most paths
hit something real. The model drifts after mutations it does not track, which
is fine: a stale path is just another probe.
"""

import argparse
import json
import random
import sys

import corpus

NUMBERS = [
    "0", "-0", "-0.0", "0.0", "1", "-1", "7", "42", "100", "1.0", "1.50", "0.1", "0.5",
    "1E2", "1e2", "1e+2", "1E-2", "1e0", "2e22", "1e21", "1e15", "1e-7", "0.000001",
    "0.30000000000000004", "3.14159", "-2.5e-3", "123.456e1",
    "9223372036854775807", "9223372036854775808", "-9223372036854775808",
    "-9223372036854775809", "18446744073709551615", "18446744073709551616",
    "123456789012345678901234567890", "12345678901234567890.5",
    "1.7976931348623157e308", "5e-324", "1e-400", "1" + "0" * 300,
]
# Accepted, but they round to infinity. valkey-json 1.0.3 asserts on a legacy-path
# NUMMULTBY by zero of such a value, so they are kept out of random documents and
# covered by corpora/numbers.jsonl instead.
HUGE_NUMBERS = ["9e308", "1.7976931348623159e308", "1" + "0" * 310, "1" + "0" * 400]
# Rejected by the module. The first three are the exponent guard, not syntax.
BAD_NUMBERS = ["0e309", "1e309", "-1e309", "01", "-01", ".5", "1.", "+1", "1e", "1e+", "NaN",
               "Infinity", "-Infinity", "0x10", "1_000", "--1", "1 2", ""]
ARITH = ["1", "-1", "0", "2", "0.5", "-0.5", "1.5", "10", "1e2", "1E2", "-0.0", "0.1",
         "1e308", "-1e308", "1e309", "9223372036854775807", "-9223372036854775808",
         "18446744073709551615", "3", "abc", "\"1\"", "[1]", "null", "true", "", "1e-400"]

# JSON string literals, as text. The first NVALID parse; the rest are rejected.
STRINGS = [
    '""', '"a"', '"hello"', '"x y"', '"\\u00e9"', '"\\ud83d\\ude00"', '"\\/"', '"a\\u0000b"',
    '"\\b\\f\\n\\r\\t"', '"\\"q\\""', '"\\\\"', '"\u00e9"', '"\u65e5\u672c\u8a9e"',
    '"\U0001f600"', '"~"', '"a.b"', '"[0]"', '"$"', '"\'"', '"*"', '"\\u001f"', '"\\u007f"',
    '"\\u0080"', '"\\uFFFF"', '"\\uffff"', '"' + "long" * 100 + '"',
    '"\\ud800"', '"\\udc00x"', '"tab\there"', '"line\nbreak"', '"\\x"', '"\\u12"',
    '"unterminated',
]
NVALID = 26
SCALARS = ["true", "false", "null", "tru", "nul", "True", "NULL"]

NAMES = ["a", "b", "c", "x", "y", "num", "str", "arr", "obj", "nested", "bool", "null",
         "a.b", "a b", "$", "*", "..", "0", "[0]", "'q'", "\"dq\"", "\u00e9", "\u65e5\u672c",
         "\U0001f600", "", "~", "~0", "~1", "/", "k\\n", "@", "a]", "a[", "a'b"]
SIMPLE_NAMES = ["a", "b", "c", "x", "y", "num", "str", "arr", "obj", "nested", "bool"]

KEYS = ["k0", "k1", "k2", "k3", "k4", "{t}a", "{t}b"]
OTHER_KEYS = ["s1", "h1", "missing"]

WS = ["", "", "", " ", "\n", "\t", "  \r\n "]

# Model value for text that is invalid or not worth modelling.
BAD = object()


class Gen(object):
    def __init__(self, seed, merge):
        self.r = random.Random(seed)
        self.merge = merge
        self.model = {}
        self.resp3 = False

    # ------------------------------------------------------------------ values
    def ws(self):
        return self.r.choice(WS) if self.r.random() < 0.15 else ""

    def name_text(self, name):
        return json.dumps(name, ensure_ascii=self.r.random() < 0.3)

    def value(self, depth=0, maxdepth=4):
        """Return (json_text, model_object)."""
        r = self.r
        roll = r.random()
        if depth >= maxdepth or roll < (0.45 if depth else 0.1):
            kind = r.random()
            if kind < 0.35:
                n = r.choice(NUMBERS)
                return n, float("nan")
            if kind < 0.7:
                s = r.choice(STRINGS[:NVALID])
                return s, json.loads(s)
            if kind < 0.995 or depth > 0:
                s = r.choice(SCALARS[:3])
                return s, json.loads(s)
            s = r.choice(SCALARS[3:] + BAD_NUMBERS[:9] + STRINGS[NVALID:])
            return s, BAD
        if roll < 0.72:
            n = r.choice([0, 0, 1, 2, 3, 4, 5, 8])
            parts, objs = [], []
            for _ in range(n):
                t, o = self.value(depth + 1, maxdepth)
                parts.append(self.ws() + t + self.ws())
                objs.append(o)
            return "[" + ",".join(parts) + "]", objs
        n = r.choice([0, 1, 2, 3, 4, 6])
        if r.random() < 0.05:
            n = r.choice([31, 32, 33, 40, 64])
        parts, obj = [], {}
        for i in range(n):
            if n > 12:
                name = "m%d" % i
            else:
                name = r.choice(NAMES if r.random() < 0.3 else SIMPLE_NAMES)
            t, o = self.value(depth + 1, maxdepth)
            parts.append(self.ws() + self.name_text(name) + self.ws() + ":" + self.ws() + t)
            obj[name] = o
        return "{" + ",".join(parts) + "}", obj

    def deep(self):
        r = self.r
        d = r.choice([126, 127, 128, 129, 130, 200])
        if r.random() < 0.5:
            return "[" * d + "]" * d, BAD
        return '{"a":' * d + "1" + "}" * d, BAD

    def doc(self):
        r = self.r
        roll = r.random()
        if roll < 0.03:
            return self.deep()
        if roll < 0.06:
            n = r.choice([31, 32, 33, 34, 50, 100])
            obj = {}
            parts = []
            for i in range(n):
                obj["f%d" % i] = i
                parts.append('"f%d":%d' % (i, i))
            return "{" + ",".join(parts) + "}", obj
        if roll < 0.08:
            t = r.choice(["", " ", "{", "[", "{]", "[1,]", "{\"a\":1,}", "{\"a\" 1}",
                          "{'a':1}", "[1 2]", "{\"a\":1}x", "\"\\u\"", "[1]]",
                          "{\"a\":1,\"a\":2}"] + BAD_NUMBERS[:13])
            return t, BAD
        t, o = self.value(0, r.choice([2, 3, 4, 5]))
        return t, o

    # ------------------------------------------------------------------- paths
    def all_paths(self, obj, prefix, out, limit=200):
        if len(out) >= limit:
            return
        out.append(prefix)
        if isinstance(obj, dict):
            for k, v in obj.items():
                self.all_paths(v, prefix + [("k", k)], out, limit)
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                self.all_paths(v, prefix + [("i", i)], out, limit)

    def lookup(self, obj, steps):
        for kind, s in steps:
            if kind == "k" and isinstance(obj, dict) and s in obj:
                obj = obj[s]
            elif kind == "i" and isinstance(obj, list) and -len(obj) <= s < len(obj):
                obj = obj[s]
            else:
                return None, False
        return obj, True

    @staticmethod
    def kind_of(v):
        if isinstance(v, bool):
            return "bool"
        if isinstance(v, (int, float)):
            return "num"
        return {dict: "obj", list: "arr", str: "str"}.get(type(v), "null")

    def pick_steps(self, key, want):
        doc = self.model.get(key)
        paths = []
        self.all_paths(doc, [], paths)
        r = self.r
        if want and r.random() < 0.7:
            typed = [p for p in paths if self.kind_of(self.lookup(doc, p)[0]) == want]
            if typed:
                paths = typed
        steps = list(r.choice(paths))
        roll = r.random()
        if roll < 0.1:
            steps.append(("k", r.choice(SIMPLE_NAMES + ["zz"])))
        elif roll < 0.15:
            steps.append(("i", r.choice([0, 1, -1, 5, 100])))
        elif roll < 0.18 and steps:
            steps[-1] = ("i", r.randint(-3, 3)) if steps[-1][0] == "k" else ("k", "a")
        return steps

    @staticmethod
    def ident(name):
        return name != "" and all(c.isalnum() or c in "_" for c in name) and not name[0].isdigit()

    def render_legacy(self, steps):
        r = self.r
        if not steps:
            return r.choice([".", ".", ".", ""]) if r.random() < 0.97 else "$"
        out = []
        for n, (kind, s) in enumerate(steps):
            if kind == "i":
                out.append("[%d]" % s)
            elif self.ident(s) and r.random() < 0.85:
                if n == 0 and r.random() < 0.2:
                    out.append(s)
                else:
                    out.append("." + s)
            else:
                q = '"' if r.random() < 0.7 else "'"
                if q == '"':
                    out.append("[" + json.dumps(s, ensure_ascii=False) + "]")
                else:
                    out.append("['" + s + "']")
        return "".join(out)

    def render_v2_step(self, kind, s):
        r = self.r
        if kind == "i":
            roll = r.random()
            if roll < 0.8:
                return "[%d]" % s
            if roll < 0.85:
                return "[ %d ]" % s
            if roll < 0.9:
                return "[%d,%d]" % (s, s + 1)
            if roll < 0.95:
                return "[%d:%d]" % (s, s + r.choice([1, 2]))
            return "[+%d]" % s if s >= 0 else "[%d]" % s
        if self.ident(s) and r.random() < 0.8:
            return "." + s
        if r.random() < 0.6:
            return "[" + json.dumps(s, ensure_ascii=False) + "]"
        return "['" + s.replace("'", "\\'") + "']"

    def filter_expr(self):
        r = self.r
        name = r.choice(SIMPLE_NAMES)
        op = r.choice(["==", "!=", "<", "<=", ">", ">="])
        val = r.choice(["1", "0", "-1", "1.5", "1e2", "\"a\"", "\"hello\"", "true", "false",
                        "null", "$.num", "$.a"])
        roll = r.random()
        if roll < 0.2:
            return "@." + name
        if roll < 0.35:
            return "@" + op + val
        if roll < 0.45:
            return val + op + "@." + name
        if roll < 0.55:
            return "@.%s%s%s && @.%s" % (name, op, val, r.choice(SIMPLE_NAMES))
        if roll < 0.6:
            return "(@.%s%s%s) || @.%s%s%s" % (name, op, val, r.choice(SIMPLE_NAMES), op, val)
        if roll < 0.63:
            return "@[\"%s\"] %s %s" % (name, op, val)
        return "@.%s %s %s" % (name, op, val)

    def render_v2(self, steps):
        r = self.r
        out = ["$"]
        for kind, s in steps:
            if r.random() < 0.06:
                step = self.render_v2_step(kind, s)
                out.append(".." + (step[1:] if step.startswith(".") else step))
                continue
            if r.random() < 0.06:
                out.append(r.choice([".*", "[*]"]))
                continue
            out.append(self.render_v2_step(kind, s))
        roll = r.random()
        if roll < 0.06:
            out.append(r.choice([".*", "[*]", "..*", "..a", "..num", "..[0]", "..[*]"]))
        elif roll < 0.1:
            out.append(r.choice(["[*]", "[0:2]", "[:2]", "[1:]", "[-2:]", "[::2]", "[::-1]",
                                 "[0,1]", "[0,-1]", "[5:1]", "[0:0]", "[1:2:0]", "[-100:100]",
                                 "['a','b']", "[\"a\",\"x\"]"]))
        elif roll < 0.15:
            out.append(r.choice(["[?(", ".*[?(", "[*][?(", "..[?("]) + self.filter_expr() + ")]")
        return "".join(out)

    def bad_path(self):
        return self.r.choice([
            "$.", "$[", "$..", "$[?(@.a>)]", ".a.", "a..", "$[1:2:0]", "$['a", "$[\"a]",
            "$.a[", "[", "]", "$$", "$.a b", "..", "...", "$.[0]", "$[a]", "$[?@.a]",
            "$[?(@.a==1]", "$[0", "$.a.b.c.d.e.f.g", "$[-0]", "$[*", "$..*..*", "$[,]",
            "$[?(@.a==\"x)]", "$.\u00e9", ".\u00e9", "$[?(@.a=1)]", "$.a[9999999999999999999]",
            "$" + ".a" * 129, "." + ".a" * 129, "$" + "[0]" * 130, "$" + "..a" * 20, " $", "$ ",
        ])

    def path(self, key, want=None):
        r = self.r
        roll = r.random()
        if roll < 0.06:
            return self.bad_path()
        steps = self.pick_steps(key, want)
        if roll < 0.45:
            return self.render_legacy(steps)
        return self.render_v2(steps)

    # ----------------------------------------------------------------- commands
    def key(self):
        r = self.r
        if r.random() < 0.06:
            return r.choice(OTHER_KEYS)
        return r.choice(KEYS)

    def resync(self, key):
        t, o = self.value(0, 4)
        while o is BAD:
            t, o = self.value(0, 4)
        self.model[key] = o
        return ["JSON.SET", key, "$" if self.r.random() < 0.5 else ".", t]

    def cmd(self):
        r = self.r
        k = self.key()
        w = r.random() * 100
        if w < 14:
            if r.random() < 0.35 or k not in self.model:
                t, o = self.doc()
                args = ["JSON.SET", k, r.choice(["$", ".", "$", "."]), t]
                if o is not BAD:
                    self.model[k] = o
            else:
                p = self.path(k)
                t, o = self.value(0, 2)
                args = ["JSON.SET", k, p, t]
            if r.random() < 0.1:
                args.append(r.choice(["NX", "XX", "nx", "xx", "YY"]))
            return args
        if w < 24:
            args = ["JSON.GET", k]
            n = r.choice([0, 1, 1, 1, 2, 3])
            if r.random() < 0.15:
                opt = r.choice([["INDENT", r.choice(["\t", "  ", "", "xx"])],
                                ["NEWLINE", r.choice(["\n", "\r\n", ""])],
                                ["SPACE", r.choice([" ", "", "  "])], ["NOESCAPE"],
                                ["INDENT", "\t", "NEWLINE", "\n", "SPACE", " "], ["INDENT"]])
                if r.random() < 0.8:
                    args += opt
                    opt = []
            else:
                opt = []
            for i in range(n):
                args.append(self.path(k))
                if i == 0 and opt:
                    args += opt
            return args
        if w < 28:
            ks = [self.key() for _ in range(r.choice([1, 2, 3, 4]))]
            return ["JSON.MGET"] + ks + [self.path(ks[0])]
        if w < 31:
            args = ["JSON.MSET"]
            for _ in range(r.choice([1, 2, 3])):
                kk = self.key()
                if r.random() < 0.6 or kk not in self.model:
                    t, o = self.doc()
                    args += [kk, "$", t]
                else:
                    t, o = self.value(0, 2)
                    args += [kk, self.path(kk), t]
            if r.random() < 0.05:
                args.pop()
            return args
        if w < 35:
            if r.random() < 0.15:
                return [r.choice(["JSON.DEL", "JSON.FORGET"]), k]
            return [r.choice(["JSON.DEL", "JSON.FORGET"]), k, self.path(k)]
        if w < 39:
            return ["JSON.TYPE", k] + ([self.path(k)] if r.random() < 0.9 else [])
        if w < 45:
            return [r.choice(["JSON.NUMINCRBY", "JSON.NUMMULTBY"]), k, self.path(k, "num"),
                    r.choice(ARITH)]
        if w < 49:
            val = r.choice(STRINGS) if r.random() < 0.9 else r.choice(["abc", "1", "[1]"])
            if r.random() < 0.2:
                return ["JSON.STRAPPEND", k, val]
            return ["JSON.STRAPPEND", k, self.path(k, "str"), val]
        if w < 52:
            return ["JSON.STRLEN", k] + ([self.path(k, "str")] if r.random() < 0.85 else [])
        if w < 57:
            vals = [self.value(0, 2)[0] for _ in range(r.choice([1, 1, 2, 3]))]
            return ["JSON.ARRAPPEND", k, self.path(k, "arr")] + vals
        if w < 60:
            args = ["JSON.ARRINDEX", k, self.path(k, "arr"), self.value(0, 1)[0]]
            if r.random() < 0.4:
                args.append(str(r.choice([0, 1, -1, 2, -5, 100])))
                if r.random() < 0.5:
                    args.append(str(r.choice([0, 1, -1, 3, 100, -100])))
            return args
        if w < 63:
            vals = [self.value(0, 2)[0] for _ in range(r.choice([1, 2]))]
            return ["JSON.ARRINSERT", k, self.path(k, "arr"),
                    r.choice(["0", "1", "-1", "2", "-100", "100", "x", "1.5"])] + vals
        if w < 66:
            return ["JSON.ARRLEN", k] + ([self.path(k, "arr")] if r.random() < 0.85 else [])
        if w < 70:
            args = ["JSON.ARRPOP", k]
            if r.random() < 0.85:
                args.append(self.path(k, "arr"))
                if r.random() < 0.5:
                    args.append(r.choice(["0", "-1", "1", "100", "-100", "x"]))
            return args
        if w < 73:
            return ["JSON.ARRTRIM", k, self.path(k, "arr"),
                    r.choice(["0", "1", "-1", "-2", "5", "100", "-100"]),
                    r.choice(["0", "1", "-1", "2", "5", "100", "-100"])]
        if w < 76:
            return ["JSON.OBJKEYS", k] + ([self.path(k, "obj")] if r.random() < 0.85 else [])
        if w < 79:
            return ["JSON.OBJLEN", k] + ([self.path(k, "obj")] if r.random() < 0.85 else [])
        if w < 82:
            return ["JSON.CLEAR", k] + ([self.path(k)] if r.random() < 0.85 else [])
        if w < 84:
            return ["JSON.TOGGLE", k] + ([self.path(k, "bool")] if r.random() < 0.85 else [])
        if w < 87:
            return ["JSON.RESP", k] + ([self.path(k)] if r.random() < 0.85 else [])
        if w < 89:
            sub = r.choice(["MEMORY", "FIELDS", "DEPTH", "memory", "fields"])
            args = ["JSON.DEBUG", sub, k]
            if sub.upper() != "DEPTH" and r.random() < 0.6:
                args.append(self.path(k))
            return args
        if w < 90 and self.merge:
            return ["JSON.MERGE", k, self.path(k, "obj"), self.value(0, 3)[0]]
        if w < 91:
            # Arity errors across the command set.
            c = r.choice(["JSON.SET", "JSON.GET", "JSON.MGET", "JSON.MSET", "JSON.DEL",
                          "JSON.NUMINCRBY", "JSON.STRAPPEND", "JSON.ARRAPPEND",
                          "JSON.ARRINSERT", "JSON.ARRTRIM", "JSON.ARRINDEX", "JSON.ARRPOP",
                          "JSON.TYPE", "JSON.DEBUG", "JSON.RESP", "JSON.CLEAR"])
            return [c] + [k, "$", "1", "2", "3", "4"][:r.choice([0, 1, 5, 6])]
        if w < 94:
            self.model.pop(k, None)
            return self.resync(k)
        if w < 95:
            self.resp3 = not self.resp3
            return ["HELLO", "3" if self.resp3 else "2"]
        if w < 96.5:
            kk = r.choice(OTHER_KEYS[:2])
            return ["SET", kk, "x"] if kk == "s1" else ["HSET", kk, "f", "v"]
        if w < 97.5:
            return [r.choice(["TYPE", "EXISTS", "DEL"]), k]
        if w < 98:
            return ["OBJECT", "ENCODING", k]
        if w < 98.5:
            return ["MEMORY", "USAGE", k]
        if w < 99:
            dst = r.choice(KEYS)
            if k in self.model:
                self.model[dst] = self.model[k]
            return ["COPY", k, dst, "REPLACE"]
        return ["JSON.GET", k]

    def run(self, count):
        out = [["FLUSHALL"]]
        for k in KEYS[:3]:
            out.append(self.resync(k))
        while len(out) < count:
            out.append(self.cmd())
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--count", type=int, default=2000)
    ap.add_argument("--merge", action="store_true", help="also emit JSON.MERGE")
    ap.add_argument("-o", "--output")
    a = ap.parse_args()
    g = Gen(a.seed, a.merge)
    f = open(a.output, "w", encoding="utf-8") if a.output else sys.stdout
    f.write("# gen.py --seed %d --count %d%s\n" % (a.seed, a.count, " --merge" if a.merge else ""))
    for args in g.run(a.count):
        f.write(corpus.dump_line(args) + "\n")
    if a.output:
        f.close()


if __name__ == "__main__":
    main()
