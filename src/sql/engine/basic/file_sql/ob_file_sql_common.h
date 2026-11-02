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

#include <cstdint>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace filesql
{

// Logical column types exposed by file table functions.  The type names are
// part of the SQL surface: DESCRIBE prints them verbatim.
enum ObFileSqlType
{
  FS_TYPE_INVALID = 0,
  FS_TYPE_NULL = 1,
  FS_TYPE_BIGINT = 2,
  FS_TYPE_DOUBLE = 3,
  FS_TYPE_BOOLEAN = 4,
  FS_TYPE_VARCHAR = 5,
  FS_TYPE_DATE = 6,
  FS_TYPE_DATETIME = 7,
};

enum ObFileFormat
{
  FS_FORMAT_INVALID = 0,
  FS_FORMAT_CSV = 1,
  FS_FORMAT_JSONL = 2,
  FS_FORMAT_PARQUET = 3,
  FS_FORMAT_GLOB = 4,
};

struct ObFileSqlOptions
{
  ObFileFormat format_;
  char delimiter_;
  char quote_;
  char escape_;
  // -1 = sniff the first row (DuckDB style), 0 = first row is data,
  // 1 = first row is the header line.
  int32_t has_header_;
  bool auto_detect_;
  // NULL representation for unquoted fields.  An empty string keeps the
  // default "empty unquoted field is NULL" behaviour.
  std::string null_str_;
  int64_t sample_size_;   // only -1 (full file) is supported
  int64_t skip_rows_;
  int64_t maximum_object_size_;

  ObFileSqlOptions()
    : format_(FS_FORMAT_INVALID),
      delimiter_(','),
      quote_('"'),
      escape_('"'),
      has_header_(-1),
      auto_detect_(false),
      null_str_(),
      sample_size_(-1),
      skip_rows_(0),
      maximum_object_size_(64LL * 1024 * 1024)
  {}
};

struct ObFileSqlColumn
{
  std::string name_;
  ObFileSqlType type_;
  bool nullable_;
  int64_t max_length_;
  int32_t datetime_scale_;

  ObFileSqlColumn()
    : name_(), type_(FS_TYPE_INVALID), nullable_(true), max_length_(0), datetime_scale_(0) {}
  ObFileSqlColumn(const std::string &name, ObFileSqlType type, bool nullable)
    : name_(name), type_(type), nullable_(nullable), max_length_(0), datetime_scale_(0) {}
};

struct ObFileSqlSchema
{
  std::vector<ObFileSqlColumn> columns_;
};

// One decoded cell.  String payloads point into storage owned by the reader and
// stay valid until the next row is fetched.
struct ObFileSqlValue
{
  bool null_;
  ObFileSqlType type_;
  int64_t int_value_;
  double double_value_;
  int32_t date_value_;
  int64_t datetime_value_; // Microseconds since 1970-01-01, including fractions.
  const char *str_ptr_;
  int32_t str_len_;

  ObFileSqlValue()
    : null_(true),
      type_(FS_TYPE_NULL),
      int_value_(0),
      double_value_(0.0),
      date_value_(0),
      datetime_value_(0),
      str_ptr_(NULL),
      str_len_(0)
  {}

  void reset()
  {
    null_ = true;
    type_ = FS_TYPE_NULL;
    int_value_ = 0;
    double_value_ = 0.0;
    date_value_ = 0;
    datetime_value_ = 0;
    str_ptr_ = NULL;
    str_len_ = 0;
  }
};

const char *file_sql_type_name(ObFileSqlType type);

///////////////////////////////////////////////////////////////////////////////
// Buffered, seekable, read-only file input.
//
// The handle is validated after it has been opened: the final path that the
// operating system associated with the handle must still live inside the
// directory allowed by secure_file_priv.  This closes the window between the
// path check performed by the resolver and the actual open.
///////////////////////////////////////////////////////////////////////////////
class ObFileSqlInput
{
public:
  ObFileSqlInput();
  ~ObFileSqlInput();

  // `secure_dir` empty means "no directory restriction" (used by tests/COPY
  // validations that already checked the path themselves).
  int open(const std::string &path, const std::string &secure_dir, std::string &real_path);
  void close();
  bool is_open() const { return opened_; }

  int64_t size() const { return size_; }
  int64_t position() const { return pos_; }

  int seek(const int64_t offset);
  // Absolute positional read.  Returns OB_SUCCESS and sets `nread`; `nread`
  // smaller than `len` means end of file.
  int read_at(const int64_t offset, char *buf, const int64_t len, int64_t &nread);

  // Sequential access with one character push-back.
  int get_char(char &c);
  int peek_char(char &c);
  void unget_char();

  const std::string &real_path() const { return real_path_; }

private:
  int refill();
  int check_unchanged() const;
  void *handle_;
  int fd_;
  bool opened_;
  int64_t size_;
  int64_t last_write_time_;
  int64_t last_write_time_extra_;
  int64_t pos_;
  std::string real_path_;
  std::vector<char> buf_;
  int64_t buf_begin_;
  int64_t buf_len_;
  int64_t buf_pos_;
  bool has_pushback_;
  char pushback_;
};

///////////////////////////////////////////////////////////////////////////////
// Value classification and type inference
///////////////////////////////////////////////////////////////////////////////

// Classifies a raw textual cell.  `quoted` cells are always treated as text,
// matching the behaviour of mainstream analytical engines.
ObFileSqlType classify_text_value(const char *ptr, const int64_t len, const bool quoted);

// Merges a newly observed type into the running column type.
ObFileSqlType merge_file_sql_type(ObFileSqlType current, ObFileSqlType incoming);

// Parses a textual cell into `value` according to `type`.  Returns false when
// the text is not representable, which the caller reports as a data error.
bool parse_text_value(const char *ptr, const int64_t len, ObFileSqlType type,
                      ObFileSqlValue &value);
bool is_valid_file_sql_utf8(const char *ptr, const int64_t len);

///////////////////////////////////////////////////////////////////////////////
// Path helpers
///////////////////////////////////////////////////////////////////////////////

// Lexically normalises a path to an absolute path, resolving "." and "..".
int normalize_file_path(const std::string &input, std::string &output);
// Validates the actual open descriptor rather than resolving its path again.
int validate_file_sql_descriptor(int fd, const std::string &secure_dir);

// True when `path` is inside directory `dir`.  Comparison is case-insensitive
// on Windows and strictly case-sensitive elsewhere.
bool path_is_within(const std::string &dir, const std::string &path);

// Expands a single-level glob pattern ("dir/*.csv").  Returns absolute,
// lexically normalised paths sorted in ascending byte order.
int expand_glob_pattern(const std::string &pattern, std::vector<std::string> &files);

// Base name of a path (after the last separator).
std::string file_base_name(const std::string &path);

// Interprets a secure_file_priv value.  "/" (or "\") means unrestricted, an
// empty value denies everything, anything else is returned as a normalised
// directory.
int resolve_secure_dir(const std::string &priv, std::string &dir, bool &unrestricted);

} // namespace filesql
} // namespace sql
} // namespace oceanbase
