#include "worker.h"
#include "user_dep_a.h"

void runUserDependencyConflictNegativeVariant(int branch) {
  int *p = new int(42);
  Worker worker;
  worker.releaseUserConflict(p, branch);
}
