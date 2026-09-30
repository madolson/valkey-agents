"""Corpus file format.

One command per line, encoded as a JSON array. Each element is either a
string (sent as its UTF-8 bytes) or {"hex": "..."} for arguments that are not
valid UTF-8. Blank lines and lines starting with '#' are ignored.

JSON arrays are used instead of shell-style quoting because POSIX shlex
splitting strips the quotes inside JSON arguments. Both servers then reply with
the same syntax error and the diff passes while testing nothing.
"""

import binascii
import json


def load(path):
    cmds = []
    with open(path, "rb") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.decode("utf-8").strip()
            if not line or line.startswith("#"):
                continue
            arr = json.loads(line)
            if not isinstance(arr, list) or not arr:
                raise ValueError("%s:%d: expected a non-empty JSON array" % (path, lineno))
            args = []
            for a in arr:
                if isinstance(a, str):
                    args.append(a.encode("utf-8"))
                elif isinstance(a, dict) and "hex" in a:
                    args.append(binascii.unhexlify(a["hex"]))
                else:
                    raise ValueError("%s:%d: bad argument %r" % (path, lineno, a))
            cmds.append((lineno, args))
    return cmds


def dump_line(args):
    out = []
    for a in args:
        if isinstance(a, bytes):
            try:
                out.append(a.decode("utf-8"))
            except UnicodeDecodeError:
                out.append({"hex": binascii.hexlify(a).decode("ascii")})
        else:
            out.append(a)
    return json.dumps(out, ensure_ascii=False)


def show(args, limit=200):
    """Human-readable rendering of a command for reports."""
    s = dump_line(args)
    if len(s) > limit:
        s = s[:limit] + "...(%d chars)" % len(s)
    return s
