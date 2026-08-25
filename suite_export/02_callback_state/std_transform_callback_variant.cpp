#include <algorithm>
#include <string>
#include <vector>

void test(int branch) {
  std::vector<int> input = {1};
  std::vector<int> output(1);
  std::string label = "event";
  int *p = new int(42);

  std::transform(input.begin(), input.end(), output.begin(), [label, p, branch](int x) {
    if (branch == 0) {
      delete p;
      delete p; // expected double free
    } else if (branch == 1) {
      delete p;
      *p = 8; // expected use after free
    } else {
      (void)p; // expected leak
    }
    return x;
  });
}
