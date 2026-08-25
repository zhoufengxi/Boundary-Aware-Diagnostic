// Base case: lambda captures only the diagnostic-relevant pointer.
void test(int branch) {
  int *p = new int(42);
  auto cb = [p, branch]() {
    if (branch == 0) {
      delete p;
      delete p; // expected double free
    } else if (branch == 1) {
      delete p;
      *p = 8; // expected use after free
    } else {
      (void)p; // expected leak
    }
  };
  cb();
}
