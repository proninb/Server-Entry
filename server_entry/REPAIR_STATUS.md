# Server Entry Architecture — Repair Status

## Architecture preserved

- Source Manager owns Source identity, filesystem state, dependency graph and acquisition.
- Parser owns source-language lookup/visibility and emits transient A′.
- Source Publisher canonicalizes names into String Registry ids only.
- Builder/Graph own stable identity, TypeRef construction, ABI semantics and canonical invariants.
- Graph is the only authoritative compiled state G.
- Runtime eligibility remains fail-closed through Graph Manager state.
- Host OS and target ABI remain separate concepts.

## Correctness repairs

- Compiled artifact canonical TypeRef record is lossless and format minor is 1.2.
- Canonical TypeRef indices are preserved exactly across checkpoint round-trip.
- Imported Graph is constructed/validated detached before live swap.
- Both windows_x64 and posix_x64 targets are accepted through centralized ABI validation.
- ABI pack is restricted to 1/2/4/8/16.
- Parser resolves user type names before Builder and preserves ordered TypeRef modifiers.
- Include visibility is positional and transitive.
- Compatible redeclarations coalesce in exported Parser interfaces.
- Canonical pending member bindings resolve at Graph prepare barrier.
- New stable_id assignment is deterministic and independent of worker publication order.
- First Source build after compiled-only load performs controlled full reconstruction.
- Live aggregate member TypeRefs are rejected if their named base becomes dead.
- Compiled import rejects live members whose TypeRef resolves to a dead type slot.
- Malformed non-integral enum constants fail closed without >64-bit shift UB.

## Concurrency/performance repairs

- String Registry and Graph candidate mutation use short internal mutation locks.
- Semantic Parser/Builder work runs through a worker pool.
- Diagnostics are worker-local and merged deterministically.
- Global full-publication mutex is removed from semantic frontend work.
- Source acquisition/discovery uses a dynamic worker pool.
- Source Manager holds its mutation lock only for candidate metadata; file open/read/SHA-256 run outside it.
- Concurrent acquisition of the same source_id is rejected through a dense claim table.
- Source acquisition telemetry is worker-local and merged after join.
- Source net-change bookkeeping uses dense source_id -> position indexing, avoiding O(n²) scans.

## Validation performed

Functional regression PASS:
- ABI validation
- Parser lookup/modifier contract
- deterministic stable_id
- canonical pending binding
- include position/transitive visibility
- redeclaration interface coalescing
- compiled checkpoint -> Source reconstruction
- parallel independent-root rebuild
- parallel fan-out include discovery
- malformed enum width rejection
- live dangling member rejection
- compiled dangling member rejection

Sanitizer PASS:
- ThreadSanitizer: parallel semantic rebuild
- ThreadSanitizer: fan-out include discovery/acquisition

Compiler audit PASS on changed critical implementation files:
- GCC C++20: -Wall -Wextra -Wpedantic
- Clang C++20: -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef

The remaining -Wswitch-enum warnings under an optional stricter Clang pass come from intentional catch-all/default handling in generic builtin/metric helpers, not from the repaired control paths.
