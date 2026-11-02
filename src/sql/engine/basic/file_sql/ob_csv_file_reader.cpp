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

#define USING_LOG_PREFIX SQL_ENG

#include "sql/engine/basic/file_sql/ob_csv_file_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "share/ob_define.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace sql
{
namespace filesql
{

using namespace oceanbase::common;

namespace
{
const int64_t CSV_MAX_COLUMNS = 4096;
} // namespace


ObCsvFileReader::ObCsvFileReader()
  : input_(),
    opt_(),
    header_names_(),
    fields_(),
    schema_ready_(false),
    header_rows_(0),
    record_no_(0),
    line_no_(1)
{
}

int ObCsvFileReader::open(const std::string &path, const ObFileSqlOptions &opt,
                          const std::string &secure_dir)
{
  int ret = OB_SUCCESS;
  std::string real_path;
  clear_error();
  opt_ = opt;
  schema_ready_ = false;
  record_no_ = 0;
  line_no_ = 1;
  if (OB_FAIL(input_.open(path, secure_dir, real_path))) {
    LOG_WARN("failed to open csv file", K(ret), "path", path.c_str());
  }
  return ret;
}

void ObCsvFileReader::close()
{
  input_.close();
  schema_ready_ = false;
}

// Consumes the character that terminates a field.  `term` is 0 for a field
// separator, 1 for end of record and 2 for end of file.
int ObCsvFileReader::consume_terminator(int &term)
{
  int ret = OB_SUCCESS;
  char c = 0;
  int rc = input_.get_char(c);
  term = 2;
  if (OB_ITER_END == rc) {
    ret = OB_SUCCESS;
  } else if (OB_SUCCESS != rc) {
    ret = rc;
  } else if (opt_.delimiter_ == c) {
    term = 0;
  } else if ('\n' == c) {
    term = 1;
    ++line_no_;
  } else if ('\r' == c) {
    char next = 0;
    if (OB_SUCCESS == input_.peek_char(next) && '\n' == next) {
      char consumed = 0;
      input_.get_char(consumed);
    }
    term = 1;
    ++line_no_;
  } else {
    ret = OB_INVALID_DATA;
    set_error(line_no_, 0, "unexpected character '%c' after quoted field", c);
  }
  return ret;
}

int ObCsvFileReader::read_field(Field &field, int &term)
{
  int ret = OB_SUCCESS;
  char c = 0;
  field.text_.clear();
  field.quoted_ = false;
  int rc = input_.peek_char(c);
  if (OB_ITER_END == rc) {
    term = 2;
    ret = OB_ITER_END;
  } else if (OB_SUCCESS != rc) {
    ret = rc;
  } else if (opt_.quote_ == c) {
    field.quoted_ = true;
    char consumed = 0;
    input_.get_char(consumed);
    bool closed = false;
    while (OB_SUCC(ret) && !closed) {
      rc = input_.get_char(c);
      if (OB_ITER_END == rc) {
        ret = OB_INVALID_DATA;
        set_error(line_no_, 0, "unterminated quoted field");
      } else if (OB_SUCCESS != rc) {
        ret = rc;
      } else if (opt_.escape_ == opt_.quote_ && opt_.quote_ == c) {
        char next = 0;
        if (OB_SUCCESS == input_.peek_char(next) && opt_.quote_ == next) {
          char escaped = 0;
          input_.get_char(escaped);
          field.text_.push_back(opt_.quote_);
        } else {
          closed = true;
        }
      } else if (opt_.escape_ != opt_.quote_ && opt_.escape_ == c) {
        char escaped = 0;
        if (OB_SUCCESS == input_.get_char(escaped)) {
          field.text_.push_back(escaped);
        } else {
          ret = OB_INVALID_DATA;
          set_error(line_no_, 0, "unterminated escape sequence");
        }
      } else if (opt_.escape_ != opt_.quote_ && opt_.quote_ == c) {
        closed = true;
      } else if ('\r' == c) {
        // Normalise CRLF inside a quoted field to LF so that the stored value
        // does not depend on the writer's line endings.
        char next = 0;
        if (OB_SUCCESS == input_.peek_char(next) && '\n' == next) {
          char crlf = 0;
          input_.get_char(crlf);
          field.text_.push_back('\n');
        } else {
          field.text_.push_back('\r');
        }
      } else {
        field.text_.push_back(c);
      }
      if (OB_SUCC(ret) && field.text_.size() > static_cast<size_t>(opt_.maximum_object_size_)) {
        ret = OB_SIZE_OVERFLOW;
        set_error(line_no_, 0, "field exceeds maximum_object_size");
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(consume_terminator(term))) {
    }
  } else {
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      rc = input_.get_char(c);
      if (OB_ITER_END == rc) {
        term = 2;
        done = true;
      } else if (OB_SUCCESS != rc) {
        ret = rc;
      } else if (opt_.delimiter_ == c) {
        term = 0;
        done = true;
      } else if ('\n' == c) {
        term = 1;
        ++line_no_;
        done = true;
      } else if ('\r' == c) {
        char next = 0;
        if (OB_SUCCESS == input_.peek_char(next) && '\n' == next) {
          char crlf = 0;
          input_.get_char(crlf);
        }
        term = 1;
        ++line_no_;
        done = true;
      } else {
        field.text_.push_back(c);
      }
      if (OB_SUCC(ret) && field.text_.size() > static_cast<size_t>(opt_.maximum_object_size_)) {
        ret = OB_SIZE_OVERFLOW;
        set_error(line_no_, 0, "field exceeds maximum_object_size");
      }
    }
  }
  if (OB_SUCC(ret) && !is_valid_file_sql_utf8(field.text_.data(), field.text_.size())) {
    ret = OB_INVALID_DATA;
    set_error(line_no_, 0, "csv field is not valid UTF-8");
  }
  return ret;
}

int ObCsvFileReader::read_record(bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  fields_.clear();
  char c = 0;
  int rc = input_.peek_char(c);
  if (OB_SUCCESS == rc && 0 == record_no_ && static_cast<unsigned char>(c) == 0xEF) {
    char bom[3] = {0};
    for (int i = 0; i < 3; ++i) {
      if (OB_SUCCESS != input_.get_char(bom[i])) { break; }
    }
    if (static_cast<unsigned char>(bom[1]) != 0xBB
        || static_cast<unsigned char>(bom[2]) != 0xBF) {
      input_.seek(0);
    }
    rc = input_.peek_char(c);
  }
  if (OB_ITER_END == rc) {
    eof = true;
  } else if (OB_SUCCESS != rc) {
    ret = rc;
  } else {
    bool done = false;
    int64_t record_size = 0;
    while (OB_SUCC(ret) && !done) {
      Field field;
      int term = 2;
      const int field_rc = read_field(field, term);
      if (OB_ITER_END == field_rc) {
        // End of file right after a field separator: the record ends with one
        // more empty field, which is what a CSV writer produced.
        if (!fields_.empty()) {
          fields_.push_back(field);
        }
        eof = true;
        done = true;
      } else if (OB_SUCCESS != field_rc) {
        ret = field_rc;
      } else {
        record_size += static_cast<int64_t>(field.text_.size());
        fields_.push_back(field);
        if (record_size > opt_.maximum_object_size_) {
          ret = OB_SIZE_OVERFLOW;
          set_error(line_no_, 0, "csv record exceeds max_object_size");
        } else if (fields_.size() > static_cast<size_t>(CSV_MAX_COLUMNS)) {
          ret = OB_SIZE_OVERFLOW;
          set_error(line_no_, static_cast<int64_t>(fields_.size()), "too many columns");
        } else if (0 == term) {
          // Another field follows.
        } else {
          done = true;
          if (2 == term) {
            eof = true;
          }
        }
      }
    }
    if (OB_SUCC(ret) && !fields_.empty()) {
      ++record_no_;
    }
  }
  return ret;
}

void ObCsvFileReader::dedup_column_names()
{
  std::vector<std::string> used;
  used.reserve(header_names_.size());
  for (size_t i = 0; i < header_names_.size(); ++i) {
    const std::string base = header_names_[i];
    std::string candidate = base;
    int64_t suffix = 1;
    bool exists = true;
    while (exists) {
      exists = false;
      for (size_t j = 0; j < used.size(); ++j) {
        if (used[j] == candidate) {
          exists = true;
          break;
        }
      }
      if (exists) {
        ++suffix;
        char buf[32];
        snprintf(buf, sizeof(buf), "__%lld", static_cast<long long>(suffix));
        candidate = base + buf;
      }
    }
    used.push_back(candidate);
    header_names_[i] = candidate;
  }
}

bool ObCsvFileReader::row_is_all_text(const std::vector<Field> &fields)
{
  bool all_text = !fields.empty();
  for (size_t i = 0; all_text && i < fields.size(); ++i) {
    if (FS_TYPE_VARCHAR != classify_text_value(fields[i].text_.data(),
                                               static_cast<int64_t>(fields[i].text_.size()),
                                               fields[i].quoted_)) {
      all_text = false;
    }
  }
  return all_text;
}

bool ObCsvFileReader::row_has_typed_value(const std::vector<Field> &fields)
{
  bool typed = false;
  for (size_t i = 0; !typed && i < fields.size(); ++i) {
    const ObFileSqlType type = classify_text_value(fields[i].text_.data(),
                                                   static_cast<int64_t>(fields[i].text_.size()),
                                                   fields[i].quoted_);
    typed = (FS_TYPE_VARCHAR != type && FS_TYPE_NULL != type);
  }
  return typed;
}

int ObCsvFileReader::infer_schema(ObFileSqlSchema &schema)
{
  int ret = OB_SUCCESS;
  bool eof = false;
  schema.columns_.clear();
  schema_.columns_.clear();
  schema_ready_ = false;
  header_rows_ = 0;
  if (OB_FAIL(input_.seek(0))) {
  } else {
    line_no_ = 1;
    record_no_ = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < opt_.skip_rows_; ++i) {
      if (OB_FAIL(read_record(eof))) {
      } else if (eof) {
        ret = OB_INVALID_DATA;
        set_error(line_no_, 0, "skip_rows consumes the entire csv file");
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(read_record(eof))) {
    } else if (eof && fields_.empty()) {
      ret = OB_INVALID_DATA;
      set_error(line_no_, 0, "csv file is empty");
    } else {
      // Keep the first two records so that the header can be sniffed without a
      // second pass over the file.
      std::vector<Field> first_record = fields_;
      std::vector<Field> second_record;
      bool has_second = false;
      if (!eof) {
        bool second_eof = false;
        if (OB_FAIL(read_record(second_eof))) {
        } else if (!(second_eof && fields_.empty())) {
          second_record = fields_;
          has_second = true;
        }
      }
      if (OB_SUCC(ret)) {
        bool has_header = false;
        if (opt_.has_header_ > 0) {
          has_header = true;
        } else if (0 == opt_.has_header_) {
          has_header = false;
        } else {
          // DuckDB style sniffing: a header is a row of plain strings followed
          // by rows that hold at least one typed value.
          has_header = has_second
                       && first_record.size() == second_record.size()
                       && row_is_all_text(first_record)
                       && row_has_typed_value(second_record);
        }
        header_rows_ = has_header ? 1 : 0;
        header_names_.clear();
        for (size_t i = 0; i < first_record.size(); ++i) {
          if (has_header) {
            header_names_.push_back(first_record[i].text_);
          } else {
            char name[32];
            snprintf(name, sizeof(name), "column%lld", static_cast<long long>(i));
            header_names_.push_back(name);
          }
        }
        if (header_names_.empty()) {
          ret = OB_INVALID_DATA;
          set_error(line_no_, 0, "csv file has no columns");
        } else {
          dedup_column_names();
          const size_t column_count = header_names_.size();
          std::vector<ObFileSqlType> types(column_count, FS_TYPE_NULL);
          std::vector<bool> nullable(column_count, false);
          std::vector<int64_t> max_length(column_count, 0);
          std::vector<int32_t> datetime_scale(column_count, 0);
          std::vector<const std::vector<Field> *> pending;
          if (!has_header) {
            pending.push_back(&first_record);
          }
          if (has_second) {
            pending.push_back(&second_record);
          }
          size_t pending_idx = 0;
          int64_t data_rows = 0;
          while (OB_SUCC(ret)) {
            const std::vector<Field> *record = NULL;
            if (pending_idx < pending.size()) {
              record = pending[pending_idx++];
            } else {
              bool next_eof = false;
              if (OB_FAIL(read_record(next_eof))) {
                break;
              } else if (next_eof && fields_.empty()) {
                break;
              } else {
                record = &fields_;
              }
            }
            if (record->size() != column_count) {
              ret = OB_INVALID_DATA;
              set_error(line_no_, static_cast<int64_t>(record->size()),
                        "csv row has %lld fields but %lld were expected",
                        static_cast<long long>(record->size()),
                        static_cast<long long>(column_count));
            } else {
              for (size_t i = 0; i < record->size(); ++i) {
                const Field &field = (*record)[i];
                const bool is_null = !field.quoted_ && field.text_ == opt_.null_str_;
                if (is_null) {
                  nullable[i] = true;
                } else {
                  types[i] = merge_file_sql_type(
                      types[i], classify_text_value(field.text_.data(),
                                                    static_cast<int64_t>(field.text_.size()),
                                                    field.quoted_ || field.text_.empty()));
                  max_length[i] = std::max<int64_t>(max_length[i],
                                                    static_cast<int64_t>(field.text_.size()));
                  if (FS_TYPE_DATETIME == types[i] && field.text_.size() > 19
                      && '.' == field.text_[19]) {
                    datetime_scale[i] = std::max<int32_t>(
                        datetime_scale[i], static_cast<int32_t>(field.text_.size() - 20));
                  }
                }
              }
              ++data_rows;
            }
          }
          if (OB_SUCC(ret)) {
            for (size_t i = 0; i < column_count; ++i) {
              ObFileSqlType type = types[i];
              bool is_nullable = nullable[i];
              if (FS_TYPE_NULL == type || FS_TYPE_INVALID == type) {
                type = FS_TYPE_NULL;
                is_nullable = true;
              }
              ObFileSqlColumn column(header_names_[i], type, is_nullable);
              column.max_length_ = max_length[i];
              column.datetime_scale_ = datetime_scale[i];
              schema_.columns_.push_back(column);
            }
            schema = schema_;
            schema_ready_ = true;
            if (0 == data_rows) {
              LOG_INFO("csv file has a header but no data rows", "columns",
                       static_cast<int64_t>(column_count));
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObCsvFileReader::start_scan()
{
  int ret = OB_SUCCESS;
  bool eof = false;
  record_no_ = 0;
  line_no_ = 1;
  row_count_ = 0;
  if (!schema_ready_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(input_.seek(0))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < opt_.skip_rows_; ++i) {
    if (OB_FAIL(read_record(eof))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < header_rows_; ++i) {
    bool skipped_eof = false;
    if (OB_FAIL(read_record(skipped_eof))) {
    } else if (skipped_eof && fields_.empty()) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObCsvFileReader::next_row(std::vector<ObFileSqlValue> &row)
{
  int ret = OB_SUCCESS;
  bool eof = false;
  row.clear();
  row.reserve(schema_.columns_.size());
  if (OB_FAIL(read_record(eof))) {
  } else if (eof && fields_.empty()) {
    ret = OB_ITER_END;
  } else if (fields_.size() != schema_.columns_.size()) {
    ret = OB_INVALID_DATA;
    set_error(line_no_, static_cast<int64_t>(fields_.size()),
              "csv row has %lld fields but %lld were expected",
              static_cast<long long>(fields_.size()),
              static_cast<long long>(schema_.columns_.size()));
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < fields_.size(); ++i) {
      const Field &field = fields_[i];
      ObFileSqlValue value;
      const bool is_null = !field.quoted_ && field.text_ == opt_.null_str_;
      if (is_null) {
        value.null_ = true;
        value.type_ = schema_.columns_[i].type_;
      } else if (!parse_text_value(field.text_.data(),
                                   static_cast<int64_t>(field.text_.size()),
                                   schema_.columns_[i].type_, value)) {
        ret = OB_INVALID_DATA;
        set_error(line_no_, static_cast<int64_t>(i) + 1,
                  "value '%.*s' does not match inferred type %s",
                  static_cast<int>(std::min<int64_t>(field.text_.size(), 64)),
                  field.text_.data(), file_sql_type_name(schema_.columns_[i].type_));
      }
      row.push_back(value);
    }
    if (OB_SUCC(ret)) {
      ++row_count_;
    }
  }
  return ret;
}

} // namespace filesql
} // namespace sql
} // namespace oceanbase
