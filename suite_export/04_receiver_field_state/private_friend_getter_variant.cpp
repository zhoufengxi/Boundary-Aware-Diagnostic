#include <string>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}
  friend int *friendGet(const Holder &h);
private:
  int *ptr_;
  std::string tag_;
};

int *friendGet(const Holder &h) { return h.ptr_; }

void test(int branch) {
  int *p = new int(42);
  Holder h(p);
  if (branch == 0) {
    delete friendGet(h);
    delete friendGet(h); // expected double free
  } else if (branch == 1) {
    delete friendGet(h);
    *friendGet(h) = 8; // expected use after free
  } else {
    (void)friendGet(h); // expected leak
  }
}
