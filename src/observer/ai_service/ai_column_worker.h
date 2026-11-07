#pragma once

#include "share/ob_thread_pool.h"

namespace oceanbase
{
namespace observer
{
class AIColumnWorker : public share::ObThreadPool
{
public:
  AIColumnWorker() : share::ObThreadPool(2) {}
  int init();
private:
  void run1() override;
  int poll();
};
} // namespace observer
} // namespace oceanbase
