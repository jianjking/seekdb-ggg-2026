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

#include "sql/engine/basic/file_sql/ob_jsonl_file_reader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

const int64_t JSONL_MAX_COLUMNS = 4096;


struct JsonCursor
{
  const char *ptr_;
  int64_t len_;
  int64_t pos_;
  JsonCursor(const char *ptr, const int64_t len) : ptr_(ptr), len_(len), pos_(0) {}

  bool eof() const { return pos_ >= len_; }
  char cur() const { return ptr_[pos_]; }
  void skip_ws()
  {
    while (pos_ < len_) {
      const char c = ptr_[pos_];
      if (' ' == c || '\t' == c || '\r' == c || '\n' == c) {
        ++pos_;
      } else {
        break;
      }
    }
  }
};

int append_utf8(std::string &out, const uint32_t code_point)
{
  int ret = OB_SUCCESS;
  if (code_point < 0x80) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

int parse_json_string(JsonCursor &cur, std::string &out)
{
  int ret = OB_SUCCESS;
  out.clear();
  if (cur.eof() || '"' != cur.cur()) {
    ret = OB_INVALID_DATA;
  } else {
    ++cur.pos_;
    bool closed = false;
    while (OB_SUCC(ret) && !cur.eof() && !closed) {
      const char c = cur.ptr_[cur.pos_++];
      if ('"' == c) {
        closed = true;
      } else if ('\\' == c) {
        if (cur.eof()) {
          ret = OB_INVALID_DATA;
        } else {
          const char esc = cur.ptr_[cur.pos_++];
          switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
              uint32_t code = 0;
              bool valid = (cur.pos_ + 4 <= cur.len_);
              for (int i = 0; valid && i < 4; ++i) {
                const char h = cur.ptr_[cur.pos_ + i];
                uint32_t digit = 0;
                if (h >= '0' && h <= '9') { digit = static_cast<uint32_t>(h - '0'); }
                else if (h >= 'a' && h <= 'f') { digit = static_cast<uint32_t>(h - 'a' + 10); }
                else if (h >= 'A' && h <= 'F') { digit = static_cast<uint32_t>(h - 'A' + 10); }
                else { valid = false; }
                code = (code << 4) | digit;
              }
              if (!valid) {
                ret = OB_INVALID_DATA;
              } else {
                cur.pos_ += 4;
                if (code >= 0xD800 && code <= 0xDBFF && cur.pos_ + 6 <= cur.len_
                    && '\\' == cur.ptr_[cur.pos_] && 'u' == cur.ptr_[cur.pos_ + 1]) {
                  uint32_t low = 0;
                  bool low_valid = true;
                  for (int i = 0; low_valid && i < 4; ++i) {
                    const char h = cur.ptr_[cur.pos_ + 2 + i];
                    uint32_t digit = 0;
                    if (h >= '0' && h <= '9') { digit = static_cast<uint32_t>(h - '0'); }
                    else if (h >= 'a' && h <= 'f') { digit = static_cast<uint32_t>(h - 'a' + 10); }
                    else if (h >= 'A' && h <= 'F') { digit = static_cast<uint32_t>(h - 'A' + 10); }
                    else { low_valid = false; }
                    low = (low << 4) | digit;
                  }
                  if (low_valid && low >= 0xDC00 && low <= 0xDFFF) {
                    cur.pos_ += 6;
                    const uint32_t combined = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    ret = append_utf8(out, combined);
                  } else {
                    ret = OB_INVALID_DATA;
                  }
                } else if (code >= 0xD800 && code <= 0xDFFF) {
                  ret = OB_INVALID_DATA;
                } else {
                  ret = append_utf8(out, code);
                }
              }
              break;
            }
            default: {
              ret = OB_INVALID_DATA;
              break;
            }
          }
        }
      } else if (static_cast<unsigned char>(c) < 0x20) {
        ret = OB_INVALID_DATA;
      } else {
        out.push_back(c);
      }
    }
    if (OB_SUCC(ret) && (!closed || !is_valid_file_sql_utf8(out.data(), out.size()))) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int parse_json_number(JsonCursor &cur, ObFileSqlValue &value)
{
  int ret = OB_SUCCESS;
  const int64_t begin = cur.pos_;
  bool is_integer = true;
  if (!cur.eof() && '-' == cur.cur()) { ++cur.pos_; }
  if (cur.eof() || cur.cur() < '0' || cur.cur() > '9') {
    ret = OB_INVALID_DATA;
  } else if ('0' == cur.cur()) {
    ++cur.pos_;
  } else {
    while (!cur.eof() && cur.cur() >= '0' && cur.cur() <= '9') { ++cur.pos_; }
  }
  if (OB_SUCC(ret) && !cur.eof() && '.' == cur.cur()) {
    is_integer = false;
    ++cur.pos_;
    const int64_t digits_begin = cur.pos_;
    while (!cur.eof() && cur.cur() >= '0' && cur.cur() <= '9') { ++cur.pos_; }
    if (digits_begin == cur.pos_) { ret = OB_INVALID_DATA; }
  }
  if (OB_SUCC(ret) && !cur.eof() && ('e' == cur.cur() || 'E' == cur.cur())) {
    is_integer = false;
    ++cur.pos_;
    if (!cur.eof() && ('+' == cur.cur() || '-' == cur.cur())) { ++cur.pos_; }
    const int64_t digits_begin = cur.pos_;
    while (!cur.eof() && cur.cur() >= '0' && cur.cur() <= '9') { ++cur.pos_; }
    if (digits_begin == cur.pos_) { ret = OB_INVALID_DATA; }
  }
  if (OB_SUCC(ret)) {
    std::string literal(cur.ptr_ + begin, static_cast<size_t>(cur.pos_ - begin));
    value.null_ = false;
    if (is_integer) {
      ObFileSqlValue probe;
      if (parse_text_value(literal.data(), static_cast<int64_t>(literal.size()),
                           FS_TYPE_BIGINT, probe)) {
        value.type_ = FS_TYPE_BIGINT;
        value.int_value_ = probe.int_value_;
      } else {
        if (!parse_text_value(literal.data(), static_cast<int64_t>(literal.size()),
                              FS_TYPE_DOUBLE, value)) {
          ret = OB_INVALID_DATA;
        }
      }
    } else {
      if (!parse_text_value(literal.data(), static_cast<int64_t>(literal.size()),
                            FS_TYPE_DOUBLE, value)) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

// Classifies a JSON string payload: dates and datetimes are recognised, other
// text stays VARCHAR (numeric looking strings are never silently converted).
ObFileSqlType classify_json_string(const std::string &text)
{
  ObFileSqlType type = FS_TYPE_VARCHAR;
  ObFileSqlValue probe;
  if (text.empty()) {
    type = FS_TYPE_VARCHAR;
  } else if (parse_text_value(text.data(), static_cast<int64_t>(text.size()), FS_TYPE_DATE, probe)) {
    type = FS_TYPE_DATE;
  } else if (parse_text_value(text.data(), static_cast<int64_t>(text.size()),
                              FS_TYPE_DATETIME, probe)) {
    type = FS_TYPE_DATETIME;
  }
  return type;
}

} // namespace

ObJsonlFileReader::ObJsonlFileReader()
  : input_(),
    opt_(),
    line_(),
    members_(),
    scratch_(),
    schema_ready_(false),
    line_no_(0),
    column_index_()
{
}

int ObJsonlFileReader::open(const std::string &path, const ObFileSqlOptions &opt,
                            const std::string &secure_dir)
{
  int ret = OB_SUCCESS;
  std::string real_path;
  clear_error();
  opt_ = opt;
  schema_ready_ = false;
  line_no_ = 0;
  if (OB_FAIL(input_.open(path, secure_dir, real_path))) {
    LOG_WARN("failed to open jsonl file", K(ret), "path", path.c_str());
  }
  return ret;
}

void ObJsonlFileReader::close()
{
  input_.close();
  schema_ready_ = false;
}

int ObJsonlFileReader::read_line(bool &eof)
{
  int ret = OB_SUCCESS;
  eof = false;
  line_.clear();
  char c = 0;
  bool got_any = false;
  while (true) {
    const int rc = input_.get_char(c);
    if (OB_ITER_END == rc) {
      eof = true;
      break;
    } else if (OB_SUCCESS != rc) {
      ret = rc;
      break;
    } else if ('\n' == c) {
      got_any = true;
      break;
    } else {
      line_.push_back(c);
      got_any = true;
    }
    if (line_.size() > static_cast<size_t>(opt_.maximum_object_size_)) {
      ret = OB_SIZE_OVERFLOW;
      set_error(line_no_ + 1, 0, "json line exceeds maximum_object_size");
      break;
    }
  }
  if (OB_SUCC(ret)) {
    if (got_any || !eof) {
      ++line_no_;
    }
    eof = eof && !got_any;
  }
  return ret;
}

int ObJsonlFileReader::parse_object(const char *ptr, const int64_t len,
                                    std::vector<Member> &members)
{
  int ret = OB_SUCCESS;
  members.clear();
  JsonCursor cur(ptr, len);
  cur.skip_ws();
  if (cur.eof() || '{' != cur.cur()) {
    ret = OB_INVALID_DATA;
  } else {
    ++cur.pos_;
    cur.skip_ws();
    if (!cur.eof() && '}' == cur.cur()) {
      ++cur.pos_;
    } else {
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        cur.skip_ws();
        Member member;
        if (OB_FAIL(parse_json_string(cur, member.key_))) {
          ret = OB_INVALID_DATA;
        } else {
          cur.skip_ws();
          if (cur.eof() || ':' != cur.cur()) {
            ret = OB_INVALID_DATA;
          } else {
            ++cur.pos_;
            cur.skip_ws();
            if (cur.eof()) {
              ret = OB_INVALID_DATA;
            } else {
              const char c = cur.cur();
              if ('"' == c) {
                std::string text;
                if (OB_FAIL(parse_json_string(cur, text))) {
                  ret = OB_INVALID_DATA;
                } else {
                  member.text_ = text;
                  member.value_.type_ = classify_json_string(text);
                  if (!parse_text_value(member.text_.data(),
                                        static_cast<int64_t>(member.text_.size()),
                                        member.value_.type_, member.value_)) {
                    ret = OB_INVALID_DATA;
                  }
                }
              } else if ('t' == c || 'f' == c) {
                const bool is_true = ('t' == c);
                const char *literal = is_true ? "true" : "false";
                const int64_t literal_len = is_true ? 4 : 5;
                if (cur.pos_ + literal_len > cur.len_
                    || 0 != strncmp(cur.ptr_ + cur.pos_, literal, literal_len)) {
                  ret = OB_INVALID_DATA;
                } else {
                  cur.pos_ += literal_len;
                  member.value_.null_ = false;
                  member.value_.type_ = FS_TYPE_BOOLEAN;
                  member.value_.int_value_ = is_true ? 1 : 0;
                }
              } else if ('n' == c) {
                if (cur.pos_ + 4 <= cur.len_
                    && 0 == strncmp(cur.ptr_ + cur.pos_, "null", 4)) {
                  cur.pos_ += 4;
                  member.value_.null_ = true;
                  member.value_.type_ = FS_TYPE_NULL;
                } else {
                  ret = OB_INVALID_DATA;
                }
              } else if ('{' == c || '[' == c) {
                ret = OB_NOT_SUPPORTED;
                set_error(line_no_, 0,
                          "nested objects and arrays are not supported by read_json");
              } else {
                if (OB_FAIL(parse_json_number(cur, member.value_))) {
                  ret = OB_INVALID_DATA;
                }
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          bool duplicate = false;
          for (size_t i = 0; i < members.size(); ++i) {
            if (members[i].key_ == member.key_) {
              // Last value wins, mirroring JSON object semantics.
              members[i] = member;
              duplicate = true;
              break;
            }
          }
          if (!duplicate) {
            members.push_back(member);
          }
          cur.skip_ws();
          if (cur.eof()) {
            ret = OB_INVALID_DATA;
          } else if (',' == cur.cur()) {
            ++cur.pos_;
          } else if ('}' == cur.cur()) {
            ++cur.pos_;
            done = true;
          } else {
            ret = OB_INVALID_DATA;
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      cur.skip_ws();
      if (!cur.eof()) {
        ret = OB_INVALID_DATA;
      }
    }
    if (OB_SUCC(ret)) {
      // The member vector is stable now, so text payload addresses are final.
      for (size_t i = 0; i < members.size(); ++i) {
        if (FS_TYPE_VARCHAR == members[i].value_.type_ && !members[i].value_.null_) {
          members[i].value_.str_ptr_ = members[i].text_.data();
          members[i].value_.str_len_ = static_cast<int32_t>(members[i].text_.size());
        }
      }
    }
  }
  return ret;
}

int ObJsonlFileReader::infer_schema(ObFileSqlSchema &schema)
{
  int ret = OB_SUCCESS;
  bool eof = false;
  schema.columns_.clear();
  schema_.columns_.clear();
  schema_ready_ = false;
  std::vector<std::string> names;
  std::vector<ObFileSqlType> types;
  std::vector<bool> nullable;
  std::vector<int64_t> seen_rows;
  std::vector<int64_t> max_length;
  std::vector<int32_t> datetime_scale;
  int64_t rows = 0;
  if (OB_FAIL(input_.seek(0))) {
  } else {
    line_no_ = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(read_line(eof))) {
      } else if (eof) {
        break;
      } else {
        // Blank lines are ignored rather than treated as malformed rows.
        bool blank = true;
        for (size_t i = 0; blank && i < line_.size(); ++i) {
          const char c = line_[i];
          blank = (' ' == c || '\t' == c || '\r' == c);
        }
        if (blank) {
          continue;
        }
        if (OB_FAIL(parse_object(line_.data(), static_cast<int64_t>(line_.size()), members_))) {
          if (OB_NOT_SUPPORTED != ret) {
            ret = OB_INVALID_DATA;
            set_error(line_no_, 0, "invalid json object");
          }
        } else {
          ++rows;
          for (size_t i = 0; i < members_.size(); ++i) {
            const Member &member = members_[i];
            int64_t index = -1;
            for (size_t j = 0; j < names.size(); ++j) {
              if (names[j] == member.key_) {
                index = static_cast<int64_t>(j);
                break;
              }
            }
            if (index < 0) {
              if (names.size() >= static_cast<size_t>(JSONL_MAX_COLUMNS)) {
                ret = OB_SIZE_OVERFLOW;
                set_error(line_no_, 0, "too many json columns");
                break;
              }
              names.push_back(member.key_);
              types.push_back(FS_TYPE_NULL);
              nullable.push_back(false);
              seen_rows.push_back(0);
              max_length.push_back(0);
              datetime_scale.push_back(0);
              index = static_cast<int64_t>(names.size()) - 1;
            }
            ++seen_rows[index];
            if (member.value_.null_) {
              nullable[index] = true;
            } else {
              types[index] = merge_file_sql_type(types[index], member.value_.type_);
              int64_t length = 32;
              if (FS_TYPE_VARCHAR == member.value_.type_) {
                length = static_cast<int64_t>(member.text_.size());
              }
              max_length[index] = std::max(max_length[index], length);
              if (FS_TYPE_DATETIME == member.value_.type_ && member.text_.size() > 19
                  && '.' == member.text_[19]) {
                datetime_scale[index] = std::max<int32_t>(
                    datetime_scale[index], static_cast<int32_t>(member.text_.size() - 20));
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (names.empty()) {
        ret = OB_INVALID_DATA;
        set_error(line_no_, 0, "jsonl file contains no objects");
      } else {
        for (size_t i = 0; i < names.size(); ++i) {
          ObFileSqlType type = types[i];
          bool is_nullable = nullable[i] || seen_rows[i] < rows;
          if (FS_TYPE_NULL == type || FS_TYPE_INVALID == type) {
            type = FS_TYPE_NULL;
            is_nullable = true;
          }
          ObFileSqlColumn column(names[i], type, is_nullable);
          column.max_length_ = max_length[i];
          column.datetime_scale_ = datetime_scale[i];
          schema_.columns_.push_back(column);
        }
        schema = schema_;
        schema_ready_ = true;
      }
    }
  }
  return ret;
}

int ObJsonlFileReader::start_scan()
{
  int ret = OB_SUCCESS;
  row_count_ = 0;
  line_no_ = 0;
  column_index_.assign(schema_.columns_.size(), -1);
  scratch_.assign(schema_.columns_.size(), std::string());
  if (!schema_ready_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(input_.seek(0))) {
  }
  return ret;
}

int ObJsonlFileReader::coerce_to_schema(std::vector<ObFileSqlValue> &row)
{
  int ret = OB_SUCCESS;
  for (size_t i = 0; OB_SUCC(ret) && i < row.size(); ++i) {
    ObFileSqlValue &value = row[i];
    const ObFileSqlType target = schema_.columns_[i].type_;
    if (value.null_) {
      value.type_ = target;
    } else if (value.type_ == target) {
      // nothing to do
    } else if (FS_TYPE_DOUBLE == target
               && (FS_TYPE_BIGINT == value.type_ || FS_TYPE_BOOLEAN == value.type_)) {
      value.double_value_ = static_cast<double>(value.int_value_);
      value.type_ = FS_TYPE_DOUBLE;
    } else if (FS_TYPE_BIGINT == target
               && (FS_TYPE_BOOLEAN == value.type_ || FS_TYPE_DOUBLE == value.type_)) {
      if (FS_TYPE_DOUBLE == value.type_) {
        value.int_value_ = static_cast<int64_t>(value.double_value_);
      }
      value.type_ = FS_TYPE_BIGINT;
    } else if (FS_TYPE_BOOLEAN == target && FS_TYPE_BIGINT == value.type_) {
      value.int_value_ = (0 != value.int_value_) ? 1 : 0;
      value.type_ = FS_TYPE_BOOLEAN;
    } else if (FS_TYPE_DATETIME == target && FS_TYPE_DATE == value.type_) {
      value.datetime_value_ = static_cast<int64_t>(value.date_value_) * 86400 * 1000000;
      value.type_ = FS_TYPE_DATETIME;
    } else if (FS_TYPE_VARCHAR == target) {
      char buf[64];
      int32_t len = 0;
      switch (value.type_) {
        case FS_TYPE_BIGINT:
          len = snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value.int_value_));
          break;
        case FS_TYPE_DOUBLE:
          len = snprintf(buf, sizeof(buf), "%.17g", value.double_value_);
          break;
        case FS_TYPE_BOOLEAN:
          len = snprintf(buf, sizeof(buf), "%s", value.int_value_ ? "true" : "false");
          break;
        default:
          ret = OB_INVALID_DATA;
          break;
      }
      if (OB_SUCC(ret) && len > 0) {
        scratch_[i].assign(buf, static_cast<size_t>(len));
        value.str_ptr_ = scratch_[i].data();
        value.str_len_ = len;
        value.type_ = FS_TYPE_VARCHAR;
      }
    } else {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int ObJsonlFileReader::next_row(std::vector<ObFileSqlValue> &row)
{
  int ret = OB_SUCCESS;
  bool eof = false;
  row.clear();
  row.assign(schema_.columns_.size(), ObFileSqlValue());
  while (OB_SUCC(ret)) {
    if (OB_FAIL(read_line(eof))) {
    } else if (eof) {
      ret = OB_ITER_END;
    } else {
      bool blank = true;
      for (size_t i = 0; blank && i < line_.size(); ++i) {
        const char c = line_[i];
        blank = (' ' == c || '\t' == c || '\r' == c);
      }
      if (blank) {
        continue;
      } else if (OB_FAIL(parse_object(line_.data(), static_cast<int64_t>(line_.size()),
                                      members_))) {
        if (OB_NOT_SUPPORTED != ret) {
          ret = OB_INVALID_DATA;
          set_error(line_no_, 0, "invalid json object");
        }
      } else {
        for (size_t i = 0; i < schema_.columns_.size(); ++i) {
          row[i].null_ = true;
          row[i].type_ = schema_.columns_[i].type_;
        }
        for (size_t i = 0; i < members_.size(); ++i) {
          for (size_t j = 0; j < schema_.columns_.size(); ++j) {
            if (schema_.columns_[j].name_ == members_[i].key_) {
              row[j] = members_[i].value_;
              // Preserve the original JSON string when date inference widens
              // to VARCHAR after a later row introduces ordinary text.
              if (!row[j].null_ && FS_TYPE_VARCHAR == schema_.columns_[j].type_
                  && (FS_TYPE_DATE == row[j].type_ || FS_TYPE_DATETIME == row[j].type_)) {
                row[j].type_ = FS_TYPE_VARCHAR;
                row[j].str_ptr_ = members_[i].text_.data();
                row[j].str_len_ = static_cast<int32_t>(members_[i].text_.size());
              }
              break;
            }
          }
        }
        if (OB_FAIL(coerce_to_schema(row))) {
          set_error(line_no_, 0, "json value cannot be converted to the inferred column type");
        }
        break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ++row_count_;
  }
  return ret;
}

} // namespace filesql
} // namespace sql
} // namespace oceanbase
