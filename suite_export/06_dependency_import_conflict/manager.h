#pragma once
#include "worker.h"

class Manager {
public:
  void run(Worker w, int branch);
};
