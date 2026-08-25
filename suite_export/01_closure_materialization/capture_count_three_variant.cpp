#include <string>

struct Metadata {
  std::string tag;
  Metadata() : tag("m") {}
};

void test(int branch) {
  int guard = 1;
  Metadata meta;
  std::string label = "event";
  int *p = new int(42);

  auto cb = [guard, meta, label, p, branch]() {
    if (guard != 1)
      return;
    if (branch == 0) {
      delete p;
      delete p; // expected double free
    } else if (branch == 1) {
      delete p;
      *p = 8; // expected use after free
    } else if  (branch == 2){
      (void)p; // expected leak
    }
  };
  cb();
}
