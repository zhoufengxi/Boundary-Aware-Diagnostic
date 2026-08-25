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

template <class T>
void outerRelease(T p, int branch) { releaseByBranch<T>(p, branch); }

template <class T>
void topRelease(T p, int branch) { outerRelease<T>(p, branch); }

void test(int branch) {
  int *p = new int(42);
  topRelease<int *>(p, branch);
}
