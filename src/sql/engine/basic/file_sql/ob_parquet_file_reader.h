/**
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

#include <string>
#include <vector>

#include "sql/engine/basic/file_sql/ob_file_sql_reader.h"

namespace oceanbase
{
namespace sql
{
namespace filesql
{

// Minimal but self contained Parquet reader: footer metadata, row groups,
// dictionary and plain pages, definition levels and Snappy decompression.
// Nested schemas are rejected instead of being silently flattened.
class ObParquetFileReader : public ObFileSqlReader
{
public:
  ObParquetFileReader();
  virtual ~ObParquetFileReader() {}

  virtual int open(const std::string &path, const ObFileSqlOptions &opt,
                   const std::string &secure_dir) override;
  virtual int infer_schema(ObFileSqlSchema &schema) override;
  virtual int set_projection(const std::vector<int64_t> &cols) override;
  virtual int start_scan() override;
  virtual int next_row(std::vector<ObFileSqlValue> &row) override;
  virtual void close() override;

private:
  struct ParquetColumn
  {
    std::string name_;
    int32_t physical_type_;
    int32_t converted_type_;
    int32_t repetition_type_;
    int32_t type_length_;
    int64_t schema_index_;
    ParquetColumn()
      : name_(), physical_type_(0), converted_type_(-1), repetition_type_(0),
        type_length_(0), schema_index_(-1) {}
  };

  struct ParquetChunk
  {
    int32_t physical_type_;
    int32_t codec_;
    int64_t num_values_;
    int64_t data_page_offset_;
    int64_t dictionary_page_offset_;
    int64_t total_compressed_size_;
    ParquetChunk()
      : physical_type_(0), codec_(0), num_values_(0), data_page_offset_(0),
        dictionary_page_offset_(-1), total_compressed_size_(0) {}
  };

  struct RowGroup
  {
    int64_t num_rows_;
    int64_t total_byte_size_;
    std::vector<ParquetChunk> chunks_;
    RowGroup() : num_rows_(0), total_byte_size_(0), chunks_() {}
  };

  int parse_footer();
  int load_row_group(const int64_t rg_index);
  int decode_column(const RowGroup &rg, const int64_t column_index,
                    std::vector<ObFileSqlValue> &values);
  int decode_chunk(const ParquetChunk &chunk, const ParquetColumn &col,
                   std::vector<ObFileSqlValue> &values);
  int decode_plain_value(const ParquetColumn &col, const char *data, const int64_t len,
                         int64_t &offset, ObFileSqlValue &value, std::string &arena);
  int decode_values(const ParquetColumn &col, const int32_t encoding, const char *data,
                    const int64_t len, int64_t &offset, const int64_t logical_count,
                    const std::vector<uint32_t> &def_levels,
                    const std::vector<ObFileSqlValue> &dictionary,
                    const bool dictionary_ready, std::vector<ObFileSqlValue> &values,
                    std::string &arena);

  ObFileSqlInput input_;
  ObFileSqlOptions opt_;
  std::vector<ParquetColumn> columns_;
  std::vector<RowGroup> row_groups_;
  std::vector<std::vector<ObFileSqlValue> > materialized_;
  std::string value_arena_;
  int64_t current_row_group_;
  int64_t current_row_;
  bool schema_ready_;
  bool footer_ready_;
  bool projection_set_;
};

} // namespace filesql
} // namespace sql
} // namespace oceanbase
