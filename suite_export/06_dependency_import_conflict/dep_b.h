#pragma once
#pragma clang system_header

namespace dep {
struct Key {
  long x;
};
inline bool shouldRelease(Key k) { return k.x == 1; }
}
