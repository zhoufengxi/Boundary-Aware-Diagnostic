#include <algorithm>
#include <string>
#include <vector>

// Standard library algorithm variant.
void test(int branch) {
  std::vector<int> values = {1};
  std::string label = "event";
  int *p = new int(42);

  std::for_each(values.begin(), values.end(), [label, p, branch](int) {
    if (branch == 0) {
      delete p;
      delete p; // expected double free
    } else if (branch == 1) {
      delete p;
      *p = 8; // expected use after free
    } else {
      (void)p; // expected leak
    }
  });
}

