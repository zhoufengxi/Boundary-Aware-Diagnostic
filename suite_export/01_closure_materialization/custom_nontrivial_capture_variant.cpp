#include <string>

struct PayloadName {
  std::string value;
  PayloadName() : value("event") {}
};

// Variant: custom non-trivial capture, not tied to std::string directly.
void test(int branch) {
  PayloadName name;
  int *p = new int(42);
  auto cb = [name, p, branch]() {
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
