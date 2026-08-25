#include <string>

struct Executor {
  template <class Fn>
  void run(Fn fn) { fn(); }
};

struct Callback {
  int *p;
  std::string label;
  int branch;
  void operator()() const {
    if (label.empty())
      return;
    if (branch == 0) {
      delete p;
      delete p; // expected double free
    } else if (branch == 1) {
      delete p;
      *p = 8; // expected use after free
    } else {
      (void)p; // expected leak
    }
  }
};

void test(int branch) {
  Executor exec;
  int *p = new int(42);
  exec.run(Callback{p, "event", branch});
}
