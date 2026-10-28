#pragma once

#include "sql/engine/expr/ob_expr_operator.h"

namespace oceanbase
{
namespace sql
{

class ObExprGlob : public ObFuncExprOperator
{
public:
  explicit ObExprGlob(common::ObIAllocator &alloc);
  virtual ~ObExprGlob();

  virtual int calc_result_type1(ObExprResType &type,
                                ObExprResType &pattern,
                                common::ObExprTypeCtx &type_ctx) const;

  virtual int cg_expr(ObExprCGCtx &expr_cg_ctx,
                      const ObRawExpr &raw_expr,
                      ObExpr &rt_expr) const;

  static int eval_glob(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res);

private:
  DISALLOW_COPY_AND_ASSIGN(ObExprGlob);
};

} // namespace sql
} // namespace oceanbase
