#pragma once

namespace user_dep {
struct Key {
  int x;
};
inline bool shouldRelease(Key key) { return key.x == 1; }
}
