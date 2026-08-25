#include <string>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}
  int *get() const { return ptr_; }
private:
  int *ptr_;
  std::string tag_;
};

void test(int branch) {
  int *p = new int(42);
  Holder h(p);

  if (branch == 0) {
    delete h.get();
    delete h.get(); // expected double free
  } else if (branch == 1) {
    delete h.get();
    *h.get() = 8; // expected use after free
  } else {
    (void)h.get(); // expected leak
  }
}
