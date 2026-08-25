#include <string>

struct Executor {
  template <class Fn>
  void run(Fn fn) {
    fn();
  }
};

// Callback is a temporary call argument whose captured state must remain live.
void test(int branch) {
  Executor exec;
  std::string label = "event";
  int *p = new int(42);

  exec.run([label, p, branch]() {
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
