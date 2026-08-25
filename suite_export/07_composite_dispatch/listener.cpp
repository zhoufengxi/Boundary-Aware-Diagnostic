#include "listener.h"

void Listener::onEvent(EventData *data, int branch) {
  if (branch == 0) {
    delete data->ptr;
    delete data->ptr; // expected double free
  } else if (branch == 1) {
    delete data->ptr;
    *data->ptr = 8; // expected use after free
  } else {
    (void)data->ptr; // expected leak
  }
}
