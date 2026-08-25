#include <string>

template <class T>
void releaseThroughLambda(T p, int branch) {
  std::string label = "template";
  auto cb = [label, p, branch]() {
    if (branch == 0) {
      delete p;
      delete p; // expected double free
    } else if (branch == 1) {
      delete p;
      *p = 8; // expected use after free
    } else {
      (void)p; // expected leak
    }
  };
  cb();
}

void test(int branch) {
  int *p = new int(42);
  releaseThroughLambda<int *>(p, branch);
}
