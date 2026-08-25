#include "listener.h"
#include <algorithm>
#include <string>
#include <vector>

void dispatch_capture_order_variant(std::vector<Listener> &listeners, int branch) {
  int *p = new int(42);
  EventData *data = new EventData(p);
  std::string label = "dispatch";

  std::for_each(listeners.begin(), listeners.end(), [label, data, branch](Listener &listener) {
    listener.onEvent(data, branch);
  });

  delete data;
}
