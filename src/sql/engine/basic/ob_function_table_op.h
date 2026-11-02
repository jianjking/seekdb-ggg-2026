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

#ifndef OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_
#define OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_chunk_datum_store.h"
#include "sql/engine/basic/file_sql/ob_file_sql_common.h"
#include "lib/charset/ob_charset.h"
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{

namespace filesql
{
class ObFileSqlReader;
}

class ObExpr;
class ObFunctionTableSpec : public ObOpSpec
{
OB_UNIS_VERSION_V(1);
public:
  ObFunctionTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), value_expr_(nullptr), column_exprs_(alloc), has_correlated_expr_(false),
      is_file_scan_(false), file_path_(), file_format_(0), file_delimiter_(','), file_quote_('"'),
      file_escape_('"'), file_has_header_(-1), file_sample_size_(-1), file_skip_rows_(0),
      file_null_str_(), column_indexes_(alloc), file_max_object_size_(64LL * 1024 * 1024)
  {}
  ObExpr *value_expr_;
  common::ObFixedArray<ObExpr*, common::ObIAllocator> column_exprs_;
  bool has_correlated_expr_;
  bool is_file_scan_;
  common::ObString file_path_;
  int64_t file_format_;
  int64_t file_delimiter_;
  int64_t file_quote_;
  int64_t file_escape_;
  int64_t file_has_header_;
  int64_t file_sample_size_;
  int64_t file_skip_rows_;
  common::ObString file_null_str_;
  common::ObFixedArray<int64_t, common::ObIAllocator> column_indexes_;
  int64_t file_max_object_size_;
};

class ObFunctionTableOp : public ObOperator
{
public:
  ObFunctionTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input),
    node_idx_(0),
    already_calc_(false),
    row_count_(0),
    col_count_(0),
    value_(),
    value_table_(NULL),
    file_reader_(NULL),
    file_row_(),
    file_schema_(NULL),
    file_index_(0),
    glob_files_()
  {}

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  //virtual int inner_get_next_batch(int64_t max_row_cnt) override;
  virtual int inner_close() override;
  virtual void destroy() override;
private:
  int inner_get_next_row_udf();
  int inner_get_next_row_sys_func();
  int inner_get_next_row_file_scan();
  int open_file_scan();
  int build_file_options(filesql::ObFileSqlOptions &options) const;
  int assign_expr_value(const int64_t expr_index, const filesql::ObFileSqlValue &value);
  int get_current_result(common::ObObj &result);
  int64_t node_idx_;
  bool already_calc_;
  int64_t row_count_;
  int64_t col_count_;
  common::ObObj value_;
  pl::ObPLCollection *value_table_;
  filesql::ObFileSqlReader *file_reader_;
  std::vector<filesql::ObFileSqlValue> file_row_;
  filesql::ObFileSqlSchema *file_schema_;
  int64_t file_index_;
  std::vector<std::string> glob_files_;
  int (ObFunctionTableOp::*next_row_func_)();
};

} // end namespace sql
} // end namespace oceanbase

#endif /* OCEANBASE_BASIC_OB_FUNCTION_TABLE_OP_H_ */
