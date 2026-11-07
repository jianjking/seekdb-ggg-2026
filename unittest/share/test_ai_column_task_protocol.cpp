#include <gtest/gtest.h>
#include "share/ai_service/ai_column_task_protocol.h"

using namespace oceanbase::share;

namespace
{
AIColumnTaskSnapshot pending(int64_t max_retries = 2)
{
  AIColumnTaskSnapshot task;
  task.identity_.table_id_ = 50001;
  task.identity_.column_id_ = 18;
  task.identity_.request_id_ = 1;
  task.identity_.definition_version_ = 42;
  task.identity_.rowkey_ = "row-one";
  task.max_retries_ = max_retries;
  return task;
}

AIColumnTaskOwner owner(uint64_t low)
{
  AIColumnTaskOwner value;
  value.high_ = 123;
  value.low_ = low;
  return value;
}

AIColumnTaskSnapshot running(int64_t max_retries = 2)
{
  AIColumnTaskSnapshot task;
  EXPECT_EQ(AIColumnClaimResult::CLAIMED,
            AIColumnTaskProtocol::claim(pending(max_retries), owner(1), 100, 100, task));
  return task;
}

AIColumnTaskSnapshot completed(bool success)
{
  AIColumnTaskSnapshot task = running();
  AIColumnTaskSnapshot after;
  if (!success) {
    task.max_retries_ = 0;
  }
  EXPECT_TRUE(AIColumnTaskProtocol::finish(task, task, success, 150, 10, after));
  return after;
}
}

TEST(AIColumnTaskProtocol, claim_reserves_attempt_before_external_call)
{
  AIColumnTaskSnapshot task = running();
  EXPECT_TRUE(task.is_valid());
  EXPECT_EQ(AIColumnTaskState::RUNNING, task.state_);
  EXPECT_EQ(1, task.attempt_count_);
  EXPECT_EQ(1, task.claim_epoch_);
  EXPECT_EQ(200, task.lease_until_us_);
  EXPECT_TRUE(task.owner_ == owner(1));
}

TEST(AIColumnTaskProtocol, pending_eligibility_is_inclusive)
{
  AIColumnTaskSnapshot task = pending();
  task.eligible_us_ = 101;
  AIColumnTaskSnapshot next;
  EXPECT_EQ(AIColumnClaimResult::NOT_ELIGIBLE,
            AIColumnTaskProtocol::claim(task, owner(2), 100, 50, next));
  EXPECT_EQ(AIColumnClaimResult::CLAIMED,
            AIColumnTaskProtocol::claim(task, owner(2), 101, 50, next));
}

TEST(AIColumnTaskProtocol, active_lease_blocks_competing_worker)
{
  AIColumnTaskSnapshot task = running();
  AIColumnTaskSnapshot next;
  EXPECT_EQ(AIColumnClaimResult::NOT_ELIGIBLE,
            AIColumnTaskProtocol::claim(task, owner(2), 199, 50, next));
}

TEST(AIColumnTaskProtocol, expired_lease_changes_owner_and_epoch)
{
  AIColumnTaskSnapshot old = running();
  AIColumnTaskSnapshot next;
  ASSERT_EQ(AIColumnClaimResult::CLAIMED,
            AIColumnTaskProtocol::claim(old, owner(2), 200, 50, next));
  EXPECT_TRUE(next.owner_ == owner(2));
  EXPECT_EQ(2, next.claim_epoch_);
  EXPECT_EQ(2, next.attempt_count_);
  AIColumnTaskSnapshot result;
  EXPECT_FALSE(AIColumnTaskProtocol::finish(next, old, true, 210, 10, result));
  EXPECT_TRUE(AIColumnTaskProtocol::finish(next, next, true, 210, 10, result));
}

TEST(AIColumnTaskProtocol, expired_callback_cannot_publish_without_reclaim)
{
  AIColumnTaskSnapshot old = running();
  AIColumnTaskSnapshot next;
  EXPECT_FALSE(AIColumnTaskProtocol::finish(old, old, true, 200, 10, next));
}

TEST(AIColumnTaskProtocol, request_definition_row_and_column_fence_callbacks)
{
  AIColumnTaskSnapshot old = running();
  for (int i = 0; i < 5; ++i) {
    AIColumnTaskSnapshot current = old;
    if (i == 0) { ++current.identity_.request_id_; }
    if (i == 1) { ++current.identity_.definition_version_; }
    if (i == 2) { current.identity_.rowkey_ = "row-two"; }
    if (i == 3) { ++current.identity_.table_id_; }
    if (i == 4) { ++current.identity_.column_id_; }
    AIColumnTaskSnapshot next;
    EXPECT_FALSE(AIColumnTaskProtocol::finish(current, old, true, 150, 10, next));
  }
}

TEST(AIColumnTaskProtocol, retry_wait_is_durable_and_bounded)
{
  AIColumnTaskSnapshot task = running();
  for (int i = 1; i <= 3; ++i) {
    AIColumnTaskSnapshot next;
    const int64_t now = 100 + (i - 1) * 30 + 1;
    ASSERT_TRUE(AIColumnTaskProtocol::finish(task, task, false, now, 10, next));
    EXPECT_TRUE(next.is_valid());
    if (i < 3) {
      EXPECT_EQ(AIColumnTaskState::RETRY_WAIT, next.state_);
      EXPECT_EQ(now + 10, next.eligible_us_);
      AIColumnTaskSnapshot blocked;
      EXPECT_EQ(AIColumnClaimResult::NOT_ELIGIBLE,
                AIColumnTaskProtocol::claim(next, owner(i + 1), now + 9, 100, blocked));
      ASSERT_EQ(AIColumnClaimResult::CLAIMED,
                AIColumnTaskProtocol::claim(next, owner(i + 1), now + 10, 100, task));
    } else {
      EXPECT_EQ(AIColumnTaskState::FAILED, next.state_);
      EXPECT_EQ(3, next.attempt_count_);
    }
  }
}

TEST(AIColumnTaskProtocol, zero_retries_fails_after_first_attempt)
{
  AIColumnTaskSnapshot task = running(0);
  AIColumnTaskSnapshot next;
  ASSERT_TRUE(AIColumnTaskProtocol::finish(task, task, false, 150, 10, next));
  EXPECT_EQ(AIColumnTaskState::FAILED, next.state_);
  EXPECT_FALSE(next.owner_.is_valid());
}

TEST(AIColumnTaskProtocol, repeated_crashes_cannot_reset_retry_budget)
{
  AIColumnTaskSnapshot task = running(1);
  AIColumnTaskSnapshot next;
  ASSERT_EQ(AIColumnClaimResult::CLAIMED,
            AIColumnTaskProtocol::claim(task, owner(2), 200, 100, next));
  ASSERT_EQ(AIColumnClaimResult::EXHAUSTED,
            AIColumnTaskProtocol::claim(next, owner(3), 300, 100, task));
  EXPECT_EQ(AIColumnTaskState::FAILED, task.state_);
  EXPECT_EQ(2, task.attempt_count_);
  EXPECT_TRUE(task.is_valid());
}

TEST(AIColumnTaskProtocol, terminal_task_cannot_be_claimed_or_completed_again)
{
  AIColumnTaskSnapshot task = completed(true);
  AIColumnTaskSnapshot next;
  EXPECT_EQ(AIColumnClaimResult::NOT_ELIGIBLE,
            AIColumnTaskProtocol::claim(task, owner(2), 200, 100, next));
  EXPECT_FALSE(AIColumnTaskProtocol::finish(task, running(), true, 150, 10, next));
}

TEST(AIColumnTaskProtocol, group_requires_every_captured_member)
{
  AIColumnTaskSnapshot tasks[2] = {completed(true), completed(false)};
  tasks[1].identity_.column_id_ = 19;
  AIColumnTaskIdentity expected[2] = {tasks[0].identity_, tasks[1].identity_};
  EXPECT_TRUE(AIColumnTaskProtocol::group_is_terminal(tasks, 2, expected, 2));
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(tasks, 1, expected, 2));
  tasks[1] = running();
  tasks[1].identity_.column_id_ = 19;
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(tasks, 2, expected, 2));
}

TEST(AIColumnTaskProtocol, group_accepts_arbitrary_completion_order)
{
  AIColumnTaskSnapshot tasks[2] = {completed(true), completed(false)};
  tasks[1].identity_.column_id_ = 19;
  AIColumnTaskIdentity expected[2] = {tasks[1].identity_, tasks[0].identity_};
  EXPECT_TRUE(AIColumnTaskProtocol::group_is_terminal(tasks, 2, expected, 2));
}

TEST(AIColumnTaskProtocol, duplicate_missing_and_changed_definitions_block_group)
{
  AIColumnTaskSnapshot tasks[2] = {completed(true), completed(true)};
  tasks[1].identity_.column_id_ = 19;
  AIColumnTaskIdentity expected[2] = {tasks[0].identity_, tasks[1].identity_};
  AIColumnTaskSnapshot duplicate[2] = {tasks[0], tasks[0]};
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(duplicate, 2, expected, 2));
  ++tasks[1].identity_.definition_version_;
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(tasks, 2, expected, 2));
  expected[1] = expected[0];
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(duplicate, 2, expected, 2));
}

TEST(AIColumnTaskProtocol, different_row_or_request_is_not_a_group)
{
  AIColumnTaskSnapshot tasks[2] = {completed(true), completed(true)};
  tasks[1].identity_.column_id_ = 19;
  tasks[1].identity_.rowkey_ = "another-row";
  AIColumnTaskIdentity expected[2] = {tasks[0].identity_, tasks[1].identity_};
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(tasks, 2, expected, 2));
  tasks[1].identity_.rowkey_ = tasks[0].identity_.rowkey_;
  ++tasks[1].identity_.request_id_;
  expected[1] = tasks[1].identity_;
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(tasks, 2, expected, 2));
  EXPECT_FALSE(AIColumnTaskProtocol::group_is_terminal(NULL, 0, NULL, 0));
}

TEST(AIColumnTaskProtocol, binary_rowkeys_do_not_alias_at_embedded_nul)
{
  const char first[] = {'a', '\0', 'b'};
  const char second[] = {'a', '\0', 'c'};
  AIColumnTaskSnapshot old = running();
  old.identity_.rowkey_ = std::string_view(first, sizeof(first));
  AIColumnTaskSnapshot current = old;
  current.identity_.rowkey_ = std::string_view(second, sizeof(second));
  AIColumnTaskSnapshot next;
  EXPECT_FALSE(AIColumnTaskProtocol::finish(current, old, true, 150, 10, next));
}

TEST(AIColumnTaskProtocol, malformed_state_and_arithmetic_overflow_fail_closed)
{
  AIColumnTaskSnapshot task = pending();
  AIColumnTaskSnapshot next;
  EXPECT_EQ(AIColumnClaimResult::INVALID,
            AIColumnTaskProtocol::claim(task, owner(1), INT64_MAX, 1, next));
  task.max_retries_ = 101;
  EXPECT_EQ(AIColumnClaimResult::INVALID,
            AIColumnTaskProtocol::claim(task, owner(1), 100, 100, next));
  task = pending();
  task.owner_ = owner(1);
  EXPECT_FALSE(task.is_valid());
  task = pending();
  task.attempt_count_ = 1;
  EXPECT_FALSE(task.is_valid());
  task = running();
  task.state_ = static_cast<AIColumnTaskState>(99);
  EXPECT_FALSE(task.is_valid());
}
