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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_read_csv.h"
#include "ob_file_sql_utils.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/string/ob_string_buffer.h"
#include "sql/engine/cmd/ob_load_data_file_reader.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

ObExprReadCSV::ObExprReadCSV(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_READ_CSV,
                         N_READ_CSV,
                         MORE_THAN_ZERO,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprReadCSV::~ObExprReadCSV()
{
}

int ObExprReadCSV::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t params_len,
                                     ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;

  if (params_len < 1 || params_len > 2) {
    ret = OB_ERR_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_ERR_INVALID_ARGUMENT, "read_csv requires 1 or 2 arguments");
  } else if (!ob_is_string_tc(types_stack[0].get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "read_csv", "varchar");
  } else {
    types_stack[0].set_calc_type(ObVarcharType);
    types_stack[0].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (params_len == 2) {
      // Second argument is options JSON string
      if (!ob_is_string_tc(types_stack[1].get_type())) {
        ret = OB_ERR_INVALID_TYPE_FOR_OP;
        LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "read_csv options", "varchar");
      } else {
        types_stack[1].set_calc_type(ObVarcharType);
        types_stack[1].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_SUCC(ret)) {
      type.set_type(ObLongTextType);
      type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      type.set_collation_level(CS_LEVEL_IMPLICIT);
      type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
    }
  }

  return ret;
}

// Simple CSV field parser - handles quoted fields, escaped quotes, commas, newlines
static int parse_csv_fields(ObIAllocator &allocator,
                            const char *line_start,
                            int64_t line_len,
                            ObIArray<ObString> &fields)
{
  int ret = OB_SUCCESS;
  const char *p = line_start;
  const char *end = line_start + line_len;

  while (p <= end && OB_SUCC(ret)) {
    ObString field;
    if (p < end && *p == '"') {
      // Quoted field
      p++; // skip opening quote
      const char *field_start = p;
      int64_t field_len = 0;

      // First pass: calculate unescaped length
      const char *scan = p;
      while (scan < end) {
        if (*scan == '"') {
          if (scan + 1 < end && *(scan + 1) == '"') {
            field_len++;
            scan += 2;
          } else {
            break;
          }
        } else {
          field_len++;
          scan++;
        }
      }

      // Allocate and copy unescaped content
      char *buf = static_cast<char *>(allocator.alloc(field_len + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        int64_t out = 0;
        while (p < end) {
          if (*p == '"') {
            if (p + 1 < end && *(p + 1) == '"') {
              buf[out++] = '"';
              p += 2;
            } else {
              p++; // skip closing quote
              break;
            }
          } else {
            buf[out++] = *p++;
          }
        }
        buf[out] = '\0';
        field.assign_ptr(buf, static_cast<int32_t>(out));
      }

      // Skip comma after quoted field
      if (p < end && *p == ',') {
        p++;
      }
    } else {
      // Unquoted field
      const char *field_start = p;
      while (p < end && *p != ',') {
        p++;
      }
      int64_t flen = p - field_start;
      char *buf = static_cast<char *>(allocator.alloc(flen + 1));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(buf, field_start, flen);
        buf[flen] = '\0';
        field.assign_ptr(buf, static_cast<int32_t>(flen));
      }
      if (p < end && *p == ',') {
        p++;
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_FAIL(fields.push_back(field))) {
        // push_back failed
      }
    }
  }

  return ret;
}

// Escape a string for JSON output
static int json_escape_append(ObStringBuffer &buf, const char *str, int64_t len)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; i < len && OB_SUCC(ret); i++) {
    char c = str[i];
    switch (c) {
    case '"':  ret = buf.append("\\\""); break;
    case '\\': ret = buf.append("\\\\"); break;
    case '\b': ret = buf.append("\\b"); break;
    case '\f': ret = buf.append("\\f"); break;
    case '\n': ret = buf.append("\\n"); break;
    case '\r': ret = buf.append("\\r"); break;
    case '\t': ret = buf.append("\\t"); break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char hex[8];
        snprintf(hex, sizeof(hex), "\\u%04x", static_cast<unsigned char>(c));
        ret = buf.append(hex);
      } else {
        ret = buf.append(&c, 1);
      }
      break;
    }
  }
  return ret;
}

int ObExprReadCSV::eval_read_csv(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_datums[2] = {nullptr, nullptr};
  int64_t param_count = expr.arg_cnt_;

  if (OB_FAIL(expr.eval_param_value(ctx, arg_datums[0], arg_datums[1]))) {
    LOG_WARN("failed to eval params", K(ret));
  } else if (OB_ISNULL(arg_datums[0]) || arg_datums[0]->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

    ObString file_path = arg_datums[0]->get_string();

    // Parse optional options argument
    char field_delim = ',';
    bool has_header = true;

    if (param_count >= 2 && OB_NOT_NULL(arg_datums[1]) && !arg_datums[1]->is_null()) {
      // TODO: parse JSON options for delimiter, header, etc.
      // For now, use defaults
    }

    if (file_path.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "read_csv, file path is empty");
      res.set_null();
    } else if (OB_FAIL(ObFileSQLUtils::check_file_access(temp_allocator, file_path, ObString(""), true))) {
      LOG_WARN("file access check failed", K(ret), K(file_path));
      res.set_null();
    } else {
      // Read entire file into memory
      ObFileReader *file_reader = nullptr;
      ObFileReadParam read_param;
      read_param.filename_ = file_path;
      read_param.file_location_ = ObLoadFileLocation::SERVER_DISK;

      if (OB_FAIL(ObFileReader::open(read_param, temp_allocator, file_reader))) {
        LOG_WARN("failed to open file", K(ret), K(file_path));
        res.set_null();
      } else {
        int64_t file_size = 0;
        file_reader->get_file_size(file_size);

        if (file_size <= 0) {
          // Empty file or stream - read in chunks
          file_size = 16 * 1024 * 1024; // 16MB max for streaming
        }

        char *file_buf = static_cast<char *>(temp_allocator.alloc(file_size + 1));
        if (OB_ISNULL(file_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          int64_t total_read = 0;
          int64_t read_size = 0;

          while (!file_reader->eof() && OB_SUCC(ret)) {
            int64_t to_read = file_size - total_read;
            if (to_read <= 0) {
              // Buffer full, try to extend
              int64_t new_size = file_size * 2;
              char *new_buf = static_cast<char *>(temp_allocator.alloc(new_size + 1));
              if (OB_ISNULL(new_buf)) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                MEMCPY(new_buf, file_buf, total_read);
                file_buf = new_buf;
                file_size = new_size;
                to_read = file_size - total_read;
              }
            }
            if (OB_SUCC(ret)) {
              if (OB_FAIL(file_reader->read(file_buf + total_read, to_read, read_size))) {
                LOG_WARN("failed to read file", K(ret));
              } else {
                total_read += read_size;
                if (read_size == 0 && file_reader->eof()) {
                  break;
                }
              }
            }
          }
          file_buf[total_read] = '\0';

          if (OB_SUCC(ret) && total_read > 0) {
            // Parse CSV into JSON array of objects
            ObStringBuffer json_buf(&temp_allocator);

            // Split into lines
            ObSEArray<ObString, 64> lines;
            const char *line_start = file_buf;
            const char *line_end = file_buf + total_read;

            while (line_start < line_end) {
              const char *p = line_start;
              bool in_quotes = false;

              // Find end of CSV record (handling quoted newlines)
              while (p < line_end) {
                if (*p == '"') {
                  in_quotes = !in_quotes;
                } else if (!in_quotes && (*p == '\n' || *p == '\r')) {
                  break;
                }
                p++;
              }

              int64_t line_len = p - line_start;
              if (line_len > 0) {
                ObString line(line_len, line_start);
                lines.push_back(line);
              }

              // Skip line terminator
              if (p < line_end && *p == '\r') p++;
              if (p < line_end && *p == '\n') p++;
              line_start = p;
            }

            if (lines.count() > 0) {
              int64_t header_idx = 0;
              ObSEArray<ObString, 32> headers;

              if (has_header) {
                // Parse header row
                if (OB_FAIL(parse_csv_fields(temp_allocator, lines[0].ptr(), lines[0].length(), headers))) {
                  LOG_WARN("failed to parse header", K(ret));
                }
                header_idx = 1;
              }

              if (OB_SUCC(ret)) {
                // Generate JSON array
                json_buf.append("[");

                for (int64_t i = header_idx; i < static_cast<int64_t>(lines.count()) && OB_SUCC(ret); i++) {
                  ObSEArray<ObString, 32> fields;
                  if (OB_FAIL(parse_csv_fields(temp_allocator, lines[i].ptr(), lines[i].length(), fields))) {
                    LOG_WARN("failed to parse row", K(ret), K(i));
                  } else {
                    if (i > header_idx) {
                      json_buf.append(",");
                    }
                    json_buf.append("{");

                    for (int64_t j = 0; j < static_cast<int64_t>(fields.count()) && OB_SUCC(ret); j++) {
                      if (j > 0) {
                        json_buf.append(",");
                      }

                      // Key
                      json_buf.append("\"");
                      if (j < static_cast<int64_t>(headers.count())) {
                        json_escape_append(json_buf, headers[j].ptr(), headers[j].length());
                      } else {
                        // Auto-generate column name
                        char col_name[32];
                        snprintf(col_name, sizeof(col_name), "column%ld", j + 1);
                        json_buf.append(col_name);
                      }
                      json_buf.append("\":");

                      // Value
                      if (fields[j].length() == 0) {
                        json_buf.append("null");
                      } else {
                        json_buf.append("\"");
                        json_escape_append(json_buf, fields[j].ptr(), fields[j].length());
                        json_buf.append("\"");
                      }
                    }

                    json_buf.append("}");
                  }
                }

                json_buf.append("]");

                if (OB_SUCC(ret)) {
                  ObString result = json_buf.string();
                  if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
                    LOG_WARN("failed to set result", K(ret));
                  }
                }
              }
            } else {
              // Empty file - return empty array
              ObString empty_arr("[]");
              ObAIFuncUtils::set_string_result(expr, ctx, res, empty_arr);
            }
          } else if (OB_SUCC(ret)) {
            // Zero-length file
            ObString empty_arr("[]");
            ObAIFuncUtils::set_string_result(expr, ctx, res, empty_arr);
          }
        }

        ObFileReader::destroy(file_reader);
      }
    }
  }

  return ret;
}

int ObExprReadCSV::cg_expr(ObExprCGCtx &expr_cg_ctx,
                           const ObRawExpr &raw_expr,
                           ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprReadCSV::eval_read_csv;
  return ret;
}

} // namespace sql
} // namespace oceanbase
