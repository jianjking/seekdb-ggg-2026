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

// Newline delimited JSON reader.  Only flat objects are accepted; nested
// objects and arrays are rejected with a line number so that callers never see
// a silently truncated document.
class ObJsonlFileReader : public ObFileSqlReader
{
public:
  struct Member
  {
    std::string key_;
    std::string text_;
    ObFileSqlValue value_;
    Member() : key_(), text_(), value_() {}
  };

  ObJsonlFileReader();
  virtual ~ObJsonlFileReader() {}

  virtual int open(const std::string &path, const ObFileSqlOptions &opt,
                   const std::string &secure_dir) override;
  virtual int infer_schema(ObFileSqlSchema &schema) override;
  virtual int start_scan() override;
  virtual int next_row(std::vector<ObFileSqlValue> &row) override;
  virtual void close() override;

private:
  int read_line(bool &eof);
  int parse_object(const char *ptr, const int64_t len, std::vector<Member> &members);
  int coerce_to_schema(std::vector<ObFileSqlValue> &row);

  ObFileSqlInput input_;
  ObFileSqlOptions opt_;
  std::string line_;
  std::vector<Member> members_;
  std::vector<std::string> scratch_;
  bool schema_ready_;
  int64_t line_no_;
  std::vector<int64_t> column_index_;
};

} // namespace filesql
} // namespace sql
} // namespace oceanbase
