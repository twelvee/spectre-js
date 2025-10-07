# ES2025 Module Scaffold

The es2025/ runtime layer introduces a modular layout for implementing the full ECMAScript 2025 surface while preserving real-time determinism.

## Entry Points

- spectre::es2025::Module &mdash; common interface for all standard library modules. Each module overrides:
  - Initialize for installation into the runtime realm.
  - Tick for deterministic, host-driven progress (async jobs are advanced only when the host calls SpectreRuntime::Tick).
  - OptimizeGpu for optional GPU acceleration bindings.
  - Reconfigure to react to runtime config changes.
- spectre::es2025::Environment &mdash; owns all ES2025 modules, wires them into the runtime during SpectreRuntime::Create, and relays tick/GPU/update events.

## Module Layout

Each built-in now lives in include/spectre/es2025/modules/<name>_module.h with a matching implementation in es2025/modules/<name>_module.cpp. 37 stubs are pre-registered, covering Global, Array, Temporal, Intl, and every other ES2025 feature area. A module exposes metadata via Name(), Summary(), and SpecificationReference() to keep the structure approachable for JavaScript developers.

The environment registers modules in spec order, builds an index for lookup, and keeps a local copy of RuntimeConfig so GPU toggles and memory budgets can be re-applied without rebuilding the runtime.

## Usage Notes

- All async semantics (Promises, async iterators, Temporal throttling, etc.) are advanced exclusively through SpectreRuntime::Tick, keeping frame boundaries deterministic for real-time hosts.
- GPU acceleration is opt-in per module; call OptimizeGpu with unified context data when RuntimeConfig::enableGpuAcceleration is true.
- New modules can be added by creating another <name>_module pair and calling Register inside Environment.

Refer to the stub bodies in es2025/modules/*.cpp for TODO markers that indicate where the concrete implementations should land.

### Global Module
- GlobalModule now provisions the default global.main context during initialization.
- Hosts can call EvaluateScript to compile and execute inline source against a target context while reusing Spectre caching paths.
- EnsureContext allows deterministic creation of additional global realms with tuned stack budgets.
- GPU toggles propagate through OptimizeGpu, enabling module-specific accelerators once implemented.

### Error Module
- Registers ECMAScript standard error constructors (Error, TypeError, SyntaxError, etc.) and allows hosts to register custom error types.
- RaiseError produces formatted diagnostics, records call-site metadata, and exposes history inspection/clearing APIs for tooling.
- Tick integration stamps each error with the latest frame index so realtime systems can correlate faults with frame budgets.
### Function Module
- Provides host-facing registry for callable intrinsics with per-frame statistics and deterministic invocation.
- Hosts can register callbacks via RegisterHostFunction, invoke them synchronously, and inspect call counts/latency through GetStats.
- GPU toggles propagate for future acceleration paths, mirroring other ES2025 modules.
