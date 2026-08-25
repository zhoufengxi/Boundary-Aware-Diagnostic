#include <string>

class Metadata {
public:
  Metadata() : tag_("meta") {}
private:
  std::string tag_;
};

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), meta_() {}
  friend void externalRelease(Holder *h, int branch);
private:
  int *ptr_;
  Metadata meta_;
};

void externalRelease(Holder *h, int branch) {
  if (branch == 0) {
    delete h->ptr_;
    delete h->ptr_; // expected double free
  } else if (branch == 1) {
    delete h->ptr_;
    *h->ptr_ = 8; // expected use after free
  } else {
    (void)h->ptr_; // expected leak
  }
}

void test(int branch) {
  int *p = new int(42);
  Holder *h = new Holder(p);
  externalRelease(h, branch);
  delete h;
}
