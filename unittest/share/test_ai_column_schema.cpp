#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace
{
void init_ai_column(ObColumnSchemaV2 &column, bool vector)
{
  column.set_table_id(50001);
  column.set_column_id(18);
  column.set_schema_version(42);
  column.set_data_type(vector ? ObCollectionSQLType : ObJsonType);
  ASSERT_EQ(OB_SUCCESS, column.set_column_name("result"));
  if (vector) {
    ASSERT_EQ(OB_SUCCESS, column.add_type_info(ObString::make_string("VECTOR(4)")));
  }
  column.set_is_ai_column(true);
  column.set_ai_sync_mode(OB_AI_COLUMN_SYNC_MODE_ASYNC);
  column.set_ai_max_retries(2);
  column.set_ai_func_type(vector ? OB_AI_COLUMN_FUNC_EMBED : OB_AI_COLUMN_FUNC_COMPLETE);
  column.set_ai_state_column_id(19);
  column.set_ai_request_column_id(20);
  column.set_ai_result_spec_column_id(21);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_model_name(ObString::make_string("model")));
  const char *expr = "AI_COMPLETE('model', content)";
  if (vector) {
    expr = "AI_EMBED('model', content)";
  }
  ASSERT_EQ(OB_SUCCESS, column.set_ai_generation_expr(ObString::make_string(expr)));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_normalized_expr(
      ObString::make_string("{\"v\":1,\"system\":\"Return exactly one valid JSON value\",\"deps\":[\"content\"]}")));
}

void expect_ai_metadata(const ObColumnSchemaV2 &column)
{
  EXPECT_TRUE(column.is_ai_column());
  EXPECT_EQ(OB_AI_COLUMN_SYNC_MODE_ASYNC, column.get_ai_sync_mode());
  EXPECT_EQ(2, column.get_ai_max_retries());
  EXPECT_EQ(19, column.get_ai_state_column_id());
  EXPECT_EQ(20, column.get_ai_request_column_id());
  EXPECT_EQ(21, column.get_ai_result_spec_column_id());
  EXPECT_EQ(ObString::make_string("model"), column.get_ai_model_name());
  const char *expr = "AI_COMPLETE('model', content)";
  if (column.get_ai_func_type() == OB_AI_COLUMN_FUNC_EMBED) {
    expr = "AI_EMBED('model', content)";
  }
  EXPECT_EQ(ObString::make_string(expr), column.get_ai_generation_expr());
  EXPECT_FALSE(column.get_ai_normalized_expr().empty());
}

int64_t ai_tail_size(const ObColumnSchemaV2 &column)
{
  return serialization::encoded_length(column.is_ai_column())
      + serialization::encoded_length(column.get_ai_sync_mode())
      + serialization::encoded_length(column.get_ai_max_retries())
      + serialization::encoded_length(column.get_ai_func_type())
      + serialization::encoded_length(column.get_ai_state_column_id())
      + serialization::encoded_length(column.get_ai_request_column_id())
      + serialization::encoded_length(column.get_ai_result_spec_column_id())
      + serialization::encoded_length(column.get_ai_model_name())
      + serialization::encoded_length(column.get_ai_generation_expr())
      + serialization::encoded_length(column.get_ai_normalized_expr());
}
}

TEST(AIColumnSchema, vector_blob_keeps_type_prefix_and_owns_strings)
{
  ObColumnSchemaV2 plain;
  ASSERT_EQ(OB_SUCCESS, plain.add_type_info(ObString::make_string("VECTOR(4)")));
  char plain_buf[256];
  int64_t plain_len = 0;
  ASSERT_EQ(OB_SUCCESS, plain.serialize_extended_type_info(plain_buf, sizeof(plain_buf), plain_len));

  ObColumnSchemaV2 source;
  init_ai_column(source, true);
  char buf[4096];
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(buf, sizeof(buf), len));
  ASSERT_GT(len, plain_len);
  EXPECT_EQ(0, std::memcmp(plain_buf, buf, plain_len));
  ASSERT_EQ(1, source.get_extended_type_info().count());
  EXPECT_EQ(ObString::make_string("VECTOR(4)"), source.get_extended_type_info().at(0));

  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(buf, len, pos));
  EXPECT_EQ(len, pos);
  std::memset(buf, 0, sizeof(buf));
  expect_ai_metadata(restored);
  EXPECT_EQ(OB_AI_COLUMN_FUNC_EMBED, restored.get_ai_func_type());
  ASSERT_EQ(1, restored.get_extended_type_info().count());
  EXPECT_EQ(ObString::make_string("VECTOR(4)"), restored.get_extended_type_info().at(0));

  ObColumnSchemaV2 copied;
  ASSERT_EQ(OB_SUCCESS, copied.assign(restored));
  restored.reset();
  expect_ai_metadata(copied);
}

TEST(AIColumnSchema, scalar_blob_roundtrip_and_reuse_as_plain_column)
{
  ObColumnSchemaV2 source;
  init_ai_column(source, false);
  char buf[4096];
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(buf, sizeof(buf), len));
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(buf, len, pos));
  expect_ai_metadata(restored);
  EXPECT_EQ(0, restored.get_extended_type_info().count());

  ObColumnSchemaV2 plain;
  len = 0;
  ASSERT_EQ(OB_SUCCESS, plain.serialize_extended_type_info(buf, sizeof(buf), len));
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(buf, len, pos));
  EXPECT_FALSE(restored.is_ai_column());
  EXPECT_TRUE(restored.get_ai_model_name().empty());
  EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id());
}

TEST(AIColumnSchema, plain_enum_payload_is_not_metadata)
{
  ObColumnSchemaV2 source;
  // The tag used by the previous patch is a perfectly legal enum value.
  ASSERT_EQ(OB_SUCCESS, source.add_type_info(ObString::make_string("__AI_COLUMN__")));
  for (int i = 0; i < 9; ++i) {
    ASSERT_EQ(OB_SUCCESS, source.add_type_info(ObString::make_string("1")));
  }
  char buf[4096];
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(buf, sizeof(buf), len));
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(buf, len, pos));
  EXPECT_FALSE(restored.is_ai_column());
  EXPECT_EQ(10, restored.get_extended_type_info().count());
}

TEST(AIColumnSchema, unis_roundtrip_does_not_borrow_input_buffer)
{
  ObColumnSchemaV2 source;
  init_ai_column(source, true);
  std::vector<char> buf(source.get_serialize_size());
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize(buf.data(), buf.size(), len));
  EXPECT_EQ(source.get_serialize_size(), len);
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(buf.data(), len, pos));
  EXPECT_EQ(len, pos);
  std::memset(buf.data(), 0, buf.size());
  expect_ai_metadata(restored);
  EXPECT_TRUE(source == restored);
  ASSERT_EQ(1, restored.get_extended_type_info().count());
  EXPECT_EQ(ObString::make_string("VECTOR(4)"), restored.get_extended_type_info().at(0));
}

TEST(AIColumnSchema, legacy_unis_body_clears_reused_ai_metadata)
{
  ObColumnSchemaV2 plain;
  std::vector<char> buf(plain.get_serialize_size_());
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, plain.serialize_(buf.data(), buf.size(), len));
  // Older writers end after local_session_vars_, before the appended AI fields.
  const int64_t tail_size = ai_tail_size(plain);
  ASSERT_GT(len, tail_size);
  ObColumnSchemaV2 restored;
  init_ai_column(restored, true);
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_(buf.data(), len - tail_size, pos));
  EXPECT_EQ(len - tail_size, pos);
  EXPECT_FALSE(restored.is_ai_column());
  EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id());
  EXPECT_TRUE(restored.get_ai_model_name().empty());
}

TEST(AIColumnSchema, unis_rejects_incomplete_ai_tail)
{
  ObColumnSchemaV2 source;
  init_ai_column(source, true);
  std::vector<char> buf(source.get_serialize_size_());
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_(buf.data(), buf.size(), len));
  const int64_t tail_start = len - ai_tail_size(source);
  // Retain is_ai_column=true, then truncate at every byte of the required tail.
  for (int64_t n = tail_start + serialization::encoded_length(true); n < len; ++n) {
    ObColumnSchemaV2 restored;
    int64_t pos = 0;
    EXPECT_NE(OB_SUCCESS, restored.deserialize_(buf.data(), n, pos)) << n;
    EXPECT_FALSE(restored.is_ai_column()) << n;
    EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id()) << n;
    EXPECT_TRUE(restored.get_ai_model_name().empty()) << n;
  }
}

TEST(AIColumnSchema, unis_rejects_invalid_ai_values)
{
  for (int i = 0; i < 6; ++i) {
    ObColumnSchemaV2 source;
    init_ai_column(source, false);
    switch (i) {
      case 0: source.set_ai_sync_mode(OB_AI_COLUMN_SYNC_MODE_SYNC); break;
      case 1: source.set_ai_max_retries(-1); break;
      case 2: source.set_ai_max_retries(OB_AI_COLUMN_MAX_RETRIES_LIMIT + 1); break;
      case 3: source.set_ai_func_type(OB_AI_COLUMN_FUNC_NONE); break;
      case 4: source.set_ai_state_column_id(OB_INVALID_ID); break;
      case 5: source.set_ai_request_column_id(source.get_ai_state_column_id()); break;
    }
    std::vector<char> buf(source.get_serialize_size());
    int64_t len = 0;
    ASSERT_EQ(OB_SUCCESS, source.serialize(buf.data(), buf.size(), len));
    ObColumnSchemaV2 restored;
    int64_t pos = 0;
    EXPECT_NE(OB_SUCCESS, restored.deserialize(buf.data(), len, pos)) << i;
    EXPECT_FALSE(restored.is_ai_column()) << i;
    EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id()) << i;
  }
}

TEST(AIColumnSchema, unis_transports_alter_add_before_companion_allocation)
{
  ObColumnSchemaV2 source;
  init_ai_column(source, false);
  source.set_column_id(OB_INVALID_ID);
  source.set_ai_state_column_id(OB_INVALID_ID);
  source.set_ai_request_column_id(OB_INVALID_ID);
  source.set_ai_result_spec_column_id(OB_INVALID_ID);
  std::vector<char> buf(source.get_serialize_size());
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize(buf.data(), buf.size(), len));
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(buf.data(), len, pos));
  EXPECT_EQ(len, pos);
  EXPECT_TRUE(restored.is_ai_column());
  EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id());
  EXPECT_EQ(OB_INVALID_ID, restored.get_ai_request_column_id());
  EXPECT_EQ(OB_INVALID_ID, restored.get_ai_result_spec_column_id());
  EXPECT_EQ(source.get_ai_normalized_expr(), restored.get_ai_normalized_expr());

  // A draft is valid in the DDL RPC, but never as a persisted column definition.
  len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(buf.data(), buf.size(), len));
  pos = 0;
  EXPECT_NE(OB_SUCCESS, restored.deserialize_extended_type_info(buf.data(), len, pos));
  EXPECT_FALSE(restored.is_ai_column());
}

TEST(AIColumnSchema, plain_vector_roundtrips_blob_and_unis)
{
  ObColumnSchemaV2 source;
  source.set_data_type(ObCollectionSQLType);
  ASSERT_EQ(OB_SUCCESS, source.add_type_info(ObString::make_string("VECTOR(4)")));
  char blob[256];
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(blob, sizeof(blob), len));
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(blob, len, pos));
  EXPECT_EQ(len, pos);
  EXPECT_FALSE(restored.is_ai_column());
  ASSERT_EQ(1, restored.get_extended_type_info().count());
  EXPECT_EQ(ObString::make_string("VECTOR(4)"), restored.get_extended_type_info().at(0));
  bool is_same = false;
  ASSERT_EQ(OB_SUCCESS, source.is_same_collection_column(restored, is_same));
  EXPECT_TRUE(is_same);

  std::vector<char> buf(source.get_serialize_size());
  len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize(buf.data(), buf.size(), len));
  init_ai_column(restored, false);
  pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize(buf.data(), len, pos));
  EXPECT_EQ(len, pos);
  EXPECT_FALSE(restored.is_ai_column());
  EXPECT_TRUE(restored.get_ai_model_name().empty());
  ASSERT_EQ(1, restored.get_extended_type_info().count());
  EXPECT_EQ(ObString::make_string("VECTOR(4)"), restored.get_extended_type_info().at(0));
}

TEST(AIColumnSchema, rejects_truncated_or_unknown_trailer)
{
  ObColumnSchemaV2 source;
  init_ai_column(source, false);
  char buf[4096];
  int64_t len = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(buf, sizeof(buf), len));
  for (int64_t n = 2; n < len; ++n) {
    ObColumnSchemaV2 restored;
    int64_t pos = 0;
    EXPECT_NE(OB_SUCCESS, restored.deserialize_extended_type_info(buf, n, pos)) << n;
    EXPECT_FALSE(restored.is_ai_column()) << n;
    EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id()) << n;
    EXPECT_TRUE(restored.get_ai_normalized_expr().empty()) << n;
  }
  buf[1] ^= 1;  // First byte after the empty type array is the trailer magic.
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  EXPECT_NE(OB_SUCCESS, restored.deserialize_extended_type_info(buf, len, pos));
  EXPECT_FALSE(restored.is_ai_column());
  EXPECT_EQ(OB_INVALID_ID, restored.get_ai_state_column_id());
}

TEST(AIColumnSchema, convert_size_and_companion_ids_follow_schema_copy)
{
  ObColumnSchemaV2 column;
  int64_t size = column.get_convert_size();
  ASSERT_EQ(OB_SUCCESS, column.set_ai_model_name(ObString::make_string("model")));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_generation_expr(ObString::make_string("expression")));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_normalized_expr(ObString::make_string("normalized")));
  EXPECT_EQ(size + 5 + 10 + 10, column.get_convert_size());
  column.reset();
  init_ai_column(column, true);
  oceanbase::common::hash::ObHashMap<uint64_t, uint64_t> ids;
  ASSERT_EQ(OB_SUCCESS, ids.create(8, "AIColTest"));
  for (uint64_t id = 18; id <= 21; ++id) {
    ASSERT_EQ(OB_SUCCESS, ids.set_refactored(id, id + 10));
  }
  ASSERT_EQ(OB_SUCCESS, column.convert_column_id(ids));
  EXPECT_EQ(28, column.get_column_id());
  EXPECT_EQ(29, column.get_ai_state_column_id());
  EXPECT_EQ(30, column.get_ai_request_column_id());
  EXPECT_EQ(31, column.get_ai_result_spec_column_id());
}

TEST(AIColumnSchema, dependencies_survive_blob_reload_and_column_id_reallocation)
{
  ObColumnSchemaV2 source;
  init_ai_column(source, true);
  char blob[4096];
  int64_t length = 0;
  ASSERT_EQ(OB_SUCCESS, source.serialize_extended_type_info(blob, sizeof(blob), length));
  ObColumnSchemaV2 restored;
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(blob, length, pos));
  ObTableSchema table;
  table.set_table_id(50001);
  ObColumnSchemaV2 input;
  input.set_table_id(50001);
  input.set_column_id(117);
  input.set_data_type(ObVarcharType);
  ASSERT_EQ(OB_SUCCESS, input.set_column_name("content"));
  ASSERT_EQ(OB_SUCCESS, table.add_column(input));
  ObSEArray<uint64_t, 4> ids;
  ASSERT_EQ(OB_SUCCESS, restored.get_ai_dependency_ids(table, ids));
  ASSERT_EQ(1, ids.count());
  EXPECT_EQ(117, ids.at(0));
  EXPECT_EQ(1, restored.get_extended_type_info().count());
}

TEST(AIColumnSchema, missing_and_malformed_dependencies_fail_closed)
{
  ObColumnSchemaV2 column;
  init_ai_column(column, false);
  ObTableSchema table;
  ObSEArray<uint64_t, 4> ids;
  EXPECT_EQ(OB_SCHEMA_ERROR, column.get_ai_dependency_ids(table, ids));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_normalized_expr(ObString::make_string("{\"deps\":[17]}")));
  EXPECT_EQ(OB_INVALID_DATA, column.get_ai_dependency_ids(table, ids));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_normalized_expr(ObString::make_string("{\"deps\":[]}")));
  EXPECT_EQ(OB_SUCCESS, column.get_ai_dependency_ids(table, ids));
  EXPECT_TRUE(ids.empty());
}
