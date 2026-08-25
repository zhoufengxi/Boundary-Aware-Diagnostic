#pragma once

namespace user_dep {
struct Key {
  long x;
};
inline bool shouldRelease(Key key) { return key.x == 1; }
}
