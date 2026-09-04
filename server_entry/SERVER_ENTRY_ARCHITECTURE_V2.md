# Server Entry Architecture v2

Status: Implementation Draft

## 1. Core model

The Server uses one structural rule throughout canonical state:

```text
Owning Namespace -> ID -> Entry
```

An ID is meaningful only inside exactly one owning namespace. An Entry does not
store its own ID when the ID is already implied by the Entry's position in that
namespace.

## 2. Build generations

```text
Build/Rebuild -> G0
G0 -> G1     incremental project change
G1 -> G2     incremental project change
...
Gn -> Gn+1   incremental project change

Rebuild -> G0
```

Graph generation is not canonical identity. A rebuild resets the generation
number to G0; surviving canonical identity remains governed by the Project
Canonical Entity namespace.

## 3. Identifier namespaces

```text
source_id          Project Source Universe
stable_id          Project Canonical Entity Universe
string_id          Project String Registry
type_handle        Graph Type Slot namespace
TypeRef            Graph Canonical Type table
member_index       one Aggregate definition
enumerator_index   one Enum definition
declaration_index  one Source parse/build result
```

### stable_id

`stable_id` is a persistent canonical Entity slot. Numeric values are never
reused for another canonical Entity. Existing canonical keys reuse their old
stable_id. An Entity Entry is addressed directly by stable_id where practical.

Canonical type identity is derived from source-language canonical identity, not
from source file provenance.

### source_id

`source_id` owns build-side/source-side state. Worker/thread identity is never a
semantic owner and never participates in canonical identity.

## 4. Entry zero-state

Each Entry type has exactly one zero/default-state contract. Derived state is not
stored separately.

Examples:

```text
EntityEntry{}       no live Entity in this stable_id slot
                     (liveness is derived from name != 0)
DefinitionRange{}   no canonical definition
```

A definition range uses a one-based arena position. `begin == 0` means no
definition; a defined empty aggregate/enum is represented by a non-zero begin and
`count == 0`.

There is no independent committed `declared/defined` status when definition
presence already determines it.

## 5. Hot/cold separation

Runtime-hot canonical Entries contain only data needed for canonical/runtime
queries. Source provenance, source ranges, diagnostic mappings and construction
state do not live in hot Entity Entries.

```text
HOT G
  EntityEntry
  TypeEntry
  canonical TypeRefs
  members/enumerators
  ABI/layout

COLD / BUILD
  source provenance
  SourceContribution[source_id]
  pending/conflict state
  diagnostics
  scheduling state
```

## 6. Parser / Builder boundary

Parser owns source-language meaning. A' is transient and source_context-owned.
Builder receives only source-language-resolved facts.

```text
Source -> Lexer -> Parser -> A' -> publish(A') -> canonical construction
```

After publish returns, Builder retains no source_context-owned memory.

## 7. SourceContribution

Incremental build state is keyed by source_id, never by worker id:

```text
SourceContribution[source_id]
```

It is a Builder/build-cache artifact, not authoritative G. It may be discarded
and reconstructed. Old/new SourceContribution delta is the basis for Gn -> Gn+1.

## 8. G0 vs incremental algorithms

G0 uses bulk construction:

```text
all affected Sources in parallel
-> bulk canonical key preparation
-> bulk reserve / unique where useful
-> build G0
```

Gn -> Gn+1 uses sparse delta construction:

```text
changed source_id set
-> old/new SourceContribution delta
-> affected canonical closure
-> sparse candidate overlay on Gn
-> Gn+1
```

A full-project sort/rebuild is not used for ordinary incremental changes.

## 9. Concurrency

Workers execute immutable/local work:

```text
filesystem I/O
hashing
lexing
parsing
canonical-key preparation
layout work that has no shared mutation
```

Source Manager uses an explicit single-owner acquisition boundary:

```text
coordinator: prepare_acquire(source_id)
             -> immutable source_acquire_job
worker:      execute_acquire(job)
             -> owned source_acquire_result
coordinator: apply_acquire(job, result)
```

Discovery currently runs in deterministic waves. All jobs in one wave execute
filesystem/hash work in parallel; only after workers join does the coordinator
apply Source state, resolve include identities, and mutate the dependency DAG.
A later completion-queue scheduler may remove the wave barrier without changing
this ownership contract.

Canonical domain objects are not designed as generally thread-safe mutable
containers. Worker identity does not determine Source, Entity, String or Type
identity.

Scheduler implementation may use mutex/condition_variable internally. Domain
mutexes are not part of semantic ownership.

## 10. Canonical identity

Source provenance does not define canonical type identity.

```text
a.hpp declaration --\
b.hpp declaration ----> canonical N::A -> stable_id
c.hpp definition  --/
```

Definition/source provenance is separate cold/build information.

Stable IDs are assigned only for new canonical keys. Existing keys reuse the
historical stable_id. New-key ordering must not depend on worker completion.

## 11. Graph storage

Preferred direct relationships:

```text
stable_id   -> EntityEntry
string_id   -> StringEntry
type_handle -> TypeEntry
TypeRef     -> CanonicalTypeEntry
```

No extra Entity handle/indirection is introduced without measured need.

If historical stable_id space later becomes too sparse, physical Entity storage
may become paged without changing stable_id semantics.

## 12. Type definition representation

TypeEntry stores canonical definition presence through a definition range, not a
separate status enum.

Conceptually:

```text
TypeEntry
  kind
  enum declaration semantics when applicable
  DefinitionRange definition
```

For aggregate types the definition range indexes MemberEntry arena. For enum
types it indexes EnumeratorEntry arena.

```text
definition.begin == 0  incomplete/opaque declaration
definition.begin != 0  canonical definition exists
```

An empty definition is valid because arena positions are stored one-based.

Candidate construction data is not part of TypeEntry. Pending member specs,
ordered pending modifiers, unpublished definition payloads, and anonymous/source
provenance live only in transaction-local `TypeBuildState` or build-side
`SourceContribution[source_id]` state. Publication materializes only the final
DefinitionRange and canonical enum metadata into committed TypeEntry.

## 13. String Registry

`string_id` is a one-based Project String Registry slot coordinate and is resolved
by direct indexing. Value -> string_id remains a hash lookup boundary. Numeric
string_id is storage identity, not semantic Entity identity.

Incremental `Gn -> Gn+1` publication is append-only: existing string slots never
move and new strings append new IDs. Explicit Rebuild/G0 may reclaim physical
bytes for strings no longer referenced by current G, but it never renumbers or
reuses numeric slots. Reclaimed slots become tombstones.

Historical canonical Entity-name strings are retained whenever
`identity[string_id] != 0`, including currently dead Entities. This preserves
canonical-name matching and stable_id resurrection across later incremental
builds. Current member/enumerator names are also retained. Other unused string
bytes may be reclaimed at G0.

## 14. TypeRef

Existing canonical TypeRef indices are preserved during incremental updates.
Only new canonical structural types are appended/deduplicated during
`Gn -> Gn+1`.

`TypeRef` is generation-local rather than persistent Project identity. Therefore
an explicit Rebuild/G0 constructs a fresh compact canonical TypeRef table:
fixed builtin prefix, one named TypeRef for each live type_handle, and only
derived TypeRefs reachable from current canonical member definitions. All live
member TypeRefs are remapped before publication. No TypeRef renumbering occurs
during ordinary incremental construction.

## 15. Publication

Committed Gn is immutable while candidate delta is built. Publication is
fail-closed:

```text
Gn + sparse candidate delta
-> resolve canonical pending refs
-> validate
-> prepare
-> publish
-> Gn+1
```

For Rebuild, the result is G0.

## 16. Persistence

Compiled persistence stores canonical G, String Registry, ABI and exact TypeRef
indices for the persisted generation. In artifact format 1.6, String Registry
records preserve numeric string_id slots and encode reclaimed slots as tombstones;
Entity liveness remains derived from persisted `name != 0`, with no redundant
Entity live flag. Parser state, scheduler state and SourceContribution build cache
are not part of authoritative compiled G.

Source checkpoint and compiled checkpoint remain distinct.

## 17. Frozen invariants proposed by v2

1. Parser resolves source-language meaning.
2. Builder resolves canonical identity.
3. A' is transient and source_context-owned.
4. Worker/thread identity is never semantic ownership.
5. Every ID belongs to exactly one owning namespace.
6. Entry position implies its ID; Entries do not redundantly store that ID.
7. Source provenance never defines stable_id.
8. stable_id values are never reused for another canonical Entity.
9. Committed construction status is not duplicated when it can be derived.
10. Build/Rebuild publishes G0; project deltas publish Gn+1.
11. G0 uses bulk algorithms; Gn -> Gn+1 uses sparse delta algorithms.
12. Build cache is keyed by source_id and is not authoritative G.
13. Runtime consumes committed G only.
14. Existing TypeRef indices are preserved during incremental construction.
15. Scheduling may affect performance, never semantics or canonical identity.
16. A committed/persisted Entity has exactly one liveness fact: `name != 0`.
17. Rebuild/G0 may compact generation-local TypeRef indices; Gn -> Gn+1 may not.
18. string_id slots are never renumbered or reused; G0 may reclaim only their bytes.
19. Historical canonical Entity-name string slots are retained for stable_id resurrection.

## 18. SourceContribution build cache ownership

`SourceContribution[source_id]` is build-side incremental cache, not canonical G.
It is owned by Graph Manager beside Source Manager, String Registry and Graph, but
is never Runtime-visible and is excluded from compiled persistence.

```text
source_id -> SourceContribution
```

An incremental transaction replaces only changed Source contribution entries.
Graph consumes the previous contribution to remove that Source's old canonical
delta and fills the candidate contribution with the Source's new canonical delta.

A full Build/Rebuild reconstructs both G0 and the complete contribution cache.
Contribution entries for Sources not observed by the rebuild are cleared.

Compiled or source-only checkpoint load invalidates contribution provenance. The
next Source build is therefore forced to the Rebuild/G0 path before incremental
construction may resume.

This cache is non-authoritative: losing it requires reconstruction, never changes
the meaning of an already committed G.

## Build-side canonical construction aggregation

Declaration/definition counters, definition-source selection, enum underlying-type consistency counters, and definition payload ownership are **not part of committed G**. They belong to the non-authoritative build cache.

For incremental `Gn -> Gn+1`, `SourceContribution[source_id]` and a build-side Entity construction state provide the minimum history required to subtract one Source contribution and add its replacement. For `G0` Build/Rebuild, construction state starts empty and is rebuilt from the complete current Source traversal.

`Graph` contains only the canonical materialized result. Compiled checkpoints therefore do not serialize declaration aggregation state.

## Definition Arena Lifetime Contract

Committed aggregate members and enum enumerators are stored in dense one-based
DefinitionRange arenas.

- `Gn -> Gn+1` incremental updates are append-only for definition payloads. Old
  slices are not rewritten or remapped, preserving O(delta) publication and O(1)
  DefinitionRange access.
- Explicit Build/Rebuild creates `G0` with fresh compact member and enumerator
  arenas. Every surviving definition is materialized into the fresh arenas and
  the arenas are swapped at commit, reclaiming all obsolete slices accumulated
  by earlier incremental generations.
- Arena compaction is therefore a rebuild concern, never a normal incremental
  mutation. Incremental publication must not globally remap unchanged TypeEntry
  DefinitionRanges merely to reclaim storage.
- Defined-empty aggregates/enums remain representable because DefinitionRange is
  one-based: `begin != 0 && count == 0` is a valid empty definition.

## Canonical TypeRef and String Lifetime Contract

Two superficially similar append-only namespaces have intentionally different
rebuild semantics:

```text
TypeRef
  owner: one Graph generation
  Gn -> Gn+1: preserve + append
  Rebuild -> G0: compact/reindex allowed

string_id
  owner: Project String Registry
  Gn -> Gn+1: preserve + append
  Rebuild -> G0: numeric slot preserved; unused bytes may be reclaimed
```

The difference follows identity lifetime rather than storage convenience. A
TypeRef never identifies an object across a new G0 and can therefore be rebuilt
from current reachability. A string_id may participate in the persistent
canonical-name -> stable_id reservation, so renumbering it would break historical
identity. String reclamation therefore uses tombstone slots rather than remapping.

Compiled artifact 1.6 preserves both contracts exactly: canonical TypeRef indices
are exact for the saved generation, while String Registry tombstones preserve
slot numbering without retaining unused string bytes.

## Rebuild/G0 bulk materialization contract

`Rebuild -> G0` and `Gn -> Gn+1` share canonical semantics but use different physical
construction algorithms.

### Rebuild/G0

Rebuild constructs a detached current Graph from the complete current Source universe.
The previous committed Graph contributes only the persistent Project canonical identity
mapping needed to preserve `stable_id`. Previous Entity payload, type slots, definition
arenas, and generation-local TypeRefs are not current-state inputs.

The G0 materializer MUST:

1. allocate a fresh generation-local `type_handle` namespace beginning at 1;
2. ignore prior generation named/derived TypeRef indexes during current construction;
3. build complete detached Entity, Type, definition-arena, and canonical-TypeRef storage;
4. validate the detached candidate completely;
5. publish by swapping complete storage into Graph;
6. set the Graph generation to G0;
7. preserve Project canonical-name -> `stable_id` reservations and never reuse a
   historical `stable_id` for a different Entity.

### Incremental Gn -> Gn+1

Incremental construction MUST remain sparse. It applies only changed SourceContribution
state and affected canonical closure, preserves existing physical slots/TypeRefs where
required, appends incremental definition/TypeRef payload, validates the touched result,
and publishes only the sparse delta.

The two paths MUST NOT diverge semantically: for the same complete Project Sources,
Rebuild/G0 and the equivalent sequence of valid incremental changes must represent the
same canonical program, modulo generation-local physical coordinates such as
`type_handle` and `TypeRef`.
