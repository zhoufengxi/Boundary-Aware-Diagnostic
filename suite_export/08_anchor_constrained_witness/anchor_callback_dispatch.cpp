#include "double_free_report_path_selection_anchor.h"

// The only CTU forwarding step.
void dispatchAnchorCallback(AnchorHandler handler, AnchorData *data,
                            int branch) {
  handler(data, branch);
}
