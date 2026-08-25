#pragma once
#pragma clang system_header

namespace transitive_dep {
struct Key {
  long x;
};
inline bool shouldRelease(Key key) { return key.x == 1; }
}
