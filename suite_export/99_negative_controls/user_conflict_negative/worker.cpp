#include "worker.h"
#include "dep_b.h"
void Worker::releaseOnce(int *p) {
  appdep::Payload payload{1.0};
  if (payload.x > 0.0)
    delete p;
}
