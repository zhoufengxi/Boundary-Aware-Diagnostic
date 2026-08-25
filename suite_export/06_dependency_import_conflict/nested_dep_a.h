#pragma once
#pragma clang system_header

namespace nested_dep {
namespace detail {
struct Key {
  int x;
};
inline bool shouldRelease(Key key) { return key.x == 1; }
}
}
