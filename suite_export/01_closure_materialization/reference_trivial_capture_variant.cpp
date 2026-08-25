#include <string>

void test(int branch) {
  int flag = 1;
  std::string label = "event";
  int *p = new int(42);

  auto cb = [&flag, label, p, branch]() {
    if (!flag)
      return;
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
