#include <string>

struct Holder {
  explicit Holder(int *p) : tag("holder"), ptr(p) {}
  std::string tag;
  int *ptr;
};

void test(int branch) {
  int *p = new int(42);
  Holder *h = new Holder(p);

  if (branch == 0) {
    delete h->ptr;
    delete h->ptr; // expected double free
  } else if (branch == 1) {
    delete h->ptr;
    *h->ptr = 8; // expected use after free
  } else {
    (void)h->ptr; // expected leak
  }
  delete h;
}
