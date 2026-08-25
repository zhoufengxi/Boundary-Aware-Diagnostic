#include <string>

class BaseHolder {
protected:
  int *ptr_;
  std::string tag_;
public:
  explicit BaseHolder(int *p) : ptr_(p), tag_("base") {}
  friend void externalRelease(BaseHolder *h, int branch);
};

void externalRelease(BaseHolder *h, int branch) {
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
  BaseHolder *h = new BaseHolder(p);
  externalRelease(h, branch);
  delete h;
}
