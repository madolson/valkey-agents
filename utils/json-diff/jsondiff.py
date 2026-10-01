#!/usr/bin/env python3
"""Differential replay of JSON command corpora against two servers.

Subcommands:
  replay     replay corpora against two already running servers
  run        start server A (module loaded) and server B, then replay
  selfcheck  prove the harness reports a known divergence and none for A vs A

Role A is the reference: a server with the valkey-json module loaded. Role B is
the server under test. They must be separate processes; one server sees its own
mutations twice and reports divergences that are not real.
"""

import argparse
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import corpus
import gen
import resp

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ALLOWLIST = os.path.join(HERE, "allowlist.json")
CANARY = '["JSON.SET", "__canary__", "$", "{\\"a\\":\\"q\\\\\\"\\"}"]'
CANARY_GET = b'$11\r\n{"a":"q\\""}\r\n'


# --------------------------------------------------------------------- allowlist
class Allowlist(object):
    def __init__(self, path):
        self.rules = []
        if path:
            with open(path) as f:
                for r in json.load(f):
                    r["_cmd"] = re.compile(r["cmd"], re.I)
                    r["_sub"] = re.compile(r["sub"], re.I) if "sub" in r else None
                    r["_reply"] = re.compile(r["reply"].encode(), re.I | re.S) if "reply" in r else None
                    for side in ("a", "b"):
                        r["_" + side] = re.compile(r[side].encode(), re.S) if side in r else None
                    if r["mode"] not in ("ignore", "casefold", "mask-integers", "prefix", "pair"):
                        raise ValueError("unknown allowlist mode %r" % r["mode"])
                    self.rules.append(r)

    def match(self, args, ra, rb):
        """Return the id of the rule that excuses ra != rb, or None."""
        name = args[0].decode("utf-8", "replace")
        sub = args[1].decode("utf-8", "replace") if len(args) > 1 else ""
        for r in self.rules:
            if not r["_cmd"].match(name):
                continue
            if r["_sub"] is not None and not r["_sub"].match(sub):
                continue
            mode = r["mode"]
            if mode == "ignore":
                return r["id"]
            if mode == "casefold":
                if r["_reply"].match(ra) and r["_reply"].match(rb) and ra.lower() == rb.lower():
                    return r["id"]
            elif mode == "mask-integers":
                pat = re.compile(rb"^:-?\d+\r\n", re.M)
                if pat.sub(b":N\r\n", ra) == pat.sub(b":N\r\n", rb):
                    return r["id"]
            elif mode == "prefix":
                if rb and len(rb) < len(ra) and ra.startswith(rb):
                    return r["id"]
            elif mode == "pair":
                if r["_a"].search(ra) and r["_b"].search(rb):
                    return r["id"]
        return None


# ---------------------------------------------------------------------- replay
class Result(object):
    def __init__(self):
        self.commands = 0
        self.divergences = []
        self.allowed = {}
        self.a_errors = 0
        self.fatal = None


def run_id(conn):
    info = conn.call([b"INFO", b"server"])
    m = re.search(rb"run_id:([0-9a-f]+)", info)
    return m.group(1) if m else None


def check_distinct(ha, pa, hb, pb):
    a = resp.Conn(ha, pa)
    b = resp.Conn(hb, pb)
    try:
        if run_id(a) == run_id(b):
            raise SystemExit("error: A and B are the same server (%s:%d). Use two servers." % (ha, pa))
    finally:
        a.close()
        b.close()


def check_canary(host, port):
    """Round-trip a quoted JSON argument through the corpus format to server A."""
    c = resp.Conn(host, port)
    try:
        (lineno, args), = corpus_lines([CANARY])
        r = c.call(args)
        g = c.call([b"JSON.GET", b"__canary__"])
        c.call([b"DEL", b"__canary__"])
    finally:
        c.close()
    if r != b"+OK\r\n" or g != CANARY_GET:
        raise SystemExit("error: canary failed on A, JSON arguments are not arriving intact "
                         "(or A has no JSON module): SET -> %r GET -> %r" % (r, g))


def corpus_lines(lines):
    fd, path = tempfile.mkstemp(suffix=".jsonl")
    with os.fdopen(fd, "w") as f:
        f.write("\n".join(lines) + "\n")
    try:
        return corpus.load(path)
    finally:
        os.unlink(path)


def compare(allow, args, ra, rb, where, res):
    if resp.is_error(ra):
        res.a_errors += 1
    if ra == rb:
        return
    rid = allow.match(args, ra, rb)
    if rid:
        res.allowed[rid] = res.allowed.get(rid, 0) + 1
        return
    res.divergences.append((where, args, ra, rb))


def final_state(ca, cb, allow, name, res):
    """Compare the whole keyspace after a corpus, to catch silent mutation bugs."""
    ka = sorted(parse_array(ca.call([b"KEYS", b"*"])))
    kb = sorted(parse_array(cb.call([b"KEYS", b"*"])))
    if ka != kb:
        res.divergences.append(("%s:final-keys" % name, [b"KEYS", b"*"],
                                b"\n".join(ka), b"\n".join(kb)))
    for k in ka:
        if k not in kb:
            continue
        for args in ([b"TYPE", k], [b"JSON.GET", k], [b"JSON.GET", k, b"$"]):
            ra, rb = ca.call(args), cb.call(args)
            res.commands += 1
            compare(allow, args, ra, rb, "%s:final" % name, res)
            if args[0] == b"TYPE" and ra != b"+ReJSON-RL\r\n":
                break


def parse_array(raw):
    """Parse a flat RESP2 array of bulk strings (KEYS reply)."""
    end = raw.index(b"\r\n")
    n = int(raw[1:end])
    pos = end + 2
    out = []
    for _ in range(n):
        end = raw.index(b"\r\n", pos)
        ln = int(raw[pos + 1:end])
        out.append(raw[end + 2:end + 2 + ln])
        pos = end + 2 + ln + 2
    return out


def nice(path):
    rel = os.path.relpath(path)
    return os.path.abspath(path) if rel.startswith("..") else rel


def replay_file(path, a, b, allow, res, final=True):
    cmds = corpus.load(path)
    name = nice(path)
    ca = resp.Conn(*a)
    cb = resp.Conn(*b)
    try:
        ca.call([b"FLUSHALL"])
        cb.call([b"FLUSHALL"])
        for lineno, args in cmds:
            where = "%s:%d" % (name, lineno)
            try:
                ra = ca.call(args)
            except (resp.RespError, OSError) as e:
                res.fatal = "%s: server A failed: %s" % (where, e)
                return
            try:
                rb = cb.call(args)
            except (resp.RespError, OSError) as e:
                res.fatal = "%s: server B failed (crash?): %s on %s" % (where, e, corpus.show(args))
                return
            res.commands += 1
            compare(allow, args, ra, rb, where, res)
        ca.close()
        cb.close()
        if final:
            ca = resp.Conn(*a)
            cb = resp.Conn(*b)
            final_state(ca, cb, allow, name, res)
    finally:
        ca.close()
        cb.close()


def trunc(raw, limit=300):
    r = repr(raw)
    return r if len(r) <= limit else r[:limit] + "...(%d bytes)" % len(raw)


def report(res, max_report, out=sys.stdout):
    for where, args, ra, rb in res.divergences[:max_report]:
        out.write("DIVERGENCE %s\n  cmd: %s\n  A:   %s\n  B:   %s\n"
                  % (where, corpus.show(args), trunc(ra), trunc(rb)))
    if len(res.divergences) > max_report:
        out.write("... %d more divergences not shown\n" % (len(res.divergences) - max_report))
    allowed = ", ".join("%s=%d" % kv for kv in sorted(res.allowed.items())) or "none"
    out.write("commands=%d divergences=%d a_errors=%d allowlisted: %s\n"
              % (res.commands, len(res.divergences), res.a_errors, allowed))
    if res.fatal:
        out.write("FATAL %s\n" % res.fatal)


def replay(a, b, files, allowlist, final=True):
    check_distinct(a[0], a[1], b[0], b[1])
    check_canary(*a)
    allow = Allowlist(allowlist)
    res = Result()
    for f in files:
        replay_file(f, a, b, allow, res, final)
        if res.fatal:
            break
    return res


# --------------------------------------------------------------------- servers
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server(object):
    def __init__(self, binary, module, role, workroot):
        self.dir = os.path.join(workroot, role)
        os.makedirs(self.dir)
        self.port = free_port()
        cmd = [os.path.abspath(binary), "--port", str(self.port), "--bind", "127.0.0.1",
               "--save", "", "--appendonly", "no", "--dir", self.dir,
               "--logfile", os.path.join(self.dir, "server.log")]
        if module:
            cmd += ["--loadmodule", os.path.abspath(module)]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        deadline = time.time() + 20
        while True:
            try:
                c = resp.Conn("127.0.0.1", self.port, timeout=2)
                if c.call([b"PING"]) == b"+PONG\r\n":
                    c.close()
                    break
                c.close()
            except (OSError, resp.RespError):
                pass
            if self.proc.poll() is not None or time.time() > deadline:
                raise SystemExit("error: server %s did not start, see %s/server.log" % (role, self.dir))
            time.sleep(0.05)

    @property
    def addr(self):
        return ("127.0.0.1", self.port)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()


def default_corpora(with_merge):
    files = sorted(glob.glob(os.path.join(HERE, "corpora", "*.jsonl")))
    if not with_merge:
        files = [f for f in files if not f.endswith(".merge.jsonl")]
    return files


def generated_corpus(workroot, seed, count, merge):
    path = os.path.join(workroot, "gen-seed%d-count%d.jsonl" % (seed, count))
    g = gen.Gen(seed, merge)
    with open(path, "w", encoding="utf-8") as f:
        for args in g.run(count):
            f.write(corpus.dump_line(args) + "\n")
    return path


def run_pair(bin_a, module, bin_b, b_module, files, allowlist, keep):
    workroot = tempfile.mkdtemp(prefix="json-diff-")
    servers = []
    try:
        sa = Server(bin_a, module, "A", workroot)
        servers.append(sa)
        sb = Server(bin_b, b_module, "B", workroot)
        servers.append(sb)
        res = replay(sa.addr, sb.addr, files, allowlist)
        for s in servers:
            if s.proc.poll() is not None:
                res.fatal = (res.fatal or "") + " server exited, see %s/server.log" % s.dir
        return res
    finally:
        for s in servers:
            s.stop()
        if keep:
            sys.stdout.write("kept server dirs in %s\n" % workroot)
        else:
            shutil.rmtree(workroot, ignore_errors=True)


# ------------------------------------------------------------------ subcommands
def hostport(s):
    h, _, p = s.rpartition(":")
    return (h or "127.0.0.1", int(p))


def cmd_replay(a):
    res = replay(hostport(a.a), hostport(a.b), a.corpus, a.allowlist, not a.no_final_state)
    report(res, a.max_report)
    return 1 if res.divergences or res.fatal else 0


def collect_files(a, workroot):
    files = list(a.corpus) if a.corpus else default_corpora(a.with_merge)
    if a.count > 0:
        files.append(generated_corpus(workroot, a.seed, a.count, a.with_merge))
    return files


def cmd_run(a):
    tmp = tempfile.mkdtemp(prefix="json-diff-gen-")
    try:
        files = collect_files(a, tmp)
        sys.stdout.write("A=%s + %s\nB=%s%s\ncorpora: %s\n" % (
            a.server_a, a.module, a.server_b, " + " + a.b_module if a.b_module else "",
            " ".join(nice(f) for f in files)))
        res = run_pair(a.server_a, a.module, a.server_b, a.b_module, files, a.allowlist, a.keep)
        report(res, a.max_report)
        return 1 if res.divergences or res.fatal else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def cmd_selfcheck(a):
    ok = True
    allow = Allowlist(a.allowlist)

    # 1. The comparator reports a one-byte difference and a lone RESP type change.
    probes = [
        ([b"JSON.GET", b"k"], b"$3\r\n1E2\r\n", b"$3\r\n1e2\r\n"),
        ([b"JSON.TYPE", b"k"], b"$-1\r\n", b"_\r\n"),
        ([b"JSON.DEBUG", b"MEMORY", b"k"], b":10\r\n", b"$2\r\n10\r\n"),
        ([b"JSON.SET", b"k"], b"-ERR wrong number of arguments for 'JSON.SET' command\r\n",
         b"-ERR wrong number of arguments for 'json.get' command\r\n"),
    ]
    for args, ra, rb in probes:
        res = Result()
        compare(allow, args, ra, rb, "probe", res)
        if len(res.divergences) != 1:
            sys.stdout.write("SELFCHECK FAIL: comparator missed %r vs %r\n" % (ra, rb))
            ok = False
    excused = [
        ([b"JSON.SET", b"k"], b"-ERR wrong number of arguments for 'JSON.SET' command\r\n",
         b"-ERR wrong number of arguments for 'json.set' command\r\n"),
        ([b"JSON.DEBUG", b"MEMORY", b"k", b"$"], b"*1\r\n:10\r\n", b"*1\r\n:99\r\n"),
    ]
    for args, ra, rb in excused:
        res = Result()
        compare(allow, args, ra, rb, "probe", res)
        if res.divergences:
            sys.stdout.write("SELFCHECK FAIL: allowlist did not excuse %r vs %r\n" % (ra, rb))
            ok = False

    tmp = tempfile.mkdtemp(prefix="json-diff-gen-")
    try:
        files = collect_files(a, tmp)

        # 2. Module vs module: two separate reference servers agree everywhere.
        res = run_pair(a.server, a.module, a.server, a.module, files, a.allowlist, False)
        sys.stdout.write("module vs module: ")
        report(res, 20)
        if res.divergences or res.fatal:
            sys.stdout.write("SELFCHECK FAIL: module vs module diverged\n")
            ok = False
        if res.commands == 0 or res.a_errors * 10 > res.commands * 9:
            sys.stdout.write("SELFCHECK FAIL: nearly every reply on A is an error\n")
            ok = False

        # 3. Module vs plain server: every JSON.* command must diverge.
        res = run_pair(a.server, a.module, a.server, None, files, a.allowlist, False)
        sys.stdout.write("module vs plain: ")
        report(res, 3)
        if res.fatal or len(res.divergences) * 2 < res.commands:
            sys.stdout.write("SELFCHECK FAIL: module vs plain server reported too few divergences\n")
            ok = False
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    sys.stdout.write("SELFCHECK %s\n" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="sub")

    def common(p):
        p.add_argument("--allowlist", default=DEFAULT_ALLOWLIST,
                       help="allowlist file, '' to disable (default: allowlist.json)")
        p.add_argument("--max-report", type=int, default=50)

    def gen_opts(p):
        p.add_argument("--seed", type=int, default=1)
        p.add_argument("--count", type=int, default=0,
                       help="also replay a freshly generated corpus of this many commands")
        p.add_argument("--with-merge", action="store_true",
                       help="include JSON.MERGE (needs a module built from valkey-json unstable)")
        p.add_argument("corpus", nargs="*", help="corpus files (default: corpora/*.jsonl)")

    p = sp.add_parser("replay")
    common(p)
    p.add_argument("--a", required=True, help="host:port of the reference (module) server")
    p.add_argument("--b", required=True, help="host:port of the server under test")
    p.add_argument("--no-final-state", action="store_true")
    p.add_argument("corpus", nargs="+")

    p = sp.add_parser("run")
    common(p)
    p.add_argument("--server-a", required=True)
    p.add_argument("--module", required=True)
    p.add_argument("--server-b", required=True)
    p.add_argument("--b-module", help="module to load into B (default: none)")
    p.add_argument("--keep", action="store_true", help="keep server dirs and logs")
    gen_opts(p)

    p = sp.add_parser("selfcheck")
    common(p)
    p.add_argument("--server", required=True)
    p.add_argument("--module", required=True)
    gen_opts(p)

    a = ap.parse_args()
    if a.sub == "replay":
        return cmd_replay(a)
    if a.sub == "run":
        return cmd_run(a)
    if a.sub == "selfcheck":
        return cmd_selfcheck(a)
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
