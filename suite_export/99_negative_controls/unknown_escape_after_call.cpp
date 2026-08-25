// Negative control: the pointer escapes to unknown code; precise state is not required.
extern void unknown_use(int *p);

void test() {
  int *p = new int(42);
  unknown_use(p);
  delete p;
}
