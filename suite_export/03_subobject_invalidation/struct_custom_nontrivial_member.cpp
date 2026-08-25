class NonTrivialTag {
public:
  NonTrivialTag() : value_(0) {}
  ~NonTrivialTag() {}
private:
  int value_;
};

struct Holder {
  explicit Holder(int *p) : ptr(p), tag() {}
  int *ptr;
  NonTrivialTag tag;
};

void test(int branch) {
  int *p = new int(42);
  Holder *h = new Holder(p);

  if (branch == 0) {
    delete h->ptr;
    delete h->ptr; // expected double free
  } else if (branch == 1) {
    delete h->ptr;
    *h->ptr = 8; // expected use after free
  } else {
    (void)h->ptr; // expected leak
  }
  delete h;
}
