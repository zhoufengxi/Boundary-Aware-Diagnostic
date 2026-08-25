#pragma once
#include "event_data.h"

class Listener {
public:
  void onEvent(EventData *data, int branch);
};
