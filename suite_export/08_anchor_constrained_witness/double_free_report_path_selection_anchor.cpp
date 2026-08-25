#include "double_free_report_path_selection_anchor.h"

// Keep the allocation entry before the callback definition. The complete path
// crosses one CTU boundary and returns through the constant function pointer.
void test(int branch) {
  AnchorData data = {new int(43)};
  dispatchAnchorCallback(callbackEntry, &data, branch);
}

// Independent analysis produces the shorter path without an allocation event.
void callbackEntry(AnchorData *data, int branch) {
  diagnosticTerminal(data, branch);
}
