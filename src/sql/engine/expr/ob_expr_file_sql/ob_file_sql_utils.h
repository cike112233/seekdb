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

#pragma once

#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/array/ob_se_array.h"

namespace oceanbase
{
namespace sql
{

class ObFileSQLUtils
{
public:
  static int check_file_access(common::ObIAllocator &allocator,
                               const common::ObString &file_path,
                               const common::ObString &secure_file_priv,
                               bool enable_file_sql);

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
