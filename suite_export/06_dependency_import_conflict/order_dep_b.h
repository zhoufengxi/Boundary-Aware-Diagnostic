#pragma once
#pragma clang system_header

namespace order_dep {
struct Key {
  long x;
};
inline bool shouldRelease(Key key) { return key.x == 1; }
}
