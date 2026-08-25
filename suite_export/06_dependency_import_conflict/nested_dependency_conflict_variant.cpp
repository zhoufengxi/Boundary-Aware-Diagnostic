#include "worker.h"
#include "nested_dep_a.h"

void runNestedDependencyConflictVariant(int branch) {
  int *p = new int(42);
  Worker worker;
  worker.releaseNested(p, branch);
}
