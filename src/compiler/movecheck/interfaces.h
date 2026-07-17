#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "movecheck.h"
#include "ast.h"
#include "signatures.h"
#include "types.h"
#include "threadsafety.h"

namespace scpp {

[[maybe_unused]] [[nodiscard]] const StructDef* find_struct_def(const Program& program,
                                                                const std::string& struct_name);
[[nodiscard]] bool type_forms_interface_object(const Type& type, const Program& program);
void validate_class_semantics(const Program& program, const Signatures& signatures);

} // namespace scpp
