#include <string>

// Variant: pointer capture appears before the non-trivial capture.
void test(int branch) {
  std::string label = "event";
  int *p = new int(42);
  auto cb = [p, label, branch]() {
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
