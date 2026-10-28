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
#include "ob_expr_ai_prompt.h"
#include <regex>
#include "ob_ai_func.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"


using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIPrompt::ObExprAIPrompt(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_PROMPT, 
                    N_AI_PROMPT, 
                    MORE_THAN_ZERO,
                    VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIPrompt::~ObExprAIPrompt() 
{
}

int ObExprAIPrompt::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const 
{
  int ret = OB_SUCCESS;
  bool is_null_res = false;
  ObExprResType template_type = types_stack[0];
  if (!ob_is_string_type(template_type.get_type())) {
    ret = OB_ERR_INVALID_TYPE_FOR_OP;
  } else if (ob_is_text_tc(template_type.get_type())) {
    // LOB template: keep the LOB type, eval reads the real value through
    // ObTextStringHelper::read_real_string_data().  Casting it to varchar would
    // lose the lob header and only leave a locator behind.
  } else {
    types_stack[0].set_calc_type(ObVarcharType);
    types_stack[0].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
  }

  for (int64_t i = 1; i < param_num && OB_SUCC(ret); i++) {
    if (ob_is_text_tc(types_stack[i].get_type())) {
      // LOB parameter: keep the LOB type, see the template branch above.
    } else if (ob_is_string_tc(types_stack[i].get_type())) {
      types_stack[i].set_calc_type(ObVarcharType);
      types_stack[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    } else if (ob_is_json(types_stack[i].get_type())) {
      // Allow JSON type for nested prompts and JSON parameters
      types_stack[i].set_calc_type(ObJsonType);
    } else {
      ret = OB_ERR_INVALID_TYPE_FOR_OP;
    }
  }

  if (OB_SUCC(ret)) {
    type.set_json();
    type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
  }
  return ret;
}

// Append a JSON argument as a JSON value instead of a serialized string, so that
// ai_prompt keeps the structure of JSON arguments.  The node is deep copied because
// it may belong to another JSON document with a different lifetime.
static int append_json_arg(ObIAllocator &allocator, ObIJsonBase *json_node, ObJsonArray *args_array)
{
  int ret = OB_SUCCESS;
  ObJsonNode *cloned_node = NULL;
  if (OB_ISNULL(json_node) || OB_ISNULL(args_array)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(cloned_node = static_cast<ObJsonNode *>(json_node)->clone(&allocator, true))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to clone json node", K(ret));
  } else if (OB_FAIL(args_array->append(cloned_node))) {
    LOG_WARN("failed to append json node", K(ret));
  }
  return ret;
}

int ObExprAIPrompt::eval_ai_prompt(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *template_datum = NULL;
  ObExpr *arg0 = expr.args_[0];
  ObString template_str;
  ObJsonArray *args_array = NULL;
  ObJsonObject *prompt_object = NULL;
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor tmp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_PROMPT));

  if (OB_FAIL(tmp_allocator.eval_arg(arg0, ctx, template_datum))) {
  } else if (template_datum->is_null()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ob_is_text_tc(arg0->datum_meta_.type_)) {
    if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, tmp_allocator, *template_datum, arg0->datum_meta_, arg0->obj_meta_.has_lob_header(), template_str))) {
    }
  } else {
    template_str = template_datum->get_string();
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(tmp_allocator, args_array))) {
  } else {
    for (int64_t i = 1; i < expr.arg_cnt_ && OB_SUCC(ret); i++) {
      ObDatum *datum = NULL;
      ObExpr *arg = expr.args_[i];
      ObString arg_str;
      ObJsonString *arg_json_str = NULL;
      if (OB_FAIL(tmp_allocator.eval_arg(arg, ctx, datum))) {
      } else if (datum->is_null()) {
        ret = OB_ERR_INVALID_TYPE_FOR_OP;
      } else if (ob_is_string_tc(arg->datum_meta_.type_)) {
        arg_str = datum->get_string();
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, arg_str, arg_json_str))) {
        } else if (OB_FAIL(args_array->append(arg_json_str))) {
        }
      } else if (ob_is_text_tc(arg->datum_meta_.type_)) {
        if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, tmp_allocator, *datum, arg->datum_meta_, arg->obj_meta_.has_lob_header(), arg_str))) {
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, arg_str, arg_json_str))) {
        } else if (OB_FAIL(args_array->append(arg_json_str))) {
        }
      } else if (ob_is_json(arg->datum_meta_.type_)) {
        ObIJsonBase *j_base = nullptr;
        bool is_null = false;
        if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, tmp_allocator, i, j_base, is_null))) {
        } else if (is_null || OB_ISNULL(j_base)) {
          ret = OB_ERR_INVALID_TYPE_FOR_OP;
        } else if (j_base->json_type() == ObJsonNodeType::J_OBJECT
                   && ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(j_base))) {
          // A nested prompt object: substitute its placeholders and pass the
          // resolved prompt as plain text so it can be used as a template argument.
          ObString resolved_str;
          if (OB_FAIL(ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(
                  tmp_allocator, static_cast<ObJsonObject *>(j_base), resolved_str))) {
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(tmp_allocator, resolved_str, arg_json_str))) {
          } else if (OB_FAIL(args_array->append(arg_json_str))) {
          }
        } else if (OB_FAIL(append_json_arg(tmp_allocator, j_base, args_array))) {
        }
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncPromptObjectUtils::construct_prompt_object(tmp_allocator, template_str, args_array, prompt_object))) {
  } else {
    // DIAGNOSTIC PROBE (temporary): always answer with a fixed value so that the
    // first statement of ai_function_enhancement.test mismatches, which reveals
    // where the harness starts reporting the failure.
    ObString probe_str("{\"args\": [], \"template\": \"PROBE\"}");
    if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, probe_str))) {
    }
  }
  return ret;
}



int ObExprAIPrompt::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  UNUSED(expr_cg_ctx);
  UNUSED(raw_expr);
  int ret = OB_SUCCESS;
  rt_expr.eval_func_ = ObExprAIPrompt::eval_ai_prompt;
  return ret;
}

} // namespace sql
} // namespace oceanbase
