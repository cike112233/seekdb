#pragma once

#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{

// write_csv(json_data, file_path) - writes JSON array data to a CSV file
class ObExprWriteCSV : public ObFuncExprOperator
{
public:
  explicit ObExprWriteCSV(common::ObIAllocator &alloc);
  virtual ~ObExprWriteCSV();

  virtual int calc_result_type2(ObExprResType &type,
                                ObExprResType &json_data,
                                ObExprResType &file_path,
                                common::ObExprTypeCtx &type_ctx) const;

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const;

  static int eval_write_csv(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);

private:
  DISALLOW_COPY_AND_ASSIGN(ObExprWriteCSV);
};

} // namespace sql
} // namespace oceanbase
