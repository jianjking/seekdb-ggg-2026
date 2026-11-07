#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ai_column_worker.h"
#include "observer/ob_inner_sql_connection.h"
#include "observer/ob_inner_sql_result.h"
#include "share/ob_server_struct.h"
#include "share/ai_service/ai_column_task_sql_service.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/tx/ob_timestamp_access.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/json_type/ob_json_tree.h"
#include "share/object/ob_obj_cast.h"
#include <string>
#include <vector>
#include <cmath>

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace observer
{
namespace
{
struct ColumnTask
{
  AIColumnTaskSnapshot task_;
  std::string key_;
  std::string expr_;
  std::string normalized_;
  std::string members_;
  std::string payload_;
  bool payload_is_null_ = false;
  int64_t now_ = 0;
  AIColumnTaskSnapshot snapshot() const
  {
    AIColumnTaskSnapshot value = task_;
    value.identity_.rowkey_ = std::string_view(key_.data(), key_.size());
    return value;
  }
};

ObString view(const std::string &s)
{
  return ObString(static_cast<int32_t>(s.size()), s.data());
}

int copy_field(sqlclient::ObMySQLResult &row, const char *name, std::string &out,
               bool nullable = false, bool *is_null = NULL)
{
  int ret = OB_SUCCESS;
  ObObj value;
  ObArenaAllocator alloc("AIColRead");
  if (OB_FAIL(row.get_obj(name, value))) {
  } else if (value.is_null()) {
    out.clear();
    if (NULL != is_null) { *is_null = true; }
    if (!nullable) { ret = OB_ERR_NULL_VALUE; }
  } else if (!value.is_string_type()) {
    ret = OB_OBJ_TYPE_ERROR;
  } else {
    ObString data = value.get_string();
    if (value.is_lob_storage()) {
      ObInnerSQLResult *inner = dynamic_cast<ObInnerSQLResult *>(&row);
      if (NULL == inner || NULL == inner->get_result_set()) { ret = OB_ERR_UNEXPECTED; }
      else {
        OZ(sql::ObTextStringHelper::read_real_string_data(
            inner->get_result_set()->get_exec_context(), &alloc, value, data));
      }
    }
    if (OB_SUCC(ret)) {
      if (NULL != is_null) { *is_null = false; }
      out.clear();
      if (!data.empty()) { out.assign(data.ptr(), data.length()); }
    }
  }
  return ret;
}

int load_task(sqlclient::ObMySQLResult &row, ColumnTask &out)
{
  int ret = OB_SUCCESS;
  uint64_t table_id = 0;
  uint64_t column_id = 0;
  int64_t state = 0;
  AIColumnTaskSnapshot &t = out.task_;
  OZ(row.get_uint("table_id", table_id));
  OZ(row.get_uint("column_id", column_id));
  t.identity_.table_id_ = table_id;
  t.identity_.column_id_ = column_id;
  OZ(row.get_int("request_id", t.identity_.request_id_));
  OZ(row.get_int("schema_version", t.identity_.definition_version_));
  OZ(row.get_int("state", state));
  t.state_ = static_cast<AIColumnTaskState>(state);
  OZ(row.get_int("attempt_count", t.attempt_count_));
  OZ(row.get_int("max_retries", t.max_retries_));
  OZ(row.get_int("claim_epoch", t.claim_epoch_));
  OZ(row.get_uint("owner_high", t.owner_.high_));
  OZ(row.get_uint("owner_low", t.owner_.low_));
  OZ(row.get_int("eligible_us", t.eligible_us_));
  OZ(row.get_int("lease_until_us", t.lease_until_us_));
  OZ(copy_field(row, "rowkey_bin", out.key_));
  OZ(copy_field(row, "generation_expr", out.expr_));
  OZ(copy_field(row, "normalized_expr", out.normalized_));
  OZ(copy_field(row, "group_columns", out.members_));
  OZ(copy_field(row, "result_payload", out.payload_, true, &out.payload_is_null_));
  if (OB_SUCC(ret) && !out.snapshot().is_valid()) { ret = OB_INVALID_DATA; }
  return ret;
}

// Connections are pooled. Never return an internal publishing capability or a
// system prompt to the pool, and never share plans with ordinary user DML.
class WorkerSession
{
public:
  explicit WorkerSession(ObMySQLTransaction &trans) : session_(NULL), mode_(0), cache_(false)
  {
    ObInnerSQLConnection *conn = dynamic_cast<ObInnerSQLConnection *>(trans.get_connection());
    if (NULL != conn) {
      session_ = &conn->get_session();
      mode_ = session_->get_sql_mode();
      cache_ = session_->get_local_ob_enable_plan_cache();
      session_->set_local_ob_enable_plan_cache(false);
    }
  }
  ~WorkerSession()
  {
    if (NULL != session_) {
      session_->reset_ai_column_context();
      session_->set_sql_mode(mode_);
      session_->set_local_ob_enable_plan_cache(cache_);
    }
  }
  sql::ObSQLSessionInfo *session_;
private:
  ObSQLMode mode_;
  bool cache_;
};

int identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("``")); }
    else { OZ(sql.append(name.ptr() + i, 1)); }
  }
  OZ(sql.append("`"));
  return ret;
}

int qualified_table(ObSqlString &sql, ObSchemaGetterGuard &guard, const ObTableSchema &table)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *db = NULL;
  OZ(guard.get_database_schema(table.get_database_id(), db));
  if (OB_SUCC(ret) && OB_ISNULL(db)) { ret = OB_SCHEMA_ERROR; }
  if (OB_SUCC(ret)) {
    OZ(identifier(sql, db->get_database_name_str()));
    OZ(sql.append("."));
    OZ(identifier(sql, table.get_table_name_str()));
  }
  return ret;
}

// Reconstruct SQL from the canonical key and the current schema, not from a
// persisted SQL fragment. This also detects truncated or mismatched keys.
int row_predicate(ObIAllocator &alloc, const ColumnTask &task, const ObTableSchema &table, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  ObRowkey key;
  int64_t pos = 0;
  OZ(key.deserialize(alloc, task.key_.data(), task.key_.size(), pos));
  const int64_t count = table.get_rowkey_info().get_size();
  const bool tablet_key = table.is_table_without_pk() && table.is_partitioned_table();
  if (OB_SUCC(ret) && (pos != task.key_.size() || key.get_obj_cnt() != count + (tablet_key ? 1 : 0))) {
    ret = OB_INVALID_DATA;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    uint64_t id = OB_INVALID_ID;
    OZ(table.get_rowkey_info().get_column_id(i, id));
    const ObColumnSchemaV2 *col = table.get_column_schema(id);
    char *literal = NULL;
    int64_t length = 0;
    int64_t end = 0;
    if (OB_SUCC(ret) && OB_ISNULL(col)) { ret = OB_SCHEMA_ERROR; }
    if (OB_SUCC(ret)) {
      if (i > 0) { OZ(sql.append(" AND ")); }
      OZ(identifier(sql, col->get_column_name_str()));
      OZ(sql.append(" = "));
      ObObjPrintParams params;
      params.character_hex_safe_represent_ = true;
      params.binary_string_print_hex_ = true;
      OZ(key.get_obj_ptr()[i].print_sql_literal(literal, length, end, alloc, params));
      OZ(sql.append(literal, end));
    }
  }
  return ret;
}

int qualified_task_table(ObIAllocator &alloc, ObSqlString &sql, ObSchemaGetterGuard &guard,
                         const ObTableSchema &table, const ColumnTask &task, bool &exists)
{
  int ret = OB_SUCCESS;
  exists = true;
  OZ(qualified_table(sql, guard, table));
  if (OB_SUCC(ret) && table.is_table_without_pk() && table.is_partitioned_table()) {
    ObRowkey key;
    int64_t pos = 0;
    const int64_t count = table.get_rowkey_info().get_size();
    OZ(key.deserialize(alloc, task.key_.data(), task.key_.size(), pos));
    if (OB_SUCC(ret) && (pos != task.key_.size() || key.get_obj_cnt() != count + 1
        || !key.get_obj_ptr()[count].is_uint64())) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret)) {
      const ObTabletID tablet(key.get_obj_ptr()[count].get_uint64());
      int64_t part = OB_INVALID_INDEX;
      int64_t subpart = OB_INVALID_INDEX;
      ObBasePartition *partition = NULL;
      const int part_ret = table.get_part_idx_by_tablet(tablet, part, subpart);
      if (part_ret == OB_TABLET_NOT_EXIST) { exists = false; }
      else if (part_ret != OB_SUCCESS) { ret = part_ret; }
      else if (OB_FAIL(table.get_part_by_idx(part, subpart, partition))) {
      } else if (NULL == partition) { ret = OB_SCHEMA_ERROR; }
      else {
        OZ(sql.append(" PARTITION ("));
        OZ(identifier(sql, partition->get_part_name()));
        OZ(sql.append(")"));
      }
    }
  }
  return ret;
}

bool same_definition(const ColumnTask &task, const ObColumnSchemaV2 *column)
{
  return NULL != column && column->is_ai_column()
      && column->get_ai_generation_expr() == view(task.expr_)
      && column->get_ai_normalized_expr() == view(task.normalized_);
}

int unsigned_json_value(const ObJsonNode *value, uint64_t &number)
{
  int ret = OB_SUCCESS;
  if (NULL == value) { ret = OB_INVALID_DATA; }
  else if (value->json_type() == ObJsonNodeType::J_UINT) { number = value->get_uint(); }
  else if (value->json_type() == ObJsonNodeType::J_INT && value->get_int() >= 0) {
    number = static_cast<uint64_t>(value->get_int());
  } else { ret = OB_INVALID_DATA; }
  return ret;
}

int generation_predicate(ObSqlString &sql, const ObTableSchema &table,
                          const ObColumnSchemaV2 &column, const AIColumnTaskIdentity &id)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *request = table.get_column_schema(column.get_ai_request_column_id());
  const ObColumnSchemaV2 *spec = table.get_column_schema(column.get_ai_result_spec_column_id());
  if (NULL == request || NULL == spec) { return OB_SCHEMA_ERROR; }
  OZ(sql.append(" AND "));
  OZ(identifier(sql, request->get_column_name_str()));
  OZ(sql.append_fmt(" = %lld AND ", static_cast<long long>(id.request_id_)));
  OZ(identifier(sql, spec->get_column_name_str()));
  OZ(sql.append_fmt(" = %lld", static_cast<long long>(id.definition_version_)));
  return ret;
}

int database_now(ObMySQLTransaction &trans, int64_t &now)
{
  int ret = OB_SUCCESS;
  ObISQLClient::ReadResult result;
  OZ(trans.read(result, "SELECT CAST(UNIX_TIMESTAMP(NOW(6))*1000000 AS SIGNED) AS now_us"));
  if (OB_SUCC(ret) && OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret)) {
    OZ(result.get_result()->next());
    OZ(result.get_result()->get_int("now_us", now));
  }
  return ret;
}

int validate_payload(ObIAllocator &alloc, const ObColumnSchemaV2 &column, const ObString &payload)
{
  int ret = OB_SUCCESS;
  const ObObjType type = column.get_data_type();
  if (ob_is_json(type) || type == ObCollectionSQLType || ob_is_numeric_type(type)) {
    ObIJsonBase *json = NULL;
    OZ(ObJsonBaseFactory::get_json_base(&alloc, payload, ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
    if (OB_SUCC(ret) && OB_ISNULL(json)) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret) && type == ObCollectionSQLType) {
      const ObIArray<ObString> &info = column.get_extended_type_info();
      int64_t dim = 0;
      // Vector metadata precedes the AI trailer and is preserved by schema IO.
      if (info.empty() || json->json_type() != ObJsonNodeType::J_ARRAY) { ret = OB_INVALID_DATA; }
      else {
        ObJsonArray *array = static_cast<ObJsonArray *>(json);
        const ObString &type_info = info.at(0);
        const char *open = static_cast<const char *>(memchr(type_info.ptr(), '(', type_info.length()));
        if (NULL == open) { ret = OB_INVALID_DATA; }
        else {
          for (const char *p = open + 1; p < type_info.ptr() + type_info.length() && *p >= '0' && *p <= '9'; ++p) {
            dim = dim * 10 + *p - '0';
            if (dim > 65536) { ret = OB_INVALID_DATA; break; }
          }
          if (dim <= 0 || array->element_count() != dim) { ret = OB_INVALID_DATA; }
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
          const ObJsonNode *value = array->get_value(i);
          const ObJsonNodeType kind = value->json_type();
          if (kind != ObJsonNodeType::J_INT && kind != ObJsonNodeType::J_UINT
              && kind != ObJsonNodeType::J_DOUBLE && kind != ObJsonNodeType::J_DECIMAL) { ret = OB_INVALID_DATA; }
          double number = 0;
          if (OB_SUCC(ret)) { OZ(value->to_double(number)); }
          if (OB_SUCC(ret) && (!std::isfinite(number)
              || std::fabs(number) > std::numeric_limits<float>::max())) { ret = OB_INVALID_DATA; }
        }
      }
    } else if (OB_SUCC(ret) && ob_is_numeric_type(type)) {
      const ObJsonNodeType kind = json->json_type();
      if (kind != ObJsonNodeType::J_INT && kind != ObJsonNodeType::J_UINT
          && kind != ObJsonNodeType::J_DOUBLE && kind != ObJsonNodeType::J_DECIMAL) { ret = OB_INVALID_DATA; }
      if (OB_SUCC(ret) && ob_is_integer_type(type)
          && kind != ObJsonNodeType::J_INT && kind != ObJsonNodeType::J_UINT) { ret = OB_INVALID_DATA; }
    }
  }
  if (OB_SUCC(ret) && type != ObCollectionSQLType && !ob_is_json(type)) {
    ObObj input;
    ObObj output;
    input.set_varchar(payload);
    input.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ObAccuracy accuracy = column.get_accuracy();
    ObCastCtx cast(&alloc, NULL, CM_ERROR_ON_SCALE_OVER, column.get_collation_type(), &accuracy);
    OZ(ObObjCaster::to_type(type, cast, input, output));
    OZ(ob_obj_accuracy_check_only(column.get_accuracy(), column.get_collation_type(), output));
  }
  return ret;
}

int evaluate(ColumnTask &task, std::string &payload, bool &is_null)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColEval");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = NULL;
  OZ(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(guard));
  OZ(guard.get_table_schema(task.task_.identity_.table_id_, table));
  if (OB_SUCC(ret) && OB_ISNULL(table)) { ret = OB_TABLE_NOT_EXIST; }
  const ObColumnSchemaV2 *column = NULL;
  if (OB_SUCC(ret)) { column = table->get_column_schema(task.task_.identity_.column_id_); }
  if (OB_SUCC(ret) && !same_definition(task, column)) { ret = OB_SCHEMA_ERROR; }
  ObString system;
  uint64_t sql_mode = DEFAULT_MYSQL_MODE;
  ObIJsonBase *json = NULL;
  if (OB_SUCC(ret)) {
    OZ(ObJsonBaseFactory::get_json_base(&alloc, view(task.normalized_), ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
    if (OB_SUCC(ret) && (OB_ISNULL(json) || json->json_type() != ObJsonNodeType::J_OBJECT)) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret)) {
      ObJsonObject *config = static_cast<ObJsonObject *>(json);
      ObJsonNode *prompt = config->get_value(ObString::make_string("system"));
      ObJsonNode *mode = config->get_value(ObString::make_string("sql_mode"));
      if (NULL == prompt || NULL == mode || prompt->json_type() != ObJsonNodeType::J_STRING) { ret = OB_INVALID_DATA; }
      else {
        system = ObString(prompt->get_data_length(), prompt->get_data());
        OZ(unsigned_json_value(mode, sql_mode));
      }
    }
  }
  ObSqlString sql;
  if (OB_SUCC(ret)) {
    bool location_exists = false;
    OZ(sql.append_fmt("SELECT /*+NO_USE_PLAN_CACHE*/ CAST((%s) AS CHAR) AS value FROM ", task.expr_.c_str()));
    OZ(qualified_task_table(alloc, sql, guard, *table, task, location_exists));
    if (OB_SUCC(ret) && !location_exists) { ret = OB_ITER_END; }
    OZ(sql.append(" WHERE "));
    OZ(row_predicate(alloc, task, *table, sql));
    OZ(generation_predicate(sql, *table, *column, task.snapshot().identity_));
  }
  ObMySQLTransaction trans;
  if (OB_SUCC(ret)) { OZ(trans.start(GCTX.sql_proxy_)); }
  if (OB_SUCC(ret)) {
    WorkerSession context(trans);
    if (NULL == context.session_) { ret = OB_ERR_UNEXPECTED; }
    else {
      context.session_->set_ai_column_context(table->get_table_id(), false, system);
      context.session_->set_sql_mode(sql_mode);
      ObISQLClient::ReadResult result;
      OZ(trans.read(result, sql.ptr()));
      if (OB_SUCC(ret) && OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
      if (OB_SUCC(ret)) {
        OZ(result.get_result()->next());
        OZ(copy_field(*result.get_result(), "value", payload, true, &is_null));
        if (OB_SUCC(ret) && payload.size() > 16 * 1024 * 1024) { ret = OB_SIZE_OVERFLOW; }
        if (OB_SUCC(ret) && !is_null) { OZ(validate_payload(alloc, *column, view(payload))); }
      }
    }
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(false);
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

int publish(const ColumnTask &candidate)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc("AIColPublish");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = NULL;
  OZ(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(guard));
  OZ(guard.get_table_schema(candidate.task_.identity_.table_id_, table));
  ObMySQLTransaction trans;
  if (OB_SUCC(ret)) { OZ(trans.start(GCTX.sql_proxy_)); }
  if (OB_SUCC(ret)) {
    WorkerSession context(trans);
    if (NULL == context.session_) { ret = OB_ERR_UNEXPECTED; }
    else {
      context.session_->set_ai_column_context(candidate.task_.identity_.table_id_, true, ObString());
      context.session_->set_sql_mode(DEFAULT_MYSQL_MODE | SMO_STRICT_ALL_TABLES);
      bool row_exists = false;
      ObSqlString table_name;
      ObSqlString predicate;
      if (NULL != table) {
        bool location_exists = false;
        OZ(qualified_task_table(alloc, table_name, guard, *table, candidate, location_exists));
        if (OB_SUCC(ret) && location_exists) {
          OZ(row_predicate(alloc, candidate, *table, predicate));
          ObSqlString lock;
          OZ(lock.append_fmt("SELECT /*+NO_USE_PLAN_CACHE*/ 1 FROM %s WHERE %s FOR UPDATE", table_name.ptr(), predicate.ptr()));
          ObISQLClient::ReadResult result;
          OZ(trans.read(result, lock.ptr()));
          if (OB_SUCC(ret) && OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
          if (OB_SUCC(ret)) {
            const int next = result.get_result()->next();
            if (next == OB_SUCCESS) { row_exists = true; }
            else if (next != OB_ITER_END) { ret = next; }
          }
        }
      }
      // Business-row lock always precedes task locks, matching normal DML.
      std::vector<ColumnTask> group;
      {
        ObISQLClient::ReadResult result;
        OZ(AIColumnTaskSqlService::lock_group(trans, candidate.snapshot().identity_, result));
        if (OB_SUCC(ret) && OB_ISNULL(result.get_result())) { ret = OB_ERR_UNEXPECTED; }
        while (OB_SUCC(ret)) {
          const int next = result.get_result()->next();
          if (next == OB_ITER_END) { break; }
          else if (next != OB_SUCCESS) { ret = next; }
          else {
            ColumnTask member;
            OZ(load_task(*result.get_result(), member));
            if (OB_SUCC(ret)) { group.push_back(member); }
          }
        }
      }
      bool complete = !group.empty();
      ObIJsonBase *json = NULL;
      std::vector<AIColumnTaskSnapshot> snapshots;
      std::vector<AIColumnTaskIdentity> expected;
      if (OB_SUCC(ret) && !group.empty()) {
        OZ(ObJsonBaseFactory::get_json_base(&alloc, view(group[0].members_), ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
        if (OB_SUCC(ret) && (NULL == json || json->json_type() != ObJsonNodeType::J_ARRAY)) { ret = OB_INVALID_DATA; }
        if (OB_SUCC(ret)) {
          ObJsonArray *members = static_cast<ObJsonArray *>(json);
          for (int64_t i = 0; OB_SUCC(ret) && i < members->element_count(); ++i) {
            AIColumnTaskIdentity id = candidate.snapshot().identity_;
            OZ(unsigned_json_value(members->get_value(i), id.column_id_));
            // Each column can have its own captured definition version.
            for (const ColumnTask &t : group) {
              if (t.task_.identity_.column_id_ == id.column_id_) { id.definition_version_ = t.task_.identity_.definition_version_; }
            }
            expected.push_back(id);
          }
          for (const ColumnTask &t : group) {
            snapshots.push_back(t.snapshot());
            complete = complete && t.members_ == group[0].members_;
          }
          complete = complete && AIColumnTaskProtocol::group_is_terminal(
              snapshots.data(), snapshots.size(), expected.data(), expected.size());
        }
      }
      // Removed rows/tables cannot acquire new results; stale tasks are harmless
      // to erase under their exact request and definition identities.
      if (OB_SUCC(ret) && !row_exists) {
        for (const ColumnTask &t : group) { OZ(AIColumnTaskSqlService::erase(trans, t.snapshot().identity_)); }
      } else if (OB_SUCC(ret) && complete) {
        ObSqlString update;
        ObSqlString fence;
        OZ(update.append_fmt("UPDATE /*+NO_USE_PLAN_CACHE*/ %s SET ", table_name.ptr()));
        int64_t assignments = 0;
        for (size_t i = 0; OB_SUCC(ret) && i < group.size(); ++i) {
          const ColumnTask &t = group[i];
          const ObColumnSchemaV2 *column = table->get_column_schema(t.task_.identity_.column_id_);
          if (NULL == column || !column->is_ai_column()) { continue; }
          const ObColumnSchemaV2 *state = table->get_column_schema(column->get_ai_state_column_id());
          if (OB_ISNULL(state)) { ret = OB_SCHEMA_ERROR; break; }
          if (assignments++ > 0) { OZ(update.append(",")); }
          OZ(identifier(update, column->get_column_name_str()));
          OZ(update.append(" = "));
          const bool definition_matches = same_definition(t, column);
          if (definition_matches && t.task_.state_ == AIColumnTaskState::READY && !t.payload_is_null_) {
            const int64_t length = 2 * t.payload_.size();
            char *buf = static_cast<char *>(alloc.alloc(length + 1));
            if (NULL == buf) { ret = OB_ALLOCATE_MEMORY_FAILED; }
            else {
              const char *digits = "0123456789ABCDEF";
              for (size_t k = 0; k < t.payload_.size(); ++k) {
                const unsigned char byte = static_cast<unsigned char>(t.payload_[k]);
                buf[2 * k] = digits[byte >> 4];
                buf[2 * k + 1] = digits[byte & 15];
              }
              OZ(update.append("_utf8mb4 X'"));
              OZ(update.append(buf, length));
              OZ(update.append("'"));
            }
          } else { OZ(update.append("NULL")); }
          OZ(update.append(","));
          OZ(identifier(update, state->get_column_name_str()));
          const AIColumnTaskState state_value = definition_matches ? t.task_.state_ : AIColumnTaskState::FAILED;
          OZ(update.append_fmt(" = %lld", static_cast<long long>(state_value)));
          OZ(generation_predicate(fence, *table, *column, t.snapshot().identity_));
        }
        if (OB_SUCC(ret) && assignments > 0) {
          OZ(update.append_fmt(" WHERE %s%s", predicate.ptr(), fence.ptr()));
          int64_t rows = 0;
          OZ(trans.write(update.ptr(), rows));
          if (OB_SUCC(ret) && (rows < 0 || rows > 1)) { ret = OB_ERR_UNEXPECTED; }
        }
        // A newer generation yields zero affected rows. Never publish into it.
        if (OB_SUCC(ret)) {
          for (const ColumnTask &t : group) { OZ(AIColumnTaskSqlService::erase(trans, t.snapshot().identity_)); }
        }
      }
    }
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

int execute_task(ColumnTask &task)
{
  int ret = OB_SUCCESS;
  if (task.task_.is_terminal()) { return publish(task); }
  AIColumnTaskOwner owner;
  int64_t number = 0;
  transaction::ObTimestampAccess *timestamps = server_service<transaction::ObTimestampAccess>();
  if (NULL == timestamps) { ret = OB_NOT_INIT; }
  else { OZ(timestamps->get_number(number)); owner.low_ = number; }
  AIColumnTaskSnapshot claim;
  bool changed = false;
  {
    ObMySQLTransaction trans;
    OZ(trans.start(GCTX.sql_proxy_));
    OZ(database_now(trans, task.now_));
    OZ(AIColumnTaskSqlService::claim(trans, task.snapshot(), owner, task.now_, 60000000, claim, changed));
    if (trans.is_started()) {
      const int end_ret = trans.end(OB_SUCC(ret));
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  if (OB_SUCC(ret) && changed) {
    task.task_ = claim;
    if (!claim.is_terminal()) {
      std::string payload;
      bool is_null = false;
      const int model_ret = evaluate(task, payload, is_null);
      ObMySQLTransaction trans;
      OZ(trans.start(GCTX.sql_proxy_));
      int64_t now = 0;
      AIColumnTaskSnapshot after;
      OZ(database_now(trans, now));
      // A persisted wait interval remains visible and avoids retry storms.
      OZ(AIColumnTaskSqlService::finish(trans, claim, claim, model_ret == OB_SUCCESS,
          is_null ? ObString() : view(payload), now, 1000000, after, changed));
      if (trans.is_started()) {
        const int end_ret = trans.end(OB_SUCC(ret));
        if (OB_SUCC(ret)) { ret = end_ret; }
      }
      if (OB_SUCC(ret) && changed) { task.task_ = after; }
    }
    if (OB_SUCC(ret) && changed && task.task_.is_terminal()) { OZ(publish(task)); }
  }
  return ret;
}

struct TableSeedProgress
{
  uint64_t table_id_;
  int64_t schema_version_;
  bool complete_;
};

// ALTER ADD leaves companions NULL on existing rows. Reuse the transactional
// producer instead of inventing a second enqueue protocol. NULL is the durable
// progress marker; an in-memory completed-schema cache only avoids idle scans.
int seed_column_rows(ObSchemaGetterGuard &guard, const ObTableSchema &table,
                     const ObColumnSchemaV2 &column, bool &complete)
{
  int ret = OB_SUCCESS;
  const int64_t batch_limit = 32;
  complete = false;
  const ObColumnSchemaV2 *spec = table.get_column_schema(column.get_ai_result_spec_column_id());
  if (NULL == spec) { return OB_SCHEMA_ERROR; }
  ObArenaAllocator alloc("AIColSeed");
  ObSqlString table_name;
  ObSqlString query;
  ObSqlString keys;
  OZ(qualified_table(table_name, guard, table));
  OZ(query.append("SELECT /*+NO_USE_PLAN_CACHE*/ "));
  ObSEArray<uint64_t, 8> key_ids;
  OZ(table.get_rowkey_info().get_column_ids(key_ids));
  if (OB_SUCC(ret) && table.is_table_without_pk() && table.is_partitioned_table()) {
    ObSEArray<uint64_t, 8> partition_ids;
    OZ(table.get_partition_key_info().get_column_ids(partition_ids));
    if (table.get_subpartition_key_info().get_size() > 0) {
      OZ(table.get_subpartition_key_info().get_column_ids(partition_ids));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < partition_ids.count(); ++i) {
      if (!has_exist_in_array(key_ids, partition_ids.at(i))) { OZ(key_ids.push_back(partition_ids.at(i))); }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < key_ids.count(); ++i) {
    const uint64_t id = key_ids.at(i);
    const ObColumnSchemaV2 *key = table.get_column_schema(id);
    if (NULL == key) { ret = OB_SCHEMA_ERROR; }
    else {
      if (i > 0) { OZ(query.append(",")); }
      OZ(identifier(query, key->get_column_name_str()));
    }
  }
  OZ(query.append_fmt(" FROM %s WHERE ", table_name.ptr()));
  OZ(identifier(query, spec->get_column_name_str()));
  OZ(query.append_fmt(" IS NULL LIMIT %lld", static_cast<long long>(batch_limit)));
  ObMySQLTransaction trans;
  if (OB_SUCC(ret)) { OZ(trans.start(GCTX.sql_proxy_)); }
  if (OB_SUCC(ret)) {
    WorkerSession context(trans);
    if (NULL == context.session_) { ret = OB_ERR_UNEXPECTED; }
    else {
      context.session_->set_ai_column_context(table.get_table_id(), false, ObString());
      context.session_->set_sql_mode(DEFAULT_MYSQL_MODE | SMO_STRICT_ALL_TABLES);
      int64_t count = 0;
      {
        ObISQLClient::ReadResult result;
        OZ(trans.read(result, query.ptr()));
        if (OB_SUCC(ret) && NULL == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
        while (OB_SUCC(ret)) {
          const int next = result.get_result()->next();
          if (next == OB_ITER_END) { break; }
          else if (next != OB_SUCCESS) { ret = next; }
          else {
            if (count++ > 0) { OZ(keys.append(" OR ")); }
            OZ(keys.append("("));
            for (int64_t i = 0; OB_SUCC(ret) && i < key_ids.count(); ++i) {
              const uint64_t id = key_ids.at(i);
              ObObj value;
              char *literal = NULL;
              int64_t length = 0;
              int64_t pos = 0;
              OZ(result.get_result()->get_obj(i, value));
              ObObjPrintParams params;
              params.character_hex_safe_represent_ = true;
              params.binary_string_print_hex_ = true;
              OZ(value.print_sql_literal(literal, length, pos, alloc, params));
              if (i > 0) { OZ(keys.append(" AND ")); }
              OZ(identifier(keys, table.get_column_schema(id)->get_column_name_str()));
              // Partition inputs can be NULL even though primary keys cannot.
              OZ(keys.append(" <=> "));
              OZ(keys.append(literal, pos));
            }
            OZ(keys.append(")"));
          }
        }
      }
      if (OB_SUCC(ret) && count > 0) {
        ObSqlString retry;
        OZ(retry.append_fmt("ALTER TABLE %s RETRY AI COLUMN ", table_name.ptr()));
        OZ(identifier(retry, column.get_column_name_str()));
        OZ(retry.append(" WHERE "));
        OZ(identifier(retry, spec->get_column_name_str()));
        OZ(retry.append_fmt(" IS NULL AND (%s)", keys.ptr()));
        int64_t rows = 0;
        OZ(trans.write(retry.ptr(), rows));
      }
      complete = OB_SUCC(ret) && count < batch_limit;
    }
  }
  if (trans.is_started()) {
    const int end_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end_ret; }
  }
  return ret;
}

int seed_uninitialized_rows(std::vector<TableSeedProgress> &progress)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObSEArray<const ObTableSchema *, 64> tables;
  OZ(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(guard));
  OZ(guard.get_table_schemas_in_runtime(tables));
  std::vector<TableSeedProgress> next_progress;
  for (int64_t i = 0; OB_SUCC(ret) && i < tables.count(); ++i) {
    const ObTableSchema *table = tables.at(i);
    if (NULL == table || !table->is_user_table() || table->is_user_hidden_table()) { continue; }
    bool complete = false;
    for (const TableSeedProgress &p : progress) {
      complete = complete || (p.table_id_ == table->get_table_id()
          && p.schema_version_ == table->get_schema_version() && p.complete_);
    }
    if (!complete) {
      complete = true;
      for (ObTableSchema::const_column_iterator it = table->column_begin();
           OB_SUCC(ret) && it != table->column_end(); ++it) {
        if (NULL != *it && (*it)->is_ai_column()) {
          bool column_complete = false;
          OZ(seed_column_rows(guard, *table, **it, column_complete));
          complete = complete && column_complete;
        }
      }
    }
    next_progress.push_back({table->get_table_id(), table->get_schema_version(), complete});
  }
  // A failed transaction must never mark an unfinished schema as scanned.
  if (OB_SUCC(ret)) { progress.swap(next_progress); }
  return ret;
}
} // namespace

int AIColumnWorker::init()
{
  set_run_wrapper(share::server_runtime());
  return ObThreadPool::init();
}

int AIColumnWorker::poll()
{
  int ret = OB_SUCCESS;
  std::vector<ColumnTask> tasks;
  {
    ObISQLClient::ReadResult result;
    // Finished members wait without blocking runnable members of their group.
    OZ(GCTX.sql_proxy_->read(result,
        "SELECT t.* FROM oceanbase.__all_ai_column_task t WHERE "
        "(t.state IN (0,3,4) AND t.eligible_ts <= NOW(6) AND "
        " t.lease_until_us <= CAST(UNIX_TIMESTAMP(NOW(6))*1000000 AS SIGNED)) OR "
        "(t.state IN (1,2) AND NOT EXISTS (SELECT 1 FROM oceanbase.__all_ai_column_task g "
        " WHERE g.table_id=t.table_id AND g.rowkey_bin=t.rowkey_bin AND g.request_id=t.request_id "
        " AND g.state NOT IN (1,2))) ORDER BY t.eligible_us,t.table_id,t.column_id LIMIT 8"));
    if (OB_SUCC(ret) && NULL == result.get_result()) { ret = OB_ERR_UNEXPECTED; }
    while (OB_SUCC(ret)) {
      const int next = result.get_result()->next();
      if (next == OB_ITER_END) { break; }
      else if (next != OB_SUCCESS) { ret = next; }
      else {
        ColumnTask task;
        OZ(load_task(*result.get_result(), task));
        if (OB_SUCC(ret)) { tasks.push_back(task); }
      }
    }
  }
  for (size_t i = 0; OB_SUCC(ret) && !has_set_stop() && i < tasks.size(); ++i) {
    const int task_ret = execute_task(tasks[i]);
    if (task_ret != OB_SUCCESS) { LOG_WARN("AI column task deferred", K(task_ret)); }
  }
  return ret;
}

void AIColumnWorker::run1()
{
  lib::set_thread_name("AIColumn");
  int64_t next_seed_us = 0;
  std::vector<TableSeedProgress> seed_progress;
  while (!has_set_stop()) {
    if (NULL != GCTX.sql_proxy_ && NULL != GCTX.schema_service_
        && !GCTX.in_bootstrap_ && GCTX.start_service_time_ > 0) {
      const int ret = poll();
      if (OB_SUCCESS != ret && REACH_TIME_INTERVAL(10000000)) { LOG_WARN("AI column scan deferred", K(ret)); }
      const int64_t now = ObTimeUtility::current_time();
      if (get_thread_idx() == 0 && now >= next_seed_us) {
        const int seed_ret = seed_uninitialized_rows(seed_progress);
        if (OB_SUCCESS != seed_ret && REACH_TIME_INTERVAL(10000000)) {
          LOG_WARN("AI column initialization deferred", K(seed_ret));
        }
        next_seed_us = now + 1000000;
      }
    }
    usleep(100000);
  }
}
} // namespace observer
} // namespace oceanbase
