template <int N, class T>
void releaseNTimes(T p, int branch) {
  if (N <= 0)
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

void test(int branch) {
  int *p = new int(42);
  releaseNTimes<1, int *>(p, branch);
}
