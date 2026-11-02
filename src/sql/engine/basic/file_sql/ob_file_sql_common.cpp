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

#include "sql/engine/basic/file_sql/ob_file_sql_common.h"
#include "sql/engine/basic/file_sql/ob_file_sql_reader.h"
#include "sql/engine/basic/file_sql/ob_csv_file_reader.h"
#include "sql/engine/basic/file_sql/ob_jsonl_file_reader.h"
#include "sql/engine/basic/file_sql/ob_parquet_file_reader.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <new>

#include "share/ob_define.h"
#include "share/ob_errno.h"

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace filesql
{

using namespace oceanbase::common;

void ObFileSqlReader::set_error(const int64_t line, const int64_t column, const char *fmt, ...)
{
  char buffer[512];
  buffer[0] = '\0';
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  err_msg_.assign(buffer);
  err_line_ = line;
  err_column_ = column;
}

const char *file_sql_type_name(ObFileSqlType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case FS_TYPE_NULL: name = "NULL"; break;
    case FS_TYPE_BIGINT: name = "BIGINT"; break;
    case FS_TYPE_DOUBLE: name = "DOUBLE"; break;
    case FS_TYPE_BOOLEAN: name = "BOOLEAN"; break;
    case FS_TYPE_VARCHAR: name = "VARCHAR"; break;
    case FS_TYPE_DATE: name = "DATE"; break;
    case FS_TYPE_DATETIME: name = "DATETIME"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

bool is_valid_file_sql_utf8(const char *ptr, const int64_t len)
{
  for (int64_t pos = 0; pos < len; ++pos) {
    const unsigned char lead = static_cast<unsigned char>(ptr[pos]);
    if (lead < 0x80) { continue; }
    int tail = 0;
    if (lead >= 0xC2 && lead <= 0xDF) { tail = 1; }
    else if (lead >= 0xE0 && lead <= 0xEF) { tail = 2; }
    else if (lead >= 0xF0 && lead <= 0xF4) { tail = 3; }
    else { return false; }
    if (tail > len - pos - 1) { return false; }
    const unsigned char second = static_cast<unsigned char>(ptr[pos + 1]);
    if ((lead == 0xE0 && second < 0xA0) || (lead == 0xED && second > 0x9F)
        || (lead == 0xF0 && second < 0x90) || (lead == 0xF4 && second > 0x8F)) {
      return false;
    }
    for (int i = 0; i < tail; ++i) {
      const unsigned char next = static_cast<unsigned char>(ptr[++pos]);
      if (next < 0x80 || next > 0xBF) { return false; }
    }
  }
  return true;
}

namespace
{

const int32_t FILE_SQL_INPUT_BUFFER_SIZE = 256 * 1024;

bool is_digit(const char c) { return c >= '0' && c <= '9'; }

bool equals_ignore_case(const char *ptr, const int64_t len, const char *literal)
{
  const int64_t literal_len = static_cast<int64_t>(strlen(literal));
  bool equal = (len == literal_len);
  for (int64_t i = 0; equal && i < len; ++i) {
    char a = ptr[i];
    char b = literal[i];
    if (a >= 'A' && a <= 'Z') { a = static_cast<char>(a - 'A' + 'a'); }
    if (b >= 'A' && b <= 'Z') { b = static_cast<char>(b - 'A' + 'a'); }
    equal = (a == b);
  }
  return equal;
}

bool is_all_digits(const char *ptr, const int64_t begin, const int64_t end)
{
  bool result = (end > begin);
  for (int64_t i = begin; result && i < end; ++i) {
    result = is_digit(ptr[i]);
  }
  return result;
}

int64_t days_in_month(const int64_t year, const int64_t month)
{
  static const int32_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int64_t result = days[month - 1];
  if (2 == month) {
    const bool leap = (0 == year % 4 && 0 != year % 100) || 0 == year % 400;
    if (leap) { result = 29; }
  }
  return result;
}

// Days since 1970-01-01 for a proleptic Gregorian date.
int64_t days_from_civil(int64_t year, const int64_t month, const int64_t day)
{
  year -= (month <= 2) ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Parses "YYYY-MM-DD" strictly.  Returns false when the text is not a valid
// calendar date.
bool parse_date_text(const char *ptr, const int64_t len, int32_t &days)
{
  bool ok = (10 == len)
            && is_digit(ptr[0]) && is_digit(ptr[1]) && is_digit(ptr[2]) && is_digit(ptr[3])
            && '-' == ptr[4]
            && is_digit(ptr[5]) && is_digit(ptr[6])
            && '-' == ptr[7]
            && is_digit(ptr[8]) && is_digit(ptr[9]);
  if (ok) {
    const int64_t year = (ptr[0] - '0') * 1000 + (ptr[1] - '0') * 100 + (ptr[2] - '0') * 10
                         + (ptr[3] - '0');
    const int64_t month = (ptr[5] - '0') * 10 + (ptr[6] - '0');
    const int64_t day = (ptr[8] - '0') * 10 + (ptr[9] - '0');
    ok = (year >= 1 && year <= 9999) && (month >= 1 && month <= 12) && (day >= 1)
         && (day <= days_in_month(year, month));
    if (ok) {
      days = static_cast<int32_t>(days_from_civil(year, month, day));
    }
  }
  return ok;
}

// Parses "YYYY-MM-DD[ T]HH:MM:SS[.ffffff]" strictly.
bool parse_datetime_text(const char *ptr, const int64_t len, int64_t &microseconds)
{
  bool ok = (len >= 19) && (' ' == ptr[10] || 'T' == ptr[10] || 't' == ptr[10])
            && is_digit(ptr[11]) && is_digit(ptr[12]) && ':' == ptr[13]
            && is_digit(ptr[14]) && is_digit(ptr[15]) && ':' == ptr[16]
            && is_digit(ptr[17]) && is_digit(ptr[18]);
  int32_t days = 0;
  if (ok) {
    ok = parse_date_text(ptr, 10, days);
  }
  if (ok) {
    const int64_t hour = (ptr[11] - '0') * 10 + (ptr[12] - '0');
    const int64_t minute = (ptr[14] - '0') * 10 + (ptr[15] - '0');
    const int64_t second = (ptr[17] - '0') * 10 + (ptr[18] - '0');
    ok = hour <= 23 && minute <= 59 && second <= 59;
    int64_t pos = 19;
    int64_t fraction = 0;
    if (ok && pos < len && '.' == ptr[pos]) {
      ++pos;
      const int64_t frac_begin = pos;
      while (pos < len && is_digit(ptr[pos])) {
        if (pos - frac_begin < 6) { fraction = fraction * 10 + ptr[pos] - '0'; }
        ++pos;
      }
      ok = pos > frac_begin && pos - frac_begin <= 6;
      for (int64_t i = pos - frac_begin; i < 6; ++i) { fraction *= 10; }
    }
    if (ok) {
      // Trailing characters are not accepted; the whole cell must be consumed.
      ok = (pos == len);
    }
    if (ok) {
      microseconds = (static_cast<int64_t>(days) * 86400 + hour * 3600
                      + minute * 60 + second) * 1000000 + fraction;
    }
  }
  return ok;
}

bool parse_integer_text(const char *ptr, const int64_t len, int64_t &value)
{
  bool ok = len > 0;
  int64_t pos = 0;
  bool negative = false;
  if (ok && (ptr[0] == '+' || ptr[0] == '-')) {
    negative = ('-' == ptr[0]);
    pos = 1;
  }
  ok = ok && is_all_digits(ptr, pos, len);
  if (ok) {
    // Accumulate in unsigned space so that INT64_MIN is representable.
    uint64_t acc = 0;
    const uint64_t limit = negative ? (static_cast<uint64_t>(INT64_MAX) + 1)
                                    : static_cast<uint64_t>(INT64_MAX);
    for (int64_t i = pos; ok && i < len; ++i) {
      const uint64_t digit = static_cast<uint64_t>(ptr[i] - '0');
      if (acc > (limit - digit) / 10) {
        ok = false;
      } else {
        acc = acc * 10 + digit;
      }
    }
    if (ok) {
      value = negative ? static_cast<int64_t>(0 - acc) : static_cast<int64_t>(acc);
    }
  }
  return ok;
}

bool parse_double_text(const char *ptr, const int64_t len, double &value)
{
  bool ok = len > 0 && len < 512;
  int64_t pos = 0;
  if (ok && (ptr[0] == '+' || ptr[0] == '-')) { pos = 1; }
  const int64_t int_begin = pos;
  while (ok && pos < len && is_digit(ptr[pos])) { ++pos; }
  const bool has_int = pos > int_begin;
  bool has_frac = false;
  if (ok && pos < len && '.' == ptr[pos]) {
    ++pos;
    const int64_t frac_begin = pos;
    while (ok && pos < len && is_digit(ptr[pos])) { ++pos; }
    has_frac = pos > frac_begin;
  }
  ok = ok && (has_int || has_frac);
  bool has_exp = false;
  if (ok && pos < len && ('e' == ptr[pos] || 'E' == ptr[pos])) {
    ++pos;
    if (ok && pos < len && (ptr[pos] == '+' || ptr[pos] == '-')) { ++pos; }
    const int64_t exp_begin = pos;
    while (ok && pos < len && is_digit(ptr[pos])) { ++pos; }
    has_exp = pos > exp_begin;
    ok = has_exp;
  }
  ok = ok && (pos == len);
  if (ok) {
    char stack_buf[512];
    MEMCPY(stack_buf, ptr, len);
    stack_buf[len] = '\0';
    char *end = NULL;
    errno = 0;
    const double parsed = ::strtod(stack_buf, &end);
    ok = (end == stack_buf + len) && (0 == errno);
    if (ok) {
      value = parsed;
    }
  }
  return ok;
}

} // namespace

ObFileSqlType classify_text_value(const char *ptr, const int64_t len, const bool quoted)
{
  ObFileSqlType type = FS_TYPE_VARCHAR;
  if (quoted) {
    type = FS_TYPE_VARCHAR;
  } else if (NULL == ptr || len <= 0) {
    type = FS_TYPE_NULL;
  } else if (equals_ignore_case(ptr, len, "true") || equals_ignore_case(ptr, len, "false")) {
    type = FS_TYPE_BOOLEAN;
  } else {
    int64_t int_value = 0;
    double double_value = 0.0;
    int32_t date_value = 0;
    int64_t datetime_value = 0;
    if (parse_integer_text(ptr, len, int_value)) {
      type = FS_TYPE_BIGINT;
    } else if (parse_double_text(ptr, len, double_value)) {
      type = FS_TYPE_DOUBLE;
    } else if (parse_date_text(ptr, len, date_value)) {
      type = FS_TYPE_DATE;
    } else if (parse_datetime_text(ptr, len, datetime_value)) {
      type = FS_TYPE_DATETIME;
    } else {
      type = FS_TYPE_VARCHAR;
    }
  }
  return type;
}

ObFileSqlType merge_file_sql_type(ObFileSqlType current, ObFileSqlType incoming)
{
  ObFileSqlType merged = FS_TYPE_VARCHAR;
  if (FS_TYPE_INVALID == incoming || FS_TYPE_NULL == incoming) {
    merged = (FS_TYPE_INVALID == current) ? FS_TYPE_NULL : current;
  } else if (FS_TYPE_INVALID == current || FS_TYPE_NULL == current) {
    merged = incoming;
  } else if (current == incoming) {
    merged = current;
  } else if (FS_TYPE_VARCHAR == current || FS_TYPE_VARCHAR == incoming) {
    merged = FS_TYPE_VARCHAR;
  } else if ((FS_TYPE_BIGINT == current && FS_TYPE_DOUBLE == incoming)
             || (FS_TYPE_DOUBLE == current && FS_TYPE_BIGINT == incoming)) {
    merged = FS_TYPE_DOUBLE;
  } else if ((FS_TYPE_BOOLEAN == current && FS_TYPE_BIGINT == incoming)
             || (FS_TYPE_BIGINT == current && FS_TYPE_BOOLEAN == incoming)) {
    merged = FS_TYPE_BIGINT;
  } else if ((FS_TYPE_BOOLEAN == current && FS_TYPE_DOUBLE == incoming)
             || (FS_TYPE_DOUBLE == current && FS_TYPE_BOOLEAN == incoming)) {
    merged = FS_TYPE_DOUBLE;
  } else if ((FS_TYPE_DATE == current && FS_TYPE_DATETIME == incoming)
             || (FS_TYPE_DATETIME == current && FS_TYPE_DATE == incoming)) {
    merged = FS_TYPE_DATETIME;
  } else {
    // Numeric/textual mixtures that cannot be widened losslessly degrade to
    // text, which is the only representation that preserves every value.
    merged = FS_TYPE_VARCHAR;
  }
  return merged;
}

bool parse_text_value(const char *ptr, const int64_t len, ObFileSqlType type,
                      ObFileSqlValue &value)
{
  bool ok = true;
  value.reset();
  value.type_ = type;
  if (len == 0 && FS_TYPE_VARCHAR == type) {
    value.null_ = false;
    value.str_ptr_ = ptr;
    value.str_len_ = 0;
  } else if (NULL == ptr || len <= 0) {
    value.null_ = true;
    ok = true;
  } else {
    value.null_ = false;
    switch (type) {
      case FS_TYPE_BIGINT: {
        if (equals_ignore_case(ptr, len, "true") || equals_ignore_case(ptr, len, "false")) {
          value.int_value_ = equals_ignore_case(ptr, len, "true") ? 1 : 0;
        } else {
          ok = parse_integer_text(ptr, len, value.int_value_);
        }
        break;
      }
      case FS_TYPE_DOUBLE: {
        int64_t int_value = 0;
        if (equals_ignore_case(ptr, len, "true") || equals_ignore_case(ptr, len, "false")) {
          value.double_value_ = equals_ignore_case(ptr, len, "true") ? 1.0 : 0.0;
        } else if (parse_integer_text(ptr, len, int_value)) {
          value.double_value_ = static_cast<double>(int_value);
          ok = true;
        } else {
          ok = parse_double_text(ptr, len, value.double_value_);
        }
        break;
      }
      case FS_TYPE_BOOLEAN: {
        if (equals_ignore_case(ptr, len, "true")) {
          value.int_value_ = 1;
        } else if (equals_ignore_case(ptr, len, "false")) {
          value.int_value_ = 0;
        } else if (parse_integer_text(ptr, len, value.int_value_)) {
          value.int_value_ = (0 != value.int_value_) ? 1 : 0;
        } else {
          ok = false;
        }
        break;
      }
      case FS_TYPE_DATE: {
        ok = parse_date_text(ptr, len, value.date_value_);
        break;
      }
      case FS_TYPE_DATETIME: {
        ok = parse_datetime_text(ptr, len, value.datetime_value_);
        if (!ok) {
          // A DATE value widens into DATETIME at midnight.
          int32_t days = 0;
          if (parse_date_text(ptr, len, days)) {
            value.datetime_value_ = static_cast<int64_t>(days) * 86400 * 1000000;
            ok = true;
          }
        }
        break;
      }
      case FS_TYPE_VARCHAR:
      case FS_TYPE_NULL:
      case FS_TYPE_INVALID:
      default: {
        value.type_ = FS_TYPE_VARCHAR;
        value.str_ptr_ = ptr;
        value.str_len_ = static_cast<int32_t>(len);
        ok = true;
        break;
      }
    }
  }
  return ok;
}

///////////////////////////////////////////////////////////////////////////////
// Path helpers
///////////////////////////////////////////////////////////////////////////////

#ifdef _WIN32

namespace
{

std::wstring utf8_to_wide(const std::string &input)
{
  std::wstring result;
  if (!input.empty()) {
    const int need = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(),
                                           static_cast<int>(input.size()), NULL, 0);
    if (need > 0) {
      result.resize(static_cast<size_t>(need));
      ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.c_str(), static_cast<int>(input.size()),
                            &result[0], need);
    }
  }
  return result;
}

std::string wide_to_utf8(const std::wstring &input)
{
  std::string result;
  if (!input.empty()) {
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, input.c_str(),
                                           static_cast<int>(input.size()), NULL, 0, NULL, NULL);
    if (need > 0) {
      result.resize(static_cast<size_t>(need));
      ::WideCharToMultiByte(CP_UTF8, 0, input.c_str(), static_cast<int>(input.size()),
                            &result[0], need, NULL, NULL);
    }
  }
  return result;
}

std::wstring strip_extended_prefix(const std::wstring &path)
{
  std::wstring result = path;
  if (result.size() >= 8 && 0 == result.compare(0, 8, L"\\\\?\\UNC\\")) {
    result = L"\\\\" + result.substr(8);
  } else if (result.size() >= 4 && 0 == result.compare(0, 4, L"\\\\?\\")) {
    result = result.substr(4);
  }
  return result;
}

// Resolves the final path of an existing file system object.  Returns an empty
// string when the object does not exist or cannot be resolved.
std::wstring final_path_of(const std::wstring &path)
{
  std::wstring result;
  HANDLE handle = ::CreateFileW(path.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  if (INVALID_HANDLE_VALUE != handle) {
    const DWORD need = ::GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    if (need > 0) {
      std::vector<wchar_t> buffer(static_cast<size_t>(need) + 1, L'\0');
      const DWORD written = ::GetFinalPathNameByHandleW(handle, &buffer[0], need,
                                                        FILE_NAME_NORMALIZED);
      if (written > 0 && written <= need) {
        result.assign(&buffer[0], written);
      }
    }
    ::CloseHandle(handle);
  }
  return strip_extended_prefix(result);
}

} // namespace

int normalize_file_path(const std::string &input, std::string &output)
{
  int ret = OB_SUCCESS;
  output.clear();
  if (input.empty() || std::string::npos != input.find('\0')) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const std::wstring wide_input = utf8_to_wide(input);
    const DWORD need = wide_input.empty() ? 0 : ::GetFullPathNameW(wide_input.c_str(), 0, NULL, NULL);
    if (need <= 0) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      std::vector<wchar_t> buffer(static_cast<size_t>(need) + 1, L'\0');
      const DWORD written = ::GetFullPathNameW(wide_input.c_str(), need, &buffer[0], NULL);
      if (written <= 0 || written > need) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        std::wstring lexical(&buffer[0], written);
        // Resolve symbolic links and junctions whenever the object exists.
        const std::wstring final_path = final_path_of(lexical);
        output = wide_to_utf8(final_path.empty() ? lexical : final_path);
      }
    }
  }
  return ret;
}

bool path_is_within(const std::string &dir, const std::string &path)
{
  bool within = false;
  if (!dir.empty() && !path.empty()) {
    std::wstring wide_dir = utf8_to_wide(dir);
    std::wstring wide_path = utf8_to_wide(path);
    // Drop a trailing separator of the directory, but keep a drive root intact.
    while (wide_dir.size() > 3 && (L'\\' == wide_dir[wide_dir.size() - 1]
                                   || L'/' == wide_dir[wide_dir.size() - 1])) {
      wide_dir.erase(wide_dir.size() - 1);
    }
    std::transform(wide_dir.begin(), wide_dir.end(), wide_dir.begin(), ::towlower);
    std::transform(wide_path.begin(), wide_path.end(), wide_path.begin(), ::towlower);
    if (wide_path.size() >= wide_dir.size()) {
      within = (0 == wide_path.compare(0, wide_dir.size(), wide_dir));
      if (within && wide_path.size() > wide_dir.size()) {
        const wchar_t boundary = wide_path[wide_dir.size()];
        within = (L'\\' == boundary || L'/' == boundary
                  || L'\\' == wide_dir.back() || L'/' == wide_dir.back());
      }
      // A trailing separator inside the secure directory is not a new path.
      if (!within && wide_path.size() == wide_dir.size() + 1
          && (L'\\' == wide_path[wide_dir.size()] || L'/' == wide_path[wide_dir.size()])) {
        within = (0 == wide_path.compare(0, wide_dir.size(), wide_dir));
      }
    }
  }
  return within;
}

int expand_glob_pattern(const std::string &pattern, std::vector<std::string> &files)
{
  int ret = OB_SUCCESS;
  files.clear();
  std::string dir;
  std::string name_pattern;
  const size_t last_sep = pattern.find_last_of("\\/");
  if (std::string::npos == last_sep) {
    dir = ".";
    name_pattern = pattern;
  } else {
    dir = pattern.substr(0, last_sep);
    name_pattern = pattern.substr(last_sep + 1);
    if (dir.empty()) { dir = pattern.substr(0, 1); }
  }
  if (std::string::npos != dir.find_first_of("*?")) {
    // Nested wildcards are intentionally unsupported: they would allow a
    // pattern to escape the directory authorised by secure_file_priv.
    ret = OB_NOT_SUPPORTED;
  } else if (name_pattern.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    std::string normalized_dir;
    if (OB_FAIL(normalize_file_path(dir, normalized_dir))) {
    } else {
      const std::wstring wide_dir = utf8_to_wide(normalized_dir);
      const std::wstring wide_pattern = utf8_to_wide(name_pattern);
      // DuckDB renders glob results with forward slashes on every platform, and
      // the fixture result files rely on that when splitting the path.
      std::string display_dir = normalized_dir;
      for (size_t i = 0; i < display_dir.size(); ++i) {
        if ('\\' == display_dir[i]) {
          display_dir[i] = '/';
        }
      }
      WIN32_FIND_DATAW data;
      const std::wstring search = wide_dir + L"\\" + wide_pattern;
      HANDLE handle = ::FindFirstFileW(search.c_str(), &data);
      if (INVALID_HANDLE_VALUE != handle) {
        do {
          const std::wstring name(data.cFileName);
          if (L"." == name || L".." == name) {
            continue;
          }
          if (0 != (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
          }
          files.push_back(display_dir + "/" + wide_to_utf8(name));
        } while (::FindNextFileW(handle, &data));
        ::FindClose(handle);
      }
      if (files.empty()) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        std::sort(files.begin(), files.end());
      }
    }
  }
  return ret;
}

std::string file_base_name(const std::string &path)
{
  const size_t sep = path.find_last_of("\\/");
  return (std::string::npos == sep) ? path : path.substr(sep + 1);
}

#else // _WIN32

int normalize_file_path(const std::string &input, std::string &output)
{
  int ret = OB_SUCCESS;
  output.clear();
  if (input.empty() || std::string::npos != input.find('\0')) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char resolved[PATH_MAX] = {0};
    if (NULL != ::realpath(input.c_str(), resolved)) {
      output.assign(resolved);
    } else {
      // The object may not exist yet (e.g. COPY target); normalise lexically.
      std::string absolute = input;
      if ('/' != absolute[0]) {
        char cwd[PATH_MAX] = {0};
        if (NULL == ::getcwd(cwd, sizeof(cwd))) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          absolute = std::string(cwd) + "/" + input;
        }
      }
      if (OB_SUCC(ret)) {
        std::vector<std::string> parts;
        size_t begin = 0;
        while (begin <= absolute.size()) {
          const size_t end = absolute.find('/', begin);
          const std::string part = absolute.substr(
              begin, (std::string::npos == end) ? std::string::npos : end - begin);
          if (part.empty() || "." == part) {
            // skip
          } else if (".." == part) {
            if (!parts.empty()) { parts.pop_back(); }
          } else {
            parts.push_back(part);
          }
          if (std::string::npos == end) { break; }
          begin = end + 1;
        }
        output = "/";
        for (size_t i = 0; i < parts.size(); ++i) {
          if (i > 0) { output += "/"; }
          output += parts[i];
        }
      }
    }
  }
  return ret;
}

bool path_is_within(const std::string &dir, const std::string &path)
{
  bool within = false;
  std::string normalized_dir = dir;
  while (normalized_dir.size() > 1 && '/' == normalized_dir[normalized_dir.size() - 1]) {
    normalized_dir.erase(normalized_dir.size() - 1);
  }
  if (!normalized_dir.empty() && !path.empty() && path.size() >= normalized_dir.size()) {
    within = (0 == path.compare(0, normalized_dir.size(), normalized_dir));
    if (within && path.size() > normalized_dir.size()) {
      within = ('/' == path[normalized_dir.size()] || "/" == normalized_dir);
    }
  }
  return within;
}

int expand_glob_pattern(const std::string &pattern, std::vector<std::string> &files)
{
  int ret = OB_SUCCESS;
  files.clear();
  std::string dir;
  std::string name_pattern;
  const size_t last_sep = pattern.find_last_of('/');
  if (std::string::npos == last_sep) {
    dir = ".";
    name_pattern = pattern;
  } else {
    dir = pattern.substr(0, last_sep);
    name_pattern = pattern.substr(last_sep + 1);
    if (dir.empty()) { dir = "/"; }
  }
  if (std::string::npos != dir.find_first_of("*?")) {
    ret = OB_NOT_SUPPORTED;
  } else if (name_pattern.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    std::string normalized_dir;
    DIR *handle = NULL;
    if (OB_FAIL(normalize_file_path(dir, normalized_dir))) {
    } else if (NULL == (handle = ::opendir(normalized_dir.c_str()))) {
      ret = OB_FILE_NOT_EXIST;
    } else {
      struct dirent *entry = NULL;
      while (NULL != (entry = ::readdir(handle))) {
        const std::string name(entry->d_name);
        if ("." == name || ".." == name) {
          continue;
        }
        if (fnmatch(name_pattern.c_str(), name.c_str(), 0) == 0) {
          const std::string candidate = normalized_dir + "/" + name;
          struct stat st;
          if (0 == ::stat(candidate.c_str(), &st) && S_ISREG(st.st_mode)) {
            files.push_back(candidate);
          }
        }
      }
      ::closedir(handle);
      if (files.empty()) {
        ret = OB_FILE_NOT_EXIST;
      } else {
        std::sort(files.begin(), files.end());
      }
    }
  }
  return ret;
}

std::string file_base_name(const std::string &path)
{
  const size_t sep = path.find_last_of('/');
  return (std::string::npos == sep) ? path : path.substr(sep + 1);
}

#endif // _WIN32

///////////////////////////////////////////////////////////////////////////////
// ObFileSqlInput
///////////////////////////////////////////////////////////////////////////////

ObFileSqlInput::ObFileSqlInput()
  : handle_(NULL),
    fd_(-1),
    opened_(false),
    size_(0),
    last_write_time_(0),
    last_write_time_extra_(0),
    pos_(0),
    real_path_(),
    buf_(),
    buf_begin_(0),
    buf_len_(0),
    buf_pos_(0),
    has_pushback_(false),
    pushback_(0)
{
}

ObFileSqlInput::~ObFileSqlInput()
{
  close();
}

int ObFileSqlInput::open(const std::string &path, const std::string &secure_dir,
                         std::string &real_path)
{
  int ret = OB_SUCCESS;
  close();
  if (path.empty() || std::string::npos != path.find('\0')) {
    ret = OB_INVALID_ARGUMENT;
  } else {
#ifdef _WIN32
    const std::wstring wide_path = utf8_to_wide(path);
    HANDLE handle = ::CreateFileW(wide_path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (INVALID_HANDLE_VALUE == handle) {
      ret = (ERROR_FILE_NOT_FOUND == ::GetLastError()) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
    } else {
      LARGE_INTEGER file_size;
      FILETIME write_time;
      if (!::GetFileSizeEx(handle, &file_size)) {
        ret = OB_IO_ERROR;
      } else if (!::GetFileTime(handle, NULL, NULL, &write_time)) {
        ret = OB_IO_ERROR;
      } else {
        size_ = static_cast<int64_t>(file_size.QuadPart);
        last_write_time_ = write_time.dwHighDateTime;
        last_write_time_extra_ = write_time.dwLowDateTime;
      }
      std::wstring final_path;
      const DWORD need = ::GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
      if (need > 0) {
        std::vector<wchar_t> buffer(static_cast<size_t>(need) + 1, L'\0');
        const DWORD written = ::GetFinalPathNameByHandleW(handle, &buffer[0], need,
                                                          FILE_NAME_NORMALIZED);
        if (written > 0 && written <= need) {
          final_path = strip_extended_prefix(std::wstring(&buffer[0], written));
        }
      }
      if (OB_SUCC(ret) && !final_path.empty()) {
        real_path_ = wide_to_utf8(final_path);
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_NO_PRIVILEGE;
      }
      if (OB_FAIL(ret)) {
        ::CloseHandle(handle);
      } else {
        handle_ = handle;
      }
    }
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      ret = (ENOENT == errno) ? OB_FILE_NOT_EXIST : OB_IO_ERROR;
    } else {
      struct stat st;
      if (0 != ::fstat(fd, &st)) {
        ret = OB_IO_ERROR;
      } else if (!S_ISREG(st.st_mode)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        size_ = static_cast<int64_t>(st.st_size);
#ifdef __APPLE__
        last_write_time_ = st.st_mtimespec.tv_sec;
        last_write_time_extra_ = st.st_mtimespec.tv_nsec;
#else
        last_write_time_ = st.st_mtim.tv_sec;
        last_write_time_extra_ = st.st_mtim.tv_nsec;
#endif
      }
      char resolved[PATH_MAX] = {0};
      if (OB_SUCC(ret)) {
#ifdef __linux__
        // Resolve the opened descriptor, not the pathname: an attacker may
        // replace a symlink between open() and realpath().
        char descriptor_path[64];
        snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", fd);
        const ssize_t length = ::readlink(descriptor_path, resolved, sizeof(resolved) - 1);
        if (length <= 0 || length >= static_cast<ssize_t>(sizeof(resolved) - 1)) {
          ret = OB_ERR_NO_PRIVILEGE;
        } else {
          real_path_.assign(resolved, static_cast<size_t>(length));
        }
#elif defined(__APPLE__)
        if (0 != ::fcntl(fd, F_GETPATH, resolved)) {
          ret = OB_ERR_NO_PRIVILEGE;
        } else {
          real_path_.assign(resolved);
        }
#else
        ret = OB_NOT_SUPPORTED;
#endif
      }
      if (OB_FAIL(ret)) {
        ::close(fd);
      } else {
        fd_ = fd;
      }
    }
#endif
    if (OB_SUCC(ret) && !secure_dir.empty() && !path_is_within(secure_dir, real_path_)) {
      ret = OB_ERR_NO_PRIVILEGE;
    }
    if (OB_SUCC(ret)) {
      buf_.assign(FILE_SQL_INPUT_BUFFER_SIZE, '\0');
      opened_ = true;
      pos_ = 0;
      buf_begin_ = 0;
      buf_len_ = 0;
      buf_pos_ = 0;
      has_pushback_ = false;
      real_path = real_path_;
    }
    if (OB_FAIL(ret)) {
      close();
    }
  }
  return ret;
}

void ObFileSqlInput::close()
{
#ifdef _WIN32
  if (NULL != handle_) {
    ::CloseHandle(reinterpret_cast<HANDLE>(handle_));
    handle_ = NULL;
  }
#else
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
  opened_ = false;
  size_ = 0;
  last_write_time_ = 0;
  last_write_time_extra_ = 0;
  pos_ = 0;
  buf_begin_ = 0;
  buf_len_ = 0;
  buf_pos_ = 0;
  has_pushback_ = false;
  buf_.clear();
}

int ObFileSqlInput::check_unchanged() const
{
  int ret = OB_SUCCESS;
#ifdef _WIN32
  LARGE_INTEGER file_size;
  FILETIME write_time;
  if (!::GetFileSizeEx(reinterpret_cast<HANDLE>(handle_), &file_size)
      || !::GetFileTime(reinterpret_cast<HANDLE>(handle_), NULL, NULL, &write_time)) {
    ret = OB_IO_ERROR;
  } else if (file_size.QuadPart != size_
             || write_time.dwHighDateTime != last_write_time_
             || write_time.dwLowDateTime != last_write_time_extra_) {
    ret = OB_INVALID_DATA;
  }
#else
  struct stat st;
  if (0 != ::fstat(fd_, &st)) {
    ret = OB_IO_ERROR;
  } else {
#ifdef __APPLE__
    const int64_t seconds = st.st_mtimespec.tv_sec;
    const int64_t nanoseconds = st.st_mtimespec.tv_nsec;
#else
    const int64_t seconds = st.st_mtim.tv_sec;
    const int64_t nanoseconds = st.st_mtim.tv_nsec;
#endif
    if (st.st_size != size_ || seconds != last_write_time_
        || nanoseconds != last_write_time_extra_) {
      ret = OB_INVALID_DATA;
    }
  }
#endif
  if (OB_INVALID_DATA == ret) {
    LOG_WARN("file changed while FileSQL was reading", "path", real_path_.c_str());
  }
  return ret;
}

int ObFileSqlInput::read_at(const int64_t offset, char *buf, const int64_t len,
                            int64_t &nread)
{
  int ret = OB_SUCCESS;
  nread = 0;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (NULL == buf || len < 0 || offset < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(check_unchanged())) {
  } else if (len > 0) {
#ifdef _WIN32
    LARGE_INTEGER distance;
    distance.QuadPart = offset;
    if (!::SetFilePointerEx(reinterpret_cast<HANDLE>(handle_), distance, NULL, FILE_BEGIN)) {
      ret = OB_IO_ERROR;
    } else {
      DWORD total = 0;
      const DWORD want = static_cast<DWORD>(len);
      if (!::ReadFile(reinterpret_cast<HANDLE>(handle_), buf, want, &total, NULL)) {
        ret = (ERROR_HANDLE_EOF == ::GetLastError()) ? OB_SUCCESS : OB_IO_ERROR;
      } else {
        nread = static_cast<int64_t>(total);
      }
    }
#else
    int64_t total = 0;
    while (OB_SUCC(ret) && total < len) {
      const ssize_t got = ::pread(fd_, buf + total, static_cast<size_t>(len - total), offset + total);
      if (got < 0) {
        if (EINTR == errno) { continue; }
        ret = OB_IO_ERROR;
      } else if (0 == got) {
        break;
      } else {
        total += got;
      }
    }
    nread = total;
#endif
    if (OB_SUCC(ret)) {
      ret = check_unchanged();
    }
  }
  return ret;
}

int ObFileSqlInput::refill()
{
  int ret = OB_SUCCESS;
  int64_t nread = 0;
  if (OB_FAIL(read_at(pos_, &buf_[0], static_cast<int64_t>(buf_.size()), nread))) {
  } else if (0 == nread) {
    ret = OB_ITER_END;
  } else {
    buf_begin_ = pos_;
    buf_len_ = nread;
    buf_pos_ = 0;
    pos_ += nread;
  }
  return ret;
}

int ObFileSqlInput::get_char(char &c)
{
  int ret = OB_SUCCESS;
  if (has_pushback_) {
    c = pushback_;
    has_pushback_ = false;
  } else if (buf_pos_ >= buf_len_) {
    if (OB_FAIL(refill())) {
    } else {
      c = buf_[buf_pos_++];
    }
  } else {
    c = buf_[buf_pos_++];
  }
  return ret;
}

int ObFileSqlInput::peek_char(char &c)
{
  int ret = OB_SUCCESS;
  char got = 0;
  if (OB_FAIL(get_char(got))) {
  } else {
    c = got;
    has_pushback_ = true;
    pushback_ = got;
  }
  return ret;
}

void ObFileSqlInput::unget_char()
{
  if (!has_pushback_) {
    if (buf_pos_ > 0) {
      --buf_pos_;
    }
  }
}

int ObFileSqlInput::seek(const int64_t offset)
{
  int ret = OB_SUCCESS;
  if (!opened_) {
    ret = OB_NOT_INIT;
  } else if (offset < 0 || offset > size_) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    pos_ = offset;
    buf_begin_ = offset;
    buf_len_ = 0;
    buf_pos_ = 0;
    has_pushback_ = false;
  }
  return ret;
}

int resolve_secure_dir(const std::string &priv, std::string &dir, bool &unrestricted)
{
  int ret = OB_SUCCESS;
  dir.clear();
  unrestricted = false;
  if (priv.empty() || 0 == strcasecmp(priv.c_str(), "NULL")) {
    ret = OB_ERR_NO_PRIVILEGE;
  } else if (1 == priv.size() && ('/' == priv[0] || '\\' == priv[0])) {
    unrestricted = true;
  } else if (OB_FAIL(normalize_file_path(priv, dir))) {
    ret = OB_ERR_NO_PRIVILEGE;
  }
  return ret;
}

int validate_file_sql_descriptor(int fd, const std::string &secure_dir)
{
  int ret = OB_SUCCESS;
  std::string path;
  if (fd < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (secure_dir.empty()) {
    // An explicitly unrestricted COPY does not need a directory comparison.
  } else {
#ifdef _WIN32
    HANDLE handle = reinterpret_cast<HANDLE>(::_get_osfhandle(fd));
    const DWORD need = ::GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    if (need > 0) {
      std::vector<wchar_t> buffer(static_cast<size_t>(need) + 1, L'\0');
      const DWORD written = ::GetFinalPathNameByHandleW(handle, buffer.data(), need,
                                                       FILE_NAME_NORMALIZED);
      if (written > 0 && written < buffer.size()) {
        path = wide_to_utf8(strip_extended_prefix(std::wstring(buffer.data(), written)));
      }
    }
#elif defined(__linux__)
    char link_name[64];
    char resolved[PATH_MAX + 1];
    std::snprintf(link_name, sizeof(link_name), "/proc/self/fd/%d", fd);
    const ssize_t count = ::readlink(link_name, resolved, PATH_MAX);
    if (count > 0 && count < PATH_MAX) {
      path.assign(resolved, static_cast<size_t>(count));
    }
#elif defined(__APPLE__)
    char resolved[PATH_MAX];
    if (0 == ::fcntl(fd, F_GETPATH, resolved)) {
      path.assign(resolved);
    }
#endif
    if (path.empty() || !path_is_within(secure_dir, path)) {
      ret = OB_ERR_NO_PRIVILEGE;
    }
  }
  return ret;
}

int create_file_sql_reader(const ObFileFormat format, ObFileSqlReader *&reader)
{
  int ret = OB_SUCCESS;
  reader = NULL;
  if (FS_FORMAT_CSV == format) {
    reader = new (std::nothrow) ObCsvFileReader();
  } else if (FS_FORMAT_JSONL == format) {
    reader = new (std::nothrow) ObJsonlFileReader();
  } else if (FS_FORMAT_PARQUET == format) {
    reader = new (std::nothrow) ObParquetFileReader();
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret) && NULL == reader) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  }
  return ret;
}

void destroy_file_sql_reader(ObFileSqlReader *reader)
{
  if (NULL != reader) {
    reader->close();
    delete reader;
  }
}

} // namespace filesql
} // namespace sql
} // namespace oceanbase
