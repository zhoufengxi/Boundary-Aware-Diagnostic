#include "manager.h"
void Manager::run(Worker &w) {
  int *p = new int(42);
  w.releaseOnce(p);
  delete p;
}
