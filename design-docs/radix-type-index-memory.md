# RADIX type: index memory and the rax boundary

Notes on the proposed `RADIX` type in
[PR #4506](https://github.com/valkey-io/valkey/pull/4506), design issue
[#4498](https://github.com/valkey-io/valkey/issues/4498). Two things worth
settling: how much the rax index costs relative to what it indexes, and which
of the type's workarounds belong in `rax.c` instead of `t_radix.c`.

Code: [`src/t_radix.c`](https://github.com/yangbodong22011/valkey/blob/feature-valkey-radix-tree/src/t_radix.c),
[`src/rax.c`](https://github.com/yangbodong22011/valkey/blob/feature-valkey-radix-tree/src/rax.c).

## Index memory

Measured on the cumulative-hash workload from the PR description: 2000 roots,
depth 4, fan 4, so 682k paths at 32 bytes of key each.

| | |
| --- | --- |
| Bytes per path | 48.1 |
| Nodes per path | 2.007 |
| Amplification over key bytes | 6.0x |

The index is not the whole cost. `radixCreatePayload()` (`t_radix.c:177`)
allocates a hash `robj` per path unconditionally:

```c
robj *payload = createHashObject();
raxInsert(radix->index, (unsigned char *)pathstr, sdslen(pathstr), payload, NULL);
```

`radixTypeMemUsage()` (`t_radix.c:77`) accounts the two separately, summing
`raxAllocSize()` and a sampled `objectComputeSize()` per path, so the index
share of the type tracks payload size:

| Payload per path | Index share |
| --- | --- |
| one 8-byte field | 54.6% |
| 16 bytes | 50.1% |
| 64 bytes | 33.4% |
| 1 KB | 3.6% |

The small-payload end is where the first target use case sits. A KV cache
placement path holds a handful of worker fields, not a kilobyte, so the index is
roughly half the object rather than a rounding error.

murphyjacob4 reached the same allocation from the other direction in #4498: a
prefix-set workload such as an IP blocklist pays ~50-100MB of `robj` and
listpack for 1M paths against ~15MB for the bare trie, and proposed deferring
the fix to a shared singleton payload or `isnull = 1`.

## The rax boundary

Three places where the type works around rax rather than extending it.

**Descent is implemented twice.** `raxForEachPrefix()` (`rax.c:925`) duplicates
the walk in `raxLowWalk()` (`rax.c:444`). The single-descent design is worth
keeping, at 72 ns for 4 ancestor matches against 312 ns for 4 separate
`raxFind()` calls on the same tree, but folding it into `raxLowWalk()` stops the
two drifting.

**A callback carries one result.** `raxFindLongestPrefix()` (`rax.c:978`) routes
its single answer through `raxRememberLongestPrefix()` (`rax.c:970`). Returning
the matched length directly drops the callback and its context struct. Both
functions should also take `const unsigned char *s` (`rax.h:200-201`).

**The prefix walk discards position.** It returns lengths and values, so
`RAXSET` on an existing path walks the tree twice. Returning the node and slot
would let it update in place.

## `RAXDELPREFIX` latency

`RADIX_DELETE_CHUNK_SIZE 256` (`t_radix.c:39`) collects paths into a fixed array
(`t_radix.c:615`) and deletes them individually, because rax exposes no subtree
operation. The open question is whether that stays in `t_radix.c` or rax grows a
prefix-subtree primitive and the chunking collapses into it. It decides whether
`RAXDELPREFIX` latency is bounded by the size of the subtree being deleted.

## Related

- [PR #4506](https://github.com/valkey-io/valkey/pull/4506) — the proposed type.
- [Issue #4498](https://github.com/valkey-io/valkey/issues/4498) — design discussion.
- [`rax-radix-tree.md`](rax-radix-tree.md) — rax node layout and a comparison
  with ART, on branch `rax-radix-tree-design`.
