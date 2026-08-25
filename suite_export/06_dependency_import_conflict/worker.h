#pragma once

class Worker {
public:
  void releaseOnce(int *p, int branch);
  void releaseTransitive(int *p, int branch);
  void releaseDependencyOrderBefore(int *p, int branch);
  void releaseDependencyOrderAfter(int *p, int branch);
  void releaseNested(int *p, int branch);
  void releaseUserConflict(int *p, int branch);
};
