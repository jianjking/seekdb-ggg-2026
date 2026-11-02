/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/ob_function_table_op.h"
#include <cstdlib>
#include "sql/engine/ob_exec_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/basic/file_sql/ob_file_sql_common.h"
#include "sql/engine/basic/file_sql/ob_file_sql_reader.h"


namespace oceanbase
{
using namespace common;
namespace sql
{

// OB_DELETE(T, ...) expands to `ptr->~T()`, so T must be visible unqualified.
using filesql::ObFileSqlReader;
using filesql::ObFileSqlSchema;

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    is_file_scan_, file_path_, file_format_, file_delimiter_, file_quote_,
                    file_escape_, file_has_header_, file_sample_size_, file_skip_rows_,
                    file_null_str_, column_indexes_, file_max_object_size_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (MY_SPEC.is_file_scan_) {
    if (OB_FAIL(open_file_scan())) {
      LOG_WARN("failed to open file scan", K(ret), K(MY_SPEC.file_path_));
    } else {
      next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file_scan;
    }
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.is_file_scan_) {
      file_index_ = 0;
      if (NULL != file_reader_) {
        if (OB_FAIL(file_reader_->start_scan())) {
          LOG_WARN("failed to rescan file", K(ret));
        }
      }
    } else if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  file_index_ = 0;
  file_row_.clear();
  glob_files_.clear();
  if (NULL != file_reader_) {
    filesql::destroy_file_sql_reader(file_reader_);
    file_reader_ = NULL;
  }
  if (NULL != file_schema_) {
    OB_DELETE(ObFileSqlSchema, "FileSqlSchema", file_schema_);
    file_schema_ = NULL;
  }
  return ret;
}

void ObFunctionTableOp::destroy()
{
  if (NULL != file_reader_) {
    filesql::destroy_file_sql_reader(file_reader_);
    file_reader_ = NULL;
  }
  if (NULL != file_schema_) {
    OB_DELETE(ObFileSqlSchema, "FileSqlSchema", file_schema_);
    file_schema_ = NULL;
  }
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::build_file_options(filesql::ObFileSqlOptions &options) const
{
  int ret = OB_SUCCESS;
  options.format_ = static_cast<filesql::ObFileFormat>(MY_SPEC.file_format_);
  options.delimiter_ = static_cast<char>(MY_SPEC.file_delimiter_);
  options.quote_ = static_cast<char>(MY_SPEC.file_quote_);
  options.escape_ = static_cast<char>(MY_SPEC.file_escape_);
  options.has_header_ = static_cast<int32_t>(MY_SPEC.file_has_header_);
  options.sample_size_ = MY_SPEC.file_sample_size_;
  options.skip_rows_ = MY_SPEC.file_skip_rows_;
  options.maximum_object_size_ = MY_SPEC.file_max_object_size_;
  if (MY_SPEC.file_null_str_.length() > 0) {
    options.null_str_.assign(MY_SPEC.file_null_str_.ptr(),
                             static_cast<size_t>(MY_SPEC.file_null_str_.length()));
  }
  return ret;
}

int ObFunctionTableOp::open_file_scan()
{
  int ret = OB_SUCCESS;
  filesql::ObFileSqlOptions options;
  if (OB_FAIL(build_file_options(options))) {
    LOG_WARN("failed to build file options", K(ret));
  } else if (filesql::FS_FORMAT_GLOB == options.format_) {
    const std::string pattern(MY_SPEC.file_path_.ptr(), MY_SPEC.file_path_.length());
    if (OB_FAIL(filesql::expand_glob_pattern(pattern, glob_files_))) {
      LOG_WARN("failed to expand glob pattern", K(ret), "pattern", pattern.c_str());
    }
    const ObSQLSessionInfo *session = ctx_.get_my_session();
    common::ObString priv;
    std::string secure_dir;
    bool unrestricted = false;
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(session)) {
        ret = OB_NOT_INIT;
      } else if (OB_FAIL(session->get_secure_file_priv(priv))) {
      } else if (OB_FAIL(filesql::resolve_secure_dir(
                     std::string(OB_ISNULL(priv.ptr()) ? "" : priv.ptr(), priv.length()),
                     secure_dir, unrestricted))) {
      } else if (!unrestricted) {
        for (size_t i = 0; OB_SUCC(ret) && i < glob_files_.size(); ++i) {
          filesql::ObFileSqlInput candidate;
          std::string real_path;
          if (OB_FAIL(candidate.open(glob_files_[i], secure_dir, real_path))) {
            LOG_WARN("glob entry is inaccessible or outside secure_file_priv", K(ret));
          }
        }
      }
    }
    file_index_ = 0;
  } else {
    const std::string path(MY_SPEC.file_path_.ptr(), MY_SPEC.file_path_.length());
    std::string secure_dir;
    bool unrestricted = false;
    const ObSQLSessionInfo *session = ctx_.get_my_session();
    if (OB_ISNULL(session)) {
      ret = OB_NOT_INIT;
    } else {
      common::ObString priv;
      if (OB_FAIL(session->get_secure_file_priv(priv))) {
      } else if (OB_FAIL(filesql::resolve_secure_dir(
                   std::string(OB_ISNULL(priv.ptr()) ? "" : priv.ptr(),
                               static_cast<size_t>(priv.length())),
                   secure_dir, unrestricted))) {
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "secure_file_priv is not configured");
      } else if (unrestricted) {
        secure_dir.clear();
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(file_schema_ = OB_NEW(filesql::ObFileSqlSchema, "FileSqlSchema"))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(filesql::create_file_sql_reader(options.format_, file_reader_))) {
        LOG_WARN("failed to create file reader", K(ret));
      } else if (OB_FAIL(file_reader_->open(path, options, secure_dir))) {
        LOG_WARN("failed to open data file", K(ret), "path", path.c_str());
      } else if (OB_FAIL(file_reader_->infer_schema(*file_schema_))) {
        LOG_WARN("failed to infer file schema", K(ret), "path", path.c_str(),
                 "detail", file_reader_->error_message().c_str(),
                 "line", file_reader_->error_line(), "column", file_reader_->error_column());
        ret = OB_INVALID_DATA;
      } else {
        std::vector<int64_t> projection;
        for (int64_t i = 0; i < MY_SPEC.column_indexes_.count(); ++i) {
          projection.push_back(MY_SPEC.column_indexes_.at(i));
        }
        if (OB_FAIL(file_reader_->set_projection(projection))) {
        } else if (OB_FAIL(file_reader_->start_scan())) {
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::assign_expr_value(const int64_t expr_index,
                                         const filesql::ObFileSqlValue &value)
{
  int ret = OB_SUCCESS;
  if (expr_index < 0 || expr_index >= MY_SPEC.column_exprs_.count()) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObExpr *expr = MY_SPEC.column_exprs_.at(expr_index);
    ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
    if (value.null_) {
      datum.set_null();
    } else {
      ObObj obj;
      switch (value.type_) {
        case filesql::FS_TYPE_BIGINT: {
          obj.set_int(value.int_value_);
          break;
        }
        case filesql::FS_TYPE_DOUBLE: {
          obj.set_double(value.double_value_);
          break;
        }
        case filesql::FS_TYPE_BOOLEAN: {
          obj.set_tinyint(static_cast<int8_t>(0 != value.int_value_ ? 1 : 0));
          break;
        }
        case filesql::FS_TYPE_DATE: {
          obj.set_date(value.date_value_);
          break;
        }
        case filesql::FS_TYPE_DATETIME: {
          obj.set_datetime(value.datetime_value_);
          break;
        }
        default: {
          obj.set_varchar(value.str_ptr_, value.str_len_);
          break;
        }
      }
      if (obj.get_type() != expr->obj_meta_.get_type()) {
        ret = OB_INVALID_DATA;
        LOG_WARN("file column type changed between resolution and execution", K(ret),
                 "actual_type", obj.get_type(), "planned_type", expr->obj_meta_.get_type());
      } else {
        ret = datum.from_obj(obj, expr->obj_datum_map_);
      }
    }
    if (OB_SUCC(ret)) {
      expr->set_evaluated_projected(eval_ctx_);
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_file_scan()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (filesql::FS_FORMAT_GLOB == static_cast<filesql::ObFileFormat>(MY_SPEC.file_format_)) {
    if (file_index_ >= static_cast<int64_t>(glob_files_.size())) {
      ret = OB_ITER_END;
    } else if (MY_SPEC.column_exprs_.empty()) {
      ++file_index_;
    } else {
      const std::string &path = glob_files_[static_cast<size_t>(file_index_)];
      ObObj obj;
      obj.set_varchar(path.data(), static_cast<int32_t>(path.size()));
      ObExpr *expr = MY_SPEC.column_exprs_.at(0);
      ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
      if (OB_FAIL(datum.from_obj(obj, expr->obj_datum_map_))) {
      } else {
        expr->set_evaluated_projected(eval_ctx_);
        ++file_index_;
      }
    }
  } else if (OB_ISNULL(file_reader_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(file_reader_->next_row(file_row_))) {
    if (OB_ITER_END != ret) {
      LOG_WARN("failed to read row from file", K(ret), "detail",
               file_reader_->error_message().c_str(), "line", file_reader_->error_line(),
               "column", file_reader_->error_column());
    }
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      const int64_t column_index = (i < MY_SPEC.column_indexes_.count())
                                       ? MY_SPEC.column_indexes_.at(i)
                                       : i;
      if (column_index < 0 || column_index >= static_cast<int64_t>(file_row_.size())) {
        ObExpr *expr = MY_SPEC.column_exprs_.at(i);
        expr->locate_datum_for_write(eval_ctx_).set_null();
        expr->set_evaluated_projected(eval_ctx_);
      } else if (OB_FAIL(assign_expr_value(i, file_row_[static_cast<size_t>(column_index)]))) {
        LOG_WARN("failed to convert file value", K(ret), K(i), K(column_index));
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
