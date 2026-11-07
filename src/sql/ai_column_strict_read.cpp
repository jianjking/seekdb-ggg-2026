#define USING_LOG_PREFIX SQL
#include "sql/ob_result_set.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "share/schema/ob_schema_getter_guard.h"

namespace oceanbase
{
using namespace common;
using namespace share::schema;
namespace sql
{
namespace
{
int strict_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("``")); }
    else { OZ(sql.append(name.ptr() + i, 1)); }
  }
  OZ(sql.append("`"));
  return ret;
}
}

int ObResultSet::check_ai_column_strict_read(ObExecContext &ctx, const ObPhysicalPlan &plan)
{
  int ret = OB_SUCCESS;
  const ObIArray<uint64_t> &refs = plan.get_ai_column_references();
  if (refs.empty() || my_session_.is_ai_column_worker()
      || NULL != my_session_.get_ai_column_strict_snapshot()
      || my_session_.get_ddl_info().is_ddl()) { return ret; }
  int64_t enabled = 0;
  OZ(my_session_.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled));
  if (OB_FAIL(ret) || enabled == 0) { return ret; }
  ObSqlCtx *sql_ctx = ctx.get_sql_ctx();
  if (NULL == sql_ctx || NULL == sql_ctx->schema_guard_ || refs.count() % 2 != 0
      || !ctx.get_das_ctx().get_snapshot().is_valid()) { return OB_ERR_UNEXPECTED; }
  sqlclient::ObISQLConnectionGuard connection;
  ObIInnerSQLConnection *inner = as_inner_sql_connection(my_session_.get_inner_conn());
  bool owned = false;
  if (NULL == inner) {
    OZ(query::ObInnerSQLConnectionAccess::create_connection_with_external_session(&my_session_, connection));
    if (OB_SUCC(ret)) {
      inner = as_inner_sql_connection(connection.get_ptr());
      if (NULL == inner) { ret = OB_ERR_UNEXPECTED; }
      else { my_session_.set_inner_conn(inner); owned = true; }
    }
  }
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_conn;
  bool nested = false;
  const bool cache = my_session_.get_local_ob_enable_plan_cache();
  if (OB_SUCC(ret)) {
    OZ(inner->begin_nested_session(saved_session, saved_conn, true));
    nested = OB_SUCC(ret);
  }
  if (nested) {
    my_session_.set_local_ob_enable_plan_cache(false);
    for (int64_t i = 0; OB_SUCC(ret) && i < refs.count(); i += 2) {
      const ObTableSchema *table = NULL;
      const ObDatabaseSchema *db = NULL;
      OZ(sql_ctx->schema_guard_->get_table_schema(refs.at(i), table));
      if (OB_SUCC(ret) && NULL == table) { ret = OB_SCHEMA_ERROR; }
      const ObColumnSchemaV2 *column = NULL == table ? NULL : table->get_column_schema(refs.at(i + 1));
      const ObColumnSchemaV2 *state = NULL == column ? NULL : table->get_column_schema(column->get_ai_state_column_id());
      if (OB_SUCC(ret) && (NULL == column || !column->is_ai_column() || NULL == state)) { ret = OB_SCHEMA_ERROR; }
      if (OB_SUCC(ret)) { OZ(sql_ctx->schema_guard_->get_database_schema(table->get_database_id(), db)); }
      if (OB_SUCC(ret) && NULL == db) { ret = OB_SCHEMA_ERROR; }
      ObSqlString query;
      if (OB_SUCC(ret)) {
        my_session_.set_ai_column_strict_context(table->get_table_id(), &ctx.get_das_ctx().get_snapshot());
        OZ(query.append("SELECT /*+NO_USE_PLAN_CACHE*/ 1 FROM "));
        OZ(strict_identifier(query, db->get_database_name_str()));
        OZ(query.append("."));
        OZ(strict_identifier(query, table->get_table_name_str()));
        OZ(query.append(" WHERE "));
        OZ(strict_identifier(query, state->get_column_name_str()));
        OZ(query.append(" IS NULL OR "));
        OZ(strict_identifier(query, state->get_column_name_str()));
        OZ(query.append(" <> 1 LIMIT 1"));
        // Destroy the read result before restoring the outer statement/session.
        ObISQLClient::ReadResult result;
        OZ(inner->execute_read(query.string(), result));
        if (OB_SUCC(ret) && NULL == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
        if (OB_SUCC(ret)) {
          const int next = result.get_result()->next();
          if (next == OB_SUCCESS) {
            ret = OB_AI_COLUMN_INCOMPLETE;
            LOG_USER_ERROR(OB_AI_COLUMN_INCOMPLETE, refs.at(i), refs.at(i + 1));
          } else if (next != OB_ITER_END) { ret = next; }
        }
      }
    }
    my_session_.set_ai_column_strict_context(OB_INVALID_ID, NULL);
    my_session_.set_local_ob_enable_plan_cache(cache);
    const int end_ret = inner->end_nested_session(saved_session, saved_conn);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  if (owned) { my_session_.set_inner_conn(NULL); }
  return ret;
}
} // namespace sql
} // namespace oceanbase
