module;

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

export module scpp.constexpr_engine;

import scpp.ast;

export namespace scpp {

struct ConstexprLimits {
    int max_steps = 100000;
    int max_recursion_depth = 512;
    int max_loop_iterations = 100000;
};

struct ConstexprError : std::runtime_error {
    SourceLocation loc;

    ConstexprError(const SourceLocation& loc, const std::string& message)
        : std::runtime_error(std::to_string(loc.line) + ":" + std::to_string(loc.column) + ": " + message),
          loc(loc) {}
};

void fold_immediate_calls(Program& program, ConstexprLimits limits = {});

} // namespace scpp

namespace scpp {
namespace {

struct Cell;

struct PointerValue {
    std::shared_ptr<Cell> storage;
    std::string storage_id;
    long long index = 0;
};

struct ObjectValue {
    std::string type_name;
    std::unordered_map<std::string, std::shared_ptr<Cell>> fields;
};

struct ArrayValue {
    Type element_type;
    std::vector<std::shared_ptr<Cell>> elements;
};

using CellData = std::variant<std::monostate, long long, double, bool, PointerValue, ObjectValue, ArrayValue>;

struct Cell {
    Type type;
    CellData data;
};

struct Binding {
    std::shared_ptr<Cell> cell;
    bool read_only = false;
};

struct LValue {
    std::shared_ptr<Cell> cell;
    bool read_only = false;
};

struct ReturnSignal {
    std::shared_ptr<Cell> value;
};

struct BreakSignal {};
struct ContinueSignal {};

[[nodiscard]] bool types_equal(const Type& a, const Type& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case TypeKind::Named:
            if (a.name != b.name || a.template_args.size() != b.template_args.size() ||
                a.non_type_args.size() != b.non_type_args.size()) {
                return false;
            }
            for (size_t i = 0; i < a.template_args.size(); ++i) {
                if (!types_equal(a.template_args[i], b.template_args[i])) return false;
            }
            return true;
        case TypeKind::Pointer:
            return a.is_mutable_pointee == b.is_mutable_pointee && a.pointee && b.pointee &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Reference:
            return a.is_mutable_ref == b.is_mutable_ref && a.is_rvalue_ref == b.is_rvalue_ref && a.pointee && b.pointee &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Array:
            return a.array_size == b.array_size && a.element && b.element && types_equal(*a.element, *b.element);
        case TypeKind::Span:
            return a.is_mutable_ref == b.is_mutable_ref && a.pointee && b.pointee &&
                   types_equal(*a.pointee, *b.pointee);
        case TypeKind::Function:
        case TypeKind::FunctionPointer:
            if (a.function_params.size() != b.function_params.size() || !a.function_return || !b.function_return ||
                !types_equal(*a.function_return, *b.function_return)) {
                return false;
            }
            for (size_t i = 0; i < a.function_params.size(); ++i) {
                if (!types_equal(a.function_params[i], b.function_params[i])) return false;
            }
            return a.is_const_function == b.is_const_function &&
                   a.function_ref_qualifier == b.function_ref_qualifier &&
                   a.is_unsafe_function_pointer == b.is_unsafe_function_pointer;
    }
    return false;
}

[[nodiscard]] Type make_const_char_pointer_type() {
    Type result;
    result.kind = TypeKind::Pointer;
    result.pointee = std::make_shared<Type>(named_type("char"));
    result.is_mutable_pointee = false;
    return result;
}

[[nodiscard]] bool is_named_type(const Type& type, std::string_view name) {
    return type.kind == TypeKind::Named && type.name == name && type.template_args.empty();
}

[[nodiscard]] bool is_integer_like(const Type& type) {
    return is_named_type(type, "int") || is_named_type(type, "char") || is_named_type(type, "bool");
}

[[nodiscard]] bool is_floating_like(const Type& type) { return is_named_type(type, "double"); }

class ConstexprEngine {
public:
    ConstexprEngine(const Program& program, ConstexprLimits limits)
        : program_(program), limits_(limits) {
        for (const Function& fn : program_.functions) functions_by_name_[fn.name].push_back(&fn);
        for (const ClassDef& def : program_.classes) classes_by_name_.emplace(def.name, &def);
        for (const StructDef& def : program_.structs) structs_by_name_.emplace(def.name, &def);
    }

    std::shared_ptr<Cell> evaluate_root_expr(const Expr& expr) {
        frames_.clear();
        steps_ = 0;
        call_depth_ = 0;
        string_storage_counter_ = 0;
        return evaluate_expr(expr);
    }

private:
    const Program& program_;
    ConstexprLimits limits_;
    int steps_ = 0;
    int call_depth_ = 0;
    int string_storage_counter_ = 0;
    std::vector<std::unordered_map<std::string, Binding>> frames_;
    std::unordered_map<std::string, std::vector<const Function*>> functions_by_name_;
    std::unordered_map<std::string, const ClassDef*> classes_by_name_;
    std::unordered_map<std::string, const StructDef*> structs_by_name_;

    void tick(const SourceLocation& loc, std::string_view what) {
        ++steps_;
        if (steps_ > limits_.max_steps) {
            throw ConstexprError(loc, "constexpr evaluation exceeded step budget while " + std::string(what));
        }
    }

    [[nodiscard]] std::shared_ptr<Cell> clone_cell(const std::shared_ptr<Cell>& cell) {
        auto copy = std::make_shared<Cell>();
        copy->type = cell->type;
        std::visit(
            [&](const auto& data) {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<T, std::monostate> || std::is_same_v<T, long long> ||
                              std::is_same_v<T, double> || std::is_same_v<T, bool> ||
                              std::is_same_v<T, PointerValue>) {
                    copy->data = data;
                } else if constexpr (std::is_same_v<T, ObjectValue>) {
                    ObjectValue object_copy;
                    object_copy.type_name = data.type_name;
                    for (const auto& [name, field] : data.fields) object_copy.fields.emplace(name, clone_cell(field));
                    copy->data = std::move(object_copy);
                } else if constexpr (std::is_same_v<T, ArrayValue>) {
                    ArrayValue array_copy;
                    array_copy.element_type = data.element_type;
                    for (const auto& element : data.elements) array_copy.elements.push_back(clone_cell(element));
                    copy->data = std::move(array_copy);
                }
            },
            cell->data);
        return copy;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_scalar_cell(Type type, long long value) {
        auto cell = std::make_shared<Cell>();
        cell->type = std::move(type);
        cell->data = value;
        return cell;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_double_cell(double value) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("double");
        cell->data = value;
        return cell;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_bool_cell(bool value) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("bool");
        cell->data = value;
        return cell;
    }

    [[nodiscard]] std::vector<ClassField> collect_class_fields(const ClassDef& def) {
        std::vector<ClassField> fields;
        if (!def.base_class_name.empty()) {
            auto base_it = classes_by_name_.find(def.base_class_name);
            if (base_it == classes_by_name_.end()) {
                throw ConstexprError({}, "missing constexpr class definition for base class '" + def.base_class_name + "'");
            }
            std::vector<ClassField> base_fields = collect_class_fields(*base_it->second);
            fields.insert(fields.end(), base_fields.begin(), base_fields.end());
        }
        fields.insert(fields.end(), def.fields.begin(), def.fields.end());
        return fields;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_default_cell(const Type& type, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = type;
        switch (type.kind) {
            case TypeKind::Named:
                if (type.name == "int" || type.name == "char") {
                    cell->data = 0LL;
                    return cell;
                }
                if (type.name == "bool") {
                    cell->data = false;
                    return cell;
                }
                if (type.name == "double") {
                    cell->data = 0.0;
                    return cell;
                }
                if (type.name == "void") {
                    cell->data = std::monostate{};
                    return cell;
                }
                if (auto struct_it = structs_by_name_.find(type.name); struct_it != structs_by_name_.end()) {
                    ObjectValue object;
                    object.type_name = type.name;
                    for (const StructField& field : struct_it->second->fields) {
                        object.fields.emplace(field.name, make_default_cell(field.type, loc));
                    }
                    cell->data = std::move(object);
                    return cell;
                }
                if (auto class_it = classes_by_name_.find(type.name); class_it != classes_by_name_.end()) {
                    ObjectValue object;
                    object.type_name = type.name;
                    for (const ClassField& field : collect_class_fields(*class_it->second)) {
                        object.fields.emplace(field.name, make_default_cell(field.type, loc));
                    }
                    cell->data = std::move(object);
                    return cell;
                }
                throw ConstexprError(loc, "type '" + type.name + "' is not constexpr-compatible in Phase D1");
            case TypeKind::Pointer:
                cell->data = PointerValue{};
                return cell;
            case TypeKind::Array: {
                if (!type.element) throw ConstexprError(loc, "malformed array type in constexpr evaluator");
                ArrayValue array;
                array.element_type = *type.element;
                for (long long i = 0; i < type.array_size; ++i) array.elements.push_back(make_default_cell(*type.element, loc));
                cell->data = std::move(array);
                return cell;
            }
            case TypeKind::Reference:
            case TypeKind::Span:
            case TypeKind::Function:
            case TypeKind::FunctionPointer:
                throw ConstexprError(loc, "type is not yet supported by the constexpr evaluator in Phase D1");
        }
        throw ConstexprError(loc, "unsupported constexpr type");
    }

    [[nodiscard]] Binding lookup_binding(const std::string& name, const SourceLocation& loc) {
        for (auto it = frames_.rbegin(); it != frames_.rend(); ++it) {
            auto binding_it = it->find(name);
            if (binding_it != it->end()) return binding_it->second;
        }
        throw ConstexprError(loc, "expression is not a constant expression: identifier '" + name + "' is not available");
    }

    [[nodiscard]] long long as_integer(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (!is_integer_like(cell->type)) {
            throw ConstexprError(loc, "expected an integer-like constexpr value");
        }
        if (is_named_type(cell->type, "bool")) return std::get<bool>(cell->data) ? 1LL : 0LL;
        return std::get<long long>(cell->data);
    }

    [[nodiscard]] double as_double(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_floating_like(cell->type)) return std::get<double>(cell->data);
        if (is_integer_like(cell->type)) return static_cast<double>(as_integer(cell, loc));
        throw ConstexprError(loc, "expected a numeric constexpr value");
    }

    [[nodiscard]] bool as_bool(const std::shared_ptr<Cell>& cell, const SourceLocation& loc) {
        if (is_named_type(cell->type, "bool")) return std::get<bool>(cell->data);
        if (is_integer_like(cell->type)) return as_integer(cell, loc) != 0;
        throw ConstexprError(loc, "expected a boolean constexpr value");
    }

    void checked_assign_integer(const std::shared_ptr<Cell>& target, long long value, const SourceLocation& loc) {
        if (is_named_type(target->type, "char")) {
            if (value < 0 || value > 255) throw ConstexprError(loc, "constexpr char conversion overflow");
            target->data = value;
            return;
        }
        if (is_named_type(target->type, "bool")) {
            target->data = (value != 0);
            return;
        }
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            throw ConstexprError(loc, "constexpr integer overflow");
        }
        target->data = value;
    }

    [[nodiscard]] std::shared_ptr<Cell> make_checked_int_cell(long long value, const SourceLocation& loc) {
        auto cell = std::make_shared<Cell>();
        cell->type = named_type("int");
        checked_assign_integer(cell, value, loc);
        return cell;
    }

    void copy_into(const std::shared_ptr<Cell>& target, const std::shared_ptr<Cell>& source, const SourceLocation& loc) {
        if (!types_equal(target->type, source->type)) {
            throw ConstexprError(loc, "constexpr assignment requires exactly matching types");
        }
        std::shared_ptr<Cell> cloned = clone_cell(source);
        target->data = std::move(cloned->data);
    }

    [[nodiscard]] LValue resolve_lvalue(const Expr& expr) {
        tick(expr.loc, "resolving an lvalue");
        switch (expr.kind) {
            case ExprKind::Identifier: {
                Binding binding = lookup_binding(expr.name, expr.loc);
                return LValue{binding.cell, binding.read_only};
            }
            case ExprKind::Member: {
                LValue base = resolve_lvalue(*expr.lhs);
                auto* object = std::get_if<ObjectValue>(&base.cell->data);
                if (!object) throw ConstexprError(expr.loc, "member access requires a constexpr object value");
                auto it = object->fields.find(expr.name);
                if (it == object->fields.end()) {
                    throw ConstexprError(expr.loc, "unknown constexpr field '" + expr.name + "'");
                }
                return LValue{it->second, base.read_only};
            }
            case ExprKind::Subscript: {
                std::shared_ptr<Cell> base = evaluate_expr(*expr.lhs);
                long long index = as_integer(evaluate_expr(*expr.rhs), expr.loc);
                if (auto* array = std::get_if<ArrayValue>(&base->data)) {
                    if (index < 0 || static_cast<size_t>(index) >= array->elements.size()) {
                        throw ConstexprError(expr.loc, "constexpr subscript out of bounds");
                    }
                    return LValue{array->elements[static_cast<size_t>(index)], false};
                }
                if (auto* pointer = std::get_if<PointerValue>(&base->data)) {
                    auto* array = std::get_if<ArrayValue>(&pointer->storage->data);
                    if (!array) throw ConstexprError(expr.loc, "constexpr pointer does not point to indexable storage");
                    long long offset = pointer->index + index;
                    if (offset < 0 || static_cast<size_t>(offset) >= array->elements.size()) {
                        throw ConstexprError(expr.loc, "constexpr subscript out of bounds");
                    }
                    bool read_only = base->type.kind == TypeKind::Pointer && !base->type.is_mutable_pointee;
                    return LValue{array->elements[static_cast<size_t>(offset)], read_only};
                }
                throw ConstexprError(expr.loc, "constexpr subscript requires an array or pointer");
            }
            case ExprKind::Unary:
                if (expr.unary_op == UnaryOp::Deref) {
                    std::shared_ptr<Cell> pointer_cell = evaluate_expr(*expr.lhs);
                    auto* pointer = std::get_if<PointerValue>(&pointer_cell->data);
                    if (!pointer) throw ConstexprError(expr.loc, "constexpr dereference requires a pointer");
                    auto* array = std::get_if<ArrayValue>(&pointer->storage->data);
                    if (!array) throw ConstexprError(expr.loc, "constexpr pointer does not point to supported storage");
                    if (pointer->index < 0 || static_cast<size_t>(pointer->index) >= array->elements.size()) {
                        throw ConstexprError(expr.loc, "constexpr dereference out of bounds");
                    }
                    bool read_only = pointer_cell->type.kind == TypeKind::Pointer && !pointer_cell->type.is_mutable_pointee;
                    return LValue{array->elements[static_cast<size_t>(pointer->index)], read_only};
                }
                break;
            default:
                break;
        }
        throw ConstexprError(expr.loc, "expression is not an assignable constexpr lvalue");
    }

    [[nodiscard]] std::shared_ptr<Cell> make_string_literal_pointer(const Expr& expr) {
        Type array_type;
        array_type.kind = TypeKind::Array;
        array_type.element = std::make_shared<Type>(named_type("char"));
        array_type.array_size = static_cast<long long>(expr.name.size()) + 1;
        auto storage = std::make_shared<Cell>();
        storage->type = array_type;
        ArrayValue array;
        array.element_type = named_type("char");
        for (unsigned char ch : expr.name) array.elements.push_back(make_scalar_cell(named_type("char"), static_cast<long long>(ch)));
        array.elements.push_back(make_scalar_cell(named_type("char"), 0));
        storage->data = std::move(array);

        auto result = std::make_shared<Cell>();
        result->type = make_const_char_pointer_type();
        PointerValue pointer;
        pointer.storage = storage;
        pointer.storage_id = "string#" + std::to_string(++string_storage_counter_);
        result->data = std::move(pointer);
        return result;
    }

    [[nodiscard]] const Function* find_callable(std::string_view name, const std::vector<std::shared_ptr<Cell>>& args,
                                                bool require_constexpr) {
        auto it = functions_by_name_.find(std::string(name));
        if (it == functions_by_name_.end()) return nullptr;
        for (const Function* fn : it->second) {
            if (!fn->body) continue;
            if (require_constexpr && fn->eval_mode == FunctionEvalMode::RuntimeOnly) continue;
            if (fn->params.size() != args.size()) continue;
            bool params_match = true;
            for (size_t i = 0; i < args.size(); ++i) {
                const Type& param_type = fn->params[i].type;
                const Type& arg_type = args[i]->type;
                if (param_type.kind == TypeKind::Reference) {
                    if (!param_type.pointee || !types_equal(*param_type.pointee, arg_type)) {
                        params_match = false;
                        break;
                    }
                } else if (!types_equal(param_type, arg_type)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return fn;
        }
        return nullptr;
    }

    [[nodiscard]] bool has_runtime_only_match(std::string_view name, const std::vector<std::shared_ptr<Cell>>& args) {
        auto it = functions_by_name_.find(std::string(name));
        if (it == functions_by_name_.end()) return false;
        for (const Function* fn : it->second) {
            if (!fn->body || fn->eval_mode != FunctionEvalMode::RuntimeOnly || fn->params.size() != args.size()) continue;
            bool params_match = true;
            for (size_t i = 0; i < args.size(); ++i) {
                const Type& param_type = fn->params[i].type;
                const Type& arg_type = args[i]->type;
                if (param_type.kind == TypeKind::Reference) {
                    if (!param_type.pointee || !types_equal(*param_type.pointee, arg_type)) {
                        params_match = false;
                        break;
                    }
                } else if (!types_equal(param_type, arg_type)) {
                    params_match = false;
                    break;
                }
            }
            if (params_match) return true;
        }
        return false;
    }

    [[nodiscard]] std::shared_ptr<Cell> cast_value(const Type& target_type, const std::shared_ptr<Cell>& operand,
                                                   const SourceLocation& loc) {
        if (is_named_type(target_type, "double")) return make_double_cell(as_double(operand, loc));
        if (is_named_type(target_type, "bool")) return make_bool_cell(as_bool(operand, loc));
        if (is_named_type(target_type, "int") || is_named_type(target_type, "char")) {
            auto result = std::make_shared<Cell>();
            result->type = target_type;
            checked_assign_integer(result, static_cast<long long>(as_double(operand, loc)), loc);
            return result;
        }
        throw ConstexprError(loc, "constexpr cast only supports builtin scalar targets in Phase D1");
    }

    [[nodiscard]] std::shared_ptr<Cell> evaluate_binary_numeric(const Expr& expr, const std::shared_ptr<Cell>& lhs,
                                                                const std::shared_ptr<Cell>& rhs) {
        if (is_floating_like(lhs->type) || is_floating_like(rhs->type)) {
            double left = as_double(lhs, expr.loc);
            double right = as_double(rhs, expr.loc);
            switch (expr.binary_op) {
                case BinaryOp::Add: return make_double_cell(left + right);
                case BinaryOp::Sub: return make_double_cell(left - right);
                case BinaryOp::Mul: return make_double_cell(left * right);
                case BinaryOp::Div: return make_double_cell(left / right);
                case BinaryOp::Eq: return make_bool_cell(left == right);
                case BinaryOp::Ne: return make_bool_cell(left != right);
                case BinaryOp::Lt: return make_bool_cell(left < right);
                case BinaryOp::Gt: return make_bool_cell(left > right);
                case BinaryOp::Le: return make_bool_cell(left <= right);
                case BinaryOp::Ge: return make_bool_cell(left >= right);
                default: break;
            }
        } else {
            long long left = as_integer(lhs, expr.loc);
            long long right = as_integer(rhs, expr.loc);
            switch (expr.binary_op) {
                case BinaryOp::Add: {
                    long long result;
                    if (__builtin_add_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell(result, expr.loc);
                }
                case BinaryOp::Sub: {
                    long long result;
                    if (__builtin_sub_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell(result, expr.loc);
                }
                case BinaryOp::Mul: {
                    long long result;
                    if (__builtin_mul_overflow(left, right, &result)) throw ConstexprError(expr.loc, "constexpr integer overflow");
                    return make_checked_int_cell(result, expr.loc);
                }
                case BinaryOp::Div:
                    if (right == 0) throw ConstexprError(expr.loc, "constexpr division by zero");
                    return make_checked_int_cell(left / right, expr.loc);
                case BinaryOp::Eq: return make_bool_cell(left == right);
                case BinaryOp::Ne: return make_bool_cell(left != right);
                case BinaryOp::Lt: return make_bool_cell(left < right);
                case BinaryOp::Gt: return make_bool_cell(left > right);
                case BinaryOp::Le: return make_bool_cell(left <= right);
                case BinaryOp::Ge: return make_bool_cell(left >= right);
                default: break;
            }
        }
        throw ConstexprError(expr.loc, "unsupported constexpr binary operator");
    }

    [[nodiscard]] std::shared_ptr<Cell> call_function(const Function& fn, std::vector<Binding> bindings,
                                                      const SourceLocation& loc) {
        tick(loc, "calling an immediate function");
        if (fn.eval_mode == FunctionEvalMode::RuntimeOnly) {
            throw ConstexprError(loc, "immediate evaluation may only call constexpr/consteval functions");
        }
        if (!fn.body) throw ConstexprError(loc, "cannot evaluate a declaration-only function at compile time");
        ++call_depth_;
        if (call_depth_ > limits_.max_recursion_depth) {
            --call_depth_;
            throw ConstexprError(loc, "constexpr evaluation exceeded recursion budget");
        }
        frames_.push_back({});
        auto& frame = frames_.back();
        for (size_t i = 0; i < fn.params.size(); ++i) frame.emplace(fn.params[i].name, std::move(bindings[i]));
        try {
            execute_stmt(*fn.body, fn.return_type);
        } catch (const ReturnSignal& signal) {
            frames_.pop_back();
            --call_depth_;
            return signal.value ? clone_cell(signal.value) : make_default_cell(fn.return_type, loc);
        }
        frames_.pop_back();
        --call_depth_;
        if (is_named_type(fn.return_type, "void")) {
            auto result = std::make_shared<Cell>();
            result->type = named_type("void");
            return result;
        }
        return make_default_cell(fn.return_type, loc);
    }

    [[nodiscard]] std::shared_ptr<Cell> call_with_expr_args(const Function& fn, const std::vector<ExprPtr>& args,
                                                            const SourceLocation& loc) {
        std::vector<Binding> bindings;
        bindings.reserve(fn.params.size());
        for (size_t i = 0; i < fn.params.size(); ++i) {
            const Param& param = fn.params[i];
            if (param.type.kind == TypeKind::Reference) {
                if (param.type.is_rvalue_ref) {
                    bindings.push_back(Binding{evaluate_expr(*args[i]), false});
                    continue;
                }
                if (param.type.is_mutable_ref) {
                    LValue arg = resolve_lvalue(*args[i]);
                    bindings.push_back(Binding{arg.cell, false});
                } else {
                    bool can_bind_lvalue = false;
                    try {
                        LValue arg = resolve_lvalue(*args[i]);
                        bindings.push_back(Binding{arg.cell, true});
                        can_bind_lvalue = true;
                    } catch (const ConstexprError&) {
                    }
                    if (!can_bind_lvalue) bindings.push_back(Binding{evaluate_expr(*args[i]), true});
                }
            } else {
                bindings.push_back(Binding{evaluate_expr(*args[i]), false});
            }
        }
        return call_function(fn, std::move(bindings), loc);
    }

    [[nodiscard]] std::shared_ptr<Cell> evaluate_call_expr(const Expr& expr) {
        if (expr.lhs) throw ConstexprError(expr.loc, "constexpr method calls are not yet implemented in Phase D1");
        std::vector<std::shared_ptr<Cell>> arg_values;
        arg_values.reserve(expr.args.size());
        for (const ExprPtr& arg : expr.args) arg_values.push_back(evaluate_expr(*arg));
        const Function* fn = find_callable(expr.name, arg_values, /*require_constexpr=*/true);
        if (!fn) {
            if (has_runtime_only_match(expr.name, arg_values)) {
                throw ConstexprError(expr.loc, "immediate evaluation may only call constexpr/consteval functions");
            }
            throw ConstexprError(expr.loc, "no constexpr/consteval overload of '" + expr.name + "' matches this immediate call");
        }
        return call_with_expr_args(*fn, expr.args, expr.loc);
    }

    [[nodiscard]] std::shared_ptr<Cell> evaluate_expr(const Expr& expr) {
        tick(expr.loc, "evaluating an expression");
        switch (expr.kind) {
            case ExprKind::IntegerLiteral: return make_scalar_cell(named_type("int"), expr.int_value);
            case ExprKind::FloatLiteral: return make_double_cell(expr.float_value);
            case ExprKind::BoolLiteral: return make_bool_cell(expr.bool_value);
            case ExprKind::CharLiteral: return make_scalar_cell(named_type("char"), expr.int_value);
            case ExprKind::StringLiteral: return make_string_literal_pointer(expr);
            case ExprKind::Identifier: return clone_cell(lookup_binding(expr.name, expr.loc).cell);
            case ExprKind::Conditional:
                return as_bool(evaluate_expr(*expr.lhs), expr.loc) ? evaluate_expr(*expr.rhs) : evaluate_expr(*expr.third);
            case ExprKind::Member: return clone_cell(resolve_lvalue(expr).cell);
            case ExprKind::Subscript: return clone_cell(resolve_lvalue(expr).cell);
            case ExprKind::Call: return evaluate_call_expr(expr);
            case ExprKind::Cast: return cast_value(expr.type, evaluate_expr(*expr.lhs), expr.loc);
            case ExprKind::Binary:
                if (expr.binary_op == BinaryOp::Assign) {
                    LValue target = resolve_lvalue(*expr.lhs);
                    if (target.read_only) throw ConstexprError(expr.loc, "cannot assign through a const/constexpr binding");
                    std::shared_ptr<Cell> value = evaluate_expr(*expr.rhs);
                    copy_into(target.cell, value, expr.loc);
                    return clone_cell(target.cell);
                }
                if (expr.binary_op == BinaryOp::And) {
                    if (!as_bool(evaluate_expr(*expr.lhs), expr.loc)) return make_bool_cell(false);
                    return make_bool_cell(as_bool(evaluate_expr(*expr.rhs), expr.loc));
                }
                if (expr.binary_op == BinaryOp::Or) {
                    if (as_bool(evaluate_expr(*expr.lhs), expr.loc)) return make_bool_cell(true);
                    return make_bool_cell(as_bool(evaluate_expr(*expr.rhs), expr.loc));
                }
                return evaluate_binary_numeric(expr, evaluate_expr(*expr.lhs), evaluate_expr(*expr.rhs));
            case ExprKind::Unary:
                switch (expr.unary_op) {
                    case UnaryOp::Neg: {
                        std::shared_ptr<Cell> operand = evaluate_expr(*expr.lhs);
                        if (is_floating_like(operand->type)) return make_double_cell(-as_double(operand, expr.loc));
                        long long value = as_integer(operand, expr.loc);
                        if (value == std::numeric_limits<long long>::min()) {
                            throw ConstexprError(expr.loc, "constexpr integer overflow");
                        }
                        return make_checked_int_cell(-value, expr.loc);
                    }
                    case UnaryOp::Not: return make_bool_cell(!as_bool(evaluate_expr(*expr.lhs), expr.loc));
                    case UnaryOp::Deref: return clone_cell(resolve_lvalue(expr).cell);
                    case UnaryOp::AddressOf:
                        throw ConstexprError(expr.loc, "constexpr address-of is not yet implemented in Phase D1");
                }
                break;
            case ExprKind::TypeTrait:
                throw ConstexprError(expr.loc, "constexpr type traits are deferred to a later phase");
            case ExprKind::New:
            case ExprKind::Delete:
            case ExprKind::Move:
            case ExprKind::PackExpansion:
            case ExprKind::Lambda:
            case ExprKind::Fold:
                break;
        }
        throw ConstexprError(expr.loc, "expression kind is not yet supported by the constexpr evaluator in Phase D1");
    }

    void execute_stmt(const Stmt& stmt, const Type& return_type) {
        tick(stmt.loc, "executing a statement");
        switch (stmt.kind) {
            case StmtKind::VarDecl: {
                auto cell = make_default_cell(stmt.type, stmt.loc);
                if (stmt.has_ctor_args) {
                    std::vector<Binding> ctor_bindings;
                    ctor_bindings.reserve(stmt.ctor_args.size() + 1);
                    ctor_bindings.push_back(Binding{cell, false});
                    std::vector<std::shared_ptr<Cell>> arg_values;
                    arg_values.reserve(stmt.ctor_args.size() + 1);
                    arg_values.push_back(cell);
                    for (const ExprPtr& arg : stmt.ctor_args) arg_values.push_back(evaluate_expr(*arg));
                    const Function* ctor = find_callable(stmt.type.name + "_new", arg_values, /*require_constexpr=*/true);
                    if (!ctor) {
                        throw ConstexprError(stmt.loc, "no constexpr/consteval constructor matches for type '" + stmt.type.name + "'");
                    }
                    for (size_t i = 1; i < ctor->params.size(); ++i) {
                        const Param& param = ctor->params[i];
                        const Expr& arg_expr = *stmt.ctor_args[i - 1];
                        if (param.type.kind == TypeKind::Reference && !param.type.is_rvalue_ref && param.type.is_mutable_ref) {
                            LValue arg = resolve_lvalue(arg_expr);
                            ctor_bindings.push_back(Binding{arg.cell, false});
                        } else {
                            ctor_bindings.push_back(Binding{evaluate_expr(arg_expr), param.type.kind == TypeKind::Reference && !param.type.is_mutable_ref});
                        }
                    }
                    static_cast<void>(call_function(*ctor, std::move(ctor_bindings), stmt.loc));
                } else if (stmt.init) {
                    copy_into(cell, evaluate_expr(*stmt.init), stmt.loc);
                }
                frames_.back()[stmt.var_name] = Binding{cell, stmt.is_const || stmt.is_constexpr};
                return;
            }
            case StmtKind::Return:
                if (stmt.expr) throw ReturnSignal{evaluate_expr(*stmt.expr)};
                throw ReturnSignal{is_named_type(return_type, "void") ? std::shared_ptr<Cell>() : make_default_cell(return_type, stmt.loc)};
            case StmtKind::ExprStmt:
                if (stmt.expr) static_cast<void>(evaluate_expr(*stmt.expr));
                return;
            case StmtKind::If:
                if (stmt.if_mode != IfMode::Runtime) {
                    throw ConstexprError(stmt.loc, "if consteval folding is deferred to Phase E");
                }
                if (as_bool(evaluate_expr(*stmt.condition), stmt.loc)) {
                    execute_stmt(*stmt.then_branch, return_type);
                } else if (stmt.else_branch) {
                    execute_stmt(*stmt.else_branch, return_type);
                }
                return;
            case StmtKind::While: {
                int iterations = 0;
                while (as_bool(evaluate_expr(*stmt.condition), stmt.loc)) {
                    ++iterations;
                    if (iterations > limits_.max_loop_iterations) {
                        throw ConstexprError(stmt.loc, "constexpr evaluation exceeded loop-iteration budget");
                    }
                    try {
                        execute_stmt(*stmt.then_branch, return_type);
                    } catch (const ContinueSignal&) {
                        continue;
                    } catch (const BreakSignal&) {
                        break;
                    }
                }
                return;
            }
            case StmtKind::Break: throw BreakSignal{};
            case StmtKind::Continue: throw ContinueSignal{};
            case StmtKind::Block:
                if (stmt.is_unsafe) throw ConstexprError(stmt.loc, "unsafe blocks are not allowed in constant evaluation");
                frames_.push_back({});
                try {
                    for (const StmtPtr& nested : stmt.statements) execute_stmt(*nested, return_type);
                } catch (...) {
                    frames_.pop_back();
                    throw;
                }
                frames_.pop_back();
                return;
        }
    }
};

[[nodiscard]] const Function* find_consteval_function(const Program& program, const Expr& expr) {
    if (expr.kind != ExprKind::Call || expr.lhs) return nullptr;
    const Function* only_match = nullptr;
    for (const Function& fn : program.functions) {
        if (fn.name != expr.name || fn.eval_mode != FunctionEvalMode::Consteval) continue;
        if (fn.params.size() != expr.args.size()) continue;
        if (!only_match) {
            only_match = &fn;
        } else {
            return nullptr;
        }
    }
    return only_match;
}

void rewrite_expr_as_constant(Expr& expr, const std::shared_ptr<Cell>& value) {
    if (is_named_type(value->type, "int")) {
        expr.kind = ExprKind::IntegerLiteral;
        expr.int_value = std::get<long long>(value->data);
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named_type(value->type, "char")) {
        expr.kind = ExprKind::CharLiteral;
        expr.int_value = std::get<long long>(value->data);
        expr.float_value = 0.0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (is_named_type(value->type, "bool")) {
        expr.kind = ExprKind::BoolLiteral;
        expr.bool_value = std::get<bool>(value->data);
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.name.clear();
    } else if (is_named_type(value->type, "double")) {
        expr.kind = ExprKind::FloatLiteral;
        expr.float_value = std::get<double>(value->data);
        expr.int_value = 0;
        expr.bool_value = false;
        expr.name.clear();
    } else if (types_equal(value->type, make_const_char_pointer_type())) {
        auto* pointer = std::get_if<PointerValue>(&value->data);
        auto* array = pointer ? std::get_if<ArrayValue>(&pointer->storage->data) : nullptr;
        if (!pointer || !array || pointer->index != 0) {
            throw ConstexprError(expr.loc, "Phase D1 cannot yet lower this constexpr pointer result back into source form");
        }
        expr.kind = ExprKind::StringLiteral;
        expr.name.clear();
        for (const auto& element : array->elements) {
            long long ch = std::get<long long>(element->data);
            if (ch == 0) break;
            expr.name.push_back(static_cast<char>(ch));
        }
        expr.int_value = 0;
        expr.float_value = 0.0;
        expr.bool_value = false;
    } else {
        throw ConstexprError(expr.loc, "Phase D1 can only lower scalar and string-literal immediate results");
    }
    expr.lhs.reset();
    expr.rhs.reset();
    expr.third.reset();
    expr.args.clear();
    expr.explicit_template_args.clear();
    expr.type = value->type;
}

void fold_expr_immediate_calls(Program& program, Expr& expr, ConstexprEngine& engine, bool in_immediate_function);
void fold_stmt_immediate_calls(Program& program, Stmt& stmt, ConstexprEngine& engine, bool in_immediate_function);

void fold_stmt_immediate_calls(Program& program, Stmt& stmt, ConstexprEngine& engine, bool in_immediate_function) {
    if (stmt.init) fold_expr_immediate_calls(program, *stmt.init, engine, in_immediate_function);
    for (ExprPtr& arg : stmt.ctor_args) fold_expr_immediate_calls(program, *arg, engine, in_immediate_function);
    if (stmt.expr) fold_expr_immediate_calls(program, *stmt.expr, engine, in_immediate_function);
    if (stmt.condition) fold_expr_immediate_calls(program, *stmt.condition, engine, in_immediate_function);
    if (stmt.then_branch) fold_stmt_immediate_calls(program, *stmt.then_branch, engine, in_immediate_function);
    if (stmt.else_branch) fold_stmt_immediate_calls(program, *stmt.else_branch, engine, in_immediate_function);
    for (StmtPtr& nested : stmt.statements) fold_stmt_immediate_calls(program, *nested, engine, in_immediate_function);
}

void fold_expr_immediate_calls(Program& program, Expr& expr, ConstexprEngine& engine, bool in_immediate_function) {
    if (expr.lhs) fold_expr_immediate_calls(program, *expr.lhs, engine, in_immediate_function);
    if (expr.rhs) fold_expr_immediate_calls(program, *expr.rhs, engine, in_immediate_function);
    if (expr.third) fold_expr_immediate_calls(program, *expr.third, engine, in_immediate_function);
    for (ExprPtr& arg : expr.args) fold_expr_immediate_calls(program, *arg, engine, in_immediate_function);
    if (expr.lambda_body) fold_stmt_immediate_calls(program, *expr.lambda_body, engine, in_immediate_function);
    if (in_immediate_function) return;
    const Function* fn = find_consteval_function(program, expr);
    if (!fn) return;
    std::shared_ptr<Cell> value = engine.evaluate_root_expr(expr);
    rewrite_expr_as_constant(expr, value);
}

} // namespace

void fold_immediate_calls(Program& program, ConstexprLimits limits) {
    ConstexprEngine engine(program, limits);
    for (Function& fn : program.functions) {
        if (!fn.body) continue;
        bool in_immediate_function = fn.eval_mode != FunctionEvalMode::RuntimeOnly;
        fold_stmt_immediate_calls(program, *fn.body, engine, in_immediate_function);
    }
}

} // namespace scpp
