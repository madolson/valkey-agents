# Rax: the radix tree

`rax` is Valkey's ordered map from arbitrary byte strings to `void *`, used
wherever a hash table cannot serve sorted iteration, prefix seeks or
lexicographic ranges: streams, client tracking, hash field expiry, ACL
selectors, cluster failure reports.

Code: [`src/rax.h`](../src/rax.h), [`src/rax.c`](../src/rax.c).

## Node layout

One variable-length allocation per node: a 4-byte header, the edge bytes,
alignment padding, one slot per edge, then the node's own value if it is a key.
**Keys are never stored anywhere** — a key is the concatenation of the edge bytes
along its path. That is the property most of the tradeoffs below come from.

```c
uint32_t iskey : 1;     /* Node terminates a stored key. */
uint32_t isnull : 1;    /* Key maps to NULL; no value stored. */
uint32_t iscompr : 1;   /* Node is compressed. */
uint32_t allvalues : 1; /* Slots hold values, not child nodes. */
uint32_t size : 28;     /* Child count, or compressed string length. */
```

Two space optimizations do most of the work:

**`iscompr`** collapses a chain of single-child nodes into one inline string, so
a shared prefix costs one node instead of one node per byte.

**`allvalues`** removes the node a childless key would otherwise need. Such a
node holds nothing but a value pointer, and the parent already has a slot for it,
so the value goes in the slot and the node disappears. The bit describes the
whole node, so a walk knows what a slot holds from the parent it already has.

```
branch, 3 children, itself a key:
[hdr][a b c][pad][slot][slot][slot][value]      40 B

compressed, edge "xyz":
[hdr][x y z][pad][slot]                         16 B

allvalues: a, b, c each terminate a key, no child nodes exist:
[hdr][a b c][pad][val][val][val]
```

A node whose edges *mix* terminated keys and continuing paths cannot use
`allvalues` and keeps ordinary childless nodes. Since such a key has no node,
`iter.node` is a stand-in built inside the iterator, and callers overwrite values
through `raxIteratorDataSlot()`.

Nodes have no parent pointers, so anything walking upward — remove, iteration —
carries a `raxStack` of ancestors. Nodes are individually reallocatable `zmalloc`
blocks, which is what lets `activedefrag` relocate them one at a time.

## Measured characteristics

1M keys, jemalloc, x86-64:

| Workload | B/key | nodes/key | insert | find |
| --- | --- | --- | --- | --- |
| random 8B | 36.2 | 1.19 | 299 ns | 126 ns |
| random 16B | 44.2 | 1.18 | 312 ns | 162 ns |
| random 32B | 60.2 | 1.19 | 309 ns | 164 ns |
| stream IDs (16B) | 10.6 | 0.02 | 136 ns | 36 ns |
| 8B counters | 10.0 | 0.00 | 121 ns | 30 ns |

Sequential keys collapse to almost no nodes: the values sit in the slots of a
handful of wide branch nodes. Random keys cost a branch node plus a compressed
node holding the unique suffix.

Allocator work, not the walk, dominates insertion — the walk is about a quarter
of it — because jemalloc almost never grows a node in place, so each
reallocation is a malloc, copy and free.

## Comparison with ART

[unodb](https://github.com/unodb-dev/unodb) implements the Adaptive Radix Tree,
the other mainstream ordered in-memory trie. Measured against it directly, same
key sets in one process, each structure reporting its own memory
(`raxAllocSize()` and `db::get_current_memory_use()`), 8-byte values:

| | rax | unodb |
| --- | --- | --- |
| Node kinds | compressed, branch | Node4 56 B, Node16 160 B, Node48 656 B, Node256 2064 B |
| Path compression | unbounded per node | 7 bytes per node, then another node |
| Value in parent slot | `allvalues`, whole node | `value_bitmask`, per slot |
| Key that prefixes another | allowed | forbidden, needs an encoding layer |

| 1M keys | rax B/key | unodb B/key | rax nod/key | unodb nod/key | rax find | unodb find |
| --- | --- | --- | --- | --- | --- | --- |
| random 8B | **36.2** | 50.0 | 1.19 | 1.09 | 146 ns | **117 ns** |
| 8B counters | **10.0** | 35.1 | 0.00 | 1.00 | 30 ns | **20 ns** |
| stream IDs 16B | **10.6** | 21.0 | 0.02 | 0.01 | 39 ns | **23 ns** |
| random 16B | **44.2** | 118.9 | 1.18 | 2.09 | **178 ns** | 244 ns |
| random 32B | **60.2** | 215.0 | 1.19 | 4.09 | **180 ns** | 288 ns |

Two regimes, and the boundary is key length:

**Short or integral keys** — unodb is 1.2–1.5x faster and rax is 1.4–3.5x
smaller. unodb's speed comes from lazy expansion, which stops the tree as soon as
a key is distinguished, plus a SIMD compare or direct index at each step instead
of a `memchr`. Its extra bytes are the leaf it still allocates per key, holding a
copy of the key.

**Longer variable-length keys** — rax wins on both, and by a lot: 3.6x less
memory and 1.6x faster lookup at 32 bytes. unodb caps a node's prefix at 7 bytes,
so a 32-byte key needs about four 56-byte nodes on its path (4.09 nodes/key),
while a rax compressed node absorbs the whole run in one 16-byte node
(1.19 nodes/key). More nodes on the path is both more bytes and more dependent
loads.

Notably unodb has the same value-in-slot optimization, enabled when the value
fits in 8 bytes and the key is a `key_view`, and both structures then collapse
sequential keys to near-zero nodes per key. They differ in how a slot is
identified: unodb keeps a per-node bitmask, so one node can mix values and
children; rax spends a single header bit on the whole node, which is free but
cannot describe a mixed node.

For Valkey the qualitative rows matter more than the numbers. rax takes arbitrary
byte strings with no encoder, which prefix-ancestor lookup and stream ID ranges
depend on, and its one-allocation-per-node model is what `activedefrag` relocates
one node at a time.

## Related

- [PR #4506](https://github.com/valkey-io/valkey/pull/4506) — proposed RADIX
  type, adds `raxForEachPrefix` and `raxFindLongestPrefix`.
- [Issue #4498](https://github.com/valkey-io/valkey/issues/4498) — discussion.
- [ART paper](https://db.in.tum.de/~leis/papers/ART.pdf) — Leis et al., ICDE 2013.
