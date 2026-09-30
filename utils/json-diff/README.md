# JSON differential harness

Replays JSON command corpora against two separate servers and compares every
reply byte for byte. Server A is the reference: `valkey-server` with the
valkey-json module loaded. Server B is the server under test, normally a build
with native JSON and no module.

The two roles must be separate processes. One server replaying the same
mutations twice sees its own writes and reports divergences that are not real.
`replay` refuses to run if A and B report the same `run_id`.

## Building the reference module

Pinned release: valkey-json `1.0.3` (`c839dcc`), the latest tag. It has 23
`JSON.*` commands. `JSON.MERGE` exists only on `unstable`; to diff it, build
`unstable` the same way (`58faf1c` was used) and pass `--with-merge`.

On Amazon Linux 2 the stock toolchain works (gcc 7.3.1, `cmake3` 3.17; plain
`cmake` is 2.8 and too old). Skip `build.sh`, which clones and builds a Valkey
server; pass this tree's `valkeymodule.h` instead:

```sh
git clone --branch 1.0.3 https://github.com/valkey-io/valkey-json
cd valkey-json && mkdir build && cd build
cmake3 .. -DCMAKE_BUILD_TYPE=Release -DBUILD_RELEASE=ON \
    -DENABLE_UNIT_TESTS=OFF -DENABLE_INTEGRATION_TESTS=OFF \
    -DVALKEY_MODULE_H_PATH=<valkey>/src/valkeymodule.h
make -j
# -> build/src/libjson.so
```

## Running

```sh
# A (module) vs B (server under test, no module)
utils/json-diff/run.sh src/valkey-server path/to/libjson.so path/to/valkey-server-B

# plus a fresh random corpus
utils/json-diff/run.sh src/valkey-server libjson.so B --seed 42 --count 20000

# prove the harness works: module vs module must agree, module vs plain must not
utils/json-diff/run.sh --selfcheck src/valkey-server libjson.so
```

Other options: `--b-module SO` loads a module into B, `--keep` keeps server
directories and logs, `--max-report N`, `--allowlist FILE` (`''` disables it),
and explicit corpus files in place of `corpora/*.jsonl`. `jsondiff.py replay
--a host:port --b host:port FILE...` runs against servers you started
yourself. Exit status is 1 on any divergence or server failure.

After each corpus file the whole keyspace is compared too (`KEYS`, `TYPE`,
`JSON.GET key` and `JSON.GET key $`), so a mutation whose reply matches but
whose effect differs is still caught.

## Corpus format

One command per line as a JSON array of strings. An element may be
`{"hex": "..."}` for bytes that are not valid UTF-8. Lines starting with `#` are
comments. Do not convert to shell-style quoting: POSIX `shlex` splitting strips
the quotes inside JSON arguments, both servers return the same syntax error, and
the diff passes while testing nothing. `replay` round-trips a quoted canary
through this format to server A before every run and aborts if it does not
come back intact.

Each command is framed with an `ECHO` sentinel and everything the server sends
up to the sentinel counts as that command's reply. This matters because
valkey-json emits extra top-level replies for a legacy `JSON.RESP` on a
recursive path (`JSON.RESP k ..` on `{"a":{"b":1}}`); without framing, every
later reply shifts by one.

`corpora/`:

| file | covers |
|---|---|
| `numbers.jsonl` | number text round trips, exponent guard, arithmetic typing and overflow |
| `paths.jsonl` | legacy vs JSONPath, wildcards, `..`, slices, unions, filters, wrong-typed and unmatched paths, per command |
| `limits.jsonl` | nesting 127 to 130 and beyond, objects past 32 members, large arrays, `JSON.DEBUG` |
| `strings.jsonl` | escapes, multi-byte and invalid UTF-8, member-name quoting, `JSON.GET` formatting |
| `commands.jsonl` | arity, wrong key type, `CLEAR`/`TOGGLE` per type, array index edges, `MSET`/`MGET`, `COPY`/`RENAME` |
| `resp3.jsonl` | reads and mutations under `HELLO 3` and back |
| `generated-seed1.jsonl` | `gen.py --seed 1 --count 3000` |
| `merge.merge.jsonl` | `JSON.MERGE`, used only with `--with-merge` |

The targeted files are written by `mkcorpora.py`; edit the cases there and
rerun it rather than editing the `.jsonl` by hand. `gen.py --seed N --count N
[--merge]` writes a random corpus and is deterministic for a given seed.

## Allowlist

`allowlist.json` excuses divergences that are not behaviour. Each rule matches
the command name (`cmd`) and optionally the first argument (`sub`) as
case-insensitive regexes, with one of these modes:

- `ignore`: the reply is not compared (`OBJECT ENCODING`, `MEMORY USAGE`, `HELLO`).
- `casefold`: both replies match `reply` and are equal ignoring case (arity
  errors name `JSON.SET` for a module command and `json.set` natively).
- `mask-integers`: integer replies are masked before comparing, so the shape
  still has to match (`JSON.DEBUG MEMORY`, `MAX-SIZE-KEY`).

Counts of excused replies are printed per rule.

## Not covered

- `JSON.DEBUG KEYTABLE-*`, `TEST-SHARED-API` and `HELP`: module internals the
  native type does not have.
- `JSON.DEBUG MAX-DEPTH-KEY` and `MAX-SIZE-KEY` only run with a single key,
  since ties are broken by keyspace iteration order.
- Keyspace notifications, replication and persistence. `mkfixture.py` writes
  the module RDB fixture used for persistence tests instead:
  `tests/assets/json-module-v3.rdb` (42 keys, module encoding version 3) and
  `tests/assets/json-module-v3.expected` (`key<TAB>JSON.GET key $` per line).

## Reference module bugs found

Both 1.0.3 and `unstable` `58faf1c`:

- A stored number that rounds to infinity (`9e308`, a 310-digit integer)
  multiplied by zero through a legacy path asserts and kills the server
  (`JSON.SET x $ 9e308` then `JSON.NUMMULTBY x . 0`, `json.cc:1003`). With a
  `$` path the same operation returns `[null]`. `gen.py` keeps such numbers out
  of random documents; `numbers.jsonl` covers the non-crashing forms.
- Legacy `JSON.RESP` on a recursive path writes more replies than the command
  owns (see framing above).
