#include <string>

// Diagnostic state is stored in ptr_; tag_ is an unrelated non-trivial member.
class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}

  int *ptr_;
  std::string tag_;
};

void test(int branch) {
  int *p = new int(42);
  Holder *h = new Holder(p);

  if (branch == 0) {
    delete h->ptr_;
    delete h->ptr_; // expected double free
  } else if (branch == 1) {
    delete h->ptr_;
    *h->ptr_ = 8; // expected use after free
  } else {
    (void)h->ptr_; // expected leak
  }
  delete h;
}