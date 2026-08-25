#pragma once

#include "anchor_data.h"

using AnchorHandler = void (*)(AnchorData *, int);

void dispatchAnchorCallback(AnchorHandler handler, AnchorData *data,
                            int branch);
void callbackEntry(AnchorData *data, int branch);

// The standalone callback paths and the allocation-bearing CTU paths reach the
// same three branch-specific terminal operations.
inline void diagnosticTerminal(AnchorData *data, int branch) {
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
