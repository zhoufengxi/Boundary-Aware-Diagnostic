#include "worker.h"
#include "dep_b.h"
#include "nested_dep_b.h"
#include "order_dep_b.h"
#include "transitive_dep_b.h"
#include "user_dep_b.h"

void Worker::releaseOnce(int *p, int branch) {
  dep::Key key{1};
  if (dep::shouldRelease(key)) {
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
}

void Worker::releaseTransitive(int *p, int branch) {
  transitive_dep::Key key{1};
  if (transitive_dep::shouldRelease(key)) {
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
}

void Worker::releaseDependencyOrderBefore(int *p, int branch) {
  order_dep::Key key{1};
  if (order_dep::shouldRelease(key)) {
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
}

void Worker::releaseDependencyOrderAfter(int *p, int branch) {
  order_dep::Key key{1};
  if (order_dep::shouldRelease(key)) {
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
}

void Worker::releaseNested(int *p, int branch) {
  nested_dep::detail::Key key{1};
  if (nested_dep::detail::shouldRelease(key)) {
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
}

void Worker::releaseUserConflict(int *p, int branch) {
  user_dep::Key key{1};
  if (user_dep::shouldRelease(key)) {
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
}
