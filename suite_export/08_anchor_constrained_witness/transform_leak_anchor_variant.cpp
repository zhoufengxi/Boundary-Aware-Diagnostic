#include "double_free_report_path_selection_anchor.h"
#include <algorithm>
#include <vector>

void transformCallbackEntry(AnchorData *data, int branch);

// Structural variant using std::transform before the CTU callback dispatcher.
void test(int branch) {
  std::vector<int> input = {1};
  std::vector<int> output(1);
  AnchorData state = {new int(42)};
  AnchorData *data = &state;

  std::transform(input.begin(), input.end(), output.begin(),
                 [data, branch](int value) {
                   dispatchAnchorCallback(transformCallbackEntry, data,
                                          branch);
                   return value;
                 });
}

void transformCallbackEntry(AnchorData *data, int branch) {
  diagnosticTerminal(data, branch);
}
