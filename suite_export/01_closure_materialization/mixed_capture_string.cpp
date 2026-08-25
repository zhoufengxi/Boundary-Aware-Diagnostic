#include <string>

// Variant: a non-trivial capture should not break the captured pointer state.
void test(int branch) {
  std::string label = "event";
  int *p = new int(42);
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
