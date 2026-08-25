#include "order_support.h"
#include "order_dep_a.h"
#include "worker.h"

void runDependencyOrderBeforeVariant(int branch) {
  int *p = new int(42);
  Worker worker;
  worker.releaseDependencyOrderBefore(p, branch);
}
