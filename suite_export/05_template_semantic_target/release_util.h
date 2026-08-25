#pragma once

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
void releaseTwice(T p) { releaseByBranch<T>(p, 0); }

template <class T>
void releaseOnce(T p) { delete p; }
