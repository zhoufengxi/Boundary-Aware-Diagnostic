#include <string>

struct Element {
  int *ptr;
  std::string tag;
  Element() : ptr(nullptr), tag("element") {}
};

void test(int branch) {
  int *p = new int(42);
  Element elements[2];
  elements[0].ptr = p;
  elements[1].tag = "other";

  if (branch == 0) {
    delete elements[0].ptr;
    delete elements[0].ptr; // expected double free
  } else if (branch == 1) {
    delete elements[0].ptr;
    *elements[0].ptr = 8; // expected use after free
  } else {
    (void)elements[0].ptr; // expected leak
  }
}
