struct Releaser {
  template <class T>
  void releaseByBranch(T p, int branch) {
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
  int *p = new int(42);
  Releaser r;
  r.releaseByBranch<int *>(p, branch);
}
