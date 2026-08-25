#pragma once
#pragma clang system_header

namespace transitive_dep {
struct Key {
  int x;
};
inline bool shouldRelease(Key key) { return key.x == 1; }
}
