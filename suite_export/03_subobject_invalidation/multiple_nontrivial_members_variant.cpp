#include <string>
#include <vector>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder"), values_(1, 7) {}
  int *ptr_;
  std::string tag_;
  std::vector<int> values_;
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
