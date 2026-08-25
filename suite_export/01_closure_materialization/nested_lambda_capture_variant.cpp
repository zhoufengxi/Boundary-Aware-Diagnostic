#include <string>

void test(int branch) {
  std::string label = "event";
  int *p = new int(42);

  auto outer = [label, p, branch]() {
    auto inner = [label, p, branch]() {
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
    inner();
  };
  outer();
}
