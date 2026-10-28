#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_read_json.h"
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

ObExprReadJSON::ObExprReadJSON(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_READ_JSON,
                         N_READ_JSON,
                         MORE_THAN_ZERO,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprReadJSON::~ObExprReadJSON()
{
}

int ObExprReadJSON::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types_stack,
                                      int64_t params_len,
                                      ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;

  if (params_len < 1 || params_len > 2) {
    ret = OB_ERR_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_ERR_INVALID_ARGUMENT, "read_json requires 1 or 2 arguments");
  } else if (!ob_is_string_tc(types_stack[0].get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "read_json", "varchar");
  } else {
    types_stack[0].set_calc_type(ObVarcharType);
    types_stack[0].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (params_len == 2) {
      types_stack[1].set_calc_type(ObVarcharType);
      types_stack[1].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
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

// Escape a string for JSON output
static int json_escape_str(ObStringBuffer &buf, const char *str, int64_t len)
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

int ObExprReadJSON::eval_read_json(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_datums[2] = {nullptr, nullptr};

  if (OB_FAIL(expr.eval_param_value(ctx, arg_datums[0], arg_datums[1]))) {
    LOG_WARN("failed to eval params", K(ret));
  } else if (OB_ISNULL(arg_datums[0]) || arg_datums[0]->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

    ObString file_path = arg_datums[0]->get_string();

    if (file_path.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "read_json, file path is empty");
      res.set_null();
    } else if (OB_FAIL(ObFileSQLUtils::check_file_access(temp_allocator, file_path, ObString(""), true))) {
      LOG_WARN("file access check failed", K(ret));
      res.set_null();
    } else {
      // Read entire file
      ObFileReader *file_reader = nullptr;
      ObFileReadParam read_param;
      read_param.filename_ = file_path;
      read_param.file_location_ = ObLoadFileLocation::SERVER_DISK;

      if (OB_FAIL(ObFileReader::open(read_param, temp_allocator, file_reader))) {
        LOG_WARN("failed to open file", K(ret));
        res.set_null();
      } else {
        int64_t file_size = 0;
        file_reader->get_file_size(file_size);
        if (file_size <= 0) {
          file_size = 16 * 1024 * 1024;
        }

        char *file_buf = static_cast<char *>(temp_allocator.alloc(file_size + 1));
        if (OB_ISNULL(file_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          int64_t total_read = 0;
          int64_t read_size = 0;

          while (!file_reader->eof() && OB_SUCC(ret)) {
            int64_t to_read = file_size - total_read;
            if (to_read <= 0) break;
            if (OB_FAIL(file_reader->read(file_buf + total_read, to_read, read_size))) {
              LOG_WARN("failed to read file", K(ret));
            } else {
              total_read += read_size;
              if (read_size == 0) break;
            }
          }
          file_buf[total_read] = '\0';

          if (OB_SUCC(ret) && total_read > 0) {
            // Determine format: JSON array, JSON object, or NDJSON
            ObStringBuffer result_buf(&temp_allocator);

            // Trim leading whitespace
            const char *content = file_buf;
            const char *content_end = file_buf + total_read;
            while (content < content_end && (*content == ' ' || *content == '\t' || *content == '\n' || *content == '\r')) {
              content++;
            }

            if (content < content_end && (*content == '[' || *content == '{')) {
              // Standard JSON - pass through directly
              ObString json_str(total_read, file_buf);
              if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, json_str))) {
                LOG_WARN("failed to set result", K(ret));
              }
            } else {
              // NDJSON - each line is a JSON object
              result_buf.append("[");
              bool first = true;
              const char *line_start = content;

              while (line_start < content_end && OB_SUCC(ret)) {
                const char *line_end = line_start;
                while (line_end < content_end && *line_end != '\n' && *line_end != '\r') {
                  line_end++;
                }

                // Trim trailing whitespace
                const char *trim_end = line_end;
                while (trim_end > line_start && (*(trim_end-1) == ' ' || *(trim_end-1) == '\t')) {
                  trim_end--;
                }

                int64_t line_len = trim_end - line_start;
                if (line_len > 0) {
                  if (!first) {
                    result_buf.append(",");
                  }
                  result_buf.append(line_start, line_len);
                  first = false;
                }

                // Skip line terminator
                if (line_end < content_end && *line_end == '\r') line_end++;
                if (line_end < content_end && *line_end == '\n') line_end++;
                line_start = line_end;
              }

              result_buf.append("]");

              if (OB_SUCC(ret)) {
                ObString result = result_buf.string();
                if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
                  LOG_WARN("failed to set result", K(ret));
                }
              }
            }
          } else if (OB_SUCC(ret)) {
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

int ObExprReadJSON::cg_expr(ObExprCGCtx &expr_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprReadJSON::eval_read_json;
  return ret;
}

} // namespace sql
} // namespace oceanbase
