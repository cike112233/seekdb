#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_write_csv.h"
#include "ob_file_sql_utils.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/string/ob_string_buffer.h"
#include "lib/file/ob_file.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

ObExprWriteCSV::ObExprWriteCSV(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_WRITE_CSV,
                         N_WRITE_CSV,
                         2,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprWriteCSV::~ObExprWriteCSV()
{
}

int ObExprWriteCSV::calc_result_type2(ObExprResType &type,
                                      ObExprResType &json_data,
                                      ObExprResType &file_path,
                                      ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;

  if (!ob_is_string_tc(json_data.get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "write_csv", "varchar");
  } else if (!ob_is_string_tc(file_path.get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "write_csv path", "varchar");
  } else {
    json_data.set_calc_type(ObVarcharType);
    json_data.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    file_path.set_calc_type(ObVarcharType);
    file_path.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_type(ObLongTextType);
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_IMPLICIT);
    type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  }

  return ret;
}

// Simple JSON-aware CSV writer
// Expects input as JSON array of objects: [{"col1":"val1",...},...]
static int write_json_to_csv(ObIAllocator &allocator,
                             const ObString &json_data,
                             const ObString &file_path)
{
  int ret = OB_SUCCESS;
  const char *p = json_data.ptr();
  const char *end = json_data.ptr() + json_data.length();

  // Skip whitespace
  while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;

  if (p >= end || *p != '[') {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "write_csv, expected JSON array input");
    return ret;
  }
  p++; // skip '['

  // Open output file
  ObFileAppender file_appender;
  if (OB_FAIL(file_appender.open(file_path, false /*dio*/, true /*is_create*/, true /*is_trunc*/))) {
    LOG_WARN("failed to open output file", K(ret), K(file_path));
    return ret;
  }

  ObStringBuffer line_buf(&allocator);
  bool header_written = false;
  bool first_row = true;

  // Parse JSON array elements
  while (p < end && OB_SUCC(ret)) {
    // Skip whitespace and commas
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
    if (p >= end || *p == ']') break;

    if (*p != '{') {
      // Skip non-object elements
      while (p < end && *p != ',' && *p != ']') p++;
      continue;
    }
    p++; // skip '{'

    line_buf.reset();
    ObStringBuffer header_buf(&allocator);
    bool first_field = true;

    // Parse object fields
    while (p < end && OB_SUCC(ret)) {
      while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',')) p++;
      if (p >= end || *p == '}') { p++; break; }

      // Parse key
      if (*p != '"') { while (p < end && *p != ',' && *p != '}') p++; continue; }
      p++; // skip opening quote
      const char *key_start = p;
      while (p < end && *p != '"') {
        if (*p == '\\') p++; // skip escaped char
        p++;
      }
      int64_t key_len = p - key_start;
      if (p < end) p++; // skip closing quote

      // Skip colon
      while (p < end && *p != ':') p++;
      if (p < end) p++;

      // Parse value
      while (p < end && (*p == ' ' || *p == '\t')) p++;

      ObString value;
      bool is_null = false;
      if (p < end && *p == '"') {
        // String value
        p++; // skip opening quote
        const char *val_start = p;
        ObStringBuffer escaped_val(&allocator);
        while (p < end && *p != '"') {
          if (*p == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case '"': escaped_val.append("\""); break;
            case '\\': escaped_val.append("\\"); break;
            case 'n': escaped_val.append("\n"); break;
            case 'r': escaped_val.append("\r"); break;
            case 't': escaped_val.append("\t"); break;
            default: escaped_val.append(p, 1); break;
            }
          } else {
            escaped_val.append(p, 1);
          }
          p++;
        }
        if (p < end) p++; // skip closing quote
        value = escaped_val.string();
      } else if (p < end && *p == 'n' && p + 3 < end && memcmp(p, "null", 4) == 0) {
        is_null = true;
        p += 4;
      } else {
        // Number, boolean, or other
        const char *val_start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ' ' && *p != '\n') p++;
        value.assign_ptr(val_start, static_cast<int32_t>(p - val_start));
      }

      // Write header (first row only)
      if (!header_written) {
        if (!first_field) header_buf.append(",");
        header_buf.append(key_start, key_len);
      }

      // Write value
      if (!first_field) line_buf.append(",");
      if (is_null) {
        // empty field for NULL
      } else {
        // Check if value needs quoting
        bool need_quote = false;
        for (int64_t i = 0; i < value.length(); i++) {
          if (value.ptr()[i] == ',' || value.ptr()[i] == '"' || value.ptr()[i] == '\n') {
            need_quote = true;
            break;
          }
        }
        if (need_quote) {
          line_buf.append("\"");
          for (int64_t i = 0; i < value.length(); i++) {
            if (value.ptr()[i] == '"') {
              line_buf.append("\"\"");
            } else {
              line_buf.append(&value.ptr()[i], 1);
            }
          }
          line_buf.append("\"");
        } else {
          line_buf.append(value.ptr(), value.length());
        }
      }

      first_field = false;
    }

    // Write header line if first row
    if (!header_written && header_buf.length() > 0) {
      ObString header_line = header_buf.string();
      if (OB_FAIL(file_appender.append(header_line.ptr(), header_line.length(), false))) {
        LOG_WARN("failed to write header", K(ret));
      } else {
        const char *newline = "\n";
        file_appender.append(newline, 1, false);
      }
      header_written = true;
    }

    // Write data line
    if (OB_SUCC(ret) && line_buf.length() > 0) {
      ObString data_line = line_buf.string();
      if (OB_FAIL(file_appender.append(data_line.ptr(), data_line.length(), false))) {
        LOG_WARN("failed to write data line", K(ret));
      } else {
        const char *newline = "\n";
        file_appender.append(newline, 1, false);
      }
    }

    first_row = false;
  }

  file_appender.fsync();
  file_appender.close();
  return ret;
}

int ObExprWriteCSV::eval_write_csv(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_datums[2] = {nullptr, nullptr};

  if (OB_FAIL(expr.eval_param_value(ctx, arg_datums[0], arg_datums[1]))) {
    LOG_WARN("failed to eval params", K(ret));
  } else if (OB_ISNULL(arg_datums[0]) || arg_datums[0]->is_null() ||
             OB_ISNULL(arg_datums[1]) || arg_datums[1]->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

    ObString json_data = arg_datums[0]->get_string();
    ObString file_path = arg_datums[1]->get_string();

    if (OB_FAIL(ObFileSQLUtils::check_file_access_with_session(temp_allocator, file_path, ctx))) {
      LOG_WARN("file access check failed", K(ret));
      res.set_null();
    } else if (OB_FAIL(write_json_to_csv(temp_allocator, json_data, file_path))) {
      LOG_WARN("failed to write CSV", K(ret), K(file_path));
      res.set_null();
    } else {
      // Return success message
      ObStringBuffer result_buf(&temp_allocator);
      result_buf.append("{\"status\":\"ok\",\"file\":\"");
      result_buf.append(file_path.ptr(), file_path.length());
      result_buf.append("\"}");

      ObString result = result_buf.string();
      if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
        LOG_WARN("failed to set result", K(ret));
      }
    }
  }

  return ret;
}

int ObExprWriteCSV::cg_expr(ObExprCGCtx &expr_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprWriteCSV::eval_write_csv;
  return ret;
}

} // namespace sql
} // namespace oceanbase
