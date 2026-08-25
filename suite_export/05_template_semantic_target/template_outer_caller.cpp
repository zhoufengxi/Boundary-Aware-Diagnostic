void releaseOnceThroughOuter(int *p, int branch);

void test(int branch) {
  int *p = new int(42);
  releaseOnceThroughOuter(p, branch);
}
