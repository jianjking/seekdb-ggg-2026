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

// RFC-4180 style CSV reader.  Records are delimited by a state machine instead
// of line splitting, so quoted fields may legally contain CR/LF.
class ObCsvFileReader : public ObFileSqlReader
{
public:
  struct Field
  {
    std::string text_;
    bool quoted_;
    Field() : text_(), quoted_(false) {}
  };

  ObCsvFileReader();
  virtual ~ObCsvFileReader() {}

  virtual int open(const std::string &path, const ObFileSqlOptions &opt,
                   const std::string &secure_dir) override;
  virtual int infer_schema(ObFileSqlSchema &schema) override;
  virtual int start_scan() override;
  virtual int next_row(std::vector<ObFileSqlValue> &row) override;
  virtual void close() override;

private:
  int read_field(Field &field, int &term);
  int consume_terminator(int &term);
  int read_record(bool &eof);
  void dedup_column_names();
  // True when every field of the record is a plain string, which is what a
  // header line looks like.
  static bool row_is_all_text(const std::vector<Field> &fields);
  // True when at least one field of the record parses as a non-string value.
  static bool row_has_typed_value(const std::vector<Field> &fields);

  ObFileSqlInput input_;
  ObFileSqlOptions opt_;
  std::vector<std::string> header_names_;
  std::vector<Field> fields_;
  bool schema_ready_;
  int64_t header_rows_;
  int64_t record_no_;
  int64_t line_no_;
};

} // namespace filesql
} // namespace sql
} // namespace oceanbase
