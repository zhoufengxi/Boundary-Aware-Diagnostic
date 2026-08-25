#include <string>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}
  int *getPointer() { return ptr_; }
private:
  int *ptr_;
  std::string tag_;
};

void externalDelete(int *q) { delete q; }

void test(int branch) {
  int *p = new int(42);
  Holder h(p);
  if (branch == 0) {
    externalDelete(h.getPointer());
    externalDelete(h.getPointer()); // expected double free
  } else if (branch == 1) {
    externalDelete(h.getPointer());
    *h.getPointer() = 8; // expected use after free
  } else {
    (void)h.getPointer(); // expected leak
  }
}
