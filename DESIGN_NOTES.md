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
   adjacency is built. The largest single transactions per currency become
   whale events on a replay timeline (§6.4).
3. **Layout.** A deterministic initial placement is followed by iterations
   of a parallel Barnes-Hut force solver. The solver rebuilds a Morton-sorted
   octree every iteration and evaluates repulsion with SIMD kernels per
   interaction group. A dedicated coordinator thread runs it and publishes
   position snapshots through a lock-free triple buffer.
4. **Render.** The main thread owns the GL context. It maps the layout onto
   a globe and draws the scene with six instanced draws per frame (seven
   with a pick), then an emissive bloom and composite chain. Data comes
   from immutable buffers plus persistently mapped rings guarded by fences.

## 2. Threading

### Diagram

```
                      startup (ingest + model)                     runtime (interactive)
 ┌───────────────┐   ┌──────────────────────────────────┐   ┌──────────────────────────────────────┐
 │ main thread   │──►│ mmap, chunk, push descriptors    │   │ main = render thread (owns GL)        │
 │ (owns GL)     │   │        │                         │   │  poll input ─► pick readback ─► HUD   │
 └───────────────┘   │        ▼                         │   │  begin_frame: fence wait (slot f-3)   │
                     │  bounded MPMC queue (Vyukov)     │   │    acquire_latest() ◄───────────┐     │
                     │   │      │      │       │        │   │  draw_frame: scene+post, fence  │     │
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
| Layout control (`layout/layout_engine.cpp`) | `control_` counter with `atomic::wait`/`notify` | Release on `wake()` and acquire in the layout thread, so `paused_` and `reheat_requested_` are seen together with the wake-up. The thread loads the counter before it checks the stop request or any other state, so a `stop()` that lands mid-check still makes the next wait return instead of hanging `join()`. |
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
| Node static VBO | attribute stream, divisor 1 | `{hub u16, flags u16, activity f32}`, 8 bytes | 8 MB |
| Positions | SSBO 0 | Persistent ring, 3 regions of N × 12 bytes (layout space) | 36 MB |
| Edges | SSBO 1 | Immutable `{src, dst, attr, weight}`, 16 bytes, intra first then inter, each sorted by priority | ≤ 124 MB |
| Hubs | SSBO 2 | Layout anchor and radius, globe normal and footprint, tangent frame (64 bytes per hub) | < 1 KB |
| Highlight bits | SSBO 4 | Persistent ring, 3 regions of ⌈N/32⌉ words | 375 KB |
| HUD glyphs | SSBO 8 | Persistent ring, one region per frame slot | small |
| Hub label glyphs | SSBO 9 | `{hub, code, column, side | length}`, 16 bytes per glyph | < 4 KB |
| Whale events | SSBO 10 | `{src, dst, src hub, dst hub, start, tier}`, 32 bytes per event | < 16 KB |
| Frame uniforms | UBO 0 | Persistent ring, 3 regions | 3 × 512 B |
| Font | texture unit 1 | R8 atlas, 96 glyphs of 6×8 | small |
| Scene target | none | 2 × RGBA16F + `DEPTH_COMPONENT32F`, MSAA 8× | 332 MB at 1080p |
| Resolve, bloom and output | none | 2 × RGBA16F, 9 bloom levels, SRGB8_ALPHA8 | ~50 MB at 1080p |
| Pick target | none | R32UI + depth, plus a persistent PBO | ~16 MB at 1080p |

Edge attributes are packed into 32 bits:

| Bits | Field |
|---|---|
| 0–7 | flags |
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

### 6.1 The globe

Nodes are laid out in an abstract layout space (§5) and mapped onto a globe
of radius R = 1000 only when they are drawn, in the vertex shaders. The CPU
copy of the same formulas lives in `src/model/globe.hpp`, and
`shaders/common.glsl` mirrors it line for line.

- **Hub placement.** Every hub has a latitude, longitude and footprint (the
  angular radius of its disc) in `config/hubs.json`. Fiat centres sit at
  their city's coordinates. Chain and fallback hubs have no geography, so
  they are pinned to fixed points in open ocean.
- **Node placement.** For a node of hub h at layout position p,
  d = (p − anchor_h) / radius_h. The planar part (d.x, d.z) becomes an
  offset on the tangent plane at the hub's normal, scaled by the footprint
  and clamped to 1.6 footprints so strays stay on the disc. d.y becomes a
  small altitude relief above the surface.
- **Arcs.** An edge between two nodes is a great circle between their
  globe positions, lifted by R · `arc_lift` · ω · sin(πt), where ω is the
  angular separation. Long arcs therefore rise higher than short ones.

### 6.2 Frame structure and draw budget

| Order | Pass | Call |
|---|---|---|
| 1 | Globe: ray-cast sphere, graticule, limb, depth | `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| 2 | Nodes: 1–2.5 px discs | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)` |
| 3 | Intra-hub arcs | `glDrawArraysInstanced(LINES, 0, 2, intra)` |
| 4 | Inter-hub arcs, 32 segments each | `glDrawArraysInstanced(LINE_STRIP, 0, 33, inter)` |
| 5 | Whale arcs and flashes (emissive attachment only) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 66, 2W)` |
| 6 | Hub markers and labels | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, H + glyphs)` |
| on click | Picking (R32UI) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)` |
| post | MSAA resolve | 2 × `glBlitNamedFramebuffer` |
| post | Bloom: 5 downsamples, 4 upsamples | 9 × `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| post | Composite, then present | 2 × `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| post | HUD | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, glyphs)` |

The scene makes 6 draws every frame, or 7 with a pick pending. The post
chain makes 12 with the HUD on. The counts do not depend on the graph, the
camera or the replay clock: a pass with nothing to show still issues its
draw with zero instances or with every primitive collapsed off-screen.
There is no `glDrawElements*`, no indirect draw and no compatibility
profile.

**Render targets.** The scene renders into an MSAA framebuffer with two
RGBA16F colour attachments (scene and emissive) and a 32F depth buffer.
Colour masks keep every pass except the whale pass out of the emissive
attachment. MSAA is 8× by default and is clamped, with a log line, to
`GL_MAX_COLOR_TEXTURE_SAMPLES` when the driver offers fewer.

**Reverse-Z.** The projection is an infinite reverse-Z perspective, with
`glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE)`, depth cleared to 0 and
`GL_GREATER`.

### 6.3 Palette and passes

All colours come from `config/theme.json` as sRGB hex values and are
converted to linear RGB at load. There are four:

| Name | Value | Used for |
|---|---|---|
| Obsidian | `#050608` | Background. Flat, vignette ≤ 3%. |
| Amber Line | `#C8962E` | Graticule, alpha 0.06–0.10. |
| Ink | `#D9DCE1` | Nodes, arcs, hub markers, labels, globe limb; alpha 0.04–0.70. |
| Neon Gold | `#FFC23D` | Whale arcs only, HDR intensity 4–8×. |

The theme loader clamps every alpha and intensity to these ranges. Nothing
is coloured per hub or per currency.

- **Globe.** A fullscreen triangle ray-casts the sphere and writes its
  depth, so the far hemisphere occludes nodes and arcs. The graticule is a
  15° grid drawn analytically: the distance to the nearest grid line,
  divided by its screen-space derivative, gives the coverage of a 1 px
  line. Meridians fade out above 75° latitude, and the whole grid fades
  toward the limb with N·V. A 1 px ink hairline marks the limb, since the
  globe is as dark as the background.
- **Nodes.** Discs of 1–2.5 px with analytic edge coverage. Size and alpha
  follow log(weighted degree), normalised to [0, 1]; alpha uses its
  square, so most nodes sit near the alpha floor and only the busiest
  approach 0.6.
- **Arcs.** 1 px hairlines blended additively into RGBA16F, so overlapping
  flows read as brightness. Alpha runs from 0.03 to 0.10 for inter-hub arcs
  and stays below 0.04 for intra-hub chords, which are not drawn at all
  when the camera is higher than `intra_edge_max_altitude` above the
  surface. Each inter-hub arc gets a per-edge lift between 0.55 and 1.45 of
  the base, from a hash of its index, so arcs between the same two hubs fan
  out into separate lines instead of one saturated trunk. Alpha also tapers
  toward both endpoints, where every arc of a hub converges.
- **Hub markers and labels.** A 1.25 px ring and a 2 px dot, and an
  uppercase monospace label at 11 px with 0.14 em tracking and alpha 0.65.
  Both are placed in screen space and snapped to the pixel grid. A label
  sits beside the marker, or beside the hub's disc of nodes when that is
  larger on screen, on the side named by `label_side` in `hubs.json`.
  Labels carry a one-texel obsidian halo so they stay legible over bright
  arcs.
- **Post.** The two attachments are resolved. Bloom runs on the emissive
  attachment only: five dual-filter (Kawase) downsamples and four
  upsamples. The composite adds scene, emissive and bloom, applies the
  fitted ACES curve, adds the background and the vignette, and writes to an
  SRGB8_ALPHA8 target with `GL_FRAMEBUFFER_SRGB`. There is no FXAA; edges
  are anti-aliased by MSAA and by the analytic coverage in each shader.

### 6.4 Whale events

The dataset has no FX table, so amounts in different currencies cannot be
compared. A whale is therefore defined per currency: while parsing, each
worker keeps the 16 largest single transactions of every currency
(`src/ingest/whale_tracker.hpp`). The per-worker lists are merged with a
total order (|amount|, then time, source, destination, channel), so the
result does not depend on the thread count or the chunking. Rank 0–1 in a
currency is tier 3 (4 px), rank 2–5 tier 2 (3 px), the rest tier 1 (2 px).
Intensity grows with the tier from 4× to 8×.

**Replay.** The dataset's time range is mapped onto `replay_seconds` of
replay time (120 s by default) and looped. A whale's arc leaves its source
at its mapped time. The head travels the arc in `flight_s`, followed by a
0.4 s trail, and one flash of `flash_s` marks the arrival. Space pauses the
replay. Each whale is two instances of the same draw, the arc ribbon and
the flash; inactive ones collapse off-screen in the vertex shader. The
draw is the same every frame.

### 6.5 Per-frame CPU work

The render thread does the following each frame:

- One fence wait.
- A copy of at most 4 MiB of a new layout snapshot into a free region of
  the position ring. A 1M-node snapshot is spread over 3 frames (§6.8).
- A 464-byte uniform write.
- HUD glyph writes into the mapped ring.
- When the selection changes, the highlight bits are copied into the slot's
  region.
- The fixed sequence of state changes and draws in §6.2.

There is no `glBufferData` or `glBufferSubData` after initialisation, and
no buffer re-specification. Debug builds assert zero heap allocations per
frame (§7).

### 6.6 Edge budget and priority

**Priority.** Each edge's priority is its |amount| divided by the largest
edge of the same currency. The edge buffer stores intra-hub edges first and
then inter-hub edges, each in descending priority, so every budget is a
prefix. When a node pair has several edges (different currencies or
channels), only the highest-priority one is uploaded, since they would draw
the same arc.

**Budget.** At most 2,000,000 arcs are drawn per frame, and
`render.json:max_visible_edges` is clamped to that. Inter-hub arcs take up
to `max_inter_hub_edges` (4,000 by default) and intra-hub chords up to
`max_intra_hub_edges` (30,000) of what remains. The defaults are low on
purpose: the arcs blend additively, and a larger share turns the busiest
corridors into solid white.

**Shading weight.** log1p(|amount|) normalised min-max within the currency
and squared. It drives the arc alpha within its range.

### 6.7 Picking and selection

- **Picking.** On a click only, the node pass is re-rendered into an R32UI
  target that stores the instance id + 1. One pixel is read with
  `glReadPixels` into a persistently mapped PBO behind a fence and consumed
  a frame or two later. The result is range-checked before use.
- **Highlight.** The selection's 1-hop neighbourhood is stored as bits in
  SSBO 4. The CPU keeps a master copy and copies it into a slot's region
  only when the selection generation changed.

### 6.8 Layout snapshot upload

The layout thread publishes into a CPU-side triple buffer. The render
thread copies the newest snapshot into a free region of the 3-region
persistent ring.

A region is free when the two frames that may still be in flight do not
reference it. With 3 regions and at most 2 busy, one is always free.

A 1M-node snapshot is 12 MB. Copying it in one frame measured 2.0 ms on the
test machine (§10), the entire 2 ms CPU budget, so the copy is amortised:
each frame copies at most `kUploadBytesPerFrame` = 4 MiB into the target
region. Frames keep drawing the previous region until the fill completes.
No new snapshot is acquired during a fill, so the source, the reader-owned
front slot, stays valid. The extra latency is three frames, about 50 ms at
60 FPS, which is the layout period anyway.

### 6.9 Offscreen screenshots

`--screenshot FILE.png` renders without a window. On Linux it creates a
surfaceless EGL context (`EGL_PLATFORM_SURFACELESS_MESA`); elsewhere it
falls back to a hidden 64×64 GLFW window, since the frame goes to an
offscreen target either way. The layout runs `--prelayout` iterations and
then stays frozen. The camera comes from a preset (`globe`, `london`,
`hub:<ID>`, `whale`, `graticule`) or from `Y,P,D`. Warm-up frames step the
replay clock by 1/60 s and the captured frame lands on the preset's replay
time, or on `--time`. The output is read back from the SRGB8 target and
written with stb_image_write. The program prints an XXH3 digest of the
pixels, so two runs can be compared without a diff tool.

### 6.10 Robustness

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
- **RAII.** Every GL object is a move-only RAII wrapper created with DSA
  `glCreate*` calls (`render/gl_raii.hpp`).

### 6.11 GL 4.3 fallback

The program requires GL 4.3 plus `GL_ARB_direct_state_access`,
`GL_ARB_buffer_storage` and `GL_ARB_clip_control`. `GL_KHR_robustness` and
`GL_KHR_debug` are optional. glad is generated for core 4.5 plus exactly
these extensions, so the DSA, storage and clip-control entry points load
from either source. On a 4.3 or 4.4 context, the shader loader rewrites
`#version 450 core` to `#version 430 core`. Context creation tries 4.6,
4.5 and 4.3 in turn, and GLFW error output is muted while probing.

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

**Driver allocations.** A driver that compiles shaders lazily in C++ (Mesa
with LLVM, for example) allocates inside GL calls, and on ELF platforms its
`operator new` calls resolve to the executable's replacement. Those
allocations would be charged to the render thread. Debug builds therefore
link a second glad library generated with glad's `DEBUG` option, whose pre-
and post-call hooks wrap every GL call. The application installs hooks
that mark the call as foreign code. Allocations inside a GL call are
counted separately and reported, but they do not trigger the assertion.
Our own allocations still do. Release builds use the plain loader and pay
nothing.

**Verified result.** The Debug build ran 600 frames of the 300k-row
dataset with injected clicks, key presses (framing, replay pause, HUD and
arc toggles, hub fly-to) and a drag:

- 0 allocations in our frame code;
- 65,682 driver allocations inside GL calls, reported and not asserted;
- 18 draws per frame throughout (6 scene, 12 post).

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
  milliseconds. Days are computed with Hinnant's civil-date algorithm. Dates
  that do not exist, such as Feb 31 or Feb 29 in a non-leap year, make the
  row malformed.
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
| Edge budgets | Priority-sorted prefixes: 4,000 inter-hub arcs and 30,000 intra-hub chords by default, under the 2M hard cap. | Arcs blend additively; past a few thousand per corridor they saturate to white and hide the structure. |
| Whale definition | The 16 largest single transactions of each currency, tiered by rank within the currency. | There is no FX table, so a cross-currency "largest" would be meaningless. |
| Hub coordinates | Fiat centres at their city's latitude and longitude; chain and fallback hubs pinned to fixed open-ocean points. | Chains and unresolved jurisdictions have no geography; open ocean keeps them off real cities. |
| Draw calls | Scene geometry uses only `glDrawArraysInstanced`; fullscreen passes use `glDrawArrays(GL_TRIANGLES, 0, 3)`. No culling compute pass, no indirect draws. | A constant draw sequence; the vertex shaders collapse anything that should not show. |
| CSV columns | Matched by header name, any order, extra columns ignored. | Tolerates exports from different systems. |
| `.ftxb` layout | 64-byte header, 64-byte records with fixed field offsets, string pool of (offset, length) references. Little-endian. | Fixed-size records chunk without scanning. Documented in `ingest/ftxb_format.hpp`. |
| Self-loops | Aggregated, but not drawn and not used as springs. | They have no geometric meaning. |
| Pool size while rendering | hw − 2 workers plus `T_layout`, leaving one core for the render thread and the driver. | Rendering at 60 FPS must not compete with the layout. |
| Debug allocation check and drivers | Allocations inside GL calls are attributed to the driver (§7). | The check is about our code, and a driver JIT is outside our control. |
| Snapshot upload | Amortised at 4 MiB per frame (§6.8). | Bounds the per-frame CPU cost. |

## 10. Performance

### 10.1 What was measured, and where

All numbers in this section come from one machine:

- 4 vCPUs (Intel Xeon @ 2.10 GHz, AVX2 and AVX-512 capable) in a shared
  cloud VM, no discrete GPU.
- GCC 13.3, Release with LTO.

**Ingest of 10M rows** (`gen_synthetic --rows 10000000 --seed 42`, CSV,
1,121 MB; one run):

| Metric | Value |
|---|---|
| Parser threads | 2 (max(1, hw − 2) on 4 vCPUs) |
| Rows/s (parse phase) | 3.37 M rows/s |
| MB/s (parse phase) | 378 MB/s |
| Whole ingest stage (wall) | 3.93 s, i.e. 2.55 M rows/s end to end |
| Entities / aggregated edges | 998,146 / 7,759,391 |
| Phases (ms) | map 0.0, chunk 0.1, alloc 668, parse 2,968, registry merge 114, dense index 36, location votes 140, edge scatter 615, edge merge 229, edge SoA 176 |
| Graph build | 513 ms |
| Whale events | 336 (16 per currency, 21 currencies) |
| Startup to "ready to lay out" | 7.2 s |

**Layout at 1M nodes** (998,146 nodes, 7.76M edges; headless, 4 threads,
20 iterations):

| Phase | Mean ms per iteration |
|---|---|
| Octree build | 18.2 |
| Barnes-Hut repulsion (AVX kernel, 694 interactions per body) | 214.2 |
| Springs, anchor and integration (incl. inverse permutation) | 44.5 |
| **Total** | **276.9** (p95 315.3) |

After 20 iterations, 100% of nodes are within 1.2 R of their hub anchor.

**Snapshot upload at 1M nodes**: a single 12 MB copy took a mean of
2.04 ms (max 2.39 ms, 19 frames); amortised to ≤ 4 MiB per frame it took a
mean of 0.70 ms (max 0.91 ms).

**Rendering** was not measured on a GPU. The machine above has none, and
frame times from a software rasteriser say nothing about one. What was
verified is structural: the draw counts in §6.2 and the zero-allocation
check in §7.

**Builds**: GCC 13 Release and Debug with `-DFTVIZ_WERROR=ON` and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, zero warnings. All tests
pass and every build gives the same layout checksum.

### 10.2 Targets

| Target | Status |
|---|---|
| ≥ 1.5M rows/s at 10M rows on 8 cores | **Met on fewer cores.** 3.37M rows/s in the parse phase and 2.55M rows/s end to end, with 2 parser threads on 4 vCPUs. |
| Octree vs brute force < 2% | **Met.** 0.70% at θ = 0.8. |
| Layout checksum determinism | **Met** across runs, thread counts, compilers and build types. |
| Scene draws constant per frame | **Met.** 6 scene draws, or 7 with a pick; 12 post draws with the HUD. |
| ≤ 2M visible edges by priority | **Met.** Enforced by the budget caps. |
| Zero allocations per frame | **Met** in our code (Debug assertion, §7). |
| Zero warnings | **Met** with GCC. MSVC `/W4` was **not built**. |
| ≤ 50 ms per layout iteration at 1M nodes on 8 cores | **Not measured** on 8 cores. 277 ms on 4 vCPUs. A naive extrapolation to a modern 8-core desktop (2× threads, ~1.5–2× per-core speed) gives roughly 70–90 ms, so this target is probably **not met** yet. The most promising levers are below. |
| 60 FPS at 1080p with 1M nodes + 2M edges on a GTX 1660 or RX 5600 | **Not measured.** No GPU was available. |
| CPU render ≤ 2 ms | **Not measured** on GPU hardware. Our own per-frame CPU work is bounded: the upload chunk is ≤ 4 MiB, and the rest is HUD and uniform writes. |

### 10.3 Levers for the layout target

These are not implemented. They are listed so the trade-offs are explicit.

1. **Reuse the tree.** Rebuild the octree every k iterations once T is
   small, refitting the centres of charge in between. That saves most of
   the 18 ms tree cost.
2. **Fewer interactions.** Split large groups adaptively, or use a
   per-body θ for the bodies at the group boundary. The group bounding-box
   test is conservative, at 694 interactions per body.
3. **Faster square root.** Use `rsqrt` with one Newton step. This is
   2–3× faster in the kernel, but it is not bit-identical between CPU
   vendors, so it would only be acceptable with a determinism mode switch.
4. **Fused passes.** Fuse the inverse permutation into the repulsion write
   and stream the spring pass. Together they are worth about 15 ms here.
