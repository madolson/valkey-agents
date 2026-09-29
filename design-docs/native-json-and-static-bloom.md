# Native JSON and statically linked bloom

Tracking issue: madolson/valkey-agents#13.

## Overview

valkey-bundle ships JSON and bloom as loadable modules. This plan brings both into `valkey-server`
by two different routes, landable independently and in either order. JSON is ported to C as a
native type, `OBJ_JSON`. Bloom stays the existing Rust module, vendored in-tree and linked into
the binary as a static module through the path Lua already uses.

The routes differ because the payoff differs. Native JSON measured faster than the module on
nearly every operation (+13% to +57%), lets `TYPE` and `OBJECT ENCODING` report something real,
and drops the module's 22.4 MB `KeyTable` that is paid whether or not a JSON key exists. A native
bloom would land at the same memory as the module, because the bitmap dominates, and would rewrite
working, on-disk-format-sensitive code for no footprint gain. Bloom's only case is distribution,
and static linking answers that at the lowest cost.

All measurements quoted here come from the prototype described in #13.

## JSON: native type

Each step below is one PR with its own tests. Order matters: the harness comes first because the
module's behaviour is not readable from its source, and every compatibility rule in #13 was found
by diffing against a live module.

### 1. Differential harness and fixtures

- Seeded random corpus generator plus replay tool. It sends identical command sequences to a
  module-loaded server and a native server and compares replies byte for byte. The two roles must
  be separate servers; one server sees its own mutations and reports false divergences.
- Do not split corpus lines with `shlex` in POSIX mode. It strips quotes from JSON arguments, both
  sides return the same syntax error, and the harness reports zero divergences while testing
  nothing.
- Commit an allowlist of known, non-behavioural divergences: arity error capitalisation,
  `OBJECT ENCODING`, memory figures.
- Commit module-written RDB fixtures (encoding version 3) under `tests/assets/`.

The harness lives under `utils/` and runs in CI against the pinned valkey-json release. It is the
acceptance test for every later step.

### 2. Type wiring

Follow #4506 (native path hash, `a84b2f5b9`), which is the most recent example: 41 files, +2,785
lines. Touchpoints:

- `src/server.h:798-818`: `OBJ_JSON 8`, `OBJ_TYPE_MAX 9`, `OBJ_ENCODING_JSON`, `NOTIFY_JSON`
  (class `j`; `g$lshzxetmdn` are taken), `@json` ACL category, `json` command group.
- `src/rdb.h:133`: `RDB_TYPE_JSON = 24`, update `rdbIsObjectType()`.
- `db.c`, `object.c`, `lazyfree.c`, `aof.c`, `debug.c`, `notify.c`, `acl.c`,
  `valkey-check-rdb.c`, `module.c` (`VALKEYMODULE_KEYTYPE_JSON`, see `module.c:4451`).
- `defrag.c`: explicit no-op case until step 8. Without it the switch ends in `serverPanic`.
- `utils/generate-command-code.py` does not handle dotted command names. Fix it, and add the
  `json` group in the three places the generator requires to stay in sync.
- Makefile and CMake source lists.

### 3. DOM, parser, serializer

`src/json.c`, `src/json.h`, unit tests in `src/unit/`. Rules the DOM has to hold:

- Non-integer numbers keep their source text and re-emit it verbatim (`1E2` stays distinct from
  `1e2`; `-0.0`, `0.30000000000000004`, `18446744073709551615` round-trip, including through RDB).
- Number rejection reproduces RapidJSON's exponent guard, not an overflow check. It accepts
  `9e308` and a 401-digit integer, and rejects `0e309`. `strtod` plus `ERANGE` diverges on eight
  observed inputs.
- Objects are insertion-ordered vectors, including past 32 members where the module switches to a
  hash table internally.
- Nesting limit 128 (`json.max-path-limit`); 129 is a `LIMIT` error.
- Serializer writes through a cursor into a buffer reserved once from a length estimate, with a
  word-at-a-time escape scan and a no-escape fast path. The per-token `sdscatlen` design ran −64%
  on a 3 KB document.
- Parser sizes each string exactly before copying. Greedy sds growth doubled string cost.

### 4. Path engine

JSONPath plus the legacy `.` syntax. The spec is the rule list in #13 §3, reproduced as a table in
this document when the PR lands. The rules a clean-room implementation gets wrong:

- A missing legacy path is an error, except for `JSON.TYPE` and `JSON.OBJKEYS`, where it is null.
- A wrong-typed path step is an error only for `JSON.SET` and `JSON.DEL`.
- Wildcard on an existing scalar is `Invalid use of wildcard` for mutating commands; recursive
  steps are exempt.
- `JSON.SET` through a wildcard or recursive path that matched nothing is a no-op success; any
  other unmatched path is an error.
- `..` expands descendant-or-self in DFS preorder.

### 5. Commands

All 24 `JSON.*` commands, `src/commands/json-*.json`, and `tests/unit/type/json.tcl`. The
behaviour that needs explicit tests: `JSON.RESP` re-renders numbers as doubles (unlike
`JSON.GET`), arithmetic result typing follows the operand literal, `JSON.CLEAR` resets per type,
and `JSON.DEL` with an explicitly empty path deletes nothing.

### 6. Persistence and migration

- Native RDB format stores serialized JSON text, the same thing the module writes at encoding
  version 3. `RDB_VERSION` goes to 82 (`src/rdb.h:53`; 81 is taken by path hash).
- AOF rewrite emits `JSON.SET key $ <doc>`.
- Built-in loader for module type id `ReJSON-RL`. Module type names are 9 characters, so
  `moduleTypeEncodeId()` reproduces the id already in existing files. Encoding version 3 is a
  text parse. Version 0 is refused with a clear error.
- Upgrade path: attach a native replica to a module primary, full sync, fail over. The prototype
  verified this end to end. The reverse is refused by the RDB version check, so downgrade is
  dump-and-restore.

### 7. Upgrade compatibility

Without these a binary swap fails even though the data migrates.

- **Config routing (blocker).** `src/config.c:637` sends every dotted directive to the module
  config queue before standard config lookup, and `moduleLoadFromQueue()` exits on leftovers
  (`src/module.c:13492`). A conf file with `json.max-path-limit 128` stops the server. Look dotted
  names up in the standard config dict first, then register `json.max-document-size` and
  `json.max-path-limit` under those exact names.
- **INFO.** Add a `json` section answering `INFO json` and `INFO everything` but not default
  `INFO`, matching the module's `json_core_metrics` fields so scrapers keep working.
- **Shared API.** valkey-search indexes JSON through `SharedJSON_Get`, exported with
  `ValkeyModule_ExportSharedAPI`. Core must register the same name under a static module
  identity. Verify the exact lookup string in valkey-search and whether it type-checks keys by
  module type pointer; if it does, search needs a matching change.
- **`JSON.DEBUG`.** Add `MAX-DEPTH-KEY` and `MAX-SIZE-KEY`. Skip `KEYTABLE-*`, which describes
  interning internals that do not exist natively.

### 8. Memory parity, compact encoding, defrag

Current gap is 1.0x to 1.79x the module for anything with a container. The arithmetic to parity:

| | now | + inline values | + 16-byte value | + interned names | module |
|---|---|---|---|---|---|
| array element | 33 B | 24 B | 16 B | 16 B | 16-18 B |
| object member | 48 B | 32 B | 24 B | 24 B | 24-25 B |

Order: inline values and the 16-byte value first, then a compact encoding for small documents on
top, so the inline representation is written once.

Inline values invalidate cached `jsonValue *` on any container growth. The command layer has to
address values by `(parent, index)` and re-resolve after mutation. `JSON.SET` multi-match,
`ARR*`, `MERGE`, `CLEAR` and the arithmetic commands all hold refs across mutations. The harness
polices this.

Compact encoding: zuiderkwast asked on #13 whether to use BSON or UBJSON instead of listpack.
Listpack cannot hold nesting, so it is out. BSON loses on two requirements: it has no way to keep
number source text and it stores objects as documents whose key order clients treat as incidental.
Proposal: a small length-prefixed binary format modeled on UBJSON (type byte, length, payload,
nested containers inline) with a number-text type, used below a size threshold and converted to
the DOM above it, as small hashes convert from listpack.

Defrag comes last, once the layout is settled. It has to walk the DOM and reassign each parent's
child pointer.

**This step gates enabling JSON in default builds.** Steps 1-7 can merge behind a build flag.

## Bloom: static module

### 1. Vendor the source

`src/modules/bloom/` holds valkey-bloom at a pinned release, its `Cargo.lock`, and `cargo vendor`
output, so the build is offline and does not reach into the module repo (the constraint
zuiderkwast set in #2657). An update is a vendor bump PR.

### 2. Build flag

`BUILD_BLOOM=no|module|static`, mirroring `BUILD_LUA` (`src/Makefile:303`) and
`cmake/Modules/ValkeySetup.cmake`. `static` builds the crate as a `staticlib` and links it with
`--whole-archive` and `--export-dynamic`, like `libvalkeylua.a`. On macOS, `-force_load`.

Rust's std links statically, so unlike valkey-json this adds no `libstdc++` runtime dependency.

### 3. Entry points

Export `ValkeyModule_OnLoad_bloom` and `ValkeyModule_OnUnload_bloom` with `#[no_mangle]` and
default visibility, which `moduleLoadStaticSymbol()` resolves by `dlsym` on the running binary
(`src/module.c:13923`). Hidden visibility turns this into a startup panic rather than a link
error, so CI must start the binary, not just link it.

### 4. Startup ordering

Lua loads at `src/server.c:8177`, after `moduleLoadFromQueue()` at `src/server.c:8164` has already
exited on any unconsumed module config. Loading bloom at the same point means any `bf.bloom-*`
line in a conf file kills startup. Bloom must load inside `moduleLoadFromQueue()`, before the
leftover-config check, and in any case before `loadDataFromDisk()` (`src/server.c:8195`) so
`bloomfltr` payloads in RDB and AOF resolve.

### 5. Link hazards

Each needs a fix or a test that fails without it:

- `valkey-module` 0.1.14's `build.rs` reuses one `cc::Build` across two `compile()` calls, so the
  staticlib carries two copies of `redismodule.o`. Harmless for a `cdylib`, a duplicate symbol
  under `--whole-archive`. Fix upstream and carry the patch in the vendored crate until released.
- Whole-archive linking runs the module's global constructors before `main()`, before the module
  API table exists. A constructor calling `ValkeyModule_Alloc` crashes silently, while behaving
  fine as a `.so`. Add a startup assertion and a rule that in-tree modules have no eager
  constructors.
- All static modules share one merged `ValkeyModule_*` pointer table (`__common__` symbols),
  pinning them to one header vintage. Test Lua and bloom together.
- Binary grows about 4.4 MB stripped, 1.7 MB more than the `.so`, because whole-archive defeats
  dead-code elimination. Try `--gc-sections` and LTO and record the result.

### 6. Tests

- Run valkey-bloom's Python suite against a `BUILD_BLOOM=static` server with no `--loadmodule`.
- Tcl test: start with `bf.*` configs in the conf file and a bloom RDB fixture; assert the server
  starts, data loads, and `CONFIG GET bf.*` returns the configured values.
- CI job per platform for `BUILD_BLOOM=static`.

## Decisions for maintainers

| decision | proposal |
|---|---|
| `TYPE` on native JSON keys | keep `ReJSON-RL` for now; changing it breaks clients for a cosmetic gain |
| keyspace event class | module fires `d`, native fires `j`; fire `j` and document that `Kd` subscribers must add `j` |
| `BUILD_BLOOM` default | `no` until every CI platform has a Rust toolchain, then `static` |
| Rust in the core build | accept it, scoped to `src/modules/bloom` and opt-in until the default flips |
| Top-K, Cuckoo, Count-Min Sketch (#4374) | stay in the bloom module and ride the same static link |
| compact JSON encoding | UBJSON-style binary format, above |

## Out of scope

- valkey-search: an index lifecycle, threading model and cluster coordination, not a data type.
- valkey-ldap: an auth callback.
- A native C bloom type. The bit-exact C primitive from the prototype stays available if that is
  ever wanted.
- `MODULE UNLOAD lua` permanently disabling scripting on a server with `enable-module-command yes`.
  A real bug, filed separately.
