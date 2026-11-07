#pragma once

#include "share/ai_service/ai_column_task_protocol.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase
{
namespace share
{

// These operations never open, commit, or roll back a transaction. The caller
// must commit a successful claim before making an external request. Publishing
// business results and removing the locked group remain the caller's transaction.
class AIColumnTaskSqlService
{
public:
  static int build_enqueue_sql(const AIColumnTaskSnapshot &task,
                               const common::ObString &rowkey_predicate,
                               const common::ObString &generation_expr,
                               const common::ObString &normalized_expr,
                               const common::ObIArray<uint64_t> &group_columns,
                               common::ObSqlString &buffer);
  static int enqueue(common::ObMySQLTransaction &trans,
                     const AIColumnTaskSnapshot &task,
                     const common::ObString &rowkey_predicate,
                     const common::ObString &generation_expr,
                     const common::ObString &normalized_expr,
                     const common::ObIArray<uint64_t> &group_columns);
  static int claim(common::ObMySQLTransaction &trans,
                   const AIColumnTaskSnapshot &observed,
                   const AIColumnTaskOwner &owner,
                   int64_t database_now_us,
                   int64_t lease_us,
                   AIColumnTaskSnapshot &after,
                   bool &changed);
  static int finish(common::ObMySQLTransaction &trans,
                    const AIColumnTaskSnapshot &observed,
                    const AIColumnTaskSnapshot &claim,
                    bool succeeded,
                    const common::ObString &payload,
                    int64_t database_now_us,
                    int64_t retry_delay_us,
                    AIColumnTaskSnapshot &after,
                    bool &changed);
  static int lock_group(common::ObMySQLTransaction &trans,
                        const AIColumnTaskIdentity &identity,
                        common::ObISQLClient::ReadResult &result);
  static int erase(common::ObMySQLTransaction &trans,
                   const AIColumnTaskIdentity &identity);

private:
  static int write_transition(common::ObMySQLTransaction &trans,
                              const AIColumnTaskSnapshot &before,
                              const AIColumnTaskSnapshot &after,
                              const common::ObString &payload,
                              bool recovering_expired_lease,
                              bool &changed);
};

} // namespace share
} // namespace oceanbase
