#include "release_util.h"
void releaseThroughTemplate(int *p, int branch) {
  releaseByBranch<int *>(p, branch);
}
