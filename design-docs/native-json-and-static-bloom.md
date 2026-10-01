# Native JSON and statically linked bloom

Tracking issue: madolson/valkey-agents#13.

## Overview

valkey-bundle ships JSON and bloom as loadable modules. This plan builds a `valkey-server` that
has both with no `--loadmodule`, by two different routes. JSON is ported to C as a native type,
`OBJ_JSON`. Bloom stays the existing Rust module, vendored in-tree and linked into the binary as a
static module through the path Lua already uses.

The routes differ because the payoff differs. Native JSON measured faster than the module on
nearly every operation (+13% to +57%), lets `TYPE` and `OBJECT ENCODING` report something real,
and drops the module's 22.4 MB `KeyTable` that is paid whether or not a JSON key exists. A native
bloom would land at the same memory as the module, because the bitmap dominates, and would rewrite
working, on-disk-format-sensitive code for no footprint gain. Bloom's only case is distribution,
and static linking answers that at the lowest cost.

All measurements quoted here come from the prototype described in #13.

## Test gate

The work is a sequence of steps on one branch. Every step ends with a commit, and no step is done
until all of the following pass on that commit:

- `make` with no warnings.
- `make -C src test-unit`.
- `./runtest` full suite, compared against the baseline recorded in step 0. A test that failed on
  the baseline may still fail; nothing else may.
- `./runtest-moduleapi`, since static modules and the module config path are both touched.
- The tests the step adds.
- From step 5 on, the JSON differential harness with zero behavioural divergences.

The JSON steps (1 to 7) and the bloom steps (8 and 9) do not depend on each other and can proceed
in parallel. Step 10 joins them.

## Step 0: baseline

Build the branch base and run the full gate. Record every failing test by name. This list is the
only allowance later steps get.

## JSON

### Step 1: differential harness and fixtures

The module's behaviour is not readable from its source. Every compatibility rule in #13 was found
by diffing against a live module, so the harness comes before any JSON code.

- Build valkey-json at a pinned release as a reference `.so`, outside the tree.
- Seeded random corpus generator plus replay tool under `utils/json-diff/`. It sends identical
  command sequences to a module-loaded server and a native server and compares replies byte for
  byte. The two roles must be separate servers; one server sees its own mutations and reports
  false divergences.
- Do not split corpus lines with `shlex` in POSIX mode. It strips quotes from JSON arguments, both
  sides return the same syntax error, and the harness reports zero divergences while testing
  nothing. Include a self-check that a known-divergent pair is reported.
- Allowlist of known, non-behavioural divergences: arity error capitalisation, `OBJECT ENCODING`,
  memory figures.
- Module-written RDB fixtures (encoding version 3) under `tests/assets/`.

### Step 2: type wiring

Follow #4506 (native path hash, `a84b2f5b9`), the most recent example: 41 files, +2,785 lines.

- `src/server.h:798-818`: `OBJ_JSON 8`, `OBJ_TYPE_MAX 9`, `OBJ_ENCODING_JSON`, `NOTIFY_JSON`
  (class `j`; `g$lshzxetmdn` are taken), `@json` ACL category, `json` command group.
- `src/rdb.h:133`: `RDB_TYPE_JSON = 24`, update `rdbIsObjectType()`.
- `db.c`, `object.c`, `lazyfree.c`, `aof.c`, `debug.c`, `notify.c`, `acl.c`,
  `valkey-check-rdb.c`, `module.c` (`VALKEYMODULE_KEYTYPE_JSON`, see `module.c:4451`).
- `defrag.c`: explicit no-op case. Without it the switch ends in `serverPanic`.
- `utils/generate-command-code.py` does not handle dotted command names. Fix it, and add the
  `json` group in the three places the generator requires to stay in sync.
- Makefile and CMake source lists.

### Step 3: DOM, parser, serializer

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

### Step 4: path engine

JSONPath plus the legacy `.` syntax, with unit tests. The rules a clean-room implementation gets
wrong:

- A missing legacy path is an error, except for `JSON.TYPE` and `JSON.OBJKEYS`, where it is null.
- A wrong-typed path step is an error only for `JSON.SET` and `JSON.DEL`.
- Wildcard on an existing scalar is `Invalid use of wildcard` for mutating commands; recursive
  steps are exempt.
- `JSON.SET` through a wildcard or recursive path that matched nothing is a no-op success; any
  other unmatched path is an error.
- `..` expands descendant-or-self in DFS preorder.

### Step 5: commands

All 24 `JSON.*` commands (23 in valkey-json 1.0.3; `JSON.MERGE` exists only on its unstable
branch and is compared against that build), `src/commands/json-*.json`, and `tests/unit/type/json.tcl`. Behaviour
that needs explicit tests: `JSON.RESP` re-renders numbers as doubles (unlike `JSON.GET`),
arithmetic result typing follows the operand literal, `JSON.CLEAR` resets per type, and `JSON.DEL`
with an explicitly empty path deletes nothing. The harness joins the gate here.

### Step 6: persistence and migration

- Native RDB format stores serialized JSON text, the same thing the module writes at encoding
  version 3. `RDB_VERSION` goes to 82 (`src/rdb.h:53`; 81 is taken by path hash).
- AOF rewrite emits `JSON.SET key . <doc>`, matching the module's `aof_rewrite`.
- Load parses with a nesting limit of 10000 (`JSON_MAX_DEPTH_LIMIT`). The parser is recursive,
  so an unbounded limit lets a crafted RESTORE payload exhaust the stack.
- Built-in loader for module type id `ReJSON-RL`. Module type names are 9 characters, so
  `moduleTypeEncodeId()` reproduces the id already in existing files. Encoding version 3 is a
  text parse. Version 0 is refused with a clear error.
- Tests: load the step 1 fixtures with byte-identical observable state; `DEBUG RELOAD` and AOF
  rewrite round-trips; replication from a module-loaded primary to a native replica.

### Step 7: upgrade compatibility

Without these a binary swap fails even though the data migrates.

- **Configs.** Register the three configs valkey-json 1.0.3 has, under its exact names:
  `json.max-document-size`, `json.max-path-limit` and the hidden `json.debug-mode`. No routing
  change is needed: `loadServerConfigFromString` already looks a name up in the standard config
  table before queuing dotted names for modules. `json.max-path-limit` is capped at 10000, the load
  limit above; the module allows up to `INT_MAX`. A module that registers a config whose full name
  is already a server config is now refused, since unloading it would otherwise delete the
  server's config.
- **INFO.** Add a `json` section answering `INFO json` and `INFO everything` but not default
  `INFO`, matching the module's `json_core_metrics` fields so scrapers keep working.
- **`JSON.DEBUG`** `MAX-DEPTH-KEY` and `MAX-SIZE-KEY` land with the other commands in step 5.
  `KEYTABLE-*` is skipped; it describes interning internals that do not exist natively.

## Bloom

### Step 8: static link

- **Vendor.** `src/modules/bloom/` holds valkey-bloom at a pinned release, its `Cargo.lock`, and
  `cargo vendor` output, so the build is offline and does not reach into the module repo (the
  constraint zuiderkwast set in #2657).
- **Build flag.** `BUILD_BLOOM=no|module|static`, mirroring `BUILD_LUA` (`src/Makefile:303`) and
  `cmake/Modules/ValkeySetup.cmake`. `static` builds the crate as a `staticlib` and links it with
  `--whole-archive` and `--export-dynamic`, like `libvalkeylua.a`; `-force_load` on macOS. Rust's
  std links statically, but its unwinder adds a runtime dependency on `libgcc_s.so.1`.
- **Entry points.** The module registers as `bf`, its config prefix, and the server resolves a
  static module's unload function by its registered name. Export `ValkeyModule_OnLoad_bf` and
  `ValkeyModule_OnUnload_bf` with `#[no_mangle]` and default visibility, which
  `moduleLoadStaticSymbol()` resolves by `dlsym` on the running binary (`src/module.c:13923`).
  `MODULE LIST` shows `bf`.
- **Module options.** Upstream bloom declares only `HANDLE_IO_ERRORS`. While any loaded module
  lacks the others, the server refuses `CLUSTER MIGRATESLOTS`, disables swapdb async loading and
  falls back from forkless to fork saves, so a default build would lose all three. The vendored
  copy also declares `HANDLE_REPL_ASYNC_LOAD`, `HANDLE_ATOMIC_SLOT_MIGRATION` and
  `HANDLE_FORKLESS`; bloom keeps no state tied to key ownership. This belongs upstream in
  valkey-bloom.
- **Startup ordering.** Lua loads at `src/server.c:8177`, after `moduleLoadFromQueue()` at
  `src/server.c:8164` has already exited on any unconsumed module config. Loading bloom at the same
  point means any `bf.bloom-*` line in a conf file kills startup. Bloom loads inside
  `moduleLoadFromQueue()`, before the leftover-config check, and so before `loadDataFromDisk()`
  (`src/server.c:8195`) where `bloomfltr` payloads resolve.
- **Link hazards**, each with a fix or a test that fails without it:
  - `valkey-module` 0.1.14's `build.rs` reuses one `cc::Build` across two `compile()` calls, so
    the staticlib carries two copies of `redismodule.o`: a duplicate symbol under
    `--whole-archive`. Patch the vendored crate.
  - Whole-archive linking runs global constructors before `main()`, before the module API table
    exists. Bloom's only one is std's `ARGV_INIT_ARRAY`, which stores argv, and the Rust allocator
    already aborts with a message if used before OnLoad, so no extra assertion is needed.
  - Static modules share one merged `ValkeyModule_*` pointer table (`__common__` symbols). Test Lua
    and bloom loaded together.
  - Whole-archive pulls in all of `nix`, and glibc 2.26 lacks `memfd_create`. Only
    `nix::unistd::write` is used, so the vendored `nix` dependency drops default features.
  - Binary grows 4.17 MB stripped, 2.25 MB with Rust LTO (the default). `--gc-sections` saves
    another 63 KB and is left off.

### Step 9: bloom tests

- valkey-bloom's Python suite against a `BUILD_BLOOM=static` server with no `--loadmodule`.
- Tcl test: start with `bf.*` configs in the conf file and a module-written bloom RDB fixture;
  assert the server starts, data loads, and `CONFIG GET bf.*` returns the configured values.
- Tcl test: `MODULE UNLOAD bf` is refused.

## Step 10: default static build

Default `make` produces a server with native JSON and static bloom. Run the full gate once more on
a clean `make distclean && make`, plus a smoke test of the installed binary: `JSON.SET`, `BF.ADD`,
`SAVE`, restart, both keys present, `MODULE LIST` shows `bf` and no JSON module.

## Decisions

| decision | choice |
|---|---|
| `TYPE` on native JSON keys | keep `ReJSON-RL`; changing it breaks clients for a cosmetic gain |
| keyspace event class | module fires `d`, native fires `j`; document that `Kd` subscribers must add `j` |
| `BUILD_BLOOM` default | `static`; `BUILD_BLOOM=no` builds without a Rust toolchain |
| Top-K, Cuckoo, Count-Min Sketch (#4374) | stay in the bloom module and ride the same static link |

## Follow-on, not required for a working build

- **Memory parity.** Current gap is 1.0x to 1.79x the module for anything with a container.
  Inline values in parent vectors, a 16-byte value, and interned member names reach parity
  (16 B per array element, 24 B per member). Inline values invalidate cached `jsonValue *` on
  container growth, so the command layer must address values by `(parent, index)`.
- **Compact encoding.** zuiderkwast asked on #13 about BSON or UBJSON instead of listpack. Listpack
  cannot hold nesting; BSON cannot keep number source text. Proposal: a length-prefixed binary
  format modeled on UBJSON with a number-text type, for small documents, built after inline values
  so the representation is written once.
- **Defrag** for JSON, once the layout settles.
- **`SharedJSON_Get`** re-export for valkey-search, verified against search's actual lookup.

## Out of scope

- valkey-search: an index lifecycle, threading model and cluster coordination, not a data type.
- valkey-ldap: an auth callback.
- A native C bloom type.
