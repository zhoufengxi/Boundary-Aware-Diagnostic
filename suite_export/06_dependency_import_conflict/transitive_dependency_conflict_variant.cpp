#include "worker.h"
#include "transitive_dependency_a.h"

void runTransitiveDependencyConflictVariant(int branch) {
  int *p = new int(42);
  Worker worker;
  worker.releaseTransitive(p, branch);
}
