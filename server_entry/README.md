# RC-V2-01A Full Source

This directory contains full replacement source files, not patches.

## Replace-ready production files

- `project/graph/graph.hpp`
- `project/graph/graph.cpp`
- `project/graph/graph_build_transaction.hpp`
- `project/graph/graph_build_transaction.cpp`
- `project/graph/graph_manager.hpp`
- `project/graph/graph_manager.cpp`

## RC/benchmark-only files

- `project/graph/graph_build_transaction_test_access.hpp`
- `tests/rc_v2_01a_gates.hpp`

Compile RC/benchmark targets with:

```text
CW_GRAPH_BUILD_TRANSACTION_TESTING
```

Production builds do not contain storage telemetry, snapshots, semantic hashing,
or failure injection flags.

## Included RC-V2-01A behavior

- Sparse geometric headroom for direct-index Graph vectors.
- No paging/indirection change.
- Existing stable_id and TypeRef semantics remain unchanged.
- Per-container storage prepare telemetry.
- Logical-work vs physical-relocation counters.
- Post-Graph-prepare forced failure injection.
- Rollback of direct-index vector logical sizes on failed prepare while allowing
  non-semantic capacity growth to remain.
- Test-only semantic Graph hash excluding capacity/bucket counts.
- G0 headroom, isolated-delta, no-relocation, pointer-stability and fail-closed gates.
- CSV telemetry append helpers.

## Publication invariant

`graph_build_transaction::prepare()` may validate, reserve, grow candidate-visible
storage and fail. `publish_prepared()` performs the fixed Source -> String -> Graph
publication order and contains no allocation/validation step added by RC-V2-01A.

## Benchmark source

The File Library contains the RC-V2-01 result CSVs but did not surface the concrete
benchmark `.cpp` fixture. Therefore this bundle does not invent/replace that file.
`tests/rc_v2_01a_gates.hpp` is complete integration support for the existing harness.
