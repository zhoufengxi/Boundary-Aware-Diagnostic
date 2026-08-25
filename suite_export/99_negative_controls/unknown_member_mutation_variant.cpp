#include <string>

class Holder {
public:
  explicit Holder(int *p) : ptr_(p), tag_("holder") {}
  int *ptr_;
  std::string tag_;
};

extern void unknownMutate(Holder &h);

void test() {
  int *p = new int(42);
  Holder h(p);
  unknownMutate(h); // unknown mutation of object state
  delete h.ptr_;
  delete p;
}
