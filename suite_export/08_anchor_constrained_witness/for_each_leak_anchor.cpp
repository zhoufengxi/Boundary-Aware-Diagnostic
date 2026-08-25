#include "double_free_report_path_selection_anchor.h"
#include <algorithm>
#include <vector>

void forEachCallbackEntry(AnchorData *data, int branch);

// The allocation-bearing path crosses both the algorithm and CTU dispatcher.
// The callback definition below is also analyzed independently, producing the
// competing shorter path without an allocation witness.
void test(int branch) {
  std::vector<int> values = {1};
  AnchorData state = {new int(42)};
  AnchorData *data = &state;

  std::for_each(values.begin(), values.end(), [data, branch](int) {
    dispatchAnchorCallback(forEachCallbackEntry, data, branch);
  });
}

void forEachCallbackEntry(AnchorData *data, int branch) {
  diagnosticTerminal(data, branch);
}
