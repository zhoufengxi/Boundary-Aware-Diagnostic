#include <string>

struct Metadata {
  std::string tag;
  Metadata() : tag("metadata") {}
};

struct Holder {
  explicit Holder(int *p) : ptr(p), meta() {}
  int *ptr;
  Metadata meta;
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
