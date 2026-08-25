#include <string>
#include <vector>

template <class Fn>
void invoke(Fn fn) { fn(1); }

template <class It, class Fn>
void run_each(It begin, It end, Fn fn) {
  for (; begin != end; ++begin)
    invoke(fn);
}

void test(int branch) {
  std::vector<int> values = {1};
  std::string label = "event";
  int *p = new int(42);
  run_each(values.begin(), values.end(), [label, p, branch](int) {
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
