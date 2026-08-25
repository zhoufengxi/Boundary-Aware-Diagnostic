#include "order_dep_a.h"
#include "order_support.h"
#include "worker.h"

void runDependencyOrderAfterVariant(int branch) {
  int *p = new int(42);
  Worker worker;
  worker.releaseDependencyOrderAfter(p, branch);
}
