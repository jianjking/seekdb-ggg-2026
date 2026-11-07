#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace oceanbase
{
namespace share
{

// Values 0..3 are shared with the persisted column/task state contract.
enum class AIColumnTaskState : int64_t
{
  PENDING = 0,
  READY = 1,
  FAILED = 2,
  RETRY_WAIT = 3,
  RUNNING = 4
};

struct AIColumnTaskIdentity
{
  uint64_t table_id_ = 0;
  uint64_t column_id_ = 0;
  int64_t request_id_ = 0;
  int64_t definition_version_ = 0;
  // Borrowed canonical binary rowkey, not SQL. Callers own its lifetime.
  std::string_view rowkey_;

  bool is_valid() const
  {
    return table_id_ > 0 && table_id_ != UINT64_MAX
        && column_id_ > 0 && column_id_ != UINT64_MAX
        && request_id_ > 0 && definition_version_ > 0
        && !rowkey_.empty() && rowkey_.size() <= 2048;
  }

  bool same_group(const AIColumnTaskIdentity &other) const
  {
    return table_id_ == other.table_id_ && request_id_ == other.request_id_
        && rowkey_ == other.rowkey_;
  }

  bool operator==(const AIColumnTaskIdentity &other) const
  {
    return same_group(other) && column_id_ == other.column_id_
        && definition_version_ == other.definition_version_;
  }
};

struct AIColumnTaskOwner
{
  uint64_t high_ = 0;
  uint64_t low_ = 0;

  bool is_valid() const { return high_ != 0 || low_ != 0; }
  bool operator==(const AIColumnTaskOwner &other) const
  {
    return high_ == other.high_ && low_ == other.low_;
  }
};

struct AIColumnTaskSnapshot
{
  AIColumnTaskIdentity identity_;
  AIColumnTaskState state_ = AIColumnTaskState::PENDING;
  int64_t attempt_count_ = 0;
  int64_t max_retries_ = 0;
  int64_t claim_epoch_ = 0;
  AIColumnTaskOwner owner_;
  int64_t eligible_us_ = 0;
  int64_t lease_until_us_ = 0;

  bool is_terminal() const
  {
    return state_ == AIColumnTaskState::READY || state_ == AIColumnTaskState::FAILED;
  }

  bool is_valid() const
  {
    const int64_t state = static_cast<int64_t>(state_);
    const bool running = state_ == AIColumnTaskState::RUNNING;
    bool attempts_valid = attempt_count_ > 0 && claim_epoch_ >= attempt_count_;
    if (state_ == AIColumnTaskState::PENDING) {
      attempts_valid = attempt_count_ == 0 && claim_epoch_ == 0;
    } else if (state_ == AIColumnTaskState::RETRY_WAIT) {
      attempts_valid = attempts_valid && attempt_count_ <= max_retries_;
    }
    return identity_.is_valid() && state >= 0 && state <= 4
        && max_retries_ >= 0 && max_retries_ <= 100
        && attempt_count_ >= 0 && attempt_count_ <= max_retries_ + 1
        && attempts_valid && claim_epoch_ >= 0 && eligible_us_ >= 0
        && (running
            ? owner_.is_valid() && claim_epoch_ > 0 && attempt_count_ > 0 && lease_until_us_ > 0
            : !owner_.is_valid() && lease_until_us_ == 0);
  }
};

enum class AIColumnClaimResult
{
  INVALID,
  NOT_ELIGIBLE,
  CLAIMED,
  EXHAUSTED
};

class AIColumnTaskProtocol
{
public:
  // now_us must come from the database clock. All changes still require a
  // conditional UPDATE matching the complete observed snapshot in one transaction.
  static AIColumnClaimResult claim(const AIColumnTaskSnapshot &before,
                                   const AIColumnTaskOwner &owner,
                                   int64_t now_us,
                                   int64_t lease_us,
                                   AIColumnTaskSnapshot &after)
  {
    AIColumnClaimResult result = AIColumnClaimResult::NOT_ELIGIBLE;
    if (!before.is_valid() || !owner.is_valid() || now_us <= 0 || lease_us <= 0
        || now_us > std::numeric_limits<int64_t>::max() - lease_us) {
      result = AIColumnClaimResult::INVALID;
    } else if (before.is_terminal() || before.eligible_us_ > now_us
               || (before.state_ == AIColumnTaskState::RUNNING
                   && before.lease_until_us_ > now_us)) {
    } else if (before.attempt_count_ >= before.max_retries_ + 1) {
      after = before;
      after.state_ = AIColumnTaskState::FAILED;
      after.owner_ = AIColumnTaskOwner();
      after.lease_until_us_ = 0;
      result = AIColumnClaimResult::EXHAUSTED;
    } else if (before.claim_epoch_ == std::numeric_limits<int64_t>::max()) {
      result = AIColumnClaimResult::INVALID;
    } else {
      after = before;
      after.state_ = AIColumnTaskState::RUNNING;
      after.owner_ = owner;
      ++after.attempt_count_;
      ++after.claim_epoch_;
      after.lease_until_us_ = now_us + lease_us;
      result = AIColumnClaimResult::CLAIMED;
    }
    return result;
  }

  // A callback carries the identity, owner and epoch from its successful claim.
  static bool finish(const AIColumnTaskSnapshot &before,
                     const AIColumnTaskSnapshot &claim,
                     bool succeeded,
                     int64_t now_us,
                     int64_t retry_delay_us,
                     AIColumnTaskSnapshot &after)
  {
    bool accepted = false;
    if (before.is_valid() && claim.is_valid()
        && before.state_ == AIColumnTaskState::RUNNING
        && claim.state_ == AIColumnTaskState::RUNNING
        && before.identity_ == claim.identity_
        && before.owner_ == claim.owner_
        && before.claim_epoch_ == claim.claim_epoch_
        && before.attempt_count_ == claim.attempt_count_
        && before.max_retries_ == claim.max_retries_
        && before.lease_until_us_ == claim.lease_until_us_
        && now_us > 0 && now_us < before.lease_until_us_
        && retry_delay_us > 0
        && now_us <= std::numeric_limits<int64_t>::max() - retry_delay_us) {
      after = before;
      after.owner_ = AIColumnTaskOwner();
      after.lease_until_us_ = 0;
      if (succeeded) {
        after.state_ = AIColumnTaskState::READY;
      } else if (before.attempt_count_ <= before.max_retries_) {
        after.state_ = AIColumnTaskState::RETRY_WAIT;
        after.eligible_us_ = now_us + retry_delay_us;
      } else {
        after.state_ = AIColumnTaskState::FAILED;
      }
      accepted = true;
    }
    return accepted;
  }

  // The expected list is the captured membership of this row mutation, not the
  // set of whatever tasks happen to be left in a scan. Missing members block
  // publication. The caller must lock and revalidate the business row as well.
  static bool group_is_terminal(const AIColumnTaskSnapshot *tasks,
                                size_t task_count,
                                const AIColumnTaskIdentity *expected,
                                size_t expected_count)
  {
    bool ready = tasks != NULL && expected != NULL && expected_count > 0
        && task_count == expected_count;
    for (size_t i = 0; ready && i < expected_count; ++i) {
      if (!expected[i].is_valid() || !expected[0].same_group(expected[i])) {
        ready = false;
      }
      for (size_t j = 0; ready && j < i; ++j) {
        if (expected[i].column_id_ == expected[j].column_id_) {
          ready = false;
        }
      }
      size_t matches = 0;
      for (size_t j = 0; ready && j < task_count; ++j) {
        if (tasks[j].identity_ == expected[i]) {
          ++matches;
          if (!tasks[j].is_valid() || !tasks[j].is_terminal()) {
            ready = false;
          }
        }
      }
      if (matches != 1) {
        ready = false;
      }
    }
    return ready;
  }
};

} // namespace share
} // namespace oceanbase
