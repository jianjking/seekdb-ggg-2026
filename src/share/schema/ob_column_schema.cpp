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

#define USING_LOG_PREFIX SHARE_SCHEMA
#include "ob_column_schema.h"
#include "share/schema/ob_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "common/json_type/ob_json_tree.h"

namespace oceanbase
{
using namespace std;
using namespace common;
namespace share
{
namespace schema
{


ObColumnSchemaV2::ObColumnSchemaV2()
    : ObSchema(),
    local_session_vars_(get_allocator())
{
  reset();
}

ObColumnSchemaV2::ObColumnSchemaV2(ObIAllocator *allocator)
    : ObSchema(allocator),
    local_session_vars_(allocator)
{
  reset();
}

ObColumnSchemaV2::~ObColumnSchemaV2()
{
}

int ObColumnSchemaV2::assign(const ObColumnSchemaV2 &src_schema)
{
  int ret = OB_SUCCESS;
  if (this != &src_schema) {
    ObColumnSchemaV2::reset();
    error_ret_ = src_schema.error_ret_;
    table_id_ = src_schema.table_id_;
    column_id_ = src_schema.column_id_;
    schema_version_ = src_schema.schema_version_;
    rowkey_position_ = src_schema.rowkey_position_;
    index_position_ = src_schema.index_position_;
    order_in_rowkey_ = src_schema.order_in_rowkey_;
    tbl_part_key_pos_ = src_schema.tbl_part_key_pos_;
    meta_type_ = src_schema.meta_type_;
    accuracy_ = src_schema.accuracy_;
    is_nullable_ = src_schema.is_nullable_;
    is_zero_fill_ = src_schema.is_zero_fill_;
    is_autoincrement_ = src_schema.is_autoincrement_;
    is_hidden_ = src_schema.is_hidden_;
    charset_type_ = src_schema.charset_type_;
    is_binary_collation_ = src_schema.is_binary_collation_;
    on_update_current_timestamp_ = src_schema.on_update_current_timestamp_;
    column_flags_ = src_schema.column_flags_;
    prev_column_id_ = src_schema.prev_column_id_;
    next_column_id_ = src_schema.next_column_id_;
    encoding_type_ = src_schema.encoding_type_;
    srs_id_ = src_schema.srs_id_;
    udt_set_id_ = src_schema.udt_set_id_;
    sub_type_ = src_schema.sub_type_;
    skip_index_attr_ = src_schema.skip_index_attr_;
    lob_chunk_size_ = src_schema.lob_chunk_size_;
    is_ai_column_ = src_schema.is_ai_column_;
    ai_sync_mode_ = src_schema.ai_sync_mode_;
    ai_max_retries_ = src_schema.ai_max_retries_;
    ai_func_type_ = src_schema.ai_func_type_;
    ai_state_column_id_ = src_schema.ai_state_column_id_;
    ai_request_column_id_ = src_schema.ai_request_column_id_;
    ai_result_spec_column_id_ = src_schema.ai_result_spec_column_id_;

    if (OB_FAIL(deep_copy_obj(src_schema.orig_default_value_, orig_default_value_))) {
    } else if (OB_FAIL(deep_copy_obj(src_schema.cur_default_value_, cur_default_value_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.column_name_, column_name_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.comment_, comment_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.ai_model_name_, ai_model_name_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.ai_generation_expr_, ai_generation_expr_))) {
    } else if (OB_FAIL(deep_copy_str(src_schema.ai_normalized_expr_, ai_normalized_expr_))) {
    } else if (OB_FAIL(set_extended_type_info(src_schema.extended_type_info_))) {
    } else if (src_schema.column_ref_idxs_ != NULL) {
      if (OB_FAIL(alloc_column_ref_set())) {
      } else if (OB_FAIL(column_ref_idxs_->add_members(*src_schema.column_ref_idxs_))) {
      }
    } else {/*do nothing*/}

    if (OB_SUCC(ret)) {
      if (OB_FAIL(local_session_vars_.deep_copy(src_schema.local_session_vars_))) {
      }
    }

    if (OB_FAIL(ret)) {
      error_ret_ = ret;
    }
  }
  return ret;
}

bool ObColumnSchemaV2::operator==(const ObColumnSchemaV2 &r) const
{
  // AI column metadata is covered by schema_version_, so schema_version_ equality is
  // still the primary identity check.  The extra AI predicates below only guard against
  // a stale/partial schema image that carries the same version but lost its AI payload
  // after a schema reload.
  bool ai_equal = (is_ai_column_ == r.is_ai_column_);
  if (ai_equal && is_ai_column_) {
    ai_equal = ai_sync_mode_ == r.ai_sync_mode_
        && ai_max_retries_ == r.ai_max_retries_
        && ai_func_type_ == r.ai_func_type_
        && ai_state_column_id_ == r.ai_state_column_id_
        && ai_request_column_id_ == r.ai_request_column_id_
        && ai_result_spec_column_id_ == r.ai_result_spec_column_id_
        && ai_model_name_ == r.ai_model_name_
        && ai_generation_expr_ == r.ai_generation_expr_
        && ai_normalized_expr_ == r.ai_normalized_expr_;
  }
  return (table_id_ == r.table_id_ && column_id_ == r.column_id_
      && schema_version_ == r.schema_version_ && ai_equal);
}

bool ObColumnSchemaV2::operator !=(const ObColumnSchemaV2 &r) const
{
  return !(*this == r);
}

int64_t ObColumnSchemaV2::get_convert_size() const
{
  int64_t convert_size = sizeof(*this);

  convert_size += orig_default_value_.get_deep_copy_size();
  convert_size += cur_default_value_.get_deep_copy_size();
  convert_size += column_name_.length() + 1;
  convert_size += comment_.length() + 1;
  convert_size += ai_model_name_.length() + 1;
  convert_size += ai_generation_expr_.length() + 1;
  convert_size += ai_normalized_expr_.length() + 1;
  if (column_ref_idxs_ != NULL) {
    convert_size += sizeof(ColumnReferenceSet);
  }
  convert_size += extended_type_info_.count() * static_cast<int64_t>(sizeof(ObString));
  for (int64_t i = 0; i < extended_type_info_.count(); ++i) {
    convert_size += extended_type_info_.at(i).length() + 1;
  }
  convert_size += local_session_vars_.get_deep_copy_size();
  return convert_size;
}

bool ObColumnSchemaV2::is_prefix_column() const
{
  bool bret = false;
  if (is_hidden() && is_generated_column()) {
    const char *prefix_str = "__substr";
    int64_t min_len = min(column_name_.length(), static_cast<int64_t>(strlen(prefix_str)));
    bret = (0 == strncasecmp(get_column_name(), prefix_str, min_len));
  }
  return bret;
}

bool ObColumnSchemaV2::is_func_idx_column() const
{
  bool bret = false;
  if (is_hidden() && is_generated_column()) {
    const char *func_idx_str = "SYS_NC";
    int64_t min_len = min(column_name_.length(), static_cast<int64_t>(strlen(func_idx_str)));
    bret = (0 == strncmp(get_column_name(), func_idx_str, min_len));
  }

  return bret;
}

void ObColumnSchemaV2::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  schema_version_ = 0;
  rowkey_position_ = 0;
  index_position_ = 0;
  order_in_rowkey_ = ASC;
  tbl_part_key_pos_ = 0;
  meta_type_.reset();
  accuracy_.reset();
  is_nullable_ = true;
  is_zero_fill_ = false;
  is_autoincrement_ = false;
  is_hidden_ = false;
  charset_type_ = ObCharset::get_default_charset();
  is_binary_collation_ = false;
  on_update_current_timestamp_ = false;
  orig_default_value_.reset();
  cur_default_value_.reset();
  column_name_.reset();
  comment_.reset();
  column_flags_ = NON_CASCADE_FLAG;
  column_ref_idxs_ = NULL;
  prev_column_id_ = UINT64_MAX;
  next_column_id_ = UINT64_MAX;
  encoding_type_ = INT64_MAX;
  srs_id_ = OB_DEFAULT_COLUMN_SRS_ID;
  udt_set_id_ = 0;
  sub_type_ = 0;
  reset_string_array(extended_type_info_);
  skip_index_attr_.reset();
  lob_chunk_size_ = OB_DEFAULT_LOB_CHUNK_SIZE;
  local_session_vars_.reset();
  reset_ai_column_metadata();
  ObSchema::reset();
}

void ObColumnSchemaV2::reset_ai_column_metadata()
{
  is_ai_column_ = false;
  ai_sync_mode_ = 0;
  ai_max_retries_ = 0;
  ai_func_type_ = OB_AI_COLUMN_FUNC_NONE;
  ai_state_column_id_ = OB_INVALID_ID;
  ai_request_column_id_ = OB_INVALID_ID;
  ai_result_spec_column_id_ = OB_INVALID_ID;
  ai_model_name_.reset();
  ai_generation_expr_.reset();
  ai_normalized_expr_.reset();
}

OB_DEF_SERIALIZE(ObColumnSchemaV2)
{
  int ret = OB_SUCCESS;
  bool has_column_ref = (column_ref_idxs_ != NULL);
  LST_DO_CODE(OB_UNIS_ENCODE,
              table_id_,
              column_id_,
              schema_version_,
              rowkey_position_,
              index_position_,
              order_in_rowkey_,
              tbl_part_key_pos_,
              meta_type_,
              accuracy_,
              is_nullable_,
              is_zero_fill_,
              is_autoincrement_,
              is_hidden_,
              charset_type_,
              is_binary_collation_,
              on_update_current_timestamp_,
              orig_default_value_,
              cur_default_value_,
              column_name_,
              comment_,
              column_flags_,
              has_column_ref);
  if (OB_SUCC(ret) && has_column_ref) {
    OB_UNIS_ENCODE(*column_ref_idxs_);
  }

  LST_DO_CODE(OB_UNIS_ENCODE,
              prev_column_id_,
              next_column_id_);

  if (!OB_SUCC(ret)) {
  } else if (OB_FAIL(serialize_string_array(buf, buf_len, pos, extended_type_info_))) {
  } else {
    LST_DO_CODE(OB_UNIS_ENCODE,
                srs_id_,
                udt_set_id_,
                sub_type_,
                skip_index_attr_,
                lob_chunk_size_,
                local_session_vars_,
                is_ai_column_,
                ai_sync_mode_,
                ai_max_retries_,
                ai_func_type_,
                ai_state_column_id_,
                ai_request_column_id_,
                ai_result_spec_column_id_);
    if (OB_SUCC(ret)) {
      OB_UNIS_ENCODE(ai_model_name_);
      OB_UNIS_ENCODE(ai_generation_expr_);
      OB_UNIS_ENCODE(ai_normalized_expr_);
    }
  }

  return ret;
}

OB_DEF_DESERIALIZE(ObColumnSchemaV2)
{
  int ret = OB_SUCCESS;
  ObObj orig_default_value;
  ObObj cur_default_value;
  ObString column_name;
  ObString comment;
  ObString ai_model_name;
  ObString ai_generation_expr;
  ObString ai_normalized_expr;
  bool has_column_ref = false;
  // UNIS allows fields appended by newer writers to be absent.
  // A reused schema object must not retain an earlier AI definition.
  reset_ai_column_metadata();

  LST_DO_CODE(OB_UNIS_DECODE,
              table_id_,
              column_id_,
              schema_version_,
              rowkey_position_,
              index_position_,
              order_in_rowkey_,
              tbl_part_key_pos_,
              meta_type_,
              accuracy_,
              is_nullable_,
              is_zero_fill_,
              is_autoincrement_,
              is_hidden_,
              charset_type_,
              is_binary_collation_,
              on_update_current_timestamp_,
              orig_default_value,
              cur_default_value,
              column_name,
              comment,
              column_flags_,
              has_column_ref);
  if (OB_SUCC(ret) && has_column_ref) {
    if (OB_FAIL(alloc_column_ref_set())) {
    }
    OB_UNIS_DECODE(*column_ref_idxs_);
  }
  LST_DO_CODE(OB_UNIS_DECODE,
              prev_column_id_,
              next_column_id_);

  if (!OB_SUCC(ret)) {
  } else if (OB_FAIL(deserialize_string_array(buf, data_len, pos, extended_type_info_, get_allocator()))) {
  } else if (OB_FAIL(deep_copy_obj(orig_default_value, orig_default_value_))) {
  } else if (OB_FAIL(deep_copy_obj(cur_default_value, cur_default_value_))) {
  } else if (OB_FAIL(deep_copy_str(column_name, column_name_))) {
  } else if (OB_FAIL(deep_copy_str(comment, comment_))) {
  } else {
    LST_DO_CODE(OB_UNIS_DECODE,
                srs_id_,
                udt_set_id_,
                sub_type_,
                skip_index_attr_,
                lob_chunk_size_,
                local_session_vars_,
                is_ai_column_,
                ai_sync_mode_,
                ai_max_retries_,
                ai_func_type_,
                ai_state_column_id_,
                ai_request_column_id_,
                ai_result_spec_column_id_);
    if (OB_SUCC(ret)) {
      OB_UNIS_DECODE(ai_model_name);
      OB_UNIS_DECODE(ai_generation_expr);
      OB_UNIS_DECODE(ai_normalized_expr);
      // ALTER ADD reaches rootserver before its three companion IDs are allocated.
      const bool unallocated_ai_companions = ai_state_column_id_ == OB_INVALID_ID
          && ai_request_column_id_ == OB_INVALID_ID && ai_result_spec_column_id_ == OB_INVALID_ID;
      if (OB_FAIL(ret)) {
      } else if (pos > data_len) {
        // ObString decoding also consumes its trailing zero byte.
        ret = OB_INVALID_ARGUMENT;
      } else if (is_ai_column_
                 && (ai_sync_mode_ != OB_AI_COLUMN_SYNC_MODE_ASYNC
                     || ai_max_retries_ < 0 || ai_max_retries_ > OB_AI_COLUMN_MAX_RETRIES_LIMIT
                     || (ai_func_type_ != OB_AI_COLUMN_FUNC_COMPLETE
                         && ai_func_type_ != OB_AI_COLUMN_FUNC_EMBED)
                     || (!unallocated_ai_companions
                         && (ai_state_column_id_ == OB_INVALID_ID || ai_request_column_id_ == OB_INVALID_ID
                             || ai_result_spec_column_id_ == OB_INVALID_ID
                             || ai_state_column_id_ == ai_request_column_id_
                             || ai_state_column_id_ == ai_result_spec_column_id_
                             || ai_request_column_id_ == ai_result_spec_column_id_))
                     || ai_model_name.empty() || ai_generation_expr.empty() || ai_normalized_expr.empty())) {
        // Optional UNIS fields may be absent only for a non-AI schema image.
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(set_ai_model_name(ai_model_name))) {
      } else if (OB_FAIL(set_ai_generation_expr(ai_generation_expr))) {
      } else if (OB_FAIL(set_ai_normalized_expr(ai_normalized_expr))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
    reset_ai_column_metadata();
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObColumnSchemaV2)
{
  int64_t len = 0;
  bool has_column_ref = (column_ref_idxs_ != NULL);
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              table_id_,
              column_id_,
              schema_version_,
              rowkey_position_,
              index_position_,
              order_in_rowkey_,
              tbl_part_key_pos_,
              meta_type_,
              accuracy_,
              is_nullable_,
              is_zero_fill_,
              is_autoincrement_,
              is_hidden_,
              charset_type_,
              is_binary_collation_,
              on_update_current_timestamp_,
              orig_default_value_,
              cur_default_value_,
              column_name_,
              comment_,
              column_flags_,
              has_column_ref);
  if (has_column_ref) {
    OB_UNIS_ADD_LEN(*column_ref_idxs_);
  }
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              prev_column_id_,
              next_column_id_);
  len += get_string_array_serialize_size(extended_type_info_);
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              srs_id_,
              udt_set_id_,
              sub_type_,
              skip_index_attr_,
              lob_chunk_size_,
              local_session_vars_,
              is_ai_column_,
              ai_sync_mode_,
              ai_max_retries_,
              ai_func_type_,
              ai_state_column_id_,
              ai_request_column_id_,
              ai_result_spec_column_id_);
  LST_DO_CODE(OB_UNIS_ADD_LEN,
              ai_model_name_,
              ai_generation_expr_,
              ai_normalized_expr_);
  return len;
}

int ObColumnSchemaV2::set_part_key_pos(const int64_t part_key_pos)
{
  int ret = OB_SUCCESS;
  if (part_key_pos > UINT8_MAX) {
    ret =OB_ERR_UNEXPECTED;
  } else {
    part_pos_.part_key_pos_ = static_cast<uint8_t>(part_key_pos);
    if (0 != part_pos_.part_key_pos_) {
      add_column_flag(TABLE_PART_KEY_COLUMN_FLAG);
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_subpart_key_pos(const int64_t subpart_key_pos)
{
  int ret = OB_SUCCESS;
  if (subpart_key_pos > UINT8_MAX) {
    ret =OB_ERR_UNEXPECTED;
  } else {
    part_pos_.subpart_key_pos_ = static_cast<uint8_t>(subpart_key_pos);
    if (0 != part_pos_.subpart_key_pos_) {
      add_column_flag(TABLE_PART_KEY_COLUMN_FLAG);
    }
  }
  return ret;
}



int64_t ObColumnSchemaV2::to_string(char *buf, const int64_t buf_len) const
{
  int64_t pos = 0;

  J_OBJ_START();
  J_KV(K_(table_id),
    K_(column_id),
    K_(schema_version),
    K_(rowkey_position),
    K_(index_position),
    K_(order_in_rowkey),
    K_(tbl_part_key_pos),
    K_(meta_type),
    K_(accuracy),
    K_(is_nullable),
    K_(is_zero_fill),
    K_(is_autoincrement),
    K_(is_hidden),
    K_(charset_type),
    K_(on_update_current_timestamp),
    K_(orig_default_value),
    K_(cur_default_value),
    K_(column_name),
    K_(comment),
    K_(column_flags),
    K_(extended_type_info),
    K_(prev_column_id),
    K_(next_column_id),
    K_(encoding_type),
    K_(srs_id),
    K_(udt_set_id),
    K_(sub_type),
    K_(skip_index_attr),
    K_(lob_chunk_size),
    KPC_(column_ref_idxs),
    K(local_session_vars_));
  J_OBJ_END();
  return pos;
}

int ObColumnSchemaV2::get_byte_length(
    int64_t &length,
    const bool for_check_length) const
{
  int ret = OB_SUCCESS;
  if (CS_TYPE_INVALID == get_collation_type()
      && !ob_is_extend(meta_type_.get_type())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ob_is_text_tc(meta_type_.get_type()) || ob_is_json(meta_type_.get_type())
             || ob_is_geometry(meta_type_.get_type())) {
    if (for_check_length) {
      // when check row length, a lob will occupy at most 512B
      length = min(get_data_length(), OB_MAX_LOB_HANDLE_LENGTH);
    } else {
      length = get_data_length();
    }
  } else {
    int64_t mbmaxlen = 0;
    if (OB_FAIL(ObCharset::get_mbmaxlen_by_coll(get_collation_type(), mbmaxlen))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      length = get_data_length() * mbmaxlen;
    }
  }
  return ret;
}

int ObColumnSchemaV2::alloc_column_ref_set()
{
  int ret = OB_SUCCESS;
  void *ptr = NULL;
  if (OB_ISNULL(ptr = alloc(sizeof(ColumnReferenceSet)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("allocate cascaded column set failed");
  } else {
    column_ref_idxs_ = new(ptr) ColumnReferenceSet();
  }
  return ret;
}

int ObColumnSchemaV2::add_cascaded_column_id(uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(column_ref_idxs_)) {
    if (OB_FAIL(alloc_column_ref_set())) {
    }
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(column_ref_idxs_->add_member(column_id - OB_APP_MIN_COLUMN_ID))) {
    } else {
      LOG_DEBUG("succ to add_cascaded_column_id", K(ret), K(*this), K(column_id), K(lbt()));
    }
  }
  return ret;
}

int ObColumnSchemaV2::del_cascaded_column_id(const uint64_t column_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(column_id < OB_APP_MIN_COLUMN_ID)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(column_ref_idxs_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(column_ref_idxs_->del_member(column_id - OB_APP_MIN_COLUMN_ID))) {
  } else {
    // do nothing
  }
  return ret;
}

bool ObColumnSchemaV2::has_cascaded_column_id(uint64_t column_id) const
{
  bool bret = false;
  if (column_ref_idxs_ != NULL) {
    bret = column_ref_idxs_->has_member(column_id - OB_APP_MIN_COLUMN_ID);
  }
  return bret;
}

int ObColumnSchemaV2::get_cascaded_column_ids(ObIArray<uint64_t> &column_ids) const
{
  int ret = OB_SUCCESS;
  if (column_ref_idxs_ != NULL) {
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ref_idxs_->bit_count(); ++i) {
      if (column_ref_idxs_->has_member(i)) {
        if (OB_FAIL(column_ids.push_back(i + OB_APP_MIN_COLUMN_ID))) {
        }
      }
    }
  }
  return ret;
}

int ObColumnSchemaV2::get_ai_dependency_ids(const ObTableSchema &table, ObIArray<uint64_t> &ids) const
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AIColDeps");
  ObIJsonBase *root = NULL;
  ObIJsonBase *deps = NULL;
  if (!is_ai_column_) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, ai_normalized_expr_,
                 ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, root))) {
  } else if (NULL == root || root->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else if (OB_FAIL(root->get_object_value(ObString::make_string("deps"), deps))) {
  } else if (NULL == deps || deps->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_INVALID_DATA;
  } else {
    // column_ref_idxs_ belongs to generated columns and is rebuilt separately
    // on schema load. AI dependencies are authoritative in the persisted JSON.
    for (uint64_t i = 0; OB_SUCC(ret) && i < deps->element_count(); ++i) {
      ObIJsonBase *name = NULL;
      const ObColumnSchemaV2 *source = NULL;
      if (OB_FAIL(deps->get_array_element(i, name))) {
      } else if (NULL == name || name->json_type() != ObJsonNodeType::J_STRING) {
        ret = OB_INVALID_DATA;
      } else if (NULL == (source = table.get_column_schema(ObString(name->get_data_length(), name->get_data())))
                 || source->is_hidden() || source->is_ai_column()) {
        ret = OB_SCHEMA_ERROR;
      } else if (!has_exist_in_array(ids, source->get_column_id())) {
        OZ(ids.push_back(source->get_column_id()));
      }
    }
  }
  return ret;
}

int ObColumnSchemaV2::set_extended_type_info(const ObIArray<common::ObString> &info)
{
  return deep_copy_string_array(info, extended_type_info_);
}

int ObColumnSchemaV2::add_type_info(const ObString &type_info)
{
  return add_string_to_array(type_info, extended_type_info_);
}

int ObColumnSchemaV2::convert_column_id(const hash::ObHashMap<uint64_t, uint64_t> &column_id_map)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 1> old_column_ids;
  if (OB_FAIL(get_cascaded_column_ids(old_column_ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < old_column_ids.count(); i++) {
      const uint64_t old_column_id = old_column_ids.at(i);
      if (OB_FAIL(del_cascaded_column_id(old_column_id))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < old_column_ids.count(); i++) {
      const uint64_t old_column_id = old_column_ids.at(i);
      uint64_t new_column_id = 0;
      if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
      } else if (OB_FAIL(add_cascaded_column_id(new_column_id))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const uint64_t old_column_id = get_column_id();
    uint64_t new_column_id = 0;
    if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
    } else {
      set_column_id(new_column_id);
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const uint64_t old_column_id = get_prev_column_id();
    uint64_t new_column_id = 0;
    if (old_column_id == BORDER_COLUMN_ID || old_column_id == UINT64_MAX) {
      // do nothing
    } else if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
    } else {
      set_prev_column_id(new_column_id);
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const uint64_t old_column_id = get_next_column_id();
    uint64_t new_column_id = 0;
    if (old_column_id == BORDER_COLUMN_ID || old_column_id == UINT64_MAX) {
      // do nothing
    } else if (OB_FAIL(column_id_map.get_refactored(old_column_id, new_column_id))) {
    } else {
      set_next_column_id(new_column_id);
    }
  }
  if (OB_SUCC(ret) && is_ai_column_) {
    uint64_t old_ids[3] = {ai_state_column_id_, ai_request_column_id_, ai_result_spec_column_id_};
    uint64_t new_ids[3] = {OB_INVALID_ID, OB_INVALID_ID, OB_INVALID_ID};
    for (int64_t i = 0; OB_SUCC(ret) && i < 3; ++i) {
      if (OB_FAIL(column_id_map.get_refactored(old_ids[i], new_ids[i]))) {
      }
    }
    if (OB_SUCC(ret)) {
      ai_state_column_id_ = new_ids[0];
      ai_request_column_id_ = new_ids[1];
      ai_result_spec_column_id_ = new_ids[2];
    }
  }
  return ret;
}

namespace {
// The type string array stays unchanged (VECTOR(N) must remain its only element).
// Only the __all_column blob has this optional, versioned binary trailer.
const int64_t AI_COLUMN_METADATA_MAGIC = 0x4149434F4C;
const int64_t AI_COLUMN_METADATA_VERSION = 1;
}

int ObColumnSchemaV2::serialize_extended_type_info(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(serialize_string_array(buf, buf_len, pos, extended_type_info_))) {
  } else if (is_ai_column_) {
    LST_DO_CODE(OB_UNIS_ENCODE,
                AI_COLUMN_METADATA_MAGIC, AI_COLUMN_METADATA_VERSION,
                ai_sync_mode_, ai_max_retries_, ai_func_type_,
                ai_state_column_id_, ai_request_column_id_, ai_result_spec_column_id_,
                ai_model_name_, ai_generation_expr_, ai_normalized_expr_);
  }
  return ret;
}

int ObColumnSchemaV2::deserialize_extended_type_info(const char *buf,
                                                      const int64_t data_len,
                                                      int64_t &pos)
{
  int ret = OB_SUCCESS;
  reset_ai_column_metadata();
  if (OB_FAIL(deserialize_string_array(buf, data_len, pos, extended_type_info_, get_allocator()))) {
  } else if (pos < data_len) {
    int64_t magic = 0;
    int64_t version = 0;
    ObString model_name;
    ObString generation_expr;
    ObString normalized_expr;
    LST_DO_CODE(OB_UNIS_DECODE, magic, version,
                ai_sync_mode_, ai_max_retries_, ai_func_type_,
                ai_state_column_id_, ai_request_column_id_, ai_result_spec_column_id_,
                model_name, generation_expr, normalized_expr);
    if (OB_FAIL(ret)) {
    } else if (magic != AI_COLUMN_METADATA_MAGIC || version != AI_COLUMN_METADATA_VERSION
               || pos != data_len
               || ai_sync_mode_ != OB_AI_COLUMN_SYNC_MODE_ASYNC
               || ai_max_retries_ < 0 || ai_max_retries_ > OB_AI_COLUMN_MAX_RETRIES_LIMIT
               || (ai_func_type_ != OB_AI_COLUMN_FUNC_COMPLETE && ai_func_type_ != OB_AI_COLUMN_FUNC_EMBED)
               || ai_state_column_id_ == OB_INVALID_ID || ai_request_column_id_ == OB_INVALID_ID
               || ai_result_spec_column_id_ == OB_INVALID_ID
               || ai_state_column_id_ == ai_request_column_id_
               || ai_state_column_id_ == ai_result_spec_column_id_
               || ai_request_column_id_ == ai_result_spec_column_id_
               || model_name.empty() || generation_expr.empty() || normalized_expr.empty()) {
      ret = OB_INVALID_ARGUMENT;
      SHARE_SCHEMA_LOG(WARN, "invalid AI column metadata trailer", K(ret), K(magic), K(version));
    } else if (OB_FAIL(set_ai_model_name(model_name))) {
    } else if (OB_FAIL(set_ai_generation_expr(generation_expr))) {
    } else if (OB_FAIL(set_ai_normalized_expr(normalized_expr))) {
    } else {
      is_ai_column_ = true;
    }
  }
  if (OB_FAIL(ret)) {
    reset_ai_column_metadata();
  }
  return ret;
}

int ObColumnSchemaV2::set_geo_type(const int32_t type_val)
{
  int ret = OB_SUCCESS;
  switch (type_val) {
    case static_cast<int32_t>(common::ObGeoType::GEOMETRY): {
      set_geo_type(common::ObGeoType::GEOMETRY);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::POINT): {
      set_geo_type(common::ObGeoType::POINT);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::LINESTRING): {
      set_geo_type(common::ObGeoType::LINESTRING);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::POLYGON): {
      set_geo_type(common::ObGeoType::POLYGON);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::MULTIPOINT): {
      set_geo_type(common::ObGeoType::MULTIPOINT);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::MULTILINESTRING): {
      set_geo_type(common::ObGeoType::MULTILINESTRING);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::MULTIPOLYGON): {
      set_geo_type(common::ObGeoType::MULTIPOLYGON);
      break;
    }

    case static_cast<int32_t>(common::ObGeoType::GEOMETRYCOLLECTION): {
      set_geo_type(common::ObGeoType::GEOMETRYCOLLECTION);
      break;
    }

    default: {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
  }

  return ret;
}
int ObColumnSchemaV2::is_same_collection_column(const ObColumnSchemaV2 &other, bool &is_same) const
{
  int ret = OB_SUCCESS;
  if (get_extended_type_info().count() == other.get_extended_type_info().count()) {
    if (get_extended_type_info().count() != 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObString src_sub_name = get_extended_type_info().at(0);
      ObString dst_sub_name = other.get_extended_type_info().at(0);
      is_same = (src_sub_name.case_compare(dst_sub_name) == 0);
    }
  }
  return ret;
}

} //end of namespace schema
} //end of namespace share
} //end of namespace oceanbase
