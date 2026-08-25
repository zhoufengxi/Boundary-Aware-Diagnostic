#include <string>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}
  int *get() const { return ptr_; }
private:
  int *ptr_;
  std::string tag_;
};

class Container {
public:
  explicit Container(int *p) : holder_(p), name_("container") {}
  const Holder &holder() const { return holder_; }
private:
  Holder holder_;
  std::string name_;
};

void test(int branch) {
  int *p = new int(42);
  Container c(p);
  if (branch == 0) {
    delete c.holder().get();
    delete c.holder().get(); // expected double free
  } else if (branch == 1) {
    delete c.holder().get();
    *c.holder().get() = 8; // expected use after free
  } else {
    (void)c.holder().get(); // expected leak
  }
}
