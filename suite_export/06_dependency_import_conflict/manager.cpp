#include "manager.h"
#include "dep_a.h"

void Manager::run(Worker w, int branch) {
  int *p = new int(42);
  w.releaseOnce(p, branch);
}
