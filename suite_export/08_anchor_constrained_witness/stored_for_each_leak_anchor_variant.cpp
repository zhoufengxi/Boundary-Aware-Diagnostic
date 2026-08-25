#include "double_free_report_path_selection_anchor.h"
#include <algorithm>
#include <vector>

void storedForEachCallbackEntry(AnchorData *data, int branch);

// The algorithm callback is materialized before use, while the terminal
// callback remains an independently analyzed entry.
void test(int branch) {
  std::vector<int> values = {1};
  AnchorData state = {new int(42)};
  AnchorData *data = &state;

  auto callback = [data, branch](int) {
    dispatchAnchorCallback(storedForEachCallbackEntry, data, branch);
  };

  std::for_each(values.begin(), values.end(), callback);
}

void storedForEachCallbackEntry(AnchorData *data, int branch) {
  diagnosticTerminal(data, branch);
}
