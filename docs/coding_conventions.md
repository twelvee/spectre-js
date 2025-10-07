# Spectre-JS Coding Conventions

## Naming

- Methods and free functions use PascalCase (e.g., `CreateContext`, `LoadScript`).
- Types use PascalCase (e.g., `RuntimeConfig`, `SpectreRuntime`).
- Member variables use the `m_` prefix followed by PascalCase (e.g., `m_Config`, `m_Scripts`).
- Function parameters and local variables use lower camelCase (e.g., `contextName`, `frameIndex`).
- Compile-time constants use all caps with underscores (e.g., `MAX_SNAPSHOTS`).

## Error Handling

- Exceptions are prohibited. All fallible operations return explicit status codes or result structs containing status codes.
- Status enums should be strongly typed `enum class` values.
- Functions that return status should never throw on failure; they must propagate errors via the return value.

## Object Model

- Prefer composition over inheritance. Use lightweight structs and final classes.
- Runtime services should expose handles or interfaces rather than base-class hierarchies.

## Threading and Concurrency

- Thread-safe components must document ownership and synchronization explicitly.
- Avoid implicit global state; pass dependencies via constructors or configuration structs.

## Memory Management

- Favor RAII wrappers with explicit ownership annotations.
- Align custom allocators with cache-friendly layouts; document alignment guarantees.

## API Surface

- Public APIs must be deterministic and portable; no platform-specific headers in public includes.
- Embedding API should remain header-only declarations with opaque implementation details.

## Testing

- Every public method or configuration path requires unit coverage.
- Tests must assert status codes and avoid relying on exceptions.
