#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_calc_partition_id.h"
#include "sql/ob_sql_context.h"
#include "storage/tx/ob_timestamp_access.h"
#include "share/ai_service/ai_column_task_sql_service.h"
#include "share/ob_dml_sql_splicer.h"

namespace oceanbase
{
using namespace common;
using namespace share;
using namespace share::schema;
namespace sql
{
namespace
{
int64_t ai_column_index(const ObDMLBaseCtDef &ctdef, uint64_t id)
{
  int64_t index = OB_INVALID_INDEX;
  for (int64_t i = 0; index == OB_INVALID_INDEX && i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == id) { index = i; }
  }
  return index;
}

int ai_schema(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, const ObTableSchema *&schema)
{
  int ret = OB_SUCCESS;
  ObSqlCtx *sql_ctx = op.get_exec_ctx().get_sql_ctx();
  schema = NULL;
  if (OB_ISNULL(sql_ctx) || OB_ISNULL(sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql_ctx->schema_guard_->get_table_schema(ctdef.das_base_ctdef_.index_tid_, schema))) {
  } else if (OB_ISNULL(schema)) {
    ret = OB_SCHEMA_ERROR;
  }
  return ret;
}

int ai_value_changed(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op, uint64_t id, bool &changed)
{
  int ret = OB_SUCCESS;
  const int64_t index = ai_column_index(ctdef, id);
  if (index != OB_INVALID_INDEX) {
    ObDatum *before = NULL;
    ObDatum *after = NULL;
    if (index >= ctdef.old_row_.count() || index >= ctdef.new_row_.count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ctdef.old_row_.at(index)->eval(op.get_eval_ctx(), before))) {
    } else if (OB_FAIL(ctdef.new_row_.at(index)->eval(op.get_eval_ctx(), after))) {
    } else if (!before->is_null() && !after->is_null()
               && is_lob_storage(ctdef.new_row_.at(index)->obj_meta_.get_type())) {
      int64_t comparison = 0;
      OZ(ObDMLService::check_lob_column_changed(op.get_eval_ctx(),
          *ctdef.old_row_.at(index), *before, *ctdef.new_row_.at(index), *after, comparison));
      changed = changed || comparison != 0;
    } else {
      changed = changed || !ObDatum::binary_equal(*before, *after);
    }
  }
  return ret;
}

int ai_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = sql.append("`");
  for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("``")); }
    else { OZ(sql.append(name.ptr() + i, 1)); }
  }
  OZ(sql.append("`"));
  return ret;
}

// After batch restoration, only the captured row values are authoritative.
// Recompute partition consumers without clearing those values or their parents.
int ai_clear_partition_cache(ObExpr *expr, const ExprFixedArray &row, ObEvalCtx &ctx)
{
  int ret = OB_SUCCESS;
  if (NULL == expr) { ret = OB_ERR_UNEXPECTED; }
  else if (!has_exist_in_array(row, expr)) {
    for (int64_t i = 0; OB_SUCC(ret) && i < expr->arg_cnt_; ++i) {
      OZ(SMART_CALL(ai_clear_partition_cache(expr->args_[i], row, ctx)));
    }
    if (expr->is_batch_result()) { expr->get_evaluated_flags(ctx).unset(ctx.get_batch_idx()); }
    else { expr->get_eval_info(ctx).clear_evaluated_flag(); }
  }
  return ret;
}

int ai_row_tablet(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op,
                  const ExprFixedArray &row, ObTabletID &tablet)
{
  int ret = OB_SUCCESS;
  ObExpr *partition = NULL;
  if (ctdef.dml_type_ == DAS_OP_TABLE_INSERT) {
    const ObInsCtDef &ins = static_cast<const ObInsCtDef &>(ctdef);
    if (NULL != ins.multi_ctdef_) { partition = ins.multi_ctdef_->calc_part_id_expr_; }
  } else if (ctdef.dml_type_ == DAS_OP_TABLE_UPDATE) {
    const ObUpdCtDef &upd = static_cast<const ObUpdCtDef &>(ctdef);
    if (NULL != upd.multi_ctdef_) {
      if (&row == &ctdef.old_row_) { partition = upd.multi_ctdef_->calc_part_id_old_; }
      else { partition = upd.multi_ctdef_->calc_part_id_new_; }
    }
  } else if (ctdef.dml_type_ == DAS_OP_TABLE_DELETE) {
    const ObDelCtDef &del = static_cast<const ObDelCtDef &>(ctdef);
    if (NULL != del.multi_ctdef_) { partition = del.multi_ctdef_->calc_part_id_expr_; }
  } else { ret = OB_ERR_UNEXPECTED; }
  if (OB_SUCC(ret) && NULL != partition) {
    ObObjectID partition_id = OB_INVALID_ID;
    OZ(ai_clear_partition_cache(partition, row, op.get_eval_ctx()));
    OZ(ObExprCalcPartitionBase::calc_part_and_tablet_id(partition, op.get_eval_ctx(), partition_id, tablet));
  } else if (OB_SUCC(ret)) {
    ObTableModifyOpInput *input = op.get_input();
    if (NULL == input || NULL == input->get_tablet_loc()) { ret = OB_ERR_UNEXPECTED; }
    else { tablet = input->get_tablet_loc()->tablet_id_; }
  }
  if (OB_SUCC(ret) && !tablet.is_valid()) { ret = OB_ERR_UNEXPECTED; }
  return ret;
}

int ai_rowkey(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op,
              const ObTableSchema &schema, const ExprFixedArray &row,
              ObString &binary, ObSqlString &predicate)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObObj, 8> cells;
  const ObRowkeyInfo &info = schema.get_rowkey_info();
  ObIAllocator &alloc = op.get_exec_ctx().get_allocator();
  for (int64_t i = 0; OB_SUCC(ret) && i < info.get_size(); ++i) {
    uint64_t id = OB_INVALID_ID;
    ObDatum *datum = NULL;
    ObObj value;
    char *literal = NULL;
    int64_t length = 0;
    int64_t pos = 0;
    OZ(info.get_column_id(i, id));
    const int64_t index = ai_column_index(ctdef, id);
    const ObColumnSchemaV2 *column = schema.get_column_schema(id);
    if (OB_FAIL(ret)) {
    } else if (index < 0 || index >= row.count() || OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(row.at(index)->eval(op.get_eval_ctx(), datum))) {
    } else if (OB_FAIL(datum->to_obj(value, row.at(index)->obj_meta_))) {
    } else if (OB_FAIL(cells.push_back(value))) {
    } else if (OB_FAIL(value.print_sql_literal(literal, length, pos, alloc, op.get_obj_print_params()))) {
    } else {
      if (i > 0) { OZ(predicate.append(" AND ")); }
      OZ(ai_identifier(predicate, column->get_column_name_str()));
      OZ(predicate.append(" = "));
      OZ(predicate.append(literal, pos));
    }
  }
  if (OB_SUCC(ret) && schema.is_table_without_pk() && schema.is_partitioned_table()) {
    // Hidden primary keys are unique per Tablet, not across all partitions.
    ObTabletID tablet;
    ObObj value;
    OZ(ai_row_tablet(ctdef, op, row, tablet));
    value.set_uint64(tablet.id());
    OZ(cells.push_back(value));
  }
  if (OB_SUCC(ret)) {
    ObRowkey key(cells.get_data(), cells.count());
    const int64_t length = key.get_serialize_size();
    char *buffer = static_cast<char *>(alloc.alloc(length));
    int64_t pos = 0;
    if (OB_ISNULL(buffer)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else if (OB_FAIL(key.serialize(buffer, length, pos))) { }
    else { binary.assign_ptr(buffer, static_cast<int32_t>(pos)); }
  }
  return ret;
}

int ai_delete_tasks(ObTableModifyOp &op, uint64_t table_id, const ObString &key, uint64_t column_id)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer predicates;
  ObSqlString sql;
  OZ(predicates.add_pk_column("table_id", table_id));
  OZ(predicates.add_pk_column("rowkey_bin", ObHexEscapeSqlStr(key)));
  if (column_id != OB_INVALID_ID) { OZ(predicates.add_pk_column("column_id", column_id)); }
  OZ(predicates.splice_delete_sql("oceanbase.__all_ai_column_task", sql));
  OZ(op.execute_write(sql.ptr()));
  return ret;
}
}

int ObDMLService::prepare_ai_column_row(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op,
                                       bool row_identity_changed)
{
  int ret = OB_SUCCESS;
  if (!ctdef.has_ai_columns_ || ctdef.new_row_.empty()) { return ret; }
  const ObTableSchema *schema = NULL;
  ObSEArray<const ObColumnSchemaV2 *, 8> changed;
  bool key_changed = row_identity_changed;
  OZ(ai_schema(ctdef, op, schema));
  const bool heap_move = OB_SUCC(ret) && row_identity_changed && schema->is_table_without_pk();
  // An internal heap key/VID reassignment is not a change to model inputs.
  // Explicit business primary-key updates still regenerate all AI columns.
  if (heap_move) { key_changed = false; }
  if (OB_SUCC(ret) && !ctdef.old_row_.empty() && !heap_move) {
    for (int64_t i = 0; OB_SUCC(ret) && i < schema->get_rowkey_info().get_size(); ++i) {
      uint64_t id = OB_INVALID_ID;
      OZ(schema->get_rowkey_info().get_column_id(i, id));
      OZ(ai_value_changed(ctdef, op, id, key_changed));
    }
  }
  if (OB_SUCC(ret)) {
    for (ObTableSchema::const_column_iterator it = schema->column_begin();
         OB_SUCC(ret) && it != schema->column_end(); ++it) {
      const ObColumnSchemaV2 *column = *it;
      if (NULL != column && column->is_ai_column()
          && ai_column_index(ctdef, column->get_column_id()) != OB_INVALID_INDEX) {
        bool affected = key_changed || ctdef.old_row_.empty()
            || column->get_column_id() == ctdef.ai_column_retry_id_;
        ObSEArray<uint64_t, 8> dependencies;
        OZ(column->get_ai_dependency_ids(*schema, dependencies));
        for (int64_t i = 0; OB_SUCC(ret) && !affected && i < dependencies.count(); ++i) {
          OZ(ai_value_changed(ctdef, op, dependencies.at(i), affected));
        }
        if (OB_SUCC(ret) && affected) { OZ(changed.push_back(column)); }
      }
    }
  }
  // Move still-pending members with a superseded member, preserving unchanged
  // READY/FAILED results. Otherwise the old group's captured membership strands it.
  if (OB_SUCC(ret) && (!changed.empty() || heap_move) && !ctdef.old_row_.empty()) {
    for (ObTableSchema::const_column_iterator it = schema->column_begin();
         OB_SUCC(ret) && it != schema->column_end(); ++it) {
      const ObColumnSchemaV2 *column = *it;
      if (NULL != column && column->is_ai_column() && !has_exist_in_array(changed, column)) {
        const int64_t index = ai_column_index(ctdef, column->get_ai_state_column_id());
        ObDatum *state = NULL;
        if (index >= 0) {
          OZ(ctdef.old_row_.at(index)->eval(op.get_eval_ctx(), state));
          if (OB_SUCC(ret) && (state->is_null() || state->get_int() == OB_AI_COLUMN_STATE_PENDING
              || state->get_int() == OB_AI_COLUMN_STATE_RETRY_WAIT)) {
            OZ(changed.push_back(column));
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && !changed.empty()) {
    // The timestamp allocator reserves ranges durably. UUID_SHORT can reuse an
    // identity after a restart within the same second.
    int64_t request = 0;
    transaction::ObTimestampAccess *timestamps = server_service<transaction::ObTimestampAccess>();
    if (OB_ISNULL(timestamps)) { ret = OB_NOT_INIT; }
    else { OZ(timestamps->get_number(request)); }
    for (int64_t i = 0; OB_SUCC(ret) && i < changed.count(); ++i) {
      const ObColumnSchemaV2 &column = *changed.at(i);
      const uint64_t ids[] = {column.get_column_id(), column.get_ai_state_column_id(),
          column.get_ai_request_column_id(), column.get_ai_result_spec_column_id()};
      for (int64_t j = 0; OB_SUCC(ret) && j < 4; ++j) {
        const int64_t index = ai_column_index(ctdef, ids[j]);
        if (index < 0 || index >= ctdef.new_row_.count()
            || (!ctdef.old_row_.empty() && ctdef.old_row_.at(index) == ctdef.new_row_.at(index))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ObExpr *expr = ctdef.new_row_.at(index);
          // Clear consumers, retaining already evaluated business assignments.
          op.clear_dml_evaluated_flag(expr->parent_cnt_, expr->parents_);
          ObDatum &datum = expr->locate_datum_for_write(op.get_eval_ctx());
          if (j == 0) { datum.set_null(); }
          else if (j == 1) { datum.set_int(OB_AI_COLUMN_STATE_PENDING); }
          else if (j == 2) { datum.set_int(request); }
          else { datum.set_int(schema->get_schema_version()); }
          expr->set_evaluated_flag(op.get_eval_ctx());
        }
      }
    }
  }
  return ret;
}

int ObDMLService::prepare_ai_column_moved_row(const ObUpdCtDef &ctdef, const ObUpdRtDef &rtdef,
                                             ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  bool filtered = false;
  OZ(prepare_ai_column_row(ctdef, op, true));
  OZ(check_row_null(ctdef.new_row_, op.get_eval_ctx(), rtdef.cur_row_num_,
      ctdef.assign_columns_, false, false, op));
  OZ(filter_row_for_view_check(ctdef.view_check_exprs_, op.get_eval_ctx(), filtered));
  if (OB_SUCC(ret) && filtered) { ret = OB_ERR_CHECK_OPTION_VIOLATED; }
  OZ(filter_row_for_check_cst(ctdef.check_cst_exprs_, op.get_eval_ctx(), filtered));
  if (OB_SUCC(ret) && filtered) { ret = OB_ERR_CHECK_CONSTRAINT_VIOLATED; }
  return ret;
}

int ObDMLService::persist_ai_column_tasks(const ObDMLBaseCtDef &ctdef, ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  if (!ctdef.has_ai_columns_) { return ret; }
  const ObTableSchema *schema = NULL;
  ObString old_key;
  ObString new_key;
  ObSqlString old_predicate;
  ObSqlString new_predicate;
  ObSEArray<const ObColumnSchemaV2 *, 8> columns;
  ObSEArray<uint64_t, 8> members;
  int64_t request = 0;
  OZ(ai_schema(ctdef, op, schema));
  if (OB_SUCC(ret) && !ctdef.old_row_.empty()) {
    OZ(ai_rowkey(ctdef, op, *schema, ctdef.old_row_, old_key, old_predicate));
  }
  if (OB_SUCC(ret) && !ctdef.new_row_.empty()) {
    OZ(ai_rowkey(ctdef, op, *schema, ctdef.new_row_, new_key, new_predicate));
    for (ObTableSchema::const_column_iterator it = schema->column_begin();
         OB_SUCC(ret) && it != schema->column_end(); ++it) {
      const ObColumnSchemaV2 *column = *it;
      if (NULL != column && column->is_ai_column()) {
        const int64_t index = ai_column_index(ctdef, column->get_ai_request_column_id());
        ObDatum *before = NULL;
        ObDatum *after = NULL;
        if (index == OB_INVALID_INDEX) {
          // Unrelated updates need not project the AI companions.
        } else if (index >= ctdef.new_row_.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ctdef.new_row_.at(index)->eval(op.get_eval_ctx(), after))) {
        } else if (!ctdef.old_row_.empty()
                   && OB_FAIL(ctdef.old_row_.at(index)->eval(op.get_eval_ctx(), before))) {
        } else if (NULL == before || !ObDatum::binary_equal(*before, *after)) {
          if (after->is_null() || after->get_int() <= 0
              || (request != 0 && request != after->get_int())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            request = after->get_int();
            OZ(columns.push_back(column));
            OZ(members.push_back(column->get_column_id()));
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && !old_key.empty() && !new_key.empty()
      && old_key != new_key && columns.empty() && !schema->is_table_without_pk()) {
    // A business primary-key change must have prepared a generation. Pure
    // heap-key movement can preserve already published terminal results.
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("AI column row identity changed without regeneration", K(ret));
  }
  if (OB_SUCC(ret) && (ctdef.new_row_.empty() || !columns.empty() || old_key != new_key)) {
    if (OB_FAIL(op.begin_nested_session(true))) {
    } else {
      // The external-session connection shares the business transaction.
      // Propagate errors to the DML statement; deliberately never commit here.
      if (!old_key.empty() && (ctdef.new_row_.empty() || old_key != new_key)) {
        OZ(ai_delete_tasks(op, schema->get_table_id(), old_key, OB_INVALID_ID));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
        const ObColumnSchemaV2 &column = *columns.at(i);
        AIColumnTaskSnapshot task;
        task.identity_.table_id_ = schema->get_table_id();
        task.identity_.column_id_ = column.get_column_id();
        task.identity_.request_id_ = request;
        task.identity_.definition_version_ = schema->get_schema_version();
        task.identity_.rowkey_ = std::string_view(new_key.ptr(), new_key.length());
        task.max_retries_ = column.get_ai_max_retries();
        ObSqlString sql;
        OZ(ai_delete_tasks(op, schema->get_table_id(), new_key, column.get_column_id()));
        OZ(AIColumnTaskSqlService::build_enqueue_sql(task, new_predicate.string(),
            column.get_ai_generation_expr(), column.get_ai_normalized_expr(), members, sql));
        OZ(op.execute_write(sql.ptr()));
      }
      const int end_ret = op.end_nested_session();
      if (OB_SUCC(ret)) { ret = end_ret; }
    }
  }
  return ret;
}
} // namespace sql
} // namespace oceanbase
