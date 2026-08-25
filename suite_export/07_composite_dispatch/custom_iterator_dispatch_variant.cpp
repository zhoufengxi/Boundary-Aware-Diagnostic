#include "listener.h"
#include <string>
#include <vector>

template <class It, class Fn>
void run_each(It begin, It end, Fn fn) {
  for (; begin != end; ++begin)
    fn(*begin);
}

void dispatch_custom_iterator_variant(std::vector<Listener> &listeners, int branch) {
  int *p = new int(42);
  EventData *data = new EventData(p);
  std::string label = "dispatch";

  run_each(listeners.begin(), listeners.end(), [label, data, branch](Listener &listener) {
    listener.onEvent(data, branch);
  });

  delete data;
}
