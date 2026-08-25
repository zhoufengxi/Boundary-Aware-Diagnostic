#include "release_util.h"

template <class T>
void outerRelease(T p, int branch) {
  releaseByBranch<T>(p, branch);
}

void releaseOnceThroughOuter(int *p, int branch) {
  outerRelease<int *>(p, branch);
}
