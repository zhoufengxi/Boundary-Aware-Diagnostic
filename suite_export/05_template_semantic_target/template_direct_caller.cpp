void releaseThroughTemplate(int *p, int branch);

void test(int branch) {
  int *p = new int(42);
  releaseThroughTemplate(p, branch);
}
