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

#pragma once

#include "observer/virtual_table/ob_virtual_table_scanner_iterator.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObColumnSchemaV2;
}
}
namespace observer
{

// Exposes AI column metadata carried by the in-memory schema.
// Row key: (table_id, column_id).
class ObAllVirtualAIColumn : public common::ObVirtualTableScannerIterator
{
public:
  ObAllVirtualAIColumn();
  virtual ~ObAllVirtualAIColumn();
  virtual void reset() override;
  virtual int inner_open() override;
  virtual int inner_get_next_row(common::ObNewRow *&row) override;

private:
  enum AIColumnId
  {
    TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    COLUMN_ID,
    COLUMN_NAME,
    SYNC_MODE,
    MAX_RETRIES,
    FUNC_TYPE,
    MODEL_NAME,
    GENERATION_EXPR,
    NORMALIZED_EXPR,
    STATE_COLUMN_ID,
    REQUEST_COLUMN_ID,
    RESULT_SPEC_COLUMN_ID,
    SCHEMA_VERSION
  };

  // Collect every AI column whose (table_id, column_id) falls into a pushed down
  // key range.  Virtual table iterators apply the ranges themselves.
  int collect_ai_columns();
  bool row_matches_key_ranges(const uint64_t table_id, const uint64_t column_id) const;
  int fill_row(common::ObNewRow *&row);

private:
  common::ObSEArray<uint64_t, 64> table_ids_;
  common::ObSEArray<const share::schema::ObColumnSchemaV2 *, 64> columns_;
  int64_t idx_;
  bool opened_;

  DISALLOW_COPY_AND_ASSIGN(ObAllVirtualAIColumn);
};

} // namespace observer
} // namespace oceanbase
