# ftviz design notes

This document records how ftviz is built and why. It covers the threading
model, the memory layout, the hashing and collision analysis, the force
model and its constants, the renderer, and the measured performance. It
states plainly which targets were measured and which could not be.

Where the specification left a choice open, the highest-performance
deterministic option was taken. Those decisions are collected in
[Decisions on open points](#decisions-on-open-points).

## 1. Pipeline overview

```
 input file ──► ingest ──► model ──► layout ──► render
 (CSV/.ftxb)    src/ingest  src/model  src/layout  src/render + src/app
```

1. **Ingest.** The file is memory-mapped and split into ~16 MB chunks.
   Chunk descriptors go into a bounded MPMC queue. Parser workers read rows
   with zero allocations, normalise and hash the keys, and deduplicate
   entities and partially aggregate edges in thread-local tables. The tables
   are then merged into a 64-shard registry, and every entity gets a
   deterministic dense index.
2. **Model.** Per-worker edge partials are merged in parallel into a sorted
   SoA `EdgeList`. Nodes are assigned to hubs by majority vote, with BRIDGE
   detection. Edges are classified as intra-hub or inter-hub, a CSR
   adjacency is built, and inter-hub edges are collapsed into corridor
   bundles keyed by (hub_a, hub_b, currency).
3. **Layout.** A deterministic initial placement is followed by iterations
   of a parallel Barnes-Hut force solver. The solver rebuilds a Morton-sorted
   octree every iteration and evaluates repulsion with SIMD kernels per
   interaction group. A dedicated coordinator thread runs it and publishes
   position snapshots through a lock-free triple buffer.
4. **Render.** The main thread owns the GL context and draws everything with
   six or seven instanced or indirect draws per frame. Data comes from
   immutable buffers plus persistently mapped rings guarded by fences.

## 2. Threading

### Diagram

```
                      startup (ingest + model)                     runtime (interactive)
 ┌───────────────┐   ┌──────────────────────────────────┐   ┌──────────────────────────────────────┐
 │ main thread   │──►│ mmap, chunk, push descriptors    │   │ main = render thread (owns GL)        │
 │ (owns GL)     │   │        │                         │   │  poll input ─► pick readback ─► HUD   │
 └───────────────┘   │        ▼                         │   │  begin_frame: fence wait (slot f-3)   │
                     │  bounded MPMC queue (Vyukov)     │   │    acquire_latest() ◄───────────┐     │
                     │   │      │      │       │        │   │  draw_frame: 6-7 draws, fence   │     │
                     │   ▼      ▼      ▼       ▼        │   └─────────────────────────────────┼─────┘
                     │ T_ingest[0..N-1], N=max(1,hw-2)  │                                     │
                     │ thread-local dedup + partials    │      TripleBuffer<PositionSnapshot> │
                     │        │ jthread join            │      (one atomic byte, acq_rel)     │
                     │        ▼                         │                                     │
                     │ registry merge, dense index,     │   ┌─────────────────────────────────┴─────┐
                     │ votes, edge merge, graph build   │   │ T_layout (coordinator)                │
                     │   └─ ThreadPool::parallel_for ───┼──►│  ForceSolver::step() ─► publish()     │
                     └──────────────────────────────────┘   │   └─ ThreadPool::parallel_for (workers)│
                                                            └───────────────────────────────────────┘
```

| Thread | Count | Role |
|---|---|---|
| main | 1 | Owns the GL context: window, input, picking, HUD and all GL calls. Also produces chunk descriptors during ingest. |
| `T_ingest[i]` | N = max(1, hw − 2), override with `--parser-threads` | Pop chunks, parse rows, deduplicate keys and partially aggregate edges into worker-private tables. |
| ThreadPool workers | hw − 1 headless; hw − 2 interactive, so the render thread keeps a core; `--threads N` gives N − 1 | Shared data-parallel workers for the merge, model and layout passes. The thread that calls `parallel_for` takes part too. |
| `T_layout` | 1 | Runs `ForceSolver::step()` in a loop, publishes snapshots, and handles pause, reheat and stop. |

The render thread never touches the ThreadPool, a mutex or a condition
variable. Its only cross-thread operations are the triple-buffer exchange
and relaxed reads of the layout status counters for the HUD.

### Synchronisation points

Every non-`seq_cst` order is also justified next to the code.

| Where | Primitive | Ordering and why |
|---|---|---|
| Chunk hand-off (`core/mpmc_queue.hpp`) | Per-cell sequence numbers | The `seq` load is acquire and the `seq` store is release, so the payload is published before it can be consumed. The position CAS is relaxed because it only arbitrates cell ownership. |
| Queue close | `closed_` flag | Release on close, acquire in `pop()`, so items pushed before the close are visible to the final drain. |
| End of parsing (`ingest/ingest.cpp`) | `std::jthread` join | Join is a happens-before edge: every worker table is complete before the merge reads it. |
| Registry shards (`core/spinlock.hpp`) | TTAS spinlock, one per shard | Acquire on lock and release on unlock, which is the critical-section contract. Workers take each shard lock once and merge a whole batch under it. |
| Pool job publish (`core/thread_pool.cpp`) | `generation_` counter | The submitter writes the job fields, then does a release `fetch_add`. Workers acquire-load the counter, which makes the job visible. |
| Pool chunk claim | `next_chunk_` | Relaxed `fetch_add`: it only hands out indices; data visibility comes from the generation acquire. |
| Pool completion | `pending_workers_` | Workers do an acq_rel decrement; the submitter acquire-loads or waits on it. Every write made inside the job happens-before `parallel_for` returns. |
| Layout to render (`core/triple_buffer.hpp`) | One atomic byte: slot index plus FRESH bit | Writer and reader swap with acq_rel exchanges. Release publishes the snapshot contents; acquire guarantees a returned slot is no longer being read. A relaxed pre-check skips the RMW when nothing is new. |
| Layout control (`layout/layout_engine.cpp`) | `control_` counter with `atomic::wait`/`notify` | Release on `wake()` and acquire in the layout thread, so `paused_` and `reheat_requested_` are seen together with the wake-up. |
| HUD status counters | Independent relaxed atomics | Display only. Values may come from adjacent iterations, and nothing depends on them. |
| CPU to GPU frame slots (`render/renderer.cpp`) | `glFenceSync` per slot, `glClientWaitSync` in `begin_frame` | Frame f waits for frame f − 3. After that, slot f mod 3's uniform, highlight and HUD regions, its timer queries and its readback copy are free. |
| Pick readback (`render/picking.cpp`) | Fence after `glReadPixels` into a persistent PBO | Polled with a zero timeout, so the CPU never waits on the GPU. |

### False sharing

- Each hot atomic is `alignas(64)`: the pool counters, the queue positions
  and the triple-buffer index bytes.
- Per-worker objects are `alignas(64)`: `ParseWorker` and the octree
  `Scratch`.
- Every parallel pass writes only its own contiguous output range.
- Reductions go into one slot per fixed chunk, not per thread, and are
  summed in chunk order afterwards.
- The octree's inverse-permutation pass is partitioned by output range, so
  no two threads write the same cache line.

### Determinism

`parallel_for` splits `[0, count)` into fixed chunks of `grain` items, so
the partition depends only on `(count, grain)` and never on the number of
threads. Every reduction is combined per chunk in chunk order.

Together with the deterministic dense indices and the fixed list order in
the SIMD kernels (§5.3), the layout is bit-identical for any thread count.
The `layout: identical checksum` test checks this with 1, 4 and 8
threads, plus a repeated run. In this environment GCC 13 and Clang 18, in both Release and Debug,
produced the same checksum (`5d113fc318eaa294`) on baseline x86-64.
`FTVIZ_NATIVE` can enable FMA contraction, which changes the low bits but
not the determinism of a given binary.

## 3. Memory layout

Hot data is Structure-of-Arrays. Every hot array is an `AlignedVector<T>`,
whose allocator returns 64-byte-aligned storage. SIMD loads are therefore
aligned, and no two arrays share their first cache line.

### Ingest

| Structure | Layout |
|---|---|
| Input | Read-only `mmap` (RAII `MappedFile`). Rows are `string_view`s into the mapping, so nothing is copied. |
| Chunk | `{offset, size, index}`, 24 bytes, trivially copyable through the MPMC queue. |
| Worker tables | `FlatHashMap`: open addressing with linear probing. Control bytes, keys and values live in three separate aligned arrays. Capacity is reserved up front from size estimates, so the row loop normally never grows a table. |
| `EdgeKey` | `{src64, dst64, currency u32, channel u32}`, 24 bytes. The hash only places the tuple; equality compares the full tuple. |
| `EdgeVal` | Exact 128-bit amount sum (`Int128Acc`), count, and first and last timestamps. |
| Registry | 64 shards, each a `FlatHashMap<key64, …>` behind its own spinlock. |

### Model

- **`EdgeList`** (SoA, sorted by tuple) has `src u32`, `dst u32`,
  `weight i64` (the exact sum clamped to int64), `count u32`,
  `currency u32`, `channel u8`, `flags u8`, `first_ts i64` and
  `last_ts i64`. That is 42 bytes per edge, or ~326 MB for 7.76M edges.
- **`NodeArrays`** (SoA) has `key u64`, `hub u16`, `flags u16`,
  `size f32`, `weighted_degree f32` and `degree u32`, plus CSR adjacency
  (`offset`, `neighbor`, `edge`).

### Layout

The solver keeps its own internal node order: hubs in sequence, and
breadth-first over intra-hub edges inside each hub (a Cuthill-McKee style
bandwidth reduction). Neighbours therefore get nearby indices, and the
spring pass gathers from a cache-resident window.

`perm_` maps internal to graph order and `rank_` maps graph to internal.
Every output is gathered back to graph order.

| Array set | Bytes per node |
|---|---|
| Position, next position, velocity, previous acceleration (12 × f32) | 48 |
| Charge, hub, key, perm, rank | 22 |
| Repulsion output (3 × f32) and inverse permutation | 16 |
| Springs: CSR incidence plus `rest` and `k` per incidence | 24 per edge |
| Octree bodies: Morton codes ×2, order ×2, sorted x/y/z/q, hub | 34 |
| Octree nodes: `com` float4, `meta`, `skip`, `first`, `count` | 32 per tree node |

At 1M nodes and 7.76M edges this comes to roughly 0.3 GB.

The octree's traversal-hot node fields are packed so one visit touches as
few cache lines as possible:

- centre of charge and total charge form one 16-byte `CellCom`;
- hub id, depth and the leaf flag share one 32-bit `meta` word;
- a cell's squared size comes from a per-depth table, not a per-node array.

Nodes are stored in pre-order with skip pointers, so traversal is stackless
and walks forward through memory.

### GPU

The figures below assume 1M nodes and 7.76M edges.

| Buffer | Binding | Contents | Size |
|---|---|---|---|
| Node static VBO | attribute stream, divisor 1 | `{hub u16, flags u16, size f32, color_index u32}`, packed to 12 bytes | 12 MB |
| Positions | SSBO 0 | Persistent ring, 3 regions of N × 12 bytes | 36 MB |
| Edges | SSBO 1 | Immutable `{src, dst, attr, weight}`, 16 bytes, intra first then inter, each sorted by priority | 124 MB |
| Hubs | SSBO 2 | Anchor, radius, colour and params (48 bytes per hub) | < 1 KB |
| Corridor bundles | SSBO 3 | `{hub_a, hub_b, palette, thickness, brightness}` (32 bytes) | < 8 KB |
| Highlight bits | SSBO 4 | Persistent ring, 3 regions of ⌈N/32⌉ words | 375 KB |
| Visible lists | SSBO 5 and 6 | Compacted edge indices, capped at 2M in total | 8 MB |
| Indirect commands | SSBO 7 and `GL_DRAW_INDIRECT_BUFFER` | Two `DrawArraysIndirectCommand` | 32 B |
| HUD glyphs | SSBO 8 | Persistent ring, one region per frame slot | small |
| Frame uniforms | UBO 0 | Persistent ring, 3 regions | 3 × 256 B |
| Palette | texture unit 0 | 256 × 1 RGBA8 `sampler1D` (currencies, then 16 channel colours at 128+) | 1 KB |
| Font | texture unit 1 | R8 atlas | small |
| Main target | none | RGBA8 + `DEPTH_COMPONENT32F`, MSAA 4× | 66 MB at 1080p |
| Pick target | none | R32UI + depth, plus a persistent PBO | ~16 MB at 1080p |

Edge attributes are packed into 32 bits:

| Bits | Field |
|---|---|
| 0–7 | currency palette slot |
| 8–11 | channel |
| 12–15 | flags |
| 16–23 | source hub |
| 24–31 | destination hub |

## 4. Keys, hashing and collision analysis

```
key64 = XXH3_64bits_withSeed(normalized, len, SEED ^ namespace_tag)
SEED  = 0x6A09E667F3BCC909           (compile-time constant)
fiat namespace_tag  = 0
chain namespace_tag = mix64(packed_chain_id | 1<<63)   (never 0; distinct per chain)
```

The normaliser classifies each identifier and transforms it as follows:

| Identifier | Normalisation |
|---|---|
| Fiat account (any rail) | trimmed, ASCII upper case |
| EVM `0x` + 40 hex | lower case |
| Bech32 (`bc1…`, `tb1…`) | lower case (bech32 is case-insensitive) |
| Base58 (BTC legacy, SOL, TRON) | kept as is |
| Custodial id on an `ONCHAIN` row (not an address) | fiat rules, in the fiat namespace |

The last row is deliberate. An exchange's ledger id that appears both on
fiat rails and in on-chain rows hashes to the same entity, and that is how
BRIDGE entities are detected. The same address string on two chains gives
two different entities.

**Collision probability.** With n distinct normalised keys and a 64-bit
hash that behaves randomly (XXH3 passes SMHasher), the birthday bound gives
P(any collision) ≈ n² / 2⁶⁵.

| Distinct keys n | P(≥ 1 collision) |
|---|---|
| 1 M (target scale) | 2.7 × 10⁻⁸ |
| 10 M | 2.7 × 10⁻⁶ |
| 100 M | 2.7 × 10⁻⁴ |
| 6.1 × 10⁹ | ~0.5 |

These numbers apply within a namespace. Across namespaces the different
seeds make collisions independent events with the same bound on the total.

A collision would silently merge two entities. At the target scale that is
negligible, so Release builds do not pay for a check.

Debug builds run a `CollisionVerifier`, a map from key64 to the first
namespaced string. It reports any key64 that is seen with two different
strings. The `hash: collision verifier accepts repeats` test covers it.

Two further properties limit the effect of a collision:

- **Edge tuples cannot merge.** The aggregation tables hash the 24-byte
  tuple only for placement, and equality compares the full tuple.
- **Shards stay balanced.** The registry shard is chosen by `key64 >> 58`.
  XXH3's top bits are uniform, so each shard holds n/64 ± O(√(n/64)) keys.

## 5. Layout

### 5.1 Initial placement

Each node starts at `anchor + 0.8·R·f(key64)`, where `f` maps the hash into
the unit ball. The low 20 bits pick a Fibonacci-sphere direction, and the
next 24 bits pick a cube-root radius, so points are uniform in volume. The
placement involves no randomness and is identical for any thread count.

### 5.2 Octree build (every iteration)

1. Parallel bounding-box reduction, one slot per fixed chunk.
2. Parallel 30-bit Morton codes, 10 bits per axis.
3. Parallel stable LSD radix sort: three passes of 10 bits with per-chunk
   histograms and prefix offsets.
4. Parallel gather of position, charge and hub into sorted order.
5. Parallel subtree construction. Subtrees are counted per depth-3 cell
   (512 cells), the top levels are built serially, and the cells are built
   in parallel in pre-order at prefix offsets, with bottom-up aggregation of
   centre of charge and hub.

    Each cell carries a hub id when all its bodies share one hub, and
    `kMixedHub` otherwise. The top-level internal nodes are recorded in
    post-order and aggregated by a forward walk. A reverse walk was an
    earlier bug: it aggregated parents before their children, and the θ = 0
    exactness test caught it.
6. Interaction groups are formed. A group is a maximal single-hub subtree
   with at most 64 bodies (`kGroupSize`).

### 5.3 Barnes-Hut repulsion

- **One traversal per group.** The opening test uses the group's tight
  bounding box: a cell is accepted when `size² < θ² · d²`, where d is the
  distance from the group's box to the cell's centre of charge (0 if the
  centre lies inside the box). This is conservative, so every body in the
  group satisfies θ = 0.8. A cell that contains the group is always
  opened.
- **Hub-aware scaling.** Mixed-hub cells are always opened. Every accepted
  cell therefore has a single hub, and the same-hub (×1) or cross-hub
  (×0.05) factor applies exactly. The rare mixed-hub leaves are evaluated
  per body.
- **Shared list.** The traversal emits one interaction list (cells plus
  individual bodies) shared by all bodies of the group.
- **SIMD kernel.** The list is evaluated 8 bodies at a time, vectorised
  across bodies with the list entry broadcast. Variants:

  | Kernel | When it is used |
  |---|---|
  | AVX (8 lanes) | Detected at run time on x86-64 (`__builtin_cpu_supports` or `cpuid`/`xgetbv`) |
  | SSE2 (2 × 4 lanes) | x86-64 baseline |
  | Scalar | Everywhere else |

  The kernels use exact `sqrt` and division and no FMA. Each body
  accumulates its list strictly in list order, so all three kernels give
  bit-identical results on any vendor's CPU. `rsqrt` approximations differ
  between Intel and AMD and would break that, so they are not used.
- **Accuracy.** The relative force error against O(N²) brute force is
  0.70% at leaf 16 and 0.84% at leaf 4. The spec allows under 2%. With
  θ = 0, the result matches brute force exactly.

### 5.4 Forces and integration

For node i in hub h, with anchor a_h and radius R_h:

```
F_rep    = Σ q_i q_j (p_i − p_j) / (|p_i − p_j|² + ε²)^{3/2} · s_ij     s_ij = 1 same hub, 0.05 cross hub
F_anchor = −k_anchor (p − a) · smoothstep(0.6 R, 1.2 R, |p − a|)
F_core   = −k_core (p − a)                                            k_core = 0.25 · k_anchor
F_spring = Σ k_e (d − L0_e) · (p_j − p_i)/d
     intra-hub: L0 = clamp(10 / ln(1 + tx_count), 1.5, 14)
                k_e = 0.004 · log10(1 + |amount|) / max_e log10(1 + |amount|)
     inter-hub: L0 = |a_ha − a_hb|, k_e × 0.1
```

Velocity Verlet with damping:

```
v ← (v + ½(a_prev + a)·dt) · 0.85
x ← x + v·dt + ½·a·dt²
```

The displacement is clamped to the temperature T, and T ← max(0.02,
0.995·T). The layout counts as converged once the mean displacement stays
below ε = 0.03 for 30 consecutive iterations.

**Population-normalised charge.** A uniform ball of total charge Q and
radius r has the interior field Q·s/r³. That exactly cancels the harmonic
core pull k_core·s when Q = k_core·r³.

Each hub's charge is set to `k_core · (0.8·R)³ / population`. Every hub
then fills 80% of its configured radius, whatever its node count: New York
with 172k nodes and Offshore with 30k look equally well-formed. The shape
comes from repulsion balancing core gravity, and the smoothstep anchor only
catches outliers.

### 5.5 Force constants and rationale

All constants live in `config/hubs.json` under `"layout"`. The defaults are
in `src/layout/layout_params.hpp`.

| Constant | Value | Rationale |
|---|---|---|
| `theta` | 0.8 | Spec. The group bounding-box test is stricter than per-body θ, giving 0.70% measured error. |
| `leaf_size` | 16 | Bodies per leaf. Larger leaves make longer direct lists; smaller ones make deeper trees. 16 was fastest at 1M. |
| `kGroupSize` | 64 | One traversal per ≤ 64 bodies is 8 SIMD batches. Larger groups lengthen the shared list more than they save. |
| `softening` | 1.0 | Plummer ε in world units, about 0.2 × the mean intra-hub spacing (~5 units at 172k nodes in a 184-unit ball). It removes the singularity without blurring structure. |
| `cross_hub_repulsion` | 0.05 | Spec. Hubs should not push each other apart; the anchors place them. |
| `fill_fraction` | 0.8 | Equilibrium radius is 0.8 R. That is inside the anchor window (0.6 R–1.2 R), so the anchor is nearly inactive at rest and the configured radius sets the visual size directly. |
| `core_gravity_ratio` | 0.25 | k_core = 0.005 is weaker than the anchor, so the anchor dominates beyond 0.6 R while the core sets the equilibrium. |
| `k_anchor` (per hub) | 0.02 | At 1.2 R (~280 units) the pull is ~5.5 units per iteration², which returns escaped nodes within a few iterations at T = 30. Below 0.6 R it is zero. |
| `anchor_inner` / `anchor_outer` | 0.6 / 1.2 | Spec smoothstep window. |
| `spring_k` | 0.004 | Maximum stiffness, kept below k_core so springs shape communities inside a hub without collapsing it. |
| `spring_rest_base` / `min` / `max` | 10 / 1.5 / 14 | L0 ∝ 1/ln(1 + count). One transaction gives 14 (clamped), 10 give 4.2, and 1,000 or more give 1.5. Frequent counterparties sit closer than the mean spacing. |
| `inter_hub_spring_scale` | 0.1 | Spec. Inter-hub springs rest at the hub separation, so they turn bridge nodes toward the partner hub without pulling clusters together. |
| `damping` | 0.85 | Spec. |
| `dt` | 1.0 | Time is in iteration units. Forces are scaled to match. |
| `initial_temperature` | 30 | At most 30 units per step, 13–20% of a hub radius, so nodes can cross their hub in a few iterations at the start. |
| `cooling` | 0.995 | Spec. T halves every 138 iterations. |
| `min_temperature` | 0.02 | Floor that keeps late refinement possible after a reheat. |
| `convergence_epsilon` / `window` | 0.03 / 30 | Spec window. 0.03 units is about 0.5% of the mean spacing. |

Hub geometry from the spec:

- **Fiat ring**: R = 1000 at y = 0.
- **Chain ring**: R = 700 at y = +600, phase-offset by half a fiat step.
- **Fallback**: y = −600.

The `model: default hub config layout matches the spec` test checks all
three.

### 5.6 Per-iteration passes

1. Octree build (§5.2).
2. Repulsion per group, written to sorted-slot arrays. Each group writes
   its own contiguous slot range.
3. Inverse permutation. Each thread owns a range of bodies and scans the
   cache-resident order array.
4. Integration per body range. This pass computes the anchor, core and
   spring forces, runs Verlet with the temperature clamp, and writes the
   next-position buffer. Positions are double-buffered, so spring reads
   never race with writes.
5. Displacement reduction, one slot per fixed chunk, summed in order.

## 6. Rendering

### 6.1 Frame structure and draw budget

| Order | Pass | Call |
|---|---|---|
| compute | Edge cull and compaction, then clamp | `glDispatchCompute` ×2 (not draws) |
| 1st | Pass 4: nodes (opaque, so the translucent passes depth-test against them) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)` |
| 2nd | Pass 1: hub clouds | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, hubs × P)` |
| 3rd | Pass 2: corridors (cubic Bézier ribbons, 16 segments) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 64, bundles)` |
| 4th | Pass 3: edges, intra (`LINES`) and inter (8-vertex `LINE_STRIP`) | `glDrawArraysIndirect` ×2 |
| on click | Pass 5: pick (R32UI) | `glDrawArraysInstanced` |
| last | Pass 6: HUD | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, glyphs)` |

A frame makes 6 draws, or 7 when a pick is pending, against a budget of 8.
There is no `glDrawElements*`, no glBegin, no display list and no
compatibility profile.

The two edge draws are `glDrawArraysIndirect`. That is the indirect form of
`glDrawArraysInstanced`, and the compute culling path the spec allows feeds
it. Without compute (the GL 4.3 fallback), the same passes are issued as
`glDrawArraysInstanced`, and the vertex shader culls by emitting degenerate
primitives.

### 6.2 Per-frame CPU work

The render thread does the following each frame:

- One fence wait.
- A copy of at most 4 MiB of a new layout snapshot into a free region of
  the position ring. A 1M-node snapshot is spread over 3 frames, see §6.5.
- A ~400-byte uniform write.
- HUD glyph writes into the mapped ring.
- When the selection changes, the highlight bits are copied into the slot's
  region.
- About 40 GL calls.

There is no `glBufferData` or `glBufferSubData` after initialisation, and
no buffer re-specification. Debug builds assert zero heap allocations per
frame (§7).

### 6.3 Edges: LOD, budget and corridors

**Priority.** Each edge's priority is its |amount| divided by the largest
edge of the same currency. There is no FX table offline, so amounts in
different currencies are not compared. The edge buffer stores intra-hub
edges first and then inter-hub edges, each in descending priority, so
every LOD is a prefix.

**Budget.** At most 2,000,000 edges are drawn per frame, and
`render.json:max_visible_edges` is clamped to that. The budget is split as
follows:

| Share | Size | Notes |
|---|---|---|
| Inter-hub | min(count, `max_inter_hub_edges`, (budget − reserve)/4) | Shown individually only with E on and within `inter_hub_detail_distance` of the camera. |
| Intra-hub | the rest, further limited by `intra_edge_lod_percentile` | A prefix of the sorted intra range. |
| Reserve | 64k slots | Keeps the selected node's edges visible even when they rank below the LOD prefix. |

The compute cull writes survivors to the compact lists and bumps the
indirect instance counts with atomics. The clamp pass then caps the counts
at the list capacities, so a frame never draws more than the budget. At
1M nodes the run here drew 1,684,464 intra-hub edges against a capacity of
2,000,000.

**Colour and alpha.** Colour comes from a 1D palette, by currency or by
channel (C key). Alpha is `base · (0.25 + 0.75·w)`, where
w = log1p(|amount|) / log1p(max of that currency).

**Corridors.** Inter-hub flows are bundled per (source hub, target hub,
currency) into cubic Bézier ribbons between the source hub's exit port and
the target's entry port. Each currency gets its own lane. The lateral
offset uses cross(direction, up), which flips with direction, so A→B and
B→A never overlap.

- **Thickness** grows with √(volume relative to the largest bundle of the
  same currency).
- **Brightness** follows log(tx count).
- **Blending** is premultiplied "over", so dense bundles do not saturate to
  white.

**Individual inter-hub edges.** These follow a 3-segment route:
source → exit port → the corridor's Bézier samples → entry port → target.
They therefore sit inside their bundle instead of forming a hairball.

### 6.4 Nodes, depth and picking

- **Nodes.** Each node is a camera-facing quad with a ray-sphere
  intersection in the fragment shader. The shader writes `gl_FragDepth`
  with `layout(depth_less)`, and the quad sits on the sphere's front plane,
  so early-Z stays valid (conservative depth).
- **Reverse-Z.** The projection is an infinite reverse-Z perspective, with
  `glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`, a 32F depth buffer
  cleared to 0 and `GL_GREATER`. Precision is spread evenly in log space
  over the scene's ~10⁴ depth range.
- **Picking.** On a click only, the node pass is re-rendered into an R32UI
  target that stores the instance id + 1. One pixel is read with
  `glReadPixels` into a persistently mapped PBO behind a fence and consumed
  a frame or two later. The result is range-checked before use.
- **Highlight.** The selection's 1-hop neighbourhood is stored as bits in
  SSBO 4. The CPU keeps a master copy and copies it into a slot's region
  only when the selection generation changed.

### 6.5 Layout snapshot upload

The layout thread publishes into a CPU-side triple buffer. The render
thread copies the newest snapshot into a free region of the 3-region
persistent ring.

A region is free when the two frames that may still be in flight do not
reference it. With 3 regions and at most 2 busy, one is always free.

A 1M-node snapshot is 12 MB. Copying it in one frame measured 2.0 ms on
the test VM (mean of 19 uploads, max 2.4 ms), which is the entire 2 ms CPU
budget. The copy is therefore amortised: each frame copies at most
`kUploadBytesPerFrame` = 4 MiB into the target region.

Frames keep drawing the previous region until the fill completes. No new
snapshot is acquired during a fill, so the source, the reader-owned front
slot, stays valid. The extra latency is three frames, about 50 ms at
60 FPS, which is the layout period anyway.

Measured at 1M nodes on the test VM, the copy cost per frame fell from a
mean of 2.04 ms (max 2.39 ms) to a mean of 0.70 ms (max 0.91 ms).

The alternative was to let `T_layout` write straight into GPU-visible
memory. That was rejected for now: it needs a fenced N-region protocol
between threads and a stop handshake on context loss.

### 6.6 Robustness

- **Debug context.** A GL debug context is requested; `--no-gl-debug`
  disables it. HIGH-severity messages print and abort. LOW and MEDIUM
  messages are rate-limited to 32 through state passed as the callback's
  `userParam`, so there is no global.
- **Shaders.** Shader and program logs are printed in full on failure.
- **Resize and minimise.** Render targets are recreated on resize. A
  minimised window does no GL work and sleeps in `glfwWaitEventsTimeout`.
- **Context loss.** With `KHR_robustness`, `glGetGraphicsResetStatus` is
  checked every frame. On a reset the window and every GL object are
  destroyed and recreated, and the newest snapshot is re-primed.
- **Index validation.** Edges with out-of-range endpoints are dropped
  before upload and counted. Pick results and `--select` are range-checked.
  The graph builder validates indices and counts dropped edges.
- **RAII.** Every GL object is a move-only RAII wrapper created with DSA
  `glCreate*` calls (`render/gl_raii.hpp`).

### 6.7 GL 4.3 fallback

The program requires GL 4.3 plus `GL_ARB_direct_state_access`,
`GL_ARB_buffer_storage` and `GL_ARB_clip_control`. `GL_KHR_robustness` and
`GL_KHR_debug` are optional. glad is generated for core 4.5 plus exactly
these extensions, so the DSA, storage and clip-control entry points load
from either source.

On a 4.3 or 4.4 context, the shader loader rewrites `#version 450 core` to
`#version 430 core`. The shaders use no 4.5-only GLSL features.

Compute culling needs only 4.3. Setting `gpu_culling: false` in
`render.json` selects the direct-draw path with vertex-shader culling.
Context creation tries 4.6, 4.5 and 4.3 in turn, and GLFW error output is
muted while probing.

## 7. Zero allocations per frame (Debug assertion)

Debug builds of `ftviz` replace the global `operator new` family
(`core/alloc_counter.cpp`) with a counting version backed by a
`thread_local` counter. The frame loop takes a snapshot at the top of each
iteration and compares it after the last draw. Any allocation by the render
thread in between aborts, or warns once with `--no-alloc-assert`.

The counted region covers event polling (our GLFW callbacks), input, pick
readback, selection, camera, HUD text, the snapshot upload and every GL
call. Only these paths are excluded:

- the minimised path;
- context re-creation;
- the on-demand screenshot export.

**Driver allocations.** On the first run the check fired on frame 0 with
27,694 allocations, none of them ours. The cause:

1. Mesa's llvmpipe compiles shaders lazily with LLVM, which is C++.
2. On ELF platforms the driver's `operator new` calls resolve to the
   executable's replacement.
3. The driver's allocations were therefore charged to our thread.

Real drivers can do the same, for example radeonsi with LLVM or ACO.

The fix: Debug builds link a second glad library generated with glad's
`DEBUG` option, whose pre- and post-call hooks wrap every GL call. The
application installs hooks that mark the call as foreign code. Allocations
inside a GL call are counted separately and reported, but they do not
trigger the assertion. Our own allocations still do. Release builds use
the plain loader and pay nothing.

**Verified result.** The Debug build ran 300 frames on llvmpipe with
simulated clicks, picks, framing and HUD toggles:

- 0 allocations in our frame code;
- 58,310 driver allocations inside GL calls, reported and not asserted.

## 8. Ingest details

- **Chunking.** CSV chunks are ~16 MB, with each boundary snapped forward
  to the next `\n`. `.ftxb` chunks are whole records. A quoted field that
  contains a raw newline is not supported, because chunks are split on
  `\n`. Such a row is reported as malformed instead of corrupting its
  neighbours.
- **Amounts.** Parsed as fixed-point decimals into int64 minor units with
  the currency's scale:

  | Scale | Currencies |
  |---|---|
  | 0 | JPY, KRW and others |
  | 2 | default |
  | 3 | BHD, KWD and others |
  | 6 | USDT, USDC, TRX |
  | 8 | BTC |
  | 9 | ETH (gwei, not wei, so realistic values fit int64), SOL |

  Extra fractional digits are truncated and counted. Overflow makes the row
  malformed. No floating point is involved.
- **Timestamps.** ISO-8601 UTC, with optional fraction and offset, or epoch
  milliseconds. Days are computed with Hinnant's civil-date algorithm.
- **Currency.** Packed ASCII into a `uint32`. Channels are matched
  case-insensitively against the eight names; anything else is malformed.
- **Malformed rows.** Counted per worker. Each worker records its first K
  offsets (K ≤ 16, default 8). Workers take chunks in file order, so after
  merging, the report prints exactly the K smallest offsets in the file.
  A malformed row never aborts the run.
- **Location votes.** Every endpoint records a vote for (entity,
  jurisdiction or chain). These votes drive hub assignment.
- **Exact sums.** Amount sums are exact 128-bit (`Int128Acc`), so the
  result does not depend on aggregation order, and they are clamped to
  int64 only in the final `EdgeList`.
- **Memory footprint.** The row loop allocates nothing; the tables are
  reserved up front. The parse functions are `noexcept`, and growth in the
  cold path is the only allocation.

## 9. Decisions on open points

| Topic | Decision | Why |
|---|---|---|
| "Majority hub" for an entity | Majority by endpoint transaction count. Ties go to the lower hub id. | There is no FX table offline, so amounts in different currencies are not comparable. Counts are, and they are deterministic. |
| BRIDGE | An entity with both fiat-domain and chain-domain activity. It keeps its majority hub and gets `FLAG_BRIDGE`. | Matches the spec. Custodial ids share the fiat namespace, which makes this detectable. |
| Unknown jurisdictions | Fall back to `OFFSHORE_UNRESOLVED` and get `kNodeUnresolved`. | Spec fallback hub. |
| Spring strength over mixed currencies | log10(1 + \|amount\|) in raw minor units, normalised by the global maximum. | The log compresses the scale differences. There is no FX table. |
| Edge priority for the LOD and the 2M budget | \|amount\| relative to the largest edge of the same currency. | The same reason, and it keeps every currency represented. |
| Intra-hub LOD | A prefix of the priority-sorted intra range. `intra_edge_lod_percentile` (default 0) drops the lowest percentiles. The budget clamps the rest. | One contiguous range, so GPU culling needs no per-edge flag. |
| "Only glDrawArraysInstanced" vs "compute culling + indirect" | Every draw is `glDrawArraysInstanced` or its indirect form `glDrawArraysIndirect`. | The spec explicitly allows compute culling with indirect draws. |
| Inter-hub detail distance | 900 world units from the camera to the nearer endpoint, plus all edges of the selected node. | About one hub diameter. Close enough that the edges stay readable. |
| CSV columns | Matched by header name, any order, extra columns ignored. | Robust to exports from different systems. |
| `.ftxb` layout | 64-byte header, 64-byte records with fixed field offsets, string pool of (offset, length) references. Little-endian. | Fixed-size records chunk without scanning. Documented in `ingest/ftxb_format.hpp`. |
| Self-loops | Aggregated, but not drawn and not used as springs. | They have no geometric meaning. |
| Pool size while rendering | hw − 2 workers plus `T_layout`, leaving one core for the render thread and the driver. | Rendering at 60 FPS must not compete with the layout. |
| Debug allocation check and drivers | Allocations inside GL calls are attributed to the driver (§7). | The check is about our code, and a driver JIT is outside our control. |
| Snapshot upload | Amortised at 4 MiB per frame (§6.5). | Bounds the per-frame CPU cost. |

## 10. Performance

### 10.1 What was measured, and where

All numbers in this section come from this environment:

- 4 vCPUs (Intel Xeon @ 2.10 GHz, AVX2 and AVX-512 capable) in a shared
  cloud VM.
- Mesa llvmpipe (software GL 4.5) under Xvfb.
- GCC 13.3, Release with LTO.

There was no GPU, and there was no 8-core machine. Read the tables with
that in mind.

**Ingest of 10M rows** (synthetic CSV, 1,121 MB, 94 deliberately malformed
rows):

| Metric | Value |
|---|---|
| Parser threads | 2 (max(1, hw − 2) on 4 vCPUs) |
| Rows/s (parse phase) | 2.60–2.88 M rows/s over five runs |
| MB/s (parse phase) | 291–323 MB/s |
| Whole ingest stage (wall) | 4.45–4.97 s, i.e. ≥ 2.0 M rows/s end to end |
| Entities / aggregated edges | 998,086 / 7,757,120 |
| Phases (ms) | map 0–0.7, chunk 0.3–18, alloc 647–720, parse 3,476–3,850, registry merge 100–147, dense index 34–53, location votes 137–186, edge scatter 589–770, edge merge 260–366, edge SoA 189–245 |
| Graph build / corridor bundles | 561–810 ms / 90–111 ms (232 bundles) |
| Startup to "ready to lay out" | 7.8–9.1 s |
| Renderer initialisation (static GPU data for 7.76M edges) | 3.7–4.7 s, down from ~14 s before edge priorities were precomputed |

**Layout at 1M nodes** (998,086 nodes, 7.76M edges; headless, 4 threads):

| Phase | Mean ms per iteration |
|---|---|
| Octree build | 21.3 |
| Barnes-Hut repulsion (AVX kernel, 694 interactions per body) | 237.4 |
| Springs, anchor and integration (incl. inverse permutation) | 52.1 |
| **Total** | **310.8** |

The total was 398 ms per iteration before the SIMD, grouping and ordering
work in this branch. After 20 iterations, 100% of nodes are within 1.2 R of
their hub anchor. With 3 threads (the interactive pool size), the
prelayout averaged 348–361 ms per iteration over 200–300 iterations.

**Layout at 30k nodes** (300k-row dataset): converged in 1,205 iterations,
with 100% of nodes within 1.2 R.

**Render at 1M nodes on llvmpipe** (software rasteriser):

| Metric | Value |
|---|---|
| Draws per frame | 6 |
| Visible edges | 1,684,464 intra + up to 250,000 inter, capacity 2,000,000 |
| Snapshot copy, single 12 MB copy (before amortising) | mean 2.04 ms, max 2.39 ms (19 frames) |
| Snapshot copy, amortised (≤ 4 MiB per frame) | mean 0.70 ms, max 0.91 ms (19 frames) |
| Frame time | ~2–3 s per frame |

The llvmpipe frame time says nothing about a real GPU. It is listed only to
show that the full-scale path runs end to end.

Screenshots, all rendered with llvmpipe on a virtual display:

- `docs/screenshots/overview_1m.jpg` and `overview_1m_hud.jpg`: 1M
  entities after 300 layout iterations. The HUD reports the measured
  318 ms iteration as a 50 ms target miss.
- `docs/screenshots/overview_300k.jpg`, `hub_new_york.jpg` and
  `selection_hud.jpg`: 30k entities after 1,500 iterations.

**Debug allocation check**: 300 frames with clicks and keys, 0
allocations in our code (§7).

**Builds**: GCC 13 and Clang 18, Release and Debug, all with
`-DFTVIZ_WERROR=ON` and `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.
All four have zero warnings, 35 of 35 tests pass, and every build gives the
same layout checksum.

### 10.2 Targets

| Target | Status |
|---|---|
| ≥ 1.5M rows/s at 10M rows on 8 cores | **Met on fewer cores.** 2.6–2.8M rows/s in the parse phase and ≥ 2.0M rows/s end to end, with 2 parser threads on 4 vCPUs. |
| Octree vs brute force < 2% | **Met.** 0.70% at θ = 0.8. |
| Layout checksum determinism | **Met** across runs, thread counts, compilers and build types. |
| ≤ 8 draws per frame | **Met.** 6, or 7 with a pick. |
| ≤ 2M visible edges by priority | **Met.** Enforced by budget caps and the clamp shader. |
| Zero allocations per frame | **Met** in our code (Debug assertion, §7). |
| Zero warnings | **Met** with GCC and Clang. MSVC `/W4` was **not built here**. |
| Distinct NY, LONDON and TOKYO clusters, elevated chain clouds, bundled corridors | **Met** visually at 30k and 1M nodes (`docs/screenshots/`). |
| ≤ 50 ms per layout iteration at 1M nodes on 8 cores | **Not measured.** 311 ms on 4 vCPUs. A naive extrapolation to a modern 8-core desktop (2× threads, ~1.5–2× per-core speed) gives roughly 80–100 ms, so this target is probably **not met** yet. The most promising levers are below. |
| 60 FPS at 1080p with 1M nodes + 2M edges on a GTX 1660 or RX 5600 | **Not measured.** No GPU was available. |
| CPU render ≤ 2 ms | **Not measured** on real hardware. llvmpipe runs the GPU work on the CPU inside GL calls. Our own per-frame CPU work is bounded: the upload chunk is ≤ 4 MiB, and the rest is HUD and uniform writes. |
| GPU per-pass timings in the HUD | **Implemented** (`GL_TIME_ELAPSED` per pass). Values here are llvmpipe's. |

### 10.3 Levers for the layout target

These are not implemented. They are listed so the trade-offs are explicit.

1. **Reuse the tree.** Rebuild the octree every k iterations once T is
   small, refitting the centres of charge in between. That saves most of
   the 21 ms tree cost.
2. **Fewer interactions.** Split large groups adaptively, or use a
   per-body θ for the bodies at the group boundary. The group bounding-box
   test is conservative, at 694 interactions per body.
3. **Faster square root.** Use `rsqrt` with one Newton step. This is
   2–3× faster in the kernel, but it is not bit-identical between CPU
   vendors, so it would only be acceptable with a determinism mode switch.
4. **Fused passes.** Fuse the inverse permutation into the repulsion write
   and stream the spring pass. Together they are worth about 15 ms here.
