# Server Entry Architecture v2 — implementation status

Status: storage invariant and Source Manager single-owner migration implemented and regression-tested.

## Implemented in this pass

- `stable_id -> EntityEntry`: EntityEntry no longer stores its own stable_id.
- Hot EntityEntry contains no `source_id defining_source` provenance.
- Public Graph terminology is `entity_entry` / `type_entry`; old record aliases were removed.
- Canonical definition presence uses one-based `definition_range`.
- `begin == 0` is the only no-definition state.
- Defined-empty aggregate/enum uses `begin != 0 && count == 0`.
- Committed TypeEntry contains no aggregate/enum declared/defined status enum.
- Member and enum definition arenas are addressed with exact one-based ranges.
- Compiled Entity projection does not duplicate stable_id or liveness. Entity liveness is `name != 0`.
- Compiled artifact format minor version is 1.6.
- Compiled import validates zero/non-zero range semantics and arena bounds fail-closed.
- Tests were migrated away from `EntityEntry::id`; ID lookup is through the owning namespace (`graph::find_id`).
- The old test that concurrently mutated Graph was replaced by a single-owner pending-binding test.
- v2 tree was made link-complete by restoring diagnostics, metrics, hash, file snapshot and Source Manager persistence implementations.

## Verification

Passed with GCC C++20, `-Wall -Wextra -Wpedantic`:

- abi_validation_test
- compiled_dangling_member_test
- compiled_then_source_rebuild_test
- dangling_member_guard_test
- enum_malformed_width_test
- graph_repair_test
- include_visibility_test
- parallel_discovery_include_test
- parallel_rebuild_test
- parser_repair_test
- pending_parallel_test
- sm_change_test
- v2_entry_invariants_test

Strict Clang compile passed for `graph.cpp`, `compiled_persistence.cpp`, and `source_manager.cpp` with:

`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`

## Source Manager ownership migration

- `source_manager_update` no longer contains `mutation_mutex` or `acquisition_claimed`.
- Candidate Source state is coordinator-owned and is not a generally thread-safe mutable container.
- `prepare_acquire(source_id)` captures immutable Source-local input.
- `execute_acquire(job)` performs only file snapshot/read/SHA-256 work and can run on any worker.
- `apply_acquire(job, result)` is coordinator-owned and mutates only candidate Source state.
- Frontend discovery uses deterministic acquisition waves: prepare serially, execute in parallel, apply/discover serially by source_id.
- Existing `acquire()` remains only as a synchronous coordinator convenience wrapper.

## Important boundary

`aggregate_definition_state` / `enum_definition_state` still exist in Parser/Builder transient source semantics. This is intentional. They are no longer independent committed Graph state.

The frontend scheduler still uses its own mutex/condition variables for queue/state orchestration. This is scheduler synchronization, not Source Manager/String/Graph domain ownership.

## Verification after Source Manager migration

Full GCC regression PASS:

- abi_validation_test
- compiled_dangling_member_test
- compiled_then_source_rebuild_test
- dangling_member_guard_test
- enum_malformed_width_test
- graph_repair_test
- include_visibility_test
- parallel_discovery_include_test
- parallel_rebuild_test
- parser_repair_test
- pending_parallel_test
- sm_change_test
- v2_entry_invariants_test

ThreadSanitizer after Source Manager migration PASS:

- parallel_discovery_include_test (fanout=32)
- parallel_rebuild_test

Strict Clang compile PASS for `source_manager.cpp`, `source_frontend_generation.cpp`, `string_registry.cpp`, and `graph_build_transaction.cpp` with:

`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`

## SourceContribution extraction from Graph

Implemented:

- Removed `Graph::source_states`, `candidate_sources`, and `source_provenance_complete`.
- Added build-side `source_contribution_cache`, keyed directly by `source_id`.
- `graph_manager` owns the cache independently of canonical G.
- `graph_build_transaction` owns a sparse `source_contribution_cache_update` candidate.
- `graph_update` reads old Source contributions and writes new candidate contributions
  only through that build-cache update; Graph itself no longer retains Source provenance.
- Full Rebuild/G0 clears stale contribution entries for Sources not observed in the
  current traversal.
- Compiled-checkpoint and source-only checkpoint load invalidate the build cache.
  `graph_manager::begin_build()` automatically upgrades the next Source build to
  `graph_build_mode::rebuild` until contribution provenance has been reconstructed.
- Source contribution schema was moved out of `graph.hpp` into the Builder cache header;
  Graph exposes only forward declarations needed by its construction API.
- Compiled G format is unchanged by this extraction because the contribution cache was
  never part of compiled persistence.

Verification after extraction:

- Full GCC C++20 regression PASS for all existing v2 tests.
- Added `source_contribution_cache_test`: PASS.
- `compiled_then_source_rebuild_test`: PASS, confirming compiled-only load forces safe G0 reconstruction.
- ThreadSanitizer PASS:
  - `parallel_discovery_include_test` (fanout=32)
  - `parallel_rebuild_test`
- Strict Clang compile PASS for:
  - `source_contribution_cache.cpp`
  - `graph.cpp`
  - `graph_build_transaction.cpp`
  - `graph_manager.cpp`

Strict flags:

`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`

## Canonical construction aggregation moved out of G

Implemented:
- Removed `Graph::enum_aggregates` and candidate Entity aggregation payloads from canonical Graph storage.
- Added build-side `canonical_entity_construction_state` inside `SourceContribution` cache.
- Incremental add/remove materialization now updates this non-authoritative build cache, while `G` retains only canonical Entity/Type/TypeRef state.
- Full `G0` reconstruction starts construction aggregation from empty state and ignores historical per-source deltas; old Graph entities are retired by the rebuild barrier instead.
- New stable-ID canonicalization remaps build-side Entity construction states in one batch, preventing state loss when provisional IDs permute.
- Compiled load remains independent of construction aggregation; invalidated contribution cache forces the next Source build to reconstruct `G0`.

Validation:
- Full regression suite: PASS.
- `dangling_member_guard_test`: PASS.
- `compiled_then_source_rebuild_test`: PASS.
- parallel semantic/discovery tests: PASS.
- strict Clang warnings on changed files: PASS.


## TypeEntry / TypeBuildState separation

Implemented:
- Committed `type_storage` now contains only `type_entry record`.
- Added transaction-local `type_build_state` for pending members, pending modifiers,
  resolved member payload, enum value payload, and `definition_pending`.
- Candidate type slots own `TypeBuildState`; publication consumes it into one-based
  canonical definition arenas and destroys it before the type becomes committed G.
- Anonymous/source provenance was removed from committed Type storage and from compiled
  Type DTOs. Source ownership of anonymous types remains only in build-side
  `SourceContribution[source_id]`.
- Compiled Type record shrank from 24 bytes to 16 bytes.
- Compiled artifact minor version advanced to 1.5 during TypeEntry separation; current format is 1.6.
- `v2_entry_invariants_test` now compile-time checks that `type_entry` has no pending
  build fields and `compiled_type_slot` has no anonymous/source provenance.

Validation after separation:
- Full GCC C++20 regression suite: PASS.
- ThreadSanitizer PASS:
  - `parallel_discovery_include_test` (fanout=32)
  - `parallel_rebuild_test`
- Strict Clang compile PASS for `graph.cpp`, `compiled_persistence.cpp`, and
  `source_contribution_cache.cpp` with:
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`

## Definition arena reclamation

Implemented generation-aware definition arena lifetime:

- incremental `Gn -> Gn+1`: append-only member/enumerator slices;
- explicit Rebuild -> `G0`: build fresh compact arenas in type-handle order,
  assign fresh one-based DefinitionRanges, and swap arenas at commit;
- obsolete incremental slices are reclaimed without changing sparse incremental
  update semantics or introducing a DefinitionRange indirection layer;
- added `definition_arena_rebuild_test` covering aggregate and enum arena growth
  across incremental generations followed by exact G0 compaction;
- full functional regression PASS;
- strict Clang warning pass on graph.cpp PASS;
- TSAN parallel discovery and parallel semantic rebuild PASS.


## TypeRef G0 compaction and String Registry reclamation

Implemented:

- Incremental `Gn -> Gn+1` TypeRef construction remains append-only and preserves
  every existing TypeRef index.
- Explicit Rebuild/G0 creates a fresh canonical TypeRef table with the fixed
  builtin prefix, exactly one named ref for every live type slot, and only
  currently reachable derived chains.
- Current aggregate member TypeRefs are remapped into the rebuilt G0 table before
  publication; persistent stable_id values are unaffected.
- String Registry numeric slots are never renumbered or reused.
- G0 computes a retained string set from historical canonical identity mappings
  plus current member/enumerator definitions. Unretained physical string bytes are
  reclaimed and their numeric slots become tombstones.
- Historical Entity-name strings (`identity[string_id] != 0`) are always retained,
  including dead Entities, preserving future stable_id resurrection.
- Compiled artifact format advanced to 1.6. String records use a tombstone marker
  while preserving exact string_id slot positions; load validation rejects Graph
  references to tombstoned strings.
- Added `type_ref_rebuild_test` and `string_registry_rebuild_test`.

Validation:

- Full GCC C++20 regression suite: PASS, including both new reclamation tests.
- Strict Clang compile PASS for `graph.cpp`, `string_registry.cpp`,
  `compiled_persistence.cpp`, and `graph_build_transaction.cpp` with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`.
- ThreadSanitizer PASS for parallel discovery and parallel semantic rebuild.

## Detached bulk Rebuild/G0 path

Implemented a physically separate storage algorithm for explicit Rebuild/G0 while
preserving the same canonical semantic rules used by incremental construction.

Rebuild/G0 now:

- treats the previous committed Graph only as persistent canonical-name -> stable_id
  identity history; old Entity payload, type slots, definition slices, and TypeRefs
  are not current G0 input;
- starts a fresh dense generation-local type_handle namespace at 1;
- never reuses previous generation named/derived TypeRefs during construction;
- materializes detached `rebuilt_identity`, `rebuilt_entities`, `rebuilt_types`,
  compact definition arenas, and rebuilt canonical TypeRef indexes;
- does not resize or mutate committed Entity/Type/arena storage during prepare;
- publishes G0 by swapping the complete detached storage into Graph;
- resets Graph generation to G0 while preserving Project stable_id reservations.

Incremental `Gn -> Gn+1` remains the sparse overlay path and continues to preserve
existing generation-local handles/TypeRefs where required by the incremental contract.

Added `bulk_rebuild_test` covering:

- G0 -> G1 removal -> G2 addition;
- stable_id preservation across explicit Rebuild;
- stale Entity removal;
- fresh dense type_handle allocation after Rebuild independent of incremental slot history.

Validation:

- Full functional regression suite: PASS, including `bulk_rebuild_test`.
- Strict Clang compile of changed `graph.cpp` with
  `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef`: PASS.
- GCC ThreadSanitizer PASS:
  - `parallel_discovery_include_test` (fanout=32)
  - `parallel_rebuild_test`
- Clang TSAN linking is unavailable in this container because its Swift Clang runtime
  requires libdispatch/Blocks symbols; GCC TSAN was used for sanitizer validation.
