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

#include "sql/engine/basic/file_sql/ob_file_sql_common.h"

namespace oceanbase
{
namespace sql
{
namespace filesql
{

// Streaming reader for one file format.  A reader is used twice: once during
// name binding to infer the schema, and once during execution to produce rows.
// Both passes run the same deterministic inference over the whole file, so the
// plan-time and run-time types always agree.
class ObFileSqlReader
{
public:
  ObFileSqlReader() : schema_(), projection_(), err_msg_(), err_line_(0), err_column_(0) {}
  virtual ~ObFileSqlReader() {}

  virtual int open(const std::string &path, const ObFileSqlOptions &opt,
                   const std::string &secure_dir) = 0;
  // Scans the whole file and fills `schema`.  May be called once per reader.
  virtual int infer_schema(ObFileSqlSchema &schema) = 0;
  // Restricts decoding to `cols`; other columns are reported as NULL.  Only an
  // optimisation hint, the produced row always has schema width.
  virtual int set_projection(const std::vector<int64_t> &cols)
  {
    projection_ = cols;
    return 0;
  }
  // Rewinds to the first data row using the schema produced by infer_schema().
  virtual int start_scan() = 0;
  // Fills `row` with one value per schema column.
  virtual int next_row(std::vector<ObFileSqlValue> &row) = 0;
  virtual void close() = 0;

  int64_t row_count() const { return row_count_; }
  const std::string &error_message() const { return err_msg_; }
  int64_t error_line() const { return err_line_; }
  int64_t error_column() const { return err_column_; }

protected:
  void set_error(const int64_t line, const int64_t column, const char *fmt, ...);
  void clear_error()
  {
    err_msg_.clear();
    err_line_ = 0;
    err_column_ = 0;
  }

  ObFileSqlSchema schema_;
  std::vector<int64_t> projection_;
  std::string err_msg_;
  int64_t err_line_;
  int64_t err_column_;
  int64_t row_count_ = 0;
};

// Creates the reader matching `format`.  Returns OB_NOT_SUPPORTED for unknown
// formats so that the caller can report a syntax error.
int create_file_sql_reader(const ObFileFormat format, ObFileSqlReader *&reader);
// Releases a reader created by create_file_sql_reader().  Readers are created
// with `new`, so they must be released with `delete` (never ob_free()).
void destroy_file_sql_reader(ObFileSqlReader *reader);

} // namespace filesql
} // namespace sql
} // namespace oceanbase
