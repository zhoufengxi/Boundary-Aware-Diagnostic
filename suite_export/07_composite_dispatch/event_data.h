#pragma once
#include <string>

struct EventData {
  int *ptr;
  std::string type;
  explicit EventData(int *p) : ptr(p), type("event") {}
};
