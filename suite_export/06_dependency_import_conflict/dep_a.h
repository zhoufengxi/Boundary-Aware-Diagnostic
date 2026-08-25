#pragma once
#pragma clang system_header

namespace dep {
struct Key {
  int x;
};
inline bool shouldRelease(Key k) { return k.x == 1; }
}
