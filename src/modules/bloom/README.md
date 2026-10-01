# valkey-bloom, vendored

[valkey-bloom](https://github.com/valkey-io/valkey-bloom) release `1.0.1`, commit
`43b4e704bc8d6bd5f65529c8bd7e8488f2d54080`. By default it is linked into `valkey-server` as a
static module, so the `BF.*` commands and the `bloomfltr` type exist without `--loadmodule`.

`BUILD_BLOOM` selects the build, and like `BUILD_LUA` it is persisted in `src/.make-settings`:

| `BUILD_BLOOM` | result |
|---|---|
| `static` (default) | staticlib linked into `valkey-server` with `--whole-archive` |
| `module` | `target/release/libvalkey_bloom.so`, for `--loadmodule` |
| `no` | nothing; the only setting that builds without a Rust toolchain |

CMake takes the same values: `-DBUILD_BLOOM=static|module|no`.

Building needs `cargo` and, because `valkey-module`'s build script runs bindgen, libclang. It is
offline: `.cargo/config.toml` points crates.io at `vendor/`, and `cargo` runs with `--frozen`.

## Module name

The module registers as `bf`, which is also the prefix of its configs (`bf.bloom-capacity` and so
on). The server resolves a static module's unload entry point from its registered name, so the
entry points are `ValkeyModule_OnLoad_bf` and `ValkeyModule_OnUnload_bf`, and `MODULE LIST`
shows `bf` with path `bf`. `MODULE UNLOAD bf` is refused because the module exports a data type.

The static module loads inside `moduleLoadFromQueue()` (`src/module.c`), after any
`loadmodule` directives and before the check for unused module configs, so `bf.*` directives in
the config file or on the command line are applied, and before data is loaded. A bloom module
loaded explicitly with `loadmodule` takes precedence over the static one.

## Changes from upstream

- `src/lib.rs`: the two `_bf` entry points appended. The `.so` keeps exporting
  `RedisModule_OnLoad` and `RedisModule_OnUnload`.
- `src/lib.rs`: `initialize()` also declares `HANDLE_REPL_ASYNC_LOAD`,
  `HANDLE_ATOMIC_SLOT_MIGRATION` and `HANDLE_FORKLESS`. Upstream declares only
  `HANDLE_IO_ERRORS`, and the server turns off swapdb async loading, refuses
  `CLUSTER MIGRATESLOTS`, and falls back from forkless to fork saves while any loaded module
  lacks the matching option. Loaded by default, upstream bloom would do that to every server.
  The comment in `initialize()` records why bloom qualifies; this belongs upstream.
- `Cargo.lock`: upstream does not commit one. Generated with `cargo generate-lockfile`.
- `vendor/`: `cargo vendor-filterer --platform='*-unknown-linux-gnu'
  --platform='*-unknown-linux-musl' --platform='*-apple-darwin' --platform='*-unknown-freebsd'
  --keep-dep-kinds=no-dev --exclude-crate-path='*#tests' --exclude-crate-path='*#benches'
  --exclude-crate-path='*#examples' --exclude-crate-path='valkey-module#build' vendor`.
  Crates for other platforms and dev-dependencies are empty stubs.
- `vendor/valkey-module/build.rs`: upstream reuses one `cc::Build` for two archives, so the
  second carries another copy of `redismodule.o`. Under `--whole-archive` that is a duplicate
  `Export_RedisModule_Init`. Each archive now gets its own `cc::Build`.
- `vendor/valkey-module/Cargo.toml`: `nix` with `default-features = false`. The crate only uses
  `nix::unistd::write`, but `--whole-archive` links all of `nix`, and its `memfd_create` wrapper
  needs glibc 2.27. `Cargo.lock` was regenerated offline, which dropped `memoffset` and
  `pin-utils`.
- `.cargo/config.toml`: release LTO with one codegen unit. See the size figures below.

Patched vendored files have their hashes updated in the crate's `.cargo-checksum.json`. Cargo does
not re-fingerprint vendored crates, so run `make clean` after editing anything under `vendor/`.

## Binary size

Stripped `valkey-server`, x86_64 Linux, gcc 7.3, rustc 1.95:

| build | bytes | delta |
|---|---|---|
| `BUILD_BLOOM=no` | 4,006,840 | |
| `static`, no Rust LTO | 8,172,112 | +4,165,272 |
| `static`, Rust LTO (default) | 6,254,832 | +2,247,992 |
| `static`, Rust LTO, `-Wl,--gc-sections` | 6,191,048 | +2,184,208 |

`--gc-sections` would apply to the whole server link for 63 KB, so it is not enabled.

Rust's std is linked statically, but its unwinder adds `libgcc_s.so.1` to the server's dynamic
dependencies on Linux.

## Updating

1. Replace `src/`, `Cargo.toml` and `LICENSE` from the new tag, and reapply the `src/lib.rs` changes.
2. `cargo generate-lockfile`, then rerun the `cargo vendor-filterer` command above.
3. Reapply the two `vendor/valkey-module` patches and their checksums, or drop them if upstream
   fixed them. Regenerate the vendor entries in the top-level `REUSE.toml`.
4. Regenerate `tests/assets/bloom-module.rdb` if the RDB format changed (see
   `tests/unit/type/bloom.tcl` for what it must contain).

## Running the upstream Python suite

The Tcl tests are `tests/unit/type/bloom.tcl`. Upstream's pytest suite runs against the static
server with one change: drop `'loadmodule': os.getenv('MODULE_PATH'),` from the server args in
`tests/valkey_bloom_test_case.py` and `tests/test_bloom_replication.py`. Then, from a checkout of
the same tag:

```
mkdir -p tests/build/binaries/unstable tests/build/valkeytestframework
ln -s /path/to/valkey/src/valkey-server tests/build/binaries/unstable/valkey-server
cp -r /path/to/valkey-test-framework/src/* tests/build/valkeytestframework/
pip install pytest==7.4.3 valkey
cd tests && SERVER_VERSION=unstable python3 -m pytest -v .
```

At 1.0.1, 88 of 90 pass. Both parameterizations of
`test_rdb_restore_non_bloom_compatibility` fail by design: they start a second server without
`loadmodule` and expect bloom to be absent, which a static build cannot provide.
