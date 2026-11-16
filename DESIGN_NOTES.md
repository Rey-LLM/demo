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
   a globe and draws the scene with eight draws per frame (nine
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
| Font | texture unit 1 | R8 atlas, 99 glyphs of 6×8 | small |
| Scene target | none | 2 × RGBA16F + `DEPTH_COMPONENT32F`, MSAA 8× | 332 MB at 1080p |
| Resolve, bloom and output | none | 2 × RGBA16F, 3 bloom levels, SRGB8_ALPHA8 | not re-measured |
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

| Order | Pass (timer) | Call |
|---|---|---|
| 1 | Globe: ray-cast sphere, graticule, limb, depth (GLOBE) | `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| 2 | Atmosphere: Fresnel rim on a shell at 1.015 R (ATMO) | `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| 3 | Coastlines: one instance per segment, 1 px analytic AA (COAST) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, segments)` |
| 4 | Nodes: 1–2.5 px discs (NODES) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)` |
| 5 | Intra-hub arcs (ARCS) | `glDrawArraysInstanced(LINES, 0, 2, intra)` |
| 6 | Inter-hub arcs, 32 segments each (ARCS) | `glDrawArraysInstanced(LINE_STRIP, 0, 33, inter)` |
| 7 | Whale arcs, heads and flashes, emissive attachment only (WHALES) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 66, instances)` |
| 8 | Hub markers and labels (HUBS) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, H + glyphs)` |
| on click | Node picking, R32UI (PICK) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, N)` |
| post | MSAA resolve (POST) | 2 × `glBlitNamedFramebuffer` |
| post | Bloom: 3 downsamples, 2 upsamples (POST) | 5 × `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| post | Composite, then present (POST) | 2 × `glDrawArrays(GL_TRIANGLES, 0, 3)` |
| post | HUD, including the live layer (HUD) | `glDrawArraysInstanced(TRIANGLE_STRIP, 0, 4, glyphs)` |

The scene makes 8 draws every frame, or 9 with a pick pending. The post
chain makes 8 with the HUD on. The counts do not depend on the graph, the
camera, the feeds or the replay clock: a pass with nothing to show still
issues its draw with zero instances, with alpha 0, or with every primitive
collapsed off-screen. Every pass has a `GL_TIME_ELAPSED` query; the names
in brackets are the columns of the `--frames` report. There is no
`glDrawElements*`, no indirect draw and no compatibility profile.

**v2 passes.** Coastlines are Natural Earth polylines (§14) uploaded once:
the 1:110m set above a camera altitude of 1,400 units, the 1:50m set below
it. Their colour is amber mixed 45% toward graphite at alpha 0.22. The
atmosphere is `pow(1 − N·V, 5)` in amber at alpha 0.20 and writes to the
scene attachment only, so it never blooms. Arcs fade with view depth and
are discarded where the globe's analytic sphere occludes them. A whale has
a bright head and a tapering trail. Hub hover and pin use a CPU ray against
the sphere (§13), so they add no draw.

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
converted to linear RGB at load. There are four, plus the hot end of the
whale ramp:

| Name | Value | Used for |
|---|---|---|
| Obsidian | `#050608` | Background. Flat, vignette ≤ 3%. |
| Amber Line | `#C8962E` | Graticule, alpha 0.06–0.10. |
| Ink | `#D9DCE1` | Nodes, arcs, hub markers, labels, globe limb; alpha 0.04–0.70. |
| Deep Amber | `#FF9A1F` | Whale arcs and flow trails, HDR intensity 4–8× (trails 0.45× of tier 1). |
| White Gold | `#FFF1C9` | Hot end of the whale ramp: tier 3 heads and cores run toward it. |

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
Intensity grows with the tier from 4× to 8×. The fragment shader mixes
Deep Amber toward White Gold with a heat term (0 for trails, 0.5 for tier 2,
1.0 for tier 3) that is strongest at the head and the ribbon core, so the
largest transfers burn white-gold and the rest stay amber.

**Flow trails (v3, tier 0).** Live and replayed trades or transactions of
at least `trail_min_usd` (2,500 USD by default) between two different hubs
become short amber trails in the same draw: no flash, 0.6× width, 0.45×
the tier-1 intensity. A token bucket on the model thread (`trail_max_per_s`,
40 by default, burst half a second) caps them, and when all 256 live slots
are busy a whale evicts a trail but a trail never evicts a whale. They are
not counted as whales anywhere (HUD, exports).

**Hub glow (v3).** The hub pass writes a Gaussian halo (sigma 2.2 px × UI
scale) into the emissive attachment, pulsing ±15 % at 1.7 rad/s, so hubs
bloom softly. Glyphs write nothing there; the render tests confine
emissive pixels to whales plus that radius around hubs.

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
- 16 draws per frame throughout (8 scene, 8 post) in v2; the model thread
  reported 0 allocations while ticking in the same run.

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

**Rendering: not yet measured on discrete GPU.** The machine above has
none. The figures below are from Mesa llvmpipe (LLVM 20.1.2, software
rasteriser, 4 vCPUs), Release, 2560×1440, 4× MSAA (the driver's maximum),
300,000-row dataset (29,922 nodes, 4,000 inter-hub arcs drawn), on
2026-11-15. They compare v1 and v2 on the same software rasteriser and say
nothing about a GPU.

| | v1 (110 frames) | v2 (50 frames) |
|---|---|---|
| Frame ms mean / p50 / p95 | 331.2 / 312.5 / 423.2 | 404.9 / 402.1 / 477.7 |
| Draws per frame | 18 (6 scene, 12 post) | 16 (8 scene, 8 post) |
| GLOBE | 0.163 | 1.412 |
| ATMO | n/a | 1.715 |
| COAST | n/a | 0.855 |
| NODES | 0.119 | 0.855 |
| ARCS | 24.560 | 40.165 |
| WHALES | 0.211 | 0.523 |
| HUBS | 0.210 | 0.459 |
| POST | 95.621 | 128.002 |
| HUD | 0.109 | 0.910 |

Pass columns are `GL_TIME_ELAPSED` means in ms as llvmpipe reports them.
The v2 frame is slower on this rasteriser; the two runs were single runs
on a shared VM and were not repeated.

**Builds**: GCC 13 Release and Debug with `-DFTVIZ_WERROR=ON` and
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, zero warnings. All tests
pass and every build gives the same layout checksum.

### 10.2 Targets

| Target | Status |
|---|---|
| ≥ 1.5M rows/s at 10M rows on 8 cores | **Met on fewer cores.** 3.37M rows/s in the parse phase and 2.55M rows/s end to end, with 2 parser threads on 4 vCPUs. |
| Octree vs brute force < 2% | **Met.** 0.70% at θ = 0.8. |
| Layout checksum determinism | **Met** across runs, thread counts, compilers and build types. |
| Scene draws constant per frame | **Met.** 8 scene draws, or 9 with a pick; 8 post draws with the HUD. |
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

## 11. Live data

### 11.1 Threads and queues

```
feed thread ×6 --LiveEvent (64 B)--> MPSC ring (65,536) --> model thread
feed thread ×6 --FeedTransition----> SPSC ring per feed --> model thread
model thread   --VisualEvent-------> SPSC ring (1,024)  --> render thread
model thread   --LiveSnapshot------> double buffer      --> HUD, exporter
```

No queue blocks its producer. A full ring drops the item and counts it;
the count is on the feed's HUD row and in `session.json`. The model thread
ticks every 10 ms, applies at most 32,768 events per tick and publishes a
snapshot at 50 Hz. It allocates nothing while ticking (asserted in Debug
with the counting allocator, as in §7). Feed threads allocate only when a
connection opens.

The network client is in `src/ingest/live/net.cpp`: TLS through OpenSSL
with certificate and host name verification always on, HTTP/1.1 upgrade,
RFC 6455 framing with masking, ping/pong and an 8 MB frame limit. It
honours `HTTPS_PROXY` with a CONNECT tunnel.

### 11.2 Feed state machine

`CONNECTING → LIVE` on the first valid message. `LIVE → DEGRADED` after
30 s of silence, then a reconnect. Any failure gives `OFFLINE`; HTTP 429 or
a provider rate-limit error gives `RATE_LIMITED`. Reconnects use full
jitter: a uniform delay in `[0, min(30 s, 0.5 s · 2^attempt)]`. A feed that
has been `OFFLINE` or `RATE_LIMITED` for 15 s switches to `REPLAY`: the
model thread applies that feed's records from the bundled capture, looped
at 1×, with the label REPLAY, until a live message arrives. The adapter
keeps trying to reconnect meanwhile.

v3 adds `fallback_policy` to `feeds.json`. The shipped value
`all_offline` replays a feed only while **no** feed is LIVE, i.e. when the
machine is offline; one dead endpoint on an otherwise connected machine
stays OFFLINE instead of blending synthetic data with real data.
`per_feed` restores the v2 behaviour. Each feed can list `fallback_urls`;
a failed connect (DNS, TCP or TLS) rotates to the next URL before the
backoff delay. Measured in the cloud container, where every feed host is
blocked: all feeds showed REPLAY after 15 s.

### 11.3 Labels

| Label | Meaning | Where it comes from |
|---|---|---|
| LIVE | received from a public endpoint in this session | BTC, BTC_BLOCKS, ETH, COINBASE, KRAKEN, BINANCE |
| DELAYED | real data fetched on purpose behind the chain tip, or published once a day | SOL (32 slots behind), FX (ECB reference rates) |
| MODELED | derived from published aggregates, never observed | FIAT corridors |
| REPLAY | played back from a capture file | `--replay`, or a feed in fallback |

The label travels with every event into the flow cells, the whale journal
and the volume journal, so each export row and each HUD row carries it. A
hub panel lists every label that contributed to the hub.

### 11.4 Endpoints and rate limits

Documentation pages were read on 2026-11-15. Where a page states no limit,
that is what the table says; the adapter's own limit applies either way.

| Feed | Endpoint | Published limit | Adapter behaviour |
|---|---|---|---|
| BTC | `wss://ws.blockchain.info/inv`, `unconfirmed_sub` | none found on the provider's API page | one connection, no requests after the subscribe |
| BTC_BLOCKS | `wss://mempool.space/api/v1/ws`, `want: blocks` | "usage limits apply to our WebSocket API", no figures (mempool.space/docs/api/websocket) | one connection, one subscribe |
| ETH | `wss://ethereum-rpc.publicnode.com`, `newHeads`, then `eth_getBlockByNumber` | not checked: the provider's page was not read | one connection, ≤ 2 requests/s |
| SOL | `wss://api.mainnet-beta.solana.com` `slotSubscribe`, then `getBlock` over HTTPS | 100 requests per 10 s per IP, 40 per 10 s for a single RPC, 40 concurrent connections, 100 MB per 30 s (solana.com/docs/references/clusters) | ≤ 3 `getBlock`/s (hard cap 5 in code), 32 slots behind the tip, at most 32 transfers sampled per block |
| COINBASE | `wss://ws-feed.exchange.coinbase.com`, `matches`, `ticker`, `heartbeat` | none stated on the channels page | one connection, one subscribe |
| KRAKEN | `wss://ws.kraken.com/v2`, `trade`, `ticker` | none stated on the trade channel page | one connection, two subscribes |
| BINANCE | `wss://stream.binance.com:9443/stream?streams=<pair>@aggTrade/…`, fallbacks port 443 and `data-stream.binance.vision` | 5 incoming messages/s, 300 connections per 5 min per IP, 24 h connection lifetime (Binance WebSocket Streams docs) | one combined-stream connection, no messages sent. Binance answers HTTP 451 from US IP addresses; the feed then rotates to the fallback URLs (on the CI runner: two refusals, then LIVE) |
| FX | `https://api.frankfurter.dev/v1/latest?base=USD&…`, fallback `api.frankfurter.app` | none stated; data changes once per working day | one GET every `poll_s` (900 s) |

A Solana block is sampled: up to 32 of its transfers become events, each
carrying its own USD value and the number of transactions it stands for.
Transaction counts therefore cover the whole block and USD volume covers
the sampled transfers only. Nothing is extrapolated.

### 11.5 Valuation

Events are valued in USD with prices seen on the feeds themselves: the last
Coinbase or Kraken ticker or trade for the asset. A trade quoted in EUR,
GBP or JPY is valued through the cross rate implied by the same exchange's
USD pair (`set_price` in `parsers.cpp`). v3 adds the FX feed: the
European Central Bank's daily reference rates via Frankfurter (labelled
DELAYED, shown in the HUD with their date). They fill EUR, GBP and JPY
prices only where no exchange has supplied one for 120 s, so exchange cross
rates still win. USDT is valued at the Kraken USDT/USD price, or at par
until that tick arrives.

There is no real-time BIS feed: the BIS publishes survey data and
statistical series, not a stream. The fiat corridors therefore stay
MODELED (§11.6). No zero-auth ETF flow endpoint was found, so ETF data is
not included. An
event that arrives before any price is known has value 0 and cannot be a
whale. A whale is a single event at or above the feed's `whale_usd`; tier 2
is 3× and tier 3 is 10× that.

### 11.6 Modeled fiat corridors

`config/flows_baseline.csv` holds five corridors derived from the BIS
Triennial Central Bank Survey 2022 (global OTC FX turnover of USD 7.5
trillion per day and each currency's share of trades; the file cites the
page). Each corridor is New York ↔ the currency's home hub at a flat
per-second rate. They carry volume and no transaction count, because none
is published. CHF and SGD have no share in the cited text and are not
modeled.

### 11.7 Capture and replay

A `.ftxcap` file is a header (flags, configuration hash, start time,
duration), the 64-byte events, and a keyframe of the model state every
10 s, zero-run encoded. A seek restores the nearest keyframe at or before
the target and replays forward, so the result is identical to playing from
the start (tested by hashing the state). The bundled
`assets/replay/default.ftxcap` is **synthetic**: `tools/gen_capture.cpp`
generates it from seed 20261115 (120 s, 14,380 records). It is flagged
SYNTHETIC in the file, on the timeline and in `session.json`. No capture of
real feed traffic is bundled, because the build machine could not reach the
endpoints.

## 12. Export

`E`, the EXPORT button or `--export-after` asks the exporter thread for one
export. The request is coalesced: asking again while one is running does
nothing. The thread takes the latest model snapshot, reads the journals
under the journal gate (a Dekker-style flag pair with the model thread, so
neither side blocks on a lock) and writes five files into a new directory
`ftviz_export_<UTC timestamp>`:

| File | Content |
|---|---|
| `session.json` | version, git SHA, start and end time, mode, capture, label definitions, per-feed state history and counters, modeled corridors with source |
| `hub_flows.csv` | `src_hub,dst_hub,currency,usd_volume,tx_count,source_label` |
| `hub_flows.json` | the same flows nested by source and destination hub |
| `whales.csv` | `ts_utc,chain,asset,usd_value,tier,src,dst,source_label` |
| `volume_tps.csv` | `ts_utc,feed,tx_count,usd_volume`, one row per second and feed |

Each file is written to `<name>.tmp`, flushed, and renamed. Times are ISO
8601 UTC. CSV fields follow RFC 4180 quoting. The render thread only shows
the result as a toast for 3 s. Golden files in `tests/golden/export/` pin
the formats byte for byte.

## 13. Interaction

- **Hub picking.** A ray through the cursor is intersected with the globe
  sphere on the CPU and the nearest hub within 1.6° wins. Hover highlights
  it; a click pins its panel (1 min, 15 min and session inflow, outflow and
  net, tx/s, whale count, top three counterparties, contributing labels).
- **View presets.** Keys 1–5 fly to the globe, the Americas, Europe, Asia
  and the chain shell. The flight lasts 1.2 s: the direction is slerped and
  distance interpolated with a cubic ease in and out, and the endpoints are
  exact.
- **Pause.** Space stops the layout, the model clock and the replay clock.
  Feeds keep filling the bounded ring and count drops once it is full.
- **Timeline.** In `--replay` the 1 px amber line is the capture. Left and
  Right seek 1 s, with Shift 10 s; dragging scrubs. A seek clears the live
  whale arcs.

## 14. Dependencies and data added in v2

| Dependency | Pin | Why |
|---|---|---|
| simdjson | tag `v3.10.1` (FetchContent, single-header build, exceptions off) | Feed messages are JSON at up to thousands per second. The On-Demand parser reads them without building a tree and without allocating per message, which the zero-allocation rule for the feed hot path needs. |
| OpenSSL | system package, ≥ 1.1.1 (`find_package`) | Every endpoint is `wss://` or `https://`. TLS is not something to hand-write, and the system library gets security updates from the OS. It is the only dependency not pinned to a tag, for that reason. |

**Natural Earth.** `assets/coastlines_110m.bin` and
`assets/coastlines_50m.bin` are the `ne_110m_coastline` and
`ne_50m_coastline` layers of Natural Earth release v5.1.2, converted by
`tools/build_coastlines.py`. Natural Earth is in the public domain
(naturalearthdata.com).

## 15. v3: one standalone binary

**Embedded resources.** `cmake/embed_resources.cmake` turns every shader,
the five config files, both coastline layers and the replay capture into
`inline constexpr unsigned char` arrays (one generated header per file,
regenerated only when the input changes) and an index
(`src/core/embedded_fs.hpp`). `read_text_file` and `file_exists` accept
`embedded:/<relative path>`. A path given on the command line or found
under `--data-dir` wins; otherwise the embedded copy is used, so the binary
needs no files next to it. The unit tests compare every embedded file to
the source tree byte for byte.

**Static linking.** `FTVIZ_STATIC` (default ON): MSVC builds with
`/MT` (`CMAKE_MSVC_RUNTIME_LIBRARY`) and links vcpkg's
`openssl:x64-windows-static`; MinGW links with `-static`; Linux links
libstdc++ and libgcc statically and prefers `libssl.a`/`libcrypto.a`.
GLFW, glad, GLM, simdjson and xxHash are always static. The Windows exe
imports only system DLLs (KERNEL32, USER32, GDI32, SHELL32, ADVAPI32,
CRYPT32, WS2_32 and the C runtime for MinGW). The Linux binary depends on
libc and libm; libEGL, X11 and libGL are opened with `dlopen` at run time.

**Windows specifics.** The exe uses the GUI subsystem, so a double-click
opens no console; started from a console it attaches to it for log output,
and a fatal error is shown in a message box. TLS peers are verified against
the Windows ROOT certificate store (crypt32); elsewhere the usual CA bundle
paths are probed. Winsock is initialised before the first socket (a v3 bug
found under Wine: without it every feed failed at `getaddrinfo`).

**Default live mode.** With no arguments ftviz shows the globe, the MODELED
corridors and every feed. `--input` is optional; `--no-live` turns the
feeds off.

**HUD (v3).** The summary line shows LIVE x/y streams and total TX/s; each
feed row adds a latency column (exponential moving average of local receive
time minus the provider's event timestamp, so it includes clock skew); the
FX row shows the ECB date and EURUSD; the whale tape lists the last eight
whales, newest first.

**What was not built as asked.** ImGui, GLEW and ixwebsocket are not used:
the HUD is the existing instanced glyph renderer, glad replaces GLEW, and
the existing RFC 6455 client in `net.cpp` handles WebSockets. Swapping them
in would have replaced working, tested code with new dependencies.

## 16. v4: earth, pools, sessions, search, tape

### 16.1 Day and night

`src/model/solar.cpp` computes the subsolar point from UTC with the NOAA /
Meeus low-precision formulas (declination plus the equation of time). The
unit tests compare it with astropy 8.0.2 (`get_sun` transformed to ITRS) on
six instants from 2024 to 2026: every latitude and longitude is within
0.2°. The sun direction enters the `Frame` UBO (`u_sun`).

Day factor `d = smoothstep(-0.10, 0.10, N·sun)`, about ±6° around the
terminator. The base colour is mixed in `composite.frag`, after tone
mapping: the globe is ray-cast once more per pixel and the display-referred
base becomes `mix(Obsidian, Day Tint, d)` with analytic coverage at the limb.
Doing it there keeps `#0B0E13` and `#050608` exact on screen (mixing in HDR
before ACES would shift both). The render test checks that more than half of
the disk pixels are within ±2 of each hex on the day and night side.

Coastlines are drawn at `mix(0.5, 1, d)` of their alpha. The rim
(`atmosphere.frag`) is scaled by `mix(0.15, 1, d)` so it reads as the sunlit
limb, and inside a band of 2.5× the twilight width around the terminator it
blends toward Night Lights by `rim_twilight` (0.35).

City lights: `globe.frag` samples a BC4 map (`GL_COMPRESSED_RED_RGTC1`,
2048×1024 with its mip chain) with `textureGrad`, taking the longitude
gradient from whichever of the two longitude fields has no seam at that
pixel, so the mip level is right across the antimeridian. The value is
already log-toned (`tools/build_citylights.py`, `log1p(64 x)/log1p(64)`).
Output: `Night Lights × 1.5 × value × (1 − smoothstep(0, 0.5, d))`, into the
scene attachment only. There are no lights where `d ≥ 0.5` (asserted in the
render test against a CPU ray cast of every lit pixel), and the emissive
attachment stays black, so city lights never bloom.

### 16.2 Cables and MODELED routes

`assets/cables.bin` (format in `src/model/cables.hpp`) holds polylines
simplified with Douglas–Peucker on the sphere at 5 km cross-track
tolerance and a corridor per cable (TRANS_ATLANTIC, TRANS_PACIFIC,
EUROPE_ASIA, INTRA_ASIA, OTHER) from the regions its vertices touch. The
renderer reuses the coastline program with the cable buffer bound to SSBO
11: one instanced draw, one instance per segment, Amber at alpha 0.12
(clamped to 0.10–0.16 by the theme loader), same at night.

Routing runs once at startup. Polyline vertices within 2 km of each other
merge into one graph node. Each FIAT_CENTER hub links to the landing points
within 800 km, or to its three nearest within 2,500 km. Dijkstra from every
hub gives a path per hub pair; a path more than 2.6× the great-circle
distance, or no path, falls back to the great circle. Paths never transit
another hub. Routes are resampled to at most 512 points by arc length and
uploaded to SSBO 14 (first, count per pair) and 15 (unit vectors with the
normalised length in w). `whale.vert` binary-searches the route for each of
its 129 ribbon vertices. The HUD and README call these routes
"ROUTE · MODELED (cable geometry)": real traffic routing is not public.

### 16.3 Post

Bloom is a 6-level dual-filter (Kawase) chain on the emissive attachment
(6 downsamples, 5 upsamples). Only whales and liquidations write emissive:
v4 removed the hub glow, and flow trails (tier 0) now go to the scene
attachment in Amber. ACES fitted, 8× MSAA requested (the driver maximum
when lower), no FXAA. New GPU timers: CABLES and BLOOM (POST is now the
composite alone).

Draws per frame: scene 9 (globe, rim, coast, cables, nodes, 2 arc draws,
whales, hubs), post 14 with 6 bloom levels (2 blits are not draws). The
count does not depend on the data.

### 16.4 RPC endpoint pools

ETH and SOL list three endpoints each in `feeds.json` (`pool`), each with
its own token bucket (`max_requests_per_s`, `burst`), the provider's
published limit as text, the documentation URL and a retrieval date.
`src/ingest/live/endpoint_pool.hpp` states the rules:

- one endpoint is active; every request takes a token from the active
  endpoint's bucket. An empty bucket skips the request; it never moves the
  request to another endpoint, so the pool cannot be used to exceed a
  limit it is already at;
- only a provider-side failure rotates: connect/TLS/HTTP error, timeout or
  an explicit 429 / rate-limit message. The failed endpoint cools down
  30 s after an error, 120 s after a 429, doubling on repeats up to 15 min;
- health is a latency EWMA, an error-rate EWMA and a 429 count; the active
  endpoint's latency is on the feed's HUD row;
- every rotation is logged (time, from, to, reason) and exported.

Budgets are capped at 5 requests/s per endpoint in the loader whatever the
config says. The limits in the shipped config were not re-read from the
providers' pages for v4 and say `as_of: UNVERIFIED`.

### 16.5 New feeds

Binance USDⓈ-M `!forceOrder@arr` (liquidations, BINANCE_LIQ) and OKX
(`trades` on BTC/ETH/SOL-USDT and `liquidation-orders` on SWAP, contract
sizes from the public instruments endpoint at connect, `ping` every 20 s).
A liquidation's side is the order side: a SELL closes a LONG. When every
configured URL of a venue (its own documented hosts) answers HTTP 451, or
403 at the WebSocket upgrade, the feed becomes `GEO_BLOCKED`: it is left
out, no REPLAY stands in for it, and it re-checks after 10 minutes. No
proxy or other region is ever tried.

### 16.6 Trading sessions

`config/sessions.json` lists TSE, LSE, NYSE/Nasdaq and SSE: IANA zone,
pre-open, open, close, lunch break, holidays for this year and next, and a
source URL per exchange. At startup each zone is expanded through
`std::chrono::locate_zone` into a table of UTC offset intervals; after that
`state()` is a table lookup with no tzdb call and no allocation. States:
PRE, OPEN, LUNCH, CLOSED, HOLIDAY; the HUD chip reads e.g.
`LONDON    OPEN    02:28 to close`. Corridor intensity is the BIS daily
mean × the mean session factor of its two hubs (OPEN 1.0, LUNCH 0.5, PRE
0.4, CLOSED 0.2, HOLIDAY 0.1), labelled MODELED. The holiday lists are
marked UNVERIFIED: they were compiled from the published rules and have not
been checked line by line against the exchanges' calendars.

### 16.7 Search, tape, export

`/` or Ctrl+K opens the command bar (`src/ui/command_bar.hpp`, fixed
arrays, no allocation). One term per query: a hub or chain name (fuzzy),
`>10m`, `whale:t2`, `venue:okx`, `liq`, `addr:…`, `tx:…`; at most 6
results; Enter flies there with the 1.2 s eased slerp and pins the hub
panel. Filters show as chips above the tape and are removed by a click.
They apply to the tape and to the arcs (the render thread drops filtered
visual events before they take a slot). `addr:` and `tx:` search the rows on
the tape (address fragments and the source id hash).

The tape is the right-hand column: 12 rows newest first, scrolling with a
1 s ease when a row arrives, only the tier marker (`WHALE T1..T3`, `LIQ`) in
Neon Gold. The model keeps the last 10,000 rows in a ring; a click on a
row flies to its hub.

Exports gain `tape.csv` (the 10,000-row window) and `liquidations.csv`;
`session.json` gains `filters`, `sun`, `trading_sessions`, `endpoint_pools`
and `endpoint_rotations` (format `ftviz-export/2`). Golden files cover all
seven files.

### 16.8 Configuration override order

Lowest to highest: the copy compiled into the binary; a file of the same
name in `%APPDATA%\ftviz\` (Windows) or `$XDG_CONFIG_HOME/ftviz`,
`~/.config/ftviz` (elsewhere); `--data-dir`; an explicit option
(`--feeds`, `--theme`, …). `--no-user-config` skips the user directory. The
log names every file taken from the user directory.
