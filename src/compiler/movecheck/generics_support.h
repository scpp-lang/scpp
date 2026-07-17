#pragma once

#include <memory>
#include <string>
#include <unordered_set>

#include "movecheck.h"
#include "ast.h"
#include "types.h"

namespace scpp {

ExprPtr clone_expr(const Expr& expr);
StmtPtr clone_stmt(const Stmt& stmt);
[[nodiscard]] Function clone_function(const Function& fn);
[[nodiscard]] bool type_satisfies_concept(const Type& type, const ConceptDef& concept_def,
                                          const Program& program);
[[nodiscard]] std::string mangle_type_for_clone_name(const Type& type);

} // namespace scpp
