#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_glob.h"
#include "ob_file_sql_utils.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/string/ob_string_buffer.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

ObExprGlob::ObExprGlob(ObIAllocator &alloc)
    : ObFuncExprOperator(alloc,
                         T_FUN_SYS_GLOB,
                         N_GLOB,
                         1,
                         NOT_VALID_FOR_GENERATED_COL,
                         NOT_ROW_DIMENSION)
{
}

ObExprGlob::~ObExprGlob()
{
}

int ObExprGlob::calc_result_type1(ObExprResType &type,
                                  ObExprResType &pattern,
                                  ObExprTypeCtx &type_ctx) const
{
  UNUSED(type_ctx);
  int ret = OB_SUCCESS;

  if (!ob_is_string_tc(pattern.get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
    LOG_USER_ERROR(OB_ERR_INVALID_TYPE_FOR_OP, "glob", "varchar");
  } else {
    pattern.set_calc_type(ObVarcharType);
    pattern.set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_type(ObLongTextType);
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_IMPLICIT);
    type.set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObLongTextType]);
  }

  return ret;
}

int ObExprGlob::eval_glob(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  INIT_SUCC(ret);
  ObDatum *arg_pattern = nullptr;

  if (OB_FAIL(expr.eval_param_value(ctx, arg_pattern))) {
    LOG_WARN("failed to eval params", K(ret));
  } else if (OB_ISNULL(arg_pattern) || arg_pattern->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

    ObString pattern = arg_pattern->get_string();

    if (pattern.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "glob, pattern is empty");
      res.set_null();
    } else if (OB_FAIL(ObFileSQLUtils::check_file_access(temp_allocator, pattern, ObString(""), true))) {
      LOG_WARN("file access check failed", K(ret));
      res.set_null();
    } else {
      ObSEArray<ObString, 64> file_list;
      if (OB_FAIL(ObFileSQLUtils::glob_files(temp_allocator, pattern, file_list))) {
        LOG_WARN("glob failed", K(ret), K(pattern));
        res.set_null();
      } else {
        // Build JSON array of file paths
        ObStringBuffer json_buf(&temp_allocator);
        json_buf.append("[");

        for (int64_t i = 0; i < static_cast<int64_t>(file_list.count()) && OB_SUCC(ret); i++) {
          if (i > 0) {
            json_buf.append(",");
          }
          json_buf.append("\"");
          // Escape path for JSON
          const char *path = file_list[i].ptr();
          int64_t path_len = file_list[i].length();
          for (int64_t j = 0; j < path_len && OB_SUCC(ret); j++) {
            if (path[j] == '\\' || path[j] == '"') {
              json_buf.append("\\");
            }
            json_buf.append(&path[j], 1);
          }
          json_buf.append("\"");
        }

        json_buf.append("]");

        if (OB_SUCC(ret)) {
          ObString result = json_buf.string();
          if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
            LOG_WARN("failed to set result", K(ret));
          }
        }
      }
    }
  }

  return ret;
}

int ObExprGlob::cg_expr(ObExprCGCtx &expr_cg_ctx,
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprGlob::eval_glob;
  return ret;
}

} // namespace sql
} // namespace oceanbase
