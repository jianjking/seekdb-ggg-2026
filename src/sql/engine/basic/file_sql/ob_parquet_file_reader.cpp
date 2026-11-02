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

#include "sql/engine/basic/file_sql/ob_parquet_file_reader.h"

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


// ---------------------------------------------------------------------------
// Thrift compact protocol (only the subset used by the Parquet footer/page
// headers).  Reading is strict: any malformed input aborts the scan instead of
// producing a partial metadata tree.
// ---------------------------------------------------------------------------
struct TValue
{
  int32_t type_;
  int32_t field_id_;
  int64_t i_;
  double d_;
  std::string s_;
  std::vector<TValue> list_;
  std::vector<TValue> fields_;

  TValue() : type_(0), field_id_(0), i_(0), d_(0.0), s_(), list_(), fields_() {}
};

const TValue *thrift_find(const TValue &node, const int32_t field_id)
{
  const TValue *found = NULL;
  for (size_t i = 0; NULL == found && i < node.fields_.size(); ++i) {
    if (node.fields_[i].field_id_ == field_id) {
      found = &node.fields_[i];
    }
  }
  return found;
}

int64_t thrift_int(const TValue &node, const int32_t field_id, const int64_t def_value)
{
  const TValue *field = thrift_find(node, field_id);
  return (NULL == field) ? def_value : field->i_;
}

class ThriftReader
{
public:
  ThriftReader(const char *buf, const int64_t len) : buf_(buf), len_(len), pos_(0), depth_(0) {}

  int64_t pos() const { return pos_; }

  int read_struct(TValue &out)
  {
    int ret = OB_SUCCESS;
    out.type_ = 12;
    int32_t last_id = 0;
    bool done = false;
    if (++depth_ > 32) {
      ret = OB_INVALID_DATA;
    }
    while (OB_SUCC(ret) && !done) {
      char header = 0;
      if (OB_FAIL(read_byte(header))) {
      } else {
        const int32_t type = static_cast<int32_t>(header) & 0x0F;
        const int32_t delta = (static_cast<int32_t>(header) >> 4) & 0x0F;
        if (0 == type) {
          done = true;
        } else {
          int64_t field_id = 0;
          if (0 == delta) {
            int64_t zigzag = 0;
            if (OB_FAIL(read_zigzag(zigzag))) {
            } else {
              field_id = zigzag;
            }
          } else {
            field_id = last_id + delta;
          }
          if (OB_SUCC(ret)) {
            last_id = static_cast<int32_t>(field_id);
            TValue value;
            value.field_id_ = last_id;
            if (OB_FAIL(read_value(type, value))) {
            } else {
              out.fields_.push_back(value);
            }
          }
        }
      }
    }
    --depth_;
    return ret;
  }

private:
  int read_byte(char &c)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_INVALID_DATA;
    } else {
      c = buf_[pos_++];
    }
    return ret;
  }

  int read_varint(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    value = 0;
    int32_t shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      char c = 0;
      if (OB_FAIL(read_byte(c))) {
      } else if (shift > 63) {
        ret = OB_INVALID_DATA;
      } else {
        value |= (static_cast<uint64_t>(c) & 0x7F) << shift;
        shift += 7;
        done = (0 == (static_cast<uint64_t>(c) & 0x80));
      }
    }
    return ret;
  }

  int read_zigzag(int64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    if (OB_FAIL(read_varint(raw))) {
    } else {
      value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }

  int read_list(TValue &out)
  {
    int ret = OB_SUCCESS;
    char header = 0;
    out.type_ = 9;
    if (OB_FAIL(read_byte(header))) {
    } else {
      int64_t count = (static_cast<int64_t>(header) >> 4) & 0x0F;
      const int32_t elem_type = static_cast<int32_t>(header) & 0x0F;
      if (15 == count) {
        uint64_t size = 0;
        if (OB_FAIL(read_varint(size))) {
        } else {
          count = static_cast<int64_t>(size);
        }
      }
      if (OB_SUCC(ret)) {
        if (count < 0 || count > 1000000 || count > len_ - pos_) {
          ret = OB_INVALID_DATA;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          TValue element;
          if (OB_FAIL(read_collection_value(elem_type, element))) {
          } else {
            out.list_.push_back(element);
          }
        }
      }
    }
    return ret;
  }

  int read_collection_value(const int32_t type, TValue &out)
  {
    int ret = OB_SUCCESS;
    if (1 == type || 2 == type) {
      char value = 0;
      if (OB_FAIL(read_byte(value))) {
      } else if (value != 1 && value != 2) {
        ret = OB_INVALID_DATA;
      } else {
        out.type_ = type;
        out.i_ = value == 1 ? 1 : 0;
      }
    } else {
      ret = read_value(type, out);
    }
    return ret;
  }

  int read_value(const int32_t type, TValue &out)
  {
    int ret = OB_SUCCESS;
    if (++depth_ > 32) {
      --depth_;
      return OB_INVALID_DATA;
    }
    out.type_ = type;
    if (1 == type) {
      out.i_ = 1;
    } else if (2 == type) {
      out.i_ = 0;
    } else if (3 == type) {
      char c = 0;
      if (OB_FAIL(read_byte(c))) {
      } else {
        out.i_ = static_cast<int64_t>(c);
      }
    } else if (4 == type || 5 == type || 6 == type) {
      int64_t value = 0;
      if (OB_FAIL(read_zigzag(value))) {
      } else {
        out.i_ = value;
      }
    } else if (7 == type) {
      if (pos_ + 8 > len_) {
        ret = OB_INVALID_DATA;
      } else {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) {
          bits |= static_cast<uint64_t>(static_cast<unsigned char>(buf_[pos_ + i])) << (8 * i);
        }
        pos_ += 8;
        MEMCPY(&out.d_, &bits, sizeof(double));
      }
    } else if (8 == type) {
      uint64_t size = 0;
      if (OB_FAIL(read_varint(size))) {
      } else if (size > static_cast<uint64_t>(len_ - pos_)) {
        ret = OB_INVALID_DATA;
      } else {
        out.s_.assign(buf_ + pos_, static_cast<size_t>(size));
        pos_ += static_cast<int64_t>(size);
      }
    } else if (9 == type || 10 == type) {
      ret = read_list(out);
    } else if (11 == type) {
      // map: not present in parquet metadata, skip defensively
      uint64_t size = 0;
      if (OB_FAIL(read_varint(size))) {
      } else if (size > 1000000 || size > static_cast<uint64_t>((len_ - pos_) / 2)) {
        ret = OB_INVALID_DATA;
      } else if (size > 0) {
        char kv = 0;
        if (OB_FAIL(read_byte(kv))) {
        } else {
          const int32_t kt = (static_cast<int32_t>(kv) >> 4) & 0x0F;
          const int32_t vt = static_cast<int32_t>(kv) & 0x0F;
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            TValue key;
            TValue value;
            if (OB_FAIL(read_collection_value(kt, key))
                || OB_FAIL(read_collection_value(vt, value))) {
            }
          }
        }
      }
    } else if (12 == type) {
      ret = read_struct(out);
    } else {
      ret = OB_INVALID_DATA;
    }
    --depth_;
    return ret;
  }

  const char *buf_;
  int64_t len_;
  int64_t pos_;
  int32_t depth_;
};

// ---------------------------------------------------------------------------
// Snappy raw decompression (RFC-less but stable format used by parquet).
// ---------------------------------------------------------------------------
int snappy_decompress(const char *input, const int64_t input_len,
                      const int64_t maximum_size, std::string &output)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  uint64_t expected = 0;
  int32_t shift = 0;
  bool varint_done = false;
  while (!varint_done && pos < input_len) {
    const unsigned char c = static_cast<unsigned char>(input[pos++]);
    if (shift > 63) {
      ret = OB_INVALID_DATA;
      break;
    }
    expected |= (static_cast<uint64_t>(c) & 0x7F) << shift;
    shift += 7;
    varint_done = (0 == (c & 0x80));
  }
  if (OB_SUCC(ret) && (!varint_done || expected > static_cast<uint64_t>(maximum_size))) {
    ret = OB_INVALID_DATA;
  }
  output.clear();
  if (OB_SUCC(ret)) {
    output.reserve(static_cast<size_t>(expected));
  }
  while (OB_SUCC(ret) && pos < input_len) {
    const unsigned char tag = static_cast<unsigned char>(input[pos++]);
    const int32_t kind = tag & 0x03;
    int64_t length = 0;
    int64_t offset = 0;
    if (0 == kind) {
      int32_t extra = (tag >> 2) & 0x3F;
      if (extra < 60) {
        length = extra + 1;
      } else {
        const int32_t bytes = extra - 59;
        if (pos + bytes > input_len) {
          ret = OB_INVALID_DATA;
          break;
        }
        int64_t value = 0;
        for (int32_t i = 0; i < bytes; ++i) {
          value |= static_cast<int64_t>(static_cast<unsigned char>(input[pos + i])) << (8 * i);
        }
        pos += bytes;
        length = value + 1;
      }
      if (pos + length > input_len || length > expected - output.size()) {
        ret = OB_INVALID_DATA;
        break;
      }
      output.append(input + pos, static_cast<size_t>(length));
      pos += length;
    } else {
      if (1 == kind) {
        if (pos + 1 > input_len) {
          ret = OB_INVALID_DATA;
          break;
        }
        length = 4 + ((tag >> 2) & 0x07);
        offset = ((static_cast<int64_t>(tag) >> 5) << 8)
                 | static_cast<unsigned char>(input[pos]);
        pos += 1;
      } else if (2 == kind) {
        if (pos + 2 > input_len) {
          ret = OB_INVALID_DATA;
          break;
        }
        length = 1 + (tag >> 2);
        offset = static_cast<unsigned char>(input[pos])
                 | (static_cast<int64_t>(static_cast<unsigned char>(input[pos + 1])) << 8);
        pos += 2;
      } else {
        if (pos + 4 > input_len) {
          ret = OB_INVALID_DATA;
          break;
        }
        length = 1 + (tag >> 2);
        offset = static_cast<int64_t>(static_cast<unsigned char>(input[pos]))
                 | (static_cast<int64_t>(static_cast<unsigned char>(input[pos + 1])) << 8)
                 | (static_cast<int64_t>(static_cast<unsigned char>(input[pos + 2])) << 16)
                 | (static_cast<int64_t>(static_cast<unsigned char>(input[pos + 3])) << 24);
        pos += 4;
      }
      if (offset <= 0 || static_cast<int64_t>(output.size()) < offset
          || length > expected - output.size()) {
        ret = OB_INVALID_DATA;
        break;
      }
      const size_t begin = output.size() - static_cast<size_t>(offset);
      for (int64_t i = 0; i < length; ++i) {
        output.push_back(output[begin + static_cast<size_t>(i)]);
      }
    }
    if (OB_SUCC(ret) && output.size() > static_cast<size_t>(expected)) {
      ret = OB_INVALID_DATA;
    }
  }
  if (OB_SUCC(ret) && output.size() != static_cast<size_t>(expected)) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

// ---------------------------------------------------------------------------
// RLE / bit-packed hybrid decoder used by definition levels and dictionary
// indices.
// ---------------------------------------------------------------------------
int decode_rle_hybrid(const char *data, const int64_t len, const int64_t count,
                      const int32_t bit_width, std::vector<uint32_t> &out,
                      int64_t &consumed)
{
  int ret = OB_SUCCESS;
  out.clear();
  consumed = 0;
  int64_t pos = 0;
  if (bit_width < 0 || bit_width > 32 || count < 0 || len < 0) {
    ret = OB_INVALID_DATA;
  } else {
    out.reserve(static_cast<size_t>(count));
  }
  while (OB_SUCC(ret) && static_cast<int64_t>(out.size()) < count && pos < len) {
    uint64_t header = 0;
    int32_t shift = 0;
    bool done = false;
    while (!done && pos < len) {
      const unsigned char c = static_cast<unsigned char>(data[pos++]);
      if (shift > 63) {
        ret = OB_INVALID_DATA;
        break;
      }
      header |= (static_cast<uint64_t>(c) & 0x7F) << shift;
      shift += 7;
      done = (0 == (c & 0x80));
    }
    if (OB_FAIL(ret) || !done) {
      ret = OB_INVALID_DATA;
      break;
    }
    if (header & 1) {
      const int64_t groups = static_cast<int64_t>(header >> 1);
      if (groups <= 0 || groups > INT64_MAX / 8
          || (bit_width > 0 && groups > (len - pos) / bit_width)) {
        ret = OB_INVALID_DATA;
        break;
      }
      const int64_t values = groups * 8;
      const int64_t bytes = groups * bit_width;
      const int64_t begin = pos;
      int64_t bit_pos = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < values
                          && static_cast<int64_t>(out.size()) < count; ++i) {
        uint32_t value = 0;
        for (int32_t b = 0; b < bit_width; ++b) {
          const int64_t byte_index = begin + (bit_pos >> 3);
          const int32_t bit_index = static_cast<int32_t>(bit_pos & 7);
          if (byte_index >= len) {
            ret = OB_INVALID_DATA;
            break;
          }
          const uint32_t bit =
              (static_cast<uint32_t>(static_cast<unsigned char>(data[byte_index])) >> bit_index) & 1u;
          value |= bit << b;
          ++bit_pos;
        }
        if (OB_SUCC(ret) && static_cast<int64_t>(out.size()) < count) {
          out.push_back(value);
        }
      }
      pos += bytes;
    } else {
      const int64_t run = static_cast<int64_t>(header >> 1);
      const int32_t bytes = (bit_width + 7) / 8;
      if (run <= 0 || pos + bytes > len) {
        ret = OB_INVALID_DATA;
        break;
      }
      uint32_t value = 0;
      for (int32_t i = 0; i < bytes; ++i) {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(data[pos + i])) << (8 * i);
      }
      pos += bytes;
      for (int64_t i = 0; i < run && static_cast<int64_t>(out.size()) < count; ++i) {
        out.push_back(value);
      }
    }
  }
  if (OB_SUCC(ret) && static_cast<int64_t>(out.size()) < count) {
    ret = OB_INVALID_DATA;
  }
  consumed = pos;
  return ret;
}

uint32_t read_le32(const char *p)
{
  return static_cast<uint32_t>(static_cast<unsigned char>(p[0]))
         | (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8)
         | (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16)
         | (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

int64_t read_le64(const char *p)
{
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
  }
  return static_cast<int64_t>(value);
}

// Parquet enums
enum
{
  PQ_TYPE_BOOLEAN = 0,
  PQ_TYPE_INT32 = 1,
  PQ_TYPE_INT64 = 2,
  PQ_TYPE_INT96 = 3,
  PQ_TYPE_FLOAT = 4,
  PQ_TYPE_DOUBLE = 5,
  PQ_TYPE_BYTE_ARRAY = 6,
  PQ_TYPE_FLBA = 7,
};

enum
{
  PQ_CODEC_UNCOMPRESSED = 0,
  PQ_CODEC_SNAPPY = 1,
};

enum
{
  PQ_PAGE_DATA = 0,
  PQ_PAGE_INDEX = 1,
  PQ_PAGE_DICTIONARY = 2,
  PQ_PAGE_DATA_V2 = 3,
};

enum
{
  PQ_ENC_PLAIN = 0,
  PQ_ENC_PLAIN_DICTIONARY = 2,
  PQ_ENC_RLE = 3,
  PQ_ENC_BIT_PACKED = 4,
  PQ_ENC_RLE_DICTIONARY = 8,
};

enum
{
  PQ_CONVERTED_UTF8 = 0,
  PQ_CONVERTED_DATE = 6,
  PQ_CONVERTED_TIMESTAMP_MILLIS = 9,
  PQ_CONVERTED_TIMESTAMP_MICROS = 10,
};

} // namespace

ObParquetFileReader::ObParquetFileReader()
  : input_(),
    opt_(),
    columns_(),
    row_groups_(),
    materialized_(),
    value_arena_(),
    current_row_group_(-1),
    current_row_(0),
    schema_ready_(false),
    footer_ready_(false),
    projection_set_(false)
{
}

int ObParquetFileReader::open(const std::string &path, const ObFileSqlOptions &opt,
                              const std::string &secure_dir)
{
  int ret = OB_SUCCESS;
  std::string real_path;
  clear_error();
  opt_ = opt;
  footer_ready_ = false;
  schema_ready_ = false;
  projection_set_ = false;
  current_row_group_ = -1;
  current_row_ = 0;
  if (OB_FAIL(input_.open(path, secure_dir, real_path))) {
    LOG_WARN("failed to open parquet file", K(ret), "path", path.c_str());
  } else {
    if (OB_FAIL(parse_footer())) {
      LOG_WARN("failed to parse parquet footer", K(ret), "path", path.c_str());
      if (OB_INVALID_DATA == ret || OB_NOT_SUPPORTED == ret) {
        set_error(0, 0, "invalid parquet footer");
      }
    } else {
    }
  }
  return ret;
}

void ObParquetFileReader::close()
{
  input_.close();
  schema_ready_ = false;
  footer_ready_ = false;
  columns_.clear();
  row_groups_.clear();
  materialized_.clear();
  value_arena_.clear();
}

int ObParquetFileReader::parse_footer()
{
  int ret = OB_SUCCESS;
  const int64_t file_size = input_.size();
  char tail[8];
  int64_t nread = 0;
  if (file_size < 12) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(input_.read_at(file_size - 8, tail, 8, nread)) || 8 != nread) {
    ret = OB_INVALID_DATA;
  } else if (0 != MEMCMP(tail + 4, "PAR1", 4)) {
    ret = OB_INVALID_DATA;
  } else {
    const uint32_t footer_len = read_le32(tail);
    char magic[4];
    if (static_cast<int64_t>(footer_len) + 12 > file_size
        || footer_len > static_cast<uint64_t>(opt_.maximum_object_size_)
        || OB_FAIL(input_.read_at(0, magic, 4, nread))
        || 4 != nread || 0 != MEMCMP(magic, "PAR1", 4)) {
      ret = OB_INVALID_DATA;
    } else {
      std::string footer;
      footer.resize(footer_len);
      if (footer_len > 0
          && (OB_FAIL(input_.read_at(file_size - 8 - footer_len, &footer[0], footer_len, nread))
              || static_cast<int64_t>(nread) != static_cast<int64_t>(footer_len))) {
        ret = OB_INVALID_DATA;
      } else {
        ThriftReader reader(footer.data(), static_cast<int64_t>(footer.size()));
        TValue meta;
        if (OB_FAIL(reader.read_struct(meta))) {
          ret = OB_INVALID_DATA;
        } else {
          const TValue *schema = thrift_find(meta, 2);
          if (NULL == schema || schema->list_.empty()) {
            ret = OB_INVALID_DATA;
          } else {
            // The first element is the root group; every following element must
            // be a primitive leaf because nested schemas are not supported.
            columns_.clear();
            for (size_t i = 1; OB_SUCC(ret) && i < schema->list_.size(); ++i) {
              const TValue &element = schema->list_[i];
              const int64_t num_children = thrift_int(element, 5, 0);
              const TValue *name = thrift_find(element, 4);
              if (num_children > 0) {
                ret = OB_NOT_SUPPORTED;
                set_error(0, 0, "nested parquet schemas are not supported");
              } else if (NULL == name) {
                ret = OB_INVALID_DATA;
              } else {
                ParquetColumn column;
                column.name_ = name->s_;
                column.physical_type_ = static_cast<int32_t>(thrift_int(element, 1, -1));
                column.converted_type_ = static_cast<int32_t>(thrift_int(element, 6, -1));
                column.repetition_type_ = static_cast<int32_t>(thrift_int(element, 3, 0));
                column.type_length_ = static_cast<int32_t>(thrift_int(element, 2, 0));
                column.schema_index_ = static_cast<int64_t>(i) - 1;
                if (column.physical_type_ < PQ_TYPE_BOOLEAN
                    || column.physical_type_ > PQ_TYPE_FLBA) {
                  ret = OB_NOT_SUPPORTED;
                  set_error(0, 0, "unsupported parquet column type");
                } else {
                  columns_.push_back(column);
                }
              }
            }
            const TValue *row_groups = thrift_find(meta, 4);
            if (OB_SUCC(ret) && NULL == row_groups) {
              ret = OB_INVALID_DATA;
            }
            row_groups_.clear();
            for (size_t i = 0; OB_SUCC(ret) && row_groups != NULL
                                && i < row_groups->list_.size(); ++i) {
              const TValue &rg_node = row_groups->list_[i];
              RowGroup rg;
              rg.num_rows_ = thrift_int(rg_node, 3, 0);
              rg.total_byte_size_ = thrift_int(rg_node, 2, 0);
              const TValue *chunks = thrift_find(rg_node, 1);
              if (rg.num_rows_ < 0 || rg.total_byte_size_ < 0 || NULL == chunks) {
                ret = OB_INVALID_DATA;
              } else if (chunks->list_.size() != columns_.size()) {
                ret = OB_INVALID_DATA;
                set_error(0, 0, "parquet column chunk count does not match the schema");
              }
              for (size_t c = 0; OB_SUCC(ret) && c < chunks->list_.size(); ++c) {
                const TValue &chunk_node = chunks->list_[c];
                const TValue *meta_data = thrift_find(chunk_node, 3);
                if (NULL == meta_data) {
                  ret = OB_INVALID_DATA;
                } else {
                  ParquetChunk chunk;
                  chunk.physical_type_ = static_cast<int32_t>(thrift_int(*meta_data, 1, -1));
                  chunk.codec_ = static_cast<int32_t>(thrift_int(*meta_data, 4, 0));
                  chunk.num_values_ = thrift_int(*meta_data, 5, 0);
                  chunk.data_page_offset_ = thrift_int(*meta_data, 9, -1);
                  chunk.dictionary_page_offset_ = thrift_int(*meta_data, 11, -1);
                  chunk.total_compressed_size_ = thrift_int(*meta_data, 7, 0);
                  if (chunk.data_page_offset_ < 0 || chunk.num_values_ != rg.num_rows_
                      || chunk.total_compressed_size_ < 0
                      || chunk.physical_type_ != columns_[c].physical_type_) {
                    ret = OB_INVALID_DATA;
                  } else {
                    rg.chunks_.push_back(chunk);
                  }
                }
              }
              if (OB_SUCC(ret)) {
                row_groups_.push_back(rg);
              }
            }
            if (OB_SUCC(ret)) {
              footer_ready_ = true;
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObParquetFileReader::infer_schema(ObFileSqlSchema &schema)
{
  int ret = OB_SUCCESS;
  schema_.columns_.clear();
  schema_ready_ = false;
  if (!footer_ready_) {
    ret = OB_INVALID_DATA;
  } else {
    for (size_t i = 0; OB_SUCC(ret) && i < columns_.size(); ++i) {
      const ParquetColumn &column = columns_[i];
      ObFileSqlType type = FS_TYPE_VARCHAR;
      switch (column.physical_type_) {
        case PQ_TYPE_BOOLEAN: type = FS_TYPE_BOOLEAN; break;
        case PQ_TYPE_INT32:
          type = (PQ_CONVERTED_DATE == column.converted_type_) ? FS_TYPE_DATE : FS_TYPE_BIGINT;
          break;
        case PQ_TYPE_INT64:
          type = (PQ_CONVERTED_TIMESTAMP_MILLIS == column.converted_type_
                  || PQ_CONVERTED_TIMESTAMP_MICROS == column.converted_type_)
                     ? FS_TYPE_DATETIME
                     : FS_TYPE_BIGINT;
          break;
        case PQ_TYPE_FLOAT:
        case PQ_TYPE_DOUBLE: type = FS_TYPE_DOUBLE; break;
        case PQ_TYPE_BYTE_ARRAY:
        case PQ_TYPE_FLBA: type = FS_TYPE_VARCHAR; break;
        default: {
          ret = OB_NOT_SUPPORTED;
          set_error(0, 0, "unsupported parquet column type");
          break;
        }
      }
      if (OB_SUCC(ret)) {
        ObFileSqlColumn out(column.name_, type, 1 == column.repetition_type_);
        if (FS_TYPE_DATETIME == type) {
          out.datetime_scale_ = PQ_CONVERTED_TIMESTAMP_MICROS == column.converted_type_ ? 6 : 3;
        }
        if (FS_TYPE_VARCHAR == type) {
          out.max_length_ = (column.type_length_ > 0) ? column.type_length_ : 65535;
        }
        schema_.columns_.push_back(out);
      }
    }
    if (OB_SUCC(ret)) {
      schema = schema_;
      schema_ready_ = true;
    }
  }
  return ret;
}

int ObParquetFileReader::set_projection(const std::vector<int64_t> &cols)
{
  projection_ = cols;
  projection_set_ = true;
  return 0;
}

int ObParquetFileReader::start_scan()
{
  int ret = OB_SUCCESS;
  row_count_ = 0;
  current_row_group_ = -1;
  current_row_ = 0;
  materialized_.clear();
  if (!schema_ready_) {
    ret = OB_NOT_INIT;
  }
  return ret;
}

int ObParquetFileReader::decode_column(const RowGroup &rg, const int64_t column_index,
                                       std::vector<ObFileSqlValue> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  if (column_index < 0 || column_index >= static_cast<int64_t>(rg.chunks_.size())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(decode_chunk(rg.chunks_[column_index], columns_[column_index], values))) {
  }
  return ret;
}

int ObParquetFileReader::load_row_group(const int64_t rg_index)
{
  int ret = OB_SUCCESS;
  materialized_.clear();
  value_arena_.clear();
  current_row_group_ = rg_index;
  current_row_ = 0;
  if (rg_index < 0 || rg_index >= static_cast<int64_t>(row_groups_.size())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const RowGroup &rg = row_groups_[rg_index];
    std::vector<bool> needed(columns_.size(), false);
    if (projection_set_) {
      for (size_t i = 0; i < projection_.size(); ++i) {
        if (projection_[i] >= 0 && projection_[i] < static_cast<int64_t>(columns_.size())) {
          needed[static_cast<size_t>(projection_[i])] = true;
        }
      }
    } else {
      for (size_t i = 0; i < columns_.size(); ++i) { needed[i] = true; }
    }
    int64_t needed_count = 0;
    for (size_t c = 0; c < needed.size(); ++c) { needed_count += needed[c] ? 1 : 0; }
    const int64_t value_bytes = std::max<int64_t>(needed_count, 1) * sizeof(ObFileSqlValue);
    if (needed_count > 0 && (rg.num_rows_ > opt_.maximum_object_size_ / value_bytes
        || rg.total_byte_size_ > opt_.maximum_object_size_)) {
      ret = OB_SIZE_OVERFLOW;
      set_error(0, 0, "parquet row group exceeds max_object_size");
    } else if (needed_count > 0) {
      value_arena_.reserve(static_cast<size_t>(std::max<int64_t>(rg.total_byte_size_, 4096)));
    }
    materialized_.resize(columns_.size());
    for (size_t c = 0; OB_SUCC(ret) && c < columns_.size(); ++c) {
      if (!needed[c]) {
        // Unprojected columns do not need one placeholder allocation per row.
      } else if (OB_FAIL(decode_column(rg, static_cast<int64_t>(c), materialized_[c]))) {
        if (OB_INVALID_DATA == ret || OB_NOT_SUPPORTED == ret) {
          set_error(0, static_cast<int64_t>(c) + 1, "failed to decode parquet column %s",
                    columns_[c].name_.c_str());
        }
      } else if (static_cast<int64_t>(materialized_[c].size()) != rg.num_rows_) {
        ret = OB_INVALID_DATA;
        set_error(0, static_cast<int64_t>(c) + 1, "parquet column %s decoded %lld values, %lld rows",
                  columns_[c].name_.c_str(),
                  static_cast<long long>(materialized_[c].size()),
                  static_cast<long long>(rg.num_rows_));
      }
    }
    if (OB_SUCC(ret)) {
      // String payloads were appended to one arena per row group; resolve the
      // offsets only after the arena reached its final address.
      const char *base = value_arena_.data();
      for (size_t c = 0; c < materialized_.size(); ++c) {
        for (size_t r = 0; r < materialized_[c].size(); ++r) {
          ObFileSqlValue &value = materialized_[c][r];
          if (!value.null_ && FS_TYPE_VARCHAR == value.type_ && NULL == value.str_ptr_) {
            value.str_ptr_ = base + value.int_value_;
          }
        }
      }
    }
  }
  return ret;
}

int ObParquetFileReader::next_row(std::vector<ObFileSqlValue> &row)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret)) {
    if (current_row_group_ < 0
        || current_row_group_ >= static_cast<int64_t>(row_groups_.size())
        || current_row_ >= row_groups_[current_row_group_].num_rows_) {
      const int64_t next_group = current_row_group_ + 1;
      if (next_group >= static_cast<int64_t>(row_groups_.size())) {
        ret = OB_ITER_END;
      } else if (OB_FAIL(load_row_group(next_group))) {
      }
    }
    if (OB_SUCC(ret) && current_row_ < row_groups_[current_row_group_].num_rows_) {
      break;
    }
  }
  if (OB_SUCC(ret)) {
    row.clear();
    row.reserve(columns_.size());
    for (size_t c = 0; c < columns_.size(); ++c) {
      if (materialized_[c].empty()) {
        row.push_back(ObFileSqlValue());
      } else {
        row.push_back(materialized_[c][static_cast<size_t>(current_row_)]);
      }
    }
    ++current_row_;
    ++row_count_;
  }
  return ret;
}

int ObParquetFileReader::decode_chunk(const ParquetChunk &chunk, const ParquetColumn &col,
                                      std::vector<ObFileSqlValue> &values)
{
  int ret = OB_SUCCESS;
  values.clear();
  if (chunk.codec_ != PQ_CODEC_UNCOMPRESSED && chunk.codec_ != PQ_CODEC_SNAPPY) {
    ret = OB_NOT_SUPPORTED;
    set_error(0, 0, "unsupported parquet compression codec %d", chunk.codec_);
  } else {
    const bool optional = (1 == col.repetition_type_);
    std::vector<ObFileSqlValue> dictionary;
    bool dictionary_ready = false;
    int64_t pos = (chunk.dictionary_page_offset_ >= 0) ? chunk.dictionary_page_offset_
                                                       : chunk.data_page_offset_;
    int64_t decoded = 0;
    while (OB_SUCC(ret) && decoded < chunk.num_values_) {
      char header_buf[4096];
      int64_t nread = 0;
      const int64_t header_read = std::min<int64_t>(sizeof(header_buf),
                                                    input_.size() - pos);
      if (header_read <= 0
          || OB_FAIL(input_.read_at(pos, header_buf, header_read, nread)) || nread <= 0) {
        ret = OB_INVALID_DATA;
        break;
      }
      ThriftReader header_reader(header_buf, nread);
      TValue header;
      if (OB_FAIL(header_reader.read_struct(header))) {
        ret = OB_INVALID_DATA;
        break;
      }
      const int64_t header_size = header_reader.pos();
      const int32_t page_type = static_cast<int32_t>(thrift_int(header, 1, -1));
      const int64_t uncompressed_size = thrift_int(header, 2, 0);
      const int64_t compressed_size = thrift_int(header, 3, 0);
      if (compressed_size < 0 || uncompressed_size < 0
          || compressed_size > opt_.maximum_object_size_
          || uncompressed_size > opt_.maximum_object_size_
          || pos < 0 || header_size > input_.size() - pos
          || compressed_size > input_.size() - pos - header_size) {
        ret = OB_INVALID_DATA;
        break;
      }
      std::string page;
      page.resize(static_cast<size_t>(compressed_size));
      if (compressed_size > 0
          && (OB_FAIL(input_.read_at(pos + header_size, &page[0], compressed_size, nread))
              || nread != compressed_size)) {
        ret = OB_INVALID_DATA;
        break;
      }
      pos += header_size + compressed_size;
      std::string raw;
      const char *data = page.data();
      int64_t data_len = compressed_size;
      // A V2 data page keeps its repetition/definition levels uncompressed and
      // only compresses the value section, so the page must not be inflated as
      // a whole.  V1 data pages and dictionary pages are compressed end to end.
      if (PQ_PAGE_DATA_V2 == page_type) {
        if (PQ_CODEC_SNAPPY != chunk.codec_ && PQ_CODEC_UNCOMPRESSED != chunk.codec_) {
          ret = OB_INVALID_DATA;
          break;
        }
      } else if (PQ_CODEC_SNAPPY == chunk.codec_) {
        if (OB_FAIL(snappy_decompress(page.data(), compressed_size, uncompressed_size, raw))) {
          ret = OB_INVALID_DATA;
          break;
        }
        data = raw.data();
        data_len = static_cast<int64_t>(raw.size());
      } else if (PQ_CODEC_UNCOMPRESSED == chunk.codec_) {
        if (uncompressed_size != compressed_size) {
          ret = OB_INVALID_DATA;
          break;
        }
      }
      if (PQ_PAGE_DICTIONARY == page_type) {
        const TValue *dict_header = thrift_find(header, 7);
        const int64_t dict_count = (NULL == dict_header) ? 0 : thrift_int(*dict_header, 1, 0);
        dictionary.clear();
        int64_t offset = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < dict_count; ++i) {
          ObFileSqlValue value;
          if (OB_FAIL(decode_plain_value(col, data, data_len, offset, value,
                                         value_arena_))) {
            ret = OB_INVALID_DATA;
          } else {
            dictionary.push_back(value);
          }
        }
        dictionary_ready = true;
      } else if (PQ_PAGE_DATA == page_type) {
        const TValue *data_header = thrift_find(header, 5);
        if (NULL == data_header) {
          ret = OB_INVALID_DATA;
          break;
        }
        const int64_t num_values = thrift_int(*data_header, 1, 0);
        if (num_values <= 0 || num_values > chunk.num_values_ - decoded) {
          ret = OB_INVALID_DATA;
          break;
        }
        const int32_t encoding = static_cast<int32_t>(thrift_int(*data_header, 2, 0));
        int64_t offset = 0;
        std::vector<uint32_t> def_levels;
        if (optional) {
          if (offset + 4 > data_len) {
            ret = OB_INVALID_DATA;
            break;
          }
          const uint32_t level_len = read_le32(data + offset);
          offset += 4;
          if (level_len > static_cast<uint32_t>(data_len - offset)) {
            ret = OB_INVALID_DATA;
            break;
          }
          int64_t consumed = 0;
          if (OB_FAIL(decode_rle_hybrid(data + offset, level_len, num_values, 1, def_levels,
                                        consumed))) {
            ret = OB_INVALID_DATA;
            break;
          }
          offset += level_len;
        }
        if (OB_FAIL(ret)) {
          break;
        }
        if (OB_FAIL(decode_values(col, encoding, data, data_len, offset, num_values,
                                  def_levels, dictionary, dictionary_ready, values,
                                  value_arena_))) {
          ret = OB_INVALID_DATA;
        } else {
          decoded += num_values;
        }
      } else if (PQ_PAGE_DATA_V2 == page_type) {
        const TValue *data_header = thrift_find(header, 8);
        if (NULL == data_header) {
          ret = OB_INVALID_DATA;
          break;
        }
        const int64_t num_values = thrift_int(*data_header, 1, 0);
        const int64_t num_nulls = thrift_int(*data_header, 2, 0);
        const int32_t encoding = static_cast<int32_t>(thrift_int(*data_header, 4, 0));
        const int64_t def_len = thrift_int(*data_header, 5, 0);
        const int64_t rep_len = thrift_int(*data_header, 6, 0);
        const int64_t is_compressed = thrift_int(*data_header, 7, 1);
        if (rep_len < 0 || def_len < 0 || rep_len > data_len || def_len > data_len - rep_len
            || num_values < 0 || num_values > chunk.num_values_ - decoded
            || num_nulls < 0 || num_nulls > num_values) {
          ret = OB_INVALID_DATA;
          break;
        }
        std::string values_raw;
        const char *value_data = data + rep_len + def_len;
        int64_t value_len = data_len - rep_len - def_len;
        if (is_compressed && PQ_CODEC_SNAPPY == chunk.codec_) {
          if (uncompressed_size < rep_len + def_len
              || OB_FAIL(snappy_decompress(value_data, value_len,
                                          uncompressed_size - rep_len - def_len, values_raw))) {
            ret = OB_INVALID_DATA;
            break;
          }
          value_data = values_raw.data();
          value_len = static_cast<int64_t>(values_raw.size());
        }
        std::vector<uint32_t> def_levels;
        if (optional && def_len > 0) {
          int64_t consumed = 0;
          if (OB_FAIL(decode_rle_hybrid(data + rep_len, def_len, num_values, 1, def_levels,
                                        consumed))) {
            ret = OB_INVALID_DATA;
            break;
          }
        }
        if (optional && num_nulls > 0 && def_levels.empty()) {
          ret = OB_INVALID_DATA;
          break;
        }
        int64_t offset = 0;
        if (OB_FAIL(decode_values(col, encoding, value_data, value_len, offset, num_values,
                                  def_levels, dictionary, dictionary_ready, values,
                                  value_arena_))) {
          ret = OB_INVALID_DATA;
        } else {
          decoded += num_values;
        }
      } else {
        // Index pages carry no row data.
      }
    }
    if (OB_SUCC(ret) && static_cast<int64_t>(values.size()) != chunk.num_values_) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

ObFileSqlType parquet_logical_type_of(const int32_t physical_type, const int32_t converted_type)
{
  ObFileSqlType type = FS_TYPE_VARCHAR;
  switch (physical_type) {
    case PQ_TYPE_BOOLEAN: type = FS_TYPE_BOOLEAN; break;
    case PQ_TYPE_INT32:
      type = (PQ_CONVERTED_DATE == converted_type) ? FS_TYPE_DATE : FS_TYPE_BIGINT;
      break;
    case PQ_TYPE_INT64:
      type = (PQ_CONVERTED_TIMESTAMP_MILLIS == converted_type
              || PQ_CONVERTED_TIMESTAMP_MICROS == converted_type)
                 ? FS_TYPE_DATETIME
                 : FS_TYPE_BIGINT;
      break;
    case PQ_TYPE_FLOAT:
    case PQ_TYPE_DOUBLE: type = FS_TYPE_DOUBLE; break;
    default: type = FS_TYPE_VARCHAR; break;
  }
  return type;
}

int ObParquetFileReader::decode_plain_value(const ParquetColumn &col, const char *data,
                                            const int64_t len, int64_t &offset,
                                            ObFileSqlValue &value, std::string &arena)
{
  int ret = OB_SUCCESS;
  value.reset();
  value.null_ = false;
  switch (col.physical_type_) {
    case PQ_TYPE_INT32: {
      if (offset + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t raw = 0;
        MEMCPY(&raw, data + offset, 4);
        offset += 4;
        if (PQ_CONVERTED_DATE == col.converted_type_) {
          value.type_ = FS_TYPE_DATE;
          value.date_value_ = raw;
        } else {
          value.type_ = FS_TYPE_BIGINT;
          value.int_value_ = static_cast<int64_t>(raw);
        }
      }
      break;
    }
    case PQ_TYPE_INT64: {
      if (offset + 8 > len) {
        ret = OB_INVALID_DATA;
      } else {
        const int64_t raw = read_le64(data + offset);
        offset += 8;
        if (PQ_CONVERTED_TIMESTAMP_MILLIS == col.converted_type_) {
          value.type_ = FS_TYPE_DATETIME;
          value.datetime_value_ = raw * 1000;
        } else if (PQ_CONVERTED_TIMESTAMP_MICROS == col.converted_type_) {
          value.type_ = FS_TYPE_DATETIME;
          value.datetime_value_ = raw;
        } else {
          value.type_ = FS_TYPE_BIGINT;
          value.int_value_ = raw;
        }
      }
      break;
    }
    case PQ_TYPE_FLOAT: {
      if (offset + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        float raw = 0.0f;
        MEMCPY(&raw, data + offset, 4);
        offset += 4;
        value.type_ = FS_TYPE_DOUBLE;
        value.double_value_ = static_cast<double>(raw);
      }
      break;
    }
    case PQ_TYPE_DOUBLE: {
      if (offset + 8 > len) {
        ret = OB_INVALID_DATA;
      } else {
        double raw = 0.0;
        MEMCPY(&raw, data + offset, 8);
        offset += 8;
        value.type_ = FS_TYPE_DOUBLE;
        value.double_value_ = raw;
      }
      break;
    }
    case PQ_TYPE_BYTE_ARRAY: {
      if (offset + 4 > len) {
        ret = OB_INVALID_DATA;
      } else {
        const uint32_t length = read_le32(data + offset);
        offset += 4;
        if (length > static_cast<uint32_t>(len - offset)) {
          ret = OB_INVALID_DATA;
        } else {
          value.type_ = FS_TYPE_VARCHAR;
          value.int_value_ = static_cast<int64_t>(arena.size());
          value.str_len_ = static_cast<int32_t>(length);
          value.str_ptr_ = NULL;
          arena.append(data + offset, length);
          offset += length;
        }
      }
      break;
    }
    case PQ_TYPE_FLBA: {
      if (col.type_length_ <= 0 || offset + col.type_length_ > len) {
        ret = OB_INVALID_DATA;
      } else {
        value.type_ = FS_TYPE_VARCHAR;
        value.int_value_ = static_cast<int64_t>(arena.size());
        value.str_len_ = col.type_length_;
        value.str_ptr_ = NULL;
        arena.append(data + offset, col.type_length_);
        offset += col.type_length_;
      }
      break;
    }
    default: {
      ret = OB_NOT_SUPPORTED;
      break;
    }
  }
  return ret;
}

int ObParquetFileReader::decode_values(const ParquetColumn &col, const int32_t encoding,
                                       const char *data, const int64_t len, int64_t &offset,
                                       const int64_t logical_count,
                                       const std::vector<uint32_t> &def_levels,
                                       const std::vector<ObFileSqlValue> &dictionary,
                                       const bool dictionary_ready,
                                       std::vector<ObFileSqlValue> &values,
                                       std::string &arena)
{
  int ret = OB_SUCCESS;
  int64_t physical_count = logical_count;
  if (!def_levels.empty()) {
    physical_count = 0;
    for (size_t i = 0; i < def_levels.size(); ++i) {
      if (1 == def_levels[i]) {
        ++physical_count;
      }
    }
  }
  std::vector<ObFileSqlValue> decoded;
  decoded.reserve(static_cast<size_t>(physical_count));
  if (PQ_ENC_PLAIN == encoding) {
    if (PQ_TYPE_BOOLEAN == col.physical_type_) {
      const int64_t needed = (physical_count + 7) / 8;
      if (offset + needed > len) {
        ret = OB_INVALID_DATA;
      } else {
        for (int64_t i = 0; i < physical_count; ++i) {
          const int64_t byte_index = offset + (i >> 3);
          const int32_t bit_index = static_cast<int32_t>(i & 7);
          const uint32_t bit =
              (static_cast<uint32_t>(static_cast<unsigned char>(data[byte_index])) >> bit_index) & 1u;
          ObFileSqlValue value;
          value.null_ = false;
          value.type_ = FS_TYPE_BOOLEAN;
          value.int_value_ = static_cast<int64_t>(bit);
          decoded.push_back(value);
        }
        offset += needed;
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < physical_count; ++i) {
        ObFileSqlValue value;
        if (OB_FAIL(decode_plain_value(col, data, len, offset, value, arena))) {
          ret = OB_INVALID_DATA;
        } else {
          decoded.push_back(value);
        }
      }
    }
  } else if (PQ_ENC_RLE == encoding && PQ_TYPE_BOOLEAN == col.physical_type_) {
    // Boolean RLE values include a four-byte little-endian length prefix.
    if (offset + 4 > len) {
      ret = OB_INVALID_DATA;
    } else {
      const uint32_t encoded_len = read_le32(data + offset);
      offset += 4;
      std::vector<uint32_t> bits;
      int64_t consumed = 0;
      if (encoded_len > static_cast<uint64_t>(len - offset)
          || OB_FAIL(decode_rle_hybrid(data + offset, encoded_len, physical_count,
                                      1, bits, consumed))) {
        ret = OB_INVALID_DATA;
      } else {
        offset += encoded_len;
        for (size_t i = 0; i < bits.size(); ++i) {
          ObFileSqlValue value;
          value.null_ = false;
          value.type_ = FS_TYPE_BOOLEAN;
          value.int_value_ = bits[i];
          decoded.push_back(value);
        }
      }
    }
  } else if (PQ_ENC_RLE_DICTIONARY == encoding || PQ_ENC_PLAIN_DICTIONARY == encoding) {
    if (0 == physical_count) {
      // An all-NULL dictionary page can have no index payload.
    } else if (!dictionary_ready || offset >= len) {
      ret = OB_INVALID_DATA;
    } else {
      const int32_t bit_width = static_cast<int32_t>(static_cast<unsigned char>(data[offset]));
      ++offset;
      std::vector<uint32_t> indices;
      int64_t consumed = 0;
      if (OB_FAIL(decode_rle_hybrid(data + offset, len - offset, physical_count, bit_width,
                                    indices, consumed))) {
        ret = OB_INVALID_DATA;
      } else {
        offset += consumed;
        for (size_t i = 0; OB_SUCC(ret) && i < indices.size(); ++i) {
          if (indices[i] >= dictionary.size()) {
            ret = OB_INVALID_DATA;
          } else {
            decoded.push_back(dictionary[indices[i]]);
          }
        }
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    set_error(0, 0, "unsupported parquet page encoding %d", encoding);
  }
  if (OB_SUCC(ret)) {
    const ObFileSqlType logical_type =
        parquet_logical_type_of(col.physical_type_, col.converted_type_);
    int64_t next = 0;
    if (def_levels.empty()) {
      for (size_t i = 0; i < decoded.size(); ++i) {
        values.push_back(decoded[i]);
      }
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < logical_count; ++i) {
        if (1 == def_levels[static_cast<size_t>(i)]) {
          if (next >= static_cast<int64_t>(decoded.size())) {
            ret = OB_INVALID_DATA;
          } else {
            values.push_back(decoded[static_cast<size_t>(next++)]);
          }
        } else {
          ObFileSqlValue value;
          value.null_ = true;
          value.type_ = logical_type;
          values.push_back(value);
        }
      }
    }
  }
  return ret;
}

} // namespace filesql
} // namespace sql
} // namespace oceanbase
