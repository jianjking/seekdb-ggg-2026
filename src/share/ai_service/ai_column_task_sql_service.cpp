#define USING_LOG_PREFIX SHARE
#include "share/ai_service/ai_column_task_sql_service.h"
#include "common/object/ob_object.h"
#include "share/ob_dml_sql_splicer.h"
#include "lib/utility/ob_unify_serialize.h"

namespace oceanbase
{
namespace share
{
using namespace common;

namespace
{
const char *const AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";

ObString task_rowkey(const AIColumnTaskIdentity &identity)
{
  return ObString(static_cast<int32_t>(identity.rowkey_.size()), identity.rowkey_.data());
}

int add_task_key(ObDMLSqlSplicer &sql, const AIColumnTaskIdentity &identity)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.add_pk_column("table_id", identity.table_id_))) {
  } else if (OB_FAIL(sql.add_pk_column("column_id", identity.column_id_))) {
  } else if (OB_FAIL(sql.add_pk_column("rowkey_bin", ObHexEscapeSqlStr(task_rowkey(identity))))) {
  }
  return ret;
}

int append_task_generation(ObSqlString &sql, const AIColumnTaskIdentity &identity)
{
  return sql.append_fmt(" AND request_id = %lld AND schema_version = %lld",
                        static_cast<long long>(identity.request_id_),
                        static_cast<long long>(identity.definition_version_));
}

int add_result_payload(ObDMLSqlSplicer &sql, AIColumnTaskState state, const ObString &payload)
{
  int ret = OB_SUCCESS;
  if (state != AIColumnTaskState::READY || NULL == payload.ptr()) {
    ret = sql.add_column("result_payload", ObObj());
  } else {
    ret = sql.add_column("result_payload", ObHexEscapeSqlStr(payload));
  }
  return ret;
}
}

int AIColumnTaskSqlService::enqueue(ObMySQLTransaction &trans,
                                   const AIColumnTaskSnapshot &task,
                                   const ObString &rowkey_predicate,
                                   const ObString &generation_expr,
                                   const ObString &normalized_expr,
                                   const ObIArray<uint64_t> &group_columns)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (!trans.is_started()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(build_enqueue_sql(task, rowkey_predicate, generation_expr,
                                       normalized_expr, group_columns, buffer))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (!is_single_row(affected_rows)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int AIColumnTaskSqlService::build_enqueue_sql(const AIColumnTaskSnapshot &task,
                                             const ObString &rowkey_predicate,
                                             const ObString &generation_expr,
                                             const ObString &normalized_expr,
                                             const ObIArray<uint64_t> &group_columns,
                                             ObSqlString &buffer)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer sql;
  ObSqlString members;
  bool contains_column = false;
  if (!buffer.empty() || !task.is_valid()
      || task.state_ != AIColumnTaskState::PENDING || task.eligible_us_ != 0
      || rowkey_predicate.empty() || generation_expr.empty() || normalized_expr.empty()
      || group_columns.empty() || group_columns.count() > OB_MAX_USER_DEFINED_COLUMNS_COUNT) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(members.append("["))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group_columns.count(); ++i) {
    const uint64_t column_id = group_columns.at(i);
    if (column_id == 0 || column_id == OB_INVALID_ID) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t j = 0; OB_SUCC(ret) && j < i; ++j) {
      if (group_columns.at(j) == column_id) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      contains_column = contains_column || column_id == task.identity_.column_id_;
      if (i > 0 && OB_FAIL(members.append(","))) {
      } else if (OB_FAIL(members.append_fmt("%llu", static_cast<unsigned long long>(column_id)))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!contains_column) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(members.append("]"))) {
  } else if (OB_FAIL(add_task_key(sql, task.identity_))) {
  } else if (OB_FAIL(sql.add_column("request_id", task.identity_.request_id_))) {
  } else if (OB_FAIL(sql.add_column("schema_version", task.identity_.definition_version_))) {
  } else if (OB_FAIL(sql.add_column("state", static_cast<int64_t>(task.state_)))) {
  } else if (OB_FAIL(sql.add_column("attempt_count", 0))) {
  } else if (OB_FAIL(sql.add_column("retry_count", 0))) {
  } else if (OB_FAIL(sql.add_column("max_retries", task.max_retries_))) {
  } else if (OB_FAIL(sql.add_column("claim_epoch", 0))) {
  } else if (OB_FAIL(sql.add_column("owner_high", 0))) {
  } else if (OB_FAIL(sql.add_column("owner_low", 0))) {
  } else if (OB_FAIL(sql.add_column("lease_until_us", 0))) {
  } else if (OB_FAIL(sql.add_column("eligible_us", 0))) {
  } else if (OB_FAIL(sql.add_time_column("eligible_ts", -1))) {
  } else if (OB_FAIL(sql.add_column("rowkey_predicate", ObHexEscapeSqlStr(rowkey_predicate)))) {
  } else if (OB_FAIL(sql.add_column("generation_expr", ObHexEscapeSqlStr(generation_expr)))) {
  } else if (OB_FAIL(sql.add_column("normalized_expr", ObHexEscapeSqlStr(normalized_expr)))) {
  } else if (OB_FAIL(sql.add_column("group_columns", ObHexEscapeSqlStr(members.string())))) {
  } else if (OB_FAIL(sql.splice_insert_sql(AI_COLUMN_TASK_TABLE, buffer))) {
  }
  return ret;
}

int AIColumnTaskSqlService::claim(ObMySQLTransaction &trans,
                                 const AIColumnTaskSnapshot &observed,
                                 const AIColumnTaskOwner &owner,
                                 int64_t database_now_us,
                                 int64_t lease_us,
                                 AIColumnTaskSnapshot &after,
                                 bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  AIColumnTaskSnapshot next;
  const AIColumnClaimResult result =
      AIColumnTaskProtocol::claim(observed, owner, database_now_us, lease_us, next);
  if (!trans.is_started() || result == AIColumnClaimResult::INVALID) {
    ret = OB_INVALID_ARGUMENT;
  } else if (result == AIColumnClaimResult::CLAIMED || result == AIColumnClaimResult::EXHAUSTED) {
    if (OB_FAIL(write_transition(trans, observed, next, ObString(), true, changed))) {
    } else if (changed) {
      after = next;
    }
  }
  return ret;
}

int AIColumnTaskSqlService::finish(ObMySQLTransaction &trans,
                                  const AIColumnTaskSnapshot &observed,
                                  const AIColumnTaskSnapshot &claim,
                                  bool succeeded,
                                  const ObString &payload,
                                  int64_t database_now_us,
                                  int64_t retry_delay_us,
                                  AIColumnTaskSnapshot &after,
                                  bool &changed)
{
  int ret = OB_SUCCESS;
  changed = false;
  AIColumnTaskSnapshot next;
  if (!trans.is_started() || !observed.is_valid() || !claim.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!AIColumnTaskProtocol::finish(observed, claim, succeeded,
                                          database_now_us, retry_delay_us, next)) {
    // A lost lease/generation is an expected no-op, never an unconditional write.
  } else if (OB_FAIL(write_transition(trans, observed, next, payload, false, changed))) {
  } else if (changed) {
    after = next;
  }
  return ret;
}

int AIColumnTaskSqlService::write_transition(ObMySQLTransaction &trans,
                                            const AIColumnTaskSnapshot &before,
                                            const AIColumnTaskSnapshot &after,
                                            const ObString &payload,
                                            bool recovering_expired_lease,
                                            bool &changed)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObDMLSqlSplicer sql;
  ObSqlString buffer;
  changed = false;
  if (!trans.is_started() || !before.is_valid() || !after.is_valid()
      || !(before.identity_ == after.identity_) || before.max_retries_ != after.max_retries_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(add_task_key(sql, before.identity_))) {
  } else if (OB_FAIL(sql.add_column("state", static_cast<int64_t>(after.state_)))) {
  } else if (OB_FAIL(sql.add_column("attempt_count", after.attempt_count_))) {
  } else if (OB_FAIL(sql.add_column("retry_count",
                 after.state_ == AIColumnTaskState::RETRY_WAIT
                     ? after.attempt_count_ : after.attempt_count_ - 1))) {
  } else if (OB_FAIL(sql.add_column("claim_epoch", after.claim_epoch_))) {
  } else if (OB_FAIL(sql.add_column("owner_high", ObRealUInt64(after.owner_.high_)))) {
  } else if (OB_FAIL(sql.add_column("owner_low", ObRealUInt64(after.owner_.low_)))) {
  } else if (OB_FAIL(sql.add_column("lease_until_us", after.lease_until_us_))) {
  } else if (OB_FAIL(sql.add_column("eligible_us", after.eligible_us_))) {
  } else if (OB_FAIL(sql.add_time_column("eligible_ts", after.eligible_us_))) {
  } else if (OB_FAIL(add_result_payload(sql, after.state_, payload))) {
  } else if (OB_FAIL(sql.splice_update_sql(AI_COLUMN_TASK_TABLE, buffer))) {
  } else if (OB_FAIL(append_task_generation(buffer, before.identity_))) {
  } else if (OB_FAIL(buffer.append_fmt(
                 " AND state = %lld AND attempt_count = %lld AND max_retries = %lld"
                 " AND claim_epoch = %lld AND owner_high = %llu AND owner_low = %llu"
                 " AND lease_until_us = %lld AND eligible_us = %lld",
                 static_cast<long long>(before.state_),
                 static_cast<long long>(before.attempt_count_),
                 static_cast<long long>(before.max_retries_),
                 static_cast<long long>(before.claim_epoch_),
                 static_cast<unsigned long long>(before.owner_.high_),
                 static_cast<unsigned long long>(before.owner_.low_),
                 static_cast<long long>(before.lease_until_us_),
                 static_cast<long long>(before.eligible_us_)))) {
  } else if (before.state_ == AIColumnTaskState::RUNNING
             && after.state_ != AIColumnTaskState::RUNNING
             && after.claim_epoch_ == before.claim_epoch_) {
    const char *comparison = ">";
    if (recovering_expired_lease) {
      comparison = "<=";
    }
    ret = buffer.append_fmt(
        " AND lease_until_us %s CAST(UNIX_TIMESTAMP(NOW(6)) * 1000000 AS SIGNED)", comparison);
  }
  if (OB_SUCC(ret) && after.state_ == AIColumnTaskState::RUNNING) {
    if (OB_FAIL(buffer.append_fmt(
            " AND eligible_ts <= NOW(6)"
            " AND lease_until_us <= CAST(UNIX_TIMESTAMP(NOW(6)) * 1000000 AS SIGNED)"
            " AND CAST(UNIX_TIMESTAMP(NOW(6)) * 1000000 AS SIGNED) < %lld",
            static_cast<long long>(after.lease_until_us_)))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (affected_rows < 0 || affected_rows > 1) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    changed = is_single_row(affected_rows);
  }
  return ret;
}

int AIColumnTaskSqlService::lock_group(ObMySQLTransaction &trans,
                                      const AIColumnTaskIdentity &identity,
                                      ObISQLClient::ReadResult &result)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer key;
  ObSqlString buffer;
  if (!trans.is_started() || !identity.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(key.add_pk_column("table_id", identity.table_id_))) {
  } else if (OB_FAIL(key.add_pk_column("rowkey_bin", ObHexEscapeSqlStr(task_rowkey(identity))))) {
  } else if (OB_FAIL(key.add_pk_column("request_id", identity.request_id_))) {
  } else if (OB_FAIL(buffer.append("SELECT * FROM oceanbase.__all_ai_column_task WHERE "))) {
  } else if (OB_FAIL(key.splice_predicates(buffer))) {
  } else if (OB_FAIL(buffer.append(" ORDER BY column_id FOR UPDATE"))) {
  } else if (OB_FAIL(trans.read(result, buffer.ptr()))) {
  }
  return ret;
}

int AIColumnTaskSqlService::erase(ObMySQLTransaction &trans,
                                 const AIColumnTaskIdentity &identity)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer key;
  ObSqlString buffer;
  int64_t affected_rows = 0;
  if (!trans.is_started() || !identity.is_valid()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(add_task_key(key, identity))) {
  } else if (OB_FAIL(key.splice_delete_sql(AI_COLUMN_TASK_TABLE, buffer))) {
  } else if (OB_FAIL(append_task_generation(buffer, identity))) {
  } else if (OB_FAIL(trans.write(buffer.ptr(), affected_rows))) {
  } else if (affected_rows < 0 || affected_rows > 1) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

} // namespace share
} // namespace oceanbase
