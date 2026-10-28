#pragma once

#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/array/ob_se_array.h"

namespace oceanbase
{
namespace sql
{

class ObEvalCtx;

class ObFileSQLUtils
{
public:
  static int check_file_access(common::ObIAllocator &allocator,
                               const common::ObString &file_path,
                               const common::ObString &secure_file_priv,
                               bool enable_file_sql);

  // Session-aware version: gets secure_file_priv from session
  static int check_file_access_with_session(common::ObIAllocator &allocator,
                                            const common::ObString &file_path,
                                            const ObEvalCtx &ctx);

  static int glob_files(common::ObIAllocator &allocator,
                        const common::ObString &pattern,
                        common::ObIArray<common::ObString> &file_list);

  static bool is_safe_path(const common::ObString &path);

  static common::ObString get_file_extension(const common::ObString &filename);

private:
  DISALLOW_COPY_AND_ASSIGN(ObFileSQLUtils);
};

} // namespace sql
} // namespace oceanbase
