#define USING_LOG_PREFIX SQL_ENG
#include "ob_file_sql_utils.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/file/ob_file.h"
#include "sql/session/ob_basic_session_info.h"
#include "sql/engine/expr/ob_eval_info.h"
#include <glob.h>

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

int ObFileSQLUtils::check_file_access(ObIAllocator &allocator,
                                      const ObString &file_path,
                                      const ObString &secure_file_priv,
                                      bool enable_file_sql)
{
  int ret = OB_SUCCESS;
  UNUSED(allocator);

  if (!enable_file_sql) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "file SQL is disabled, set enable_file_sql=ON");
  } else if (file_path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path is empty");
  } else if (!is_safe_path(file_path)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unsafe file path");
  } else if (!secure_file_priv.empty()) {
    // Check if file path starts with secure_file_priv
    if (file_path.length() < secure_file_priv.length() ||
        memcmp(file_path.ptr(), secure_file_priv.ptr(), secure_file_priv.length()) != 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file path is not within secure_file_priv");
    }
  }

  return ret;
}

int ObFileSQLUtils::check_file_access_with_session(ObIAllocator &allocator,
                                                    const ObString &file_path,
                                                    const ObEvalCtx &ctx)
{
  ObString secure_file_priv;
  ObBasicSessionInfo *session = ctx.exec_ctx_.get_my_session();
  if (OB_NOT_NULL(session)) {
    session->get_secure_file_priv(secure_file_priv);
  }
  return check_file_access(allocator, file_path, secure_file_priv, true);
}

bool ObFileSQLUtils::is_safe_path(const ObString &path)
{
  // Check for path traversal attempts
  if (path.find("..") != OB_INVALID_INDEX) {
    return false;
  }
  return true;
}

ObString ObFileSQLUtils::get_file_extension(const ObString &filename)
{
  int64_t dot_pos = filename.rfind('.');
  if (dot_pos != OB_INVALID_INDEX && dot_pos < filename.length() - 1) {
    return ObString(filename.ptr() + dot_pos + 1, static_cast<int32_t>(filename.length() - dot_pos - 1));
  }
  return ObString();
}

int ObFileSQLUtils::glob_files(ObIAllocator &allocator,
                               const ObString &pattern,
                               ObIArray<ObString> &file_list)
{
  int ret = OB_SUCCESS;
  ::glob_t glob_result;
  memset(&glob_result, 0, sizeof(glob_result));

  char *pattern_str = static_cast<char *>(allocator.alloc(pattern.length() + 1));
  if (OB_ISNULL(pattern_str)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(pattern_str, pattern.ptr(), pattern.length());
    pattern_str[pattern.length()] = '\0';

    int glob_ret = ::glob(pattern_str, GLOB_NOSORT, NULL, &glob_result);
    if (glob_ret == 0) {
      for (size_t i = 0; i < glob_result.gl_pathc; i++) {
        int64_t path_len = strlen(glob_result.gl_pathv[i]);
        char *path_copy = static_cast<char *>(allocator.alloc(path_len));
        if (OB_ISNULL(path_copy)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
        MEMCPY(path_copy, glob_result.gl_pathv[i], path_len);
        ObString file_path(static_cast<int32_t>(path_len), path_copy);
        if (OB_FAIL(file_list.push_back(file_path))) {
          break;
        }
      }
    } else if (glob_ret == GLOB_NOMATCH) {
      // No files matched, return empty list
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("glob failed", K(ret), K(glob_ret));
    }

    ::globfree(&glob_result);
  }

  return ret;
}

} // namespace sql
} // namespace oceanbase
