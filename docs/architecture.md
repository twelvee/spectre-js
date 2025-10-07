# Spectre-JS Runtime Architecture

## Vision

Spectre-JS is an embeddable JavaScript runtime built for real-time interactive systems. It targets modern ECMAScript compliance while delivering deterministic low-latency execution for game engines and high-frequency UI pipelines. The runtime stack is layered around a pluggable execution core that can switch between single-threaded and multi-threaded execution while optionally enabling GPU acceleration without altering the embedding API.

## Guiding Principles

- Deterministic latency: prioritize consistent frame-to-frame execution time.
- Memory locality: aggressively exploit cache-friendly layouts and arena allocators.
- Modular acceleration: isolate hardware-specific optimizations behind mode adapters.
- Embeddability: zero external process requirements, easy host integration, no global state.
- Observability: precise timing, allocation, and instrumentation hooks.

## High-Level Architecture

```
+-------------+    +-----------------+    +-----------------+
| Host Engine | -> | Spectre Facade  | -> | Execution Modes |
+-------------+    +-----------------+    +---------+-------+
                                           | Single  |
                                           | Thread  |
                                           +---------+
                                           | Multi   |
                                           | Thread  |
                                           +---------+
                                                 |
                                          +--------------------+
                                          | GPU Acceleration   |
                                          |        Flag        |
                                          +--------------------+
                                          +--------------------+
                                          | Runtime Services   |
                                          +--------------------+
```


### Spectre Facade Layer

Provides the primary embedding API: context creation, script/module loading, execution scheduling, interop bindings, profiling, and diagnostics. This layer exposes a small surface area while coordinating with the execution modes.

### Execution Modes

1. **Single Thread Mode**
   - Zero synchronization overhead, tuned for cache coherence.
   - Cooperative scheduling with host-managed ticks.
   - Focus on minimizing tail latency for small scripts.

2. **Multi Thread Mode**
   - Work-stealing scheduler over isolate shards.
   - Tiered JIT warm-up across worker pools.
   - Deterministic barriers for frame boundaries.

### GPU Acceleration Flag

- JS bytecode hot spots can be lowered into SPIR-V-like compute kernels when the flag is enabled.
- CPU manages control flow while GPU workers offload vectorizable kernels.
- Memory broker coordinates unified snapshots to keep GPU-visible buffers coherent.

Mode selection is hot-swappable at runtime via `SpectreRuntime::reconfigure` while maintaining state continuity through snapshotting and deterministic garbage collection checkpoints.

### Runtime Services

- **Parser & Frontend**: Leverages an ECMAScript 2025 compliant parser with grammar-isolated modules and incremental parsing for hot reloads.
- **Bytecode IR**: SSA-based, trace-friendly intermediate representation designed for both CPU tiered JIT and GPU lowering.
- **JIT Compiler**: Three-tier pipeline (Baseline → Optimizing → Speculative GPU). Adaptive heuristics based on telemetry.
- **Garbage Collection**: Generational GC with region-based arenas, bump-pointer fast paths, and compacting mark-sweep fallback. Per-mode write barriers tuned for cache line friendliness.
- **Memory Subsystem**: Hybrid arena + slab allocator; per-thread caches; cache-line alignment guarantees; NUMA-aware placement.
- **Concurrency Primitives**: Lock-free queues, deterministic fences, fiber scheduler integrated with host-controlled frame ticks.
- **Interop Layer**: Zero-copy host bindings through typed views, host callbacks via trampolines, and reflection metadata for game-engine entity exposure.
- **Diagnostics**: High-resolution timers, flame charts, GC event stream, GPU kernel timelines.

## Embedding API Overview

| Concept | Description |
|---------|-------------|
| `SpectreRuntimeConfig` | Declarative configuration for startup, including mode, memory budgets, GPU acceleration flag, and feature toggles. |
| `SpectreRuntime` | Owning handle to the engine instance; responsible for lifecycle, script/module management, execution. |
| `SpectreContext` | Lightweight execution contexts for isolated script environments (e.g., UI panels, gameplay systems). |
| `SpectreScript` | Represents loaded source or bytecode artifacts. |
| `SpectreHandle` | Safe reference to JS values across context or host boundaries. |
| `SpectreProfiler` | Scoped instrumentation and telemetry retrieval. |

### Example Flow

1. Host initializes `SpectreRuntime` with preferred mode and resource budgets.
2. Host registers native bindings (rendering hooks, ECS components).
3. Host loads scripts/modules and optionally precompiles to bytecode IR.
4. On each frame, host advances the runtime with `tick(frameCtx)` providing delta time and budgets.
5. Runtime schedules jobs on CPU/GPU queues respecting deadlines and publishes telemetry.

## Execution Mode Details

### Single Thread Mode

- Hot data stored in contiguous arenas to maximize prefetch efficiency.
- Monotonic frame allocator for transient objects, reset after each frame.
- Inline caches and bytecode quickening to reduce dispatch overhead.
- Predictive branch hinting using offline profile guides.
#### Baseline CPU Pipeline
- Incremental lexer emits compact `ParsedToken` arrays with literal pools to keep hot data cache resident.
- Pratt-style expression compilation lowers return statements into stack-based instruction streams with deduplicated constant pools.
- The single-thread mode owns dedicated parser, bytecode, and execution backends to avoid cross-context contention.

#### Bytecode Serialization (`SJSB`)
- Serialized programs carry the `SJSB` signature, a format version, 64-bit program version, bytecode payload, and constant tables.
- Layout stays little-endian to align with the host ABI; payload bounds are validated before playback to prevent corrupt modules from executing.

#### Execution Behavior
- A lean stack-based interpreter executes five value kinds (number, boolean, null, undefined, string) and surfaces diagnostics alongside return values.
- Numeric paths remain in double precision, detect division-by-zero, and reuse constant-pool storage for string results to minimize heap churn.


### Multi Thread Mode

- Split contexts across isolates with snapshot cloning for deterministic replication.
- Work units encoded as fibers pinned to worker threads via LIFO deques.
- GC operates with concurrent marking and stop-the-world compaction at frame boundaries.
- Cross-context messaging via lock-free ring buffers with epoch stamps.

### GPU Acceleration Details

- Hot loops annotated through profile feedback are lowered to compute kernels.
- Uses LLVM-based pipeline with NVPTX and SPIR-V backends.
- GPU memory manager stages JS typed arrays into device buffers with double buffering.
- Deterministic kernel completion deadlines enforced with hardware timers.
#### Planned Integration Path
- The baseline bytecode format intentionally describes straight-line arithmetic that can be reinterpreted as GPU kernels without rewriting host-side scheduling.
- Kernel candidates will be discovered via interpreter telemetry; stable constant pools allow zero-copy promotion into shared GPU buffers.
- Control stays on the CPU, while the GPU executes batched arithmetic/typed-array workloads behind deterministic fences aligned with frame ticks.


## Optimization Strategies

- JIT tiering with on-stack replacement and guard recovery.
- Adaptive inlining decisions guided by perf counters.
- Bytecode layout optimized for I-cache with block reordering.
- Profile-guided recompilation stored in persistent cache.
- Ahead-of-time compilation option for shipped scripts.
- ICU-less fast path for UTF-8 operations where locale neutrality allows.
- Minimal syscalls; host-provided I/O abstractions.
- Deterministic high-resolution timers using TSC calibration.
- Per-thread task graphs to minimize cross-core contention.

## Testing Strategy

- Extensive unit coverage via proprietary runner with deterministic seeds.
- Conformance harness built from Test262 subsets with nightly full runs.
- Mode-specific stress suites (latency, throughput, GPU kernel validation).
- Integration tests embedding the runtime in sample engine loop.
- Fuzzing harness for parser, JIT, GC barriers, and interop boundary.

## Deliverables Roadmap

1. **Phase 0**: Project scaffolding, configuration parser, placeholder execution modes, mock scheduler, unit test harness.
2. **Phase 1**: ECMAScript frontend integration, baseline interpreter, single-thread mode prototype.
3. **Phase 2**: Multi-thread scheduler, fiber runtime, concurrent GC.
4. **Phase 3**: GPU lowering pipeline, kernel manager, hybrid scheduler.
5. **Phase 4**: Advanced optimizations (tiered JIT, profile-guided heuristics), full compliance suite.

## Risks and Mitigations

- **ECMAScript Compliance**: Integrate existing open-source frontends where possible (e.g., Hermes, SpiderMonkey) to accelerate.
- **GPU Portability**: Abstract codegen pipeline behind adapter pattern to flex between Vulkan, Metal, DirectX.
- **Deterministic GC**: Emphasize frame-aligned checkpoints and instrumentation to detect hiccups.
- **Integration Complexity**: Provide C/C++/C# FFI layers with consistent ABI and sandboxing options.

## Next Steps

- Validate requirements with stakeholders (latency targets, platform list).
- Finalize API headers and public surface.
- Build minimal runnable skeleton to exercise embedding API with mock workloads.
- Establish CI pipelines for tests and conformance harness.

## Coding Standards

See `docs/coding_conventions.md` for naming, error handling, and object model rules that all runtime and embedding code must follow.










