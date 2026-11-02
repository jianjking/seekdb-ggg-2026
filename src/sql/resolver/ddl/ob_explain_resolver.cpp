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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/parser/ob_parser.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/basic/file_sql/ob_file_sql_common.h"
#include "sql/engine/basic/file_sql/ob_file_sql_reader.h"

#include <string>

using namespace oceanbase;
using namespace sql;
using namespace common;

namespace oceanbase
{
namespace sql
{

namespace
{
const char *describe_file_type_name(const filesql::ObFileSqlType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case filesql::FS_TYPE_NULL: name = "NULL"; break;
    case filesql::FS_TYPE_BIGINT: name = "BIGINT"; break;
    case filesql::FS_TYPE_DOUBLE: name = "DOUBLE"; break;
    case filesql::FS_TYPE_BOOLEAN: name = "BOOLEAN"; break;
    case filesql::FS_TYPE_DATE: name = "DATE"; break;
    case filesql::FS_TYPE_DATETIME: name = "DATETIME"; break;
    default: name = "VARCHAR"; break;
  }
  return name;
}

// Hex literals preserve filenames/column names under either backslash SQL mode.
void describe_quote_literal(std::string &sql, const std::string &text)
{
  static const char hex[] = "0123456789abcdef";
  sql.append("convert(X'");
  for (size_t i = 0; i < text.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    sql.push_back(hex[c >> 4]);
    sql.push_back(hex[c & 15]);
  }
  sql.append("' using utf8mb4)");
}

bool is_describe_query(const ParseNode &parse_tree)
{
  const ParseNode *format = parse_tree.children_[0];
  return NULL != format && T_INT == format->type_ && 1 == format->value_;
}
} // namespace

} // namespace sql
} // namespace oceanbase
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;
int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (is_describe_query(parse_tree)) {
    ret = resolve_describe_query(parse_tree);
  } else if (NULL == (explain_stmt = create_stmt<ObExplainStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create explain stmt");
  } else {
    ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
    ObStmt *child_stmt = NULL;
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(child_node)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("empty parse tree for stmt");
    } else if (!(T_SELECT == child_node->type_
                 || T_DELETE == child_node->type_
                 || T_INSERT == child_node->type_
                 || T_UPDATE == child_node->type_
                 || T_REPLACE == child_node->type_)) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
    } else {
      ObResolver resolver(params_);
      if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                   *child_node,
                                   child_stmt))) {
      } else if (OB_ISNULL(child_stmt)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to generate stmt for child stmt");
      } else {
        explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
        ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
        if (NULL != format) {
          switch (format->type_) {
          case T_OUTLINE:
          {
            explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
            opt.with_tree_line_ = true;
          } break;
          case T_EXTENDED:
          {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
            opt.with_tree_line_ = true;
          } break;
          case T_PARTITIONS:
          {
            explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
            opt.with_tree_line_ = true;
          } break;
          case T_TRADITIONAL:
          {
            explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
            opt.with_tree_line_ = true;
          } break;
          case T_FORMAT_JSON:
          {
            explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
            opt.with_tree_line_ = false;
          } break;
          case T_BASIC: {
            explain_stmt->set_explain_format(EXPLAIN_BASIC);
            opt.with_tree_line_ = true;
            break;
          }
          case T_EXTENDED_NOADDR: {
            explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
            opt.with_tree_line_ = true;
            break;
          }
          case T_PLANREGRESS: {
            explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
            opt.with_tree_line_ = true;
            break;
          }
          default:
            ret = OB_ERROR;
            LOG_WARN("unknown explain format", "format", format->type_);
            break;
          }
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

int ObExplainResolver::resolve_describe_query(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ParseNode *child_node = parse_tree.children_[EXPLAIN_CHILD_STMT];
  ObStmt *child_stmt = NULL;
  ObSelectStmt *select_stmt = NULL;
  TableItem *file_item = NULL;
  filesql::ObFileSqlSchema schema;
  if (OB_ISNULL(child_node)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("empty child stmt for DESCRIBE", K(ret));
  } else {
    // Bind the inner statement first: it validates the file table function,
    // checks enable_file_sql / secure_file_priv and gives access to the bound
    // table item.
    ObResolver resolver(params_);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT, *child_node, child_stmt))) {
      LOG_WARN("failed to resolve inner statement of DESCRIBE", K(ret));
    } else if (OB_ISNULL(child_stmt) || !child_stmt->is_select_stmt()) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE of a non-select statement");
    } else {
      select_stmt = static_cast<ObSelectStmt *>(child_stmt);
      for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_table_size(); ++i) {
        TableItem *item = select_stmt->get_table_item(i);
        if (OB_NOT_NULL(item) && item->is_file_scan_) {
          file_item = item;
          break;
        }
      }
      if (OB_SUCC(ret) && OB_ISNULL(file_item)) {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "DESCRIBE <query> is only supported for file tables");
      }
    }
  }

  // Rebuild the inferred schema from the options bound to the file table item.
  if (OB_SUCC(ret)) {
    if (filesql::FS_FORMAT_GLOB == file_item->file_format_) {
      schema.columns_.push_back(filesql::ObFileSqlColumn("file", filesql::FS_TYPE_VARCHAR, false));
    } else {
      filesql::ObFileSqlOptions options;
      options.format_ = static_cast<filesql::ObFileFormat>(file_item->file_format_);
      options.delimiter_ = static_cast<char>(file_item->file_delimiter_);
      options.quote_ = static_cast<char>(file_item->file_quote_);
      options.escape_ = static_cast<char>(file_item->file_escape_);
      options.has_header_ = static_cast<int32_t>(file_item->file_has_header_);
      options.sample_size_ = file_item->file_sample_size_;
      options.skip_rows_ = file_item->file_skip_rows_;
      options.maximum_object_size_ = file_item->file_max_object_size_;
      options.null_str_.assign(OB_ISNULL(file_item->file_null_str_.ptr())
                                   ? "" : file_item->file_null_str_.ptr(),
                               static_cast<size_t>(file_item->file_null_str_.length()));
      const std::string path(OB_ISNULL(file_item->file_path_.ptr()) ? "" : file_item->file_path_.ptr(),
                             static_cast<size_t>(file_item->file_path_.length()));
      ObString priv;
      std::string secure_dir;
      bool unrestricted = false;
      if (OB_FAIL(session_info_->get_secure_file_priv(priv))) {
        LOG_WARN("failed to read secure_file_priv", K(ret));
      } else if (OB_FAIL(filesql::resolve_secure_dir(
                   std::string(OB_ISNULL(priv.ptr()) ? "" : priv.ptr(),
                               static_cast<size_t>(priv.length())),
                   secure_dir, unrestricted))) {
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "secure_file_priv is not configured");
      } else {
        filesql::ObFileSqlReader *reader = NULL;
        if (OB_FAIL(filesql::create_file_sql_reader(options.format_, reader))) {
          LOG_WARN("failed to create file reader", K(ret), K(options.format_));
        } else {
          const std::string reader_secure_dir = unrestricted ? std::string() : secure_dir;
          int tmp_ret = reader->open(path, options, reader_secure_dir);
          if (OB_SUCCESS != tmp_ret) {
            ret = tmp_ret;
            LOG_WARN("failed to open file for DESCRIBE", K(tmp_ret), "path", path.c_str());
          } else if (OB_FAIL(reader->infer_schema(schema))) {
            LOG_WARN("failed to infer file schema for DESCRIBE", K(ret), "path", path.c_str(),
                     "detail", reader->error_message().c_str(),
                     "line", reader->error_line(), "column", reader->error_column());
          }
          filesql::destroy_file_sql_reader(reader);
          reader = NULL;
        }
      }
    }
  }

  // DESCRIBE describes the SELECT output, including projection and aliases.
  if (OB_SUCC(ret)) {
    filesql::ObFileSqlSchema output;
    for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
      const SelectItem &item = select_stmt->get_select_item(i);
      filesql::ObFileSqlColumn column;
      if (OB_ISNULL(item.expr_)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        bool inferred = false;
        if (item.expr_->is_column_ref_expr()) {
          const ObColumnRefRawExpr *ref = static_cast<const ObColumnRefRawExpr *>(item.expr_);
          const int64_t index = static_cast<int64_t>(ref->get_column_id() - OB_APP_MIN_COLUMN_ID);
          if (ref->get_table_id() == file_item->table_id_
              && index >= 0 && index < static_cast<int64_t>(schema.columns_.size())) {
            column = schema.columns_[index];
            inferred = true;
          }
        }
        if (!inferred) {
          column.type_ = filesql::FS_TYPE_VARCHAR;
          const ObObjType type = item.expr_->get_data_type();
          if (ObNullType == type) {
            column.type_ = filesql::FS_TYPE_NULL;
          } else if (ob_is_int_tc(type) || ob_is_uint_tc(type)) {
            column.type_ = filesql::FS_TYPE_BIGINT;
          } else if (ObDoubleType == type || ObFloatType == type || ObNumberType == type) {
            column.type_ = filesql::FS_TYPE_DOUBLE;
          } else if (ObDateType == type) {
            column.type_ = filesql::FS_TYPE_DATE;
          } else if (ObDateTimeType == type || ObTimestampType == type) {
            column.type_ = filesql::FS_TYPE_DATETIME;
          }
        }
        ObString name = item.alias_name_;
        if (name.empty()) {
          name = item.expr_->get_expr_name();
        }
        if (!name.empty()) {
          column.name_.assign(name.ptr(), name.length());
        }
        output.columns_.push_back(column);
      }
    }
    if (OB_SUCC(ret)) {
      schema.columns_.swap(output.columns_);
    }
  }

  // The DESCRIBE result is a plain six column result set, so build the
  // equivalent SELECT and let the normal resolver produce the statement.
  if (OB_SUCC(ret)) {
    if (schema.columns_.empty()) {
      ret = OB_INVALID_DATA;
      LOG_WARN("file has no columns for DESCRIBE", K(ret));
    } else {
      std::string sql("select ");
      for (int64_t i = 0; i < static_cast<int64_t>(schema.columns_.size()); ++i) {
        if (i > 0) {
          sql.append(" union all select ");
        }
        const filesql::ObFileSqlColumn &column = schema.columns_[i];
        describe_quote_literal(sql, column.name_);
        sql.append(" as column_name,");
        describe_quote_literal(sql, describe_file_type_name(column.type_));
        sql.append(" as column_type,");
        describe_quote_literal(sql, column.nullable_ ? "YES" : "NO");
        sql.append(" as `null`,'NULL' as `key`,'NULL' as `default`,'NULL' as `extra` from dual");
      }
      ObParser parser(*allocator_, session_info_->get_sql_mode());
      ParseResult parse_result;
      parse_result.malloc_pool_ = allocator_;
      const ObString sql_text(static_cast<int32_t>(sql.size()), sql.c_str());
      if (OB_FAIL(parser.parse(sql_text, parse_result))) {
        LOG_WARN("failed to parse generated DESCRIBE query", K(ret));
      } else if (OB_ISNULL(parse_result.result_tree_)
                 || OB_ISNULL(parse_result.result_tree_->children_)
                 || OB_ISNULL(parse_result.result_tree_->children_[0])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("empty parse tree for generated DESCRIBE query", K(ret));
      } else {
        ObResolver desc_resolver(params_);
        ObStmt *desc_stmt = NULL;
        if (OB_FAIL(desc_resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                          *parse_result.result_tree_->children_[0],
                                          desc_stmt))) {
          LOG_WARN("failed to resolve generated DESCRIBE query", K(ret));
        } else if (OB_ISNULL(desc_stmt)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          stmt_ = desc_stmt;
        }
      }
    }
  }
  return ret;
}
