#include <string>

extern void unknown_use(void *);

template <class Fn>
void storeUnknown(Fn &fn) {
  unknown_use(static_cast<void *>(&fn));
}

void test() {
  std::string label = "event";
  int *p = new int(42);
  auto cb = [label, p]() { delete p; };
  storeUnknown(cb); // callback object escapes to unknown code
  delete p;         // precise callback execution is not required
}
