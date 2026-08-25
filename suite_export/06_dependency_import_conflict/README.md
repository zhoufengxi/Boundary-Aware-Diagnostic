# Dependency-import conflict tests

These CTU tests keep the memory-error relation fixed while varying only the
dependency declaration that must be imported with an external `Worker` body.
Every positive driver allocates an `int` and calls a method defined in
`worker.cpp`. The imported method reaches one of three annotated outcomes:
double free, use after free, or leak.

| Driver | Controlled dependency variable | Conflict policy |
|---|---|---|
| `manager.cpp` | Direct `dep_a.h` versus `dep_b.h` record-layout conflict | System-header conflict; positive core case |
| `transitive_dependency_conflict_variant.cpp` | Caller obtains the A declaration through a wrapper header | System-header conflict; positive variant |
| `dependency_order_before_variant.cpp` | The shared namespace is introduced before the A conflict declaration | System-header conflict; positive variant |
| `dependency_order_after_variant.cpp` | The shared namespace is introduced after the A conflict declaration | System-header conflict; positive variant |
| `nested_dependency_conflict_variant.cpp` | The conflicting record is in a nested declaration context | System-header conflict; positive variant |
| `user_dependency_conflict_negative_variant.cpp` | The same record-layout conflict comes from user headers | User-code conflict; conservative negative variant |

For each system-header case, the baseline is expected to stop importing the
external method at the dependency `NameConflict`. The modified analyzer is
expected to retain the user-level method body and report all three annotated
defects with both allocation and branch witnesses.

The user-header negative case must remain unresolved by both analyzers: relaxing
a system dependency conflict must not permit an incompatible user declaration.
Reports produced while analyzing `worker.cpp` as a standalone translation unit
are auxiliary because that file contains no suite allocation site.
