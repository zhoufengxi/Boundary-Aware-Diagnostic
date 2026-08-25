#include <string>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}
  int *ptr_;
  std::string tag_;
};

extern void mutate(Holder *h);

void test() {
  int *p = new int(42);
  Holder h(p);
  mutate(&h); // unknown mutation; precise field state is not required
  delete h.ptr_;
  delete p;
}
