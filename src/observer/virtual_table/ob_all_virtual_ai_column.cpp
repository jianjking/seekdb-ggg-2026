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

#define USING_LOG_PREFIX SERVER

#include "observer/virtual_table/ob_all_virtual_ai_column.h"
#include "lib/charset/ob_charset.h"
#include "common/rowkey/ob_rowkey.h"
#include "share/schema/ob_table_schema.h"
#include "share/ob_errno.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

ObAllVirtualAIColumn::ObAllVirtualAIColumn()
  : ObVirtualTableScannerIterator(),
    table_ids_(),
    columns_(),
    idx_(0),
    opened_(false)
{
}

ObAllVirtualAIColumn::~ObAllVirtualAIColumn()
{
}

void ObAllVirtualAIColumn::reset()
{
  table_ids_.reuse();
  columns_.reuse();
  idx_ = 0;
  opened_ = false;
  ObVirtualTableScannerIterator::reset();
}

bool ObAllVirtualAIColumn::row_matches_key_ranges(const uint64_t table_id,
                                                  const uint64_t column_id) const
{
  bool matched = key_ranges_.empty();
  if (!matched) {
    ObObj key_objs[2];
    key_objs[0].set_int(static_cast<int64_t>(table_id));
    key_objs[1].set_int(static_cast<int64_t>(column_id));
    const ObRowkey rowkey(key_objs, 2);
    for (int64_t i = 0; !matched && i < key_ranges_.count(); ++i) {
      const ObNewRange &range = key_ranges_.at(i);
      const int left_cmp = range.start_key_.compare(rowkey);
      const int right_cmp = range.end_key_.compare(rowkey);
      const bool left_ok = range.start_key_.is_min_row()
          || left_cmp < 0 || (left_cmp == 0 && range.border_flag_.inclusive_start());
      const bool right_ok = range.end_key_.is_max_row()
          || right_cmp > 0 || (right_cmp == 0 && range.border_flag_.inclusive_end());
      matched = (left_ok && right_ok);
    }
  }
  return matched;
}

int ObAllVirtualAIColumn::collect_ai_columns()
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObTableSchema *, 128> table_schemas;
  if (OB_FAIL(schema_guard_->get_table_schemas_in_runtime(table_schemas))) {
    SERVER_LOG(WARN, "failed to get table schemas in runtime", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schemas.count(); ++i) {
    const ObTableSchema *table = table_schemas.at(i);
    if (OB_ISNULL(table)) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "null table schema", K(ret), K(i));
    } else {
      ObTableSchema::const_column_iterator iter = table->column_begin();
      ObTableSchema::const_column_iterator end = table->column_end();
      for (; OB_SUCC(ret) && iter != end; ++iter) {
        const ObColumnSchemaV2 *column = *iter;
        if (NULL != column
            && column->is_ai_column()
            && row_matches_key_ranges(table->get_table_id(), column->get_column_id())) {
          if (OB_FAIL(table_ids_.push_back(table->get_table_id()))) {
            SERVER_LOG(WARN, "failed to record AI column table id", K(ret));
          } else if (OB_FAIL(columns_.push_back(column))) {
            SERVER_LOG(WARN, "failed to record AI column", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAllVirtualAIColumn::inner_open()
{
  int ret = OB_SUCCESS;
  table_ids_.reuse();
  columns_.reuse();
  idx_ = 0;
  opened_ = false;
  if (OB_UNLIKELY(NULL == schema_guard_ || NULL == allocator_ || NULL == cur_row_.cells_)) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "data member doesn't init", K(ret),
               K(schema_guard_), K(allocator_), K(cur_row_.cells_));
  } else if (OB_FAIL(collect_ai_columns())) {
    SERVER_LOG(WARN, "failed to collect AI columns", K(ret));
  } else {
    opened_ = true;
  }
  return ret;
}

int ObAllVirtualAIColumn::fill_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *column = columns_.at(idx_);
  const uint64_t table_id = table_ids_.at(idx_);
  const int64_t col_count = output_column_ids_.count();
  const ObCollationType collation_type =
      ObCharset::get_default_collation(ObCharset::get_default_charset());
  ++idx_;
  if (OB_ISNULL(column)) {
    ret = OB_ERR_UNEXPECTED;
    SERVER_LOG(WARN, "null AI column schema", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
    const uint64_t col_id = output_column_ids_.at(i);
    ObObj &cell = cur_row_.cells_[i];
    switch (col_id) {
      case TABLE_ID:
        cell.set_int(static_cast<int64_t>(table_id));
        break;
      case COLUMN_ID:
        cell.set_int(static_cast<int64_t>(column->get_column_id()));
        break;
      case COLUMN_NAME:
        cell.set_varchar(column->get_column_name_str());
        cell.set_collation_type(collation_type);
        break;
      case SYNC_MODE:
        cell.set_int(column->get_ai_sync_mode());
        break;
      case MAX_RETRIES:
        cell.set_int(column->get_ai_max_retries());
        break;
      case FUNC_TYPE:
        cell.set_int(column->get_ai_func_type());
        break;
      case MODEL_NAME:
        cell.set_varchar(column->get_ai_model_name());
        cell.set_collation_type(collation_type);
        break;
      case GENERATION_EXPR:
        cell.set_varchar(column->get_ai_generation_expr());
        cell.set_collation_type(collation_type);
        break;
      case NORMALIZED_EXPR:
        cell.set_varchar(column->get_ai_normalized_expr());
        cell.set_collation_type(collation_type);
        break;
      case STATE_COLUMN_ID:
        cell.set_int(static_cast<int64_t>(column->get_ai_state_column_id()));
        break;
      case REQUEST_COLUMN_ID:
        cell.set_int(static_cast<int64_t>(column->get_ai_request_column_id()));
        break;
      case RESULT_SPEC_COLUMN_ID:
        cell.set_int(static_cast<int64_t>(column->get_ai_result_spec_column_id()));
        break;
      case SCHEMA_VERSION:
        cell.set_int(column->get_schema_version());
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        SERVER_LOG(WARN, "unexpected column id", K(ret), K(col_id));
        break;
    }
  }
  if (OB_SUCC(ret)) {
    row = &cur_row_;
  }
  return ret;
}

int ObAllVirtualAIColumn::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!opened_)) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "virtual table is not opened", K(ret));
  } else if (idx_ >= columns_.count()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(fill_row(row))) {
    SERVER_LOG(WARN, "failed to fill AI column row", K(ret), K(idx_));
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
