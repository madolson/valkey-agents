# valkey-bloom, vendored

[valkey-bloom](https://github.com/valkey-io/valkey-bloom) release `1.0.1`, commit
`43b4e704bc8d6bd5f65529c8bd7e8488f2d54080`: `src/`, `Cargo.toml` and `LICENSE` from that tag.

The build is offline: `.cargo/config.toml` points crates.io at `vendor/`.

- `Cargo.lock`: upstream does not commit one. Generated with `cargo generate-lockfile`.
- `vendor/`: `cargo vendor-filterer --platform='*-unknown-linux-gnu'
  --platform='*-unknown-linux-musl' --platform='*-apple-darwin' --platform='*-unknown-freebsd'
  --keep-dep-kinds=no-dev --exclude-crate-path='*#tests' --exclude-crate-path='*#benches'
  --exclude-crate-path='*#examples' --exclude-crate-path='valkey-module#build' vendor`.
  Crates for other platforms and dev-dependencies are empty stubs.

The vendor entries in the top-level `REUSE.toml` are grouped by each crate's `license` field.
