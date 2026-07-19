module;

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Support/raw_ostream.h>

module scpp.compiler.codegen:functions;

import std;
import :api;

namespace scpp {

    void Codegen::declare_function(const Function& fn)
{
        if (fn.return_type.kind == TypeKind::Reference) {
            validate_reference_return_elision(fn);
            validate_reference_pointee(*fn.return_type.pointee);
        }
        if (fn.is_extern_c) {
            validate_c_abi_compatible(fn.return_type, fn.name, "return type");
        }
        std::vector<llvm::Type*> param_types;
        param_types.reserve(fn.params.size());
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            const Param& param = fn.params[i];
            if (param.type.kind == TypeKind::Reference) {
                validate_reference_pointee(*param.type.pointee);
            }
            if (fn.is_extern_c) {
                validate_c_abi_compatible(param.type, fn.name, "parameter '" + param.name + "'");
            }
            if (is_bare_void(param.type)) {
                throw CodegenError("function '" + fn.name + "': parameter '" + param.name +
                                    "' cannot have type 'void' (only a return type or a pointer's pointee "
                                    "-- 'void*' -- may be 'void')",
                    current_loc_);
            }
            param_types.push_back(llvm_param_type_for_function(fn, param, i));
        }
        llvm::FunctionType* fn_type =
            llvm::FunctionType::get(to_llvm_type(fn.return_type), param_types, fn.has_varargs);
        // ch11 §11.9: a module-private (non-exported) function *defined*
        // in this same translation unit never needs to be visible
        // outside it -- LLVM internal linkage (the same mechanism as C's
        // `static`) guarantees zero risk of colliding with an unrelated
        // module's own same-named private helper, with no mangling
        // needed at all. A bodyless declaration (extern "C", bare
        // `extern` awaiting a separate implementation unit, or a
        // function recovered from an *imported* module -- see the
        // parser's merge_imported_module, which always clears the
        // cloned Function's body) always keeps external linkage: LLVM
        // requires a definition for internal linkage, and there's
        // nothing here to hide regardless. Every other case (a
        // non-module file's own function, or an exported one, handled
        // via overload_names_'s mangled name already) is unaffected --
        // external linkage, exactly as before this chapter.
        llvm::Function::LinkageTypes linkage = llvm::Function::ExternalLinkage;
        // ch05 §5.14: a forwarding stub (Function::forwards_to) gets a
        // real, defined body too (define_forwarding_function), just
        // never an scpp-level AST one -- eligible for the same internal
        // linkage as an ordinary defined function.
        bool has_definition = fn.body != nullptr || fn.is_defaulted || !fn.forwards_to.empty();
        if (has_definition && !fn.is_exported && !fn.is_extern_c &&
            (!fn.owning_module.empty() || !program_->module_name.empty() || fn.is_compile_time_dependency)) {
            linkage = llvm::Function::InternalLinkage;
        }
        llvm::Function::Create(fn_type, linkage, overload_names_.at(&fn), *module_);
    }


    void Codegen::define_function(const Function& fn)
{
        llvm::Function* llvm_fn = module_->getFunction(overload_names_.at(&fn));
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_);
        }

        current_function_def_ = &fn;
        // Mirrors movecheck's entry_state.unsafe_depth (ch01 §1.2/§1.3):
        // every function is checked by default and starts outside any
        // unsafe context, *except* one whose own declaration carries the
        // function-level `[[scpp::unsafe]]` marker (fn.is_unsafe) --
        // its entire body is an unsafe context throughout, exactly as if
        // wrapped in one `[[scpp::unsafe]] { }` block, so overflow/
        // bounds-check codegen is skipped throughout it too (see
        // codegen_binary_op/codegen_span_subscript below). Otherwise,
        // unsafe_depth_ only increases via an explicit, lexically nested
        // `[[scpp::unsafe]] { }` block within that function's own body
        // (the old "native function = implicitly unsafe everywhere"
        // concept is fully retired).
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        attach_debug_subprogram(llvm_fn, fn);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);
        current_loc_ = fn.loc;
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());

        locals_.clear();
        scope_stack_.clear();
        std::size_t index = 0;
        for (auto& arg : llvm_fn->args()) {
            const Param& param = fn.params[index++];
            arg.setName(param.name);
            llvm::AllocaInst* slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                slot = builder_->CreateAlloca(to_llvm_type(param.type), nullptr, param.name);
                if (std::optional<llvm::Align> align = alignment_for_type(param.type)) slot->setAlignment(*align);
                llvm::Value* fat_this = build_interface_value(
                    &arg, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)));
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else {
                slot = builder_->CreateAlloca(arg.getType(), nullptr, param.name);
                if (std::optional<llvm::Align> align = alignment_for_type(param.type)) slot->setAlignment(*align);
                builder_->CreateStore(&arg, slot);
            }
            locals_[param.name] = LocalSlot{slot, param.type};
            maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned>(index));
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param.name].moved_flag = create_moved_flag_if_has_destructor(param.type.name);
            }
        }

        emit_constructor_member_initializers(fn);
        codegen_stmt(*fn.body, llvm_fn);

        // Falling off the end of a `void` function/constructor/destructor is
        // valid, exactly like C++; synthesize the implicit `return;`.
        if (builder_->GetInsertBlock()->getTerminator() == nullptr) {
            if (fn.return_type.kind == TypeKind::Named && fn.return_type.name == "void") {
                builder_->CreateRetVoid();
                builder_->SetCurrentDebugLocation(llvm::DebugLoc());
                current_debug_scope_ = nullptr;
                current_subprogram_ = nullptr;
                return;
            }
            throw CodegenError("function '" + fn.name + "' does not return on all paths",
                current_loc_);
        }
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }


    void Codegen::define_defaulted_function(const Function& fn)
{
        if (!fn.is_defaulted) {
            throw CodegenError("internal error: asked to define a non-defaulted function without a body", current_loc_);
        }
        if (!fn.name.ends_with("_delete") || fn.params.size() != 1) {
            throw CodegenError("only defaulted destructors are code-generated in this version", fn.loc);
        }

        llvm::Function* llvm_fn = module_->getFunction(overload_names_.at(&fn));
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition", fn.loc);
        }

        current_function_def_ = &fn;
        unsafe_depth_ = fn.is_unsafe ? 1 : 0;
        attach_debug_subprogram(llvm_fn, fn);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);
        current_loc_ = fn.loc;
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());

        locals_.clear();
        scope_stack_.clear();
        std::size_t index = 0;
        for (auto& arg : llvm_fn->args()) {
            const Param& param = fn.params[index++];
            arg.setName(param.name);
            llvm::AllocaInst* slot = nullptr;
            if (index == 1 && interface_destructor_uses_raw_this(fn)) {
                slot = builder_->CreateAlloca(to_llvm_type(param.type), nullptr, param.name);
                if (std::optional<llvm::Align> align = alignment_for_type(param.type)) slot->setAlignment(*align);
                llvm::Value* fat_this = build_interface_value(
                    &arg, llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)));
                create_store(fat_this, slot, alignment_for_type(param.type));
            } else {
                slot = builder_->CreateAlloca(arg.getType(), nullptr, param.name);
                if (std::optional<llvm::Align> align = alignment_for_type(param.type)) slot->setAlignment(*align);
                builder_->CreateStore(&arg, slot);
            }
            locals_[param.name] = LocalSlot{slot, param.type};
            maybe_emit_parameter_debug_decl(param, slot, static_cast<unsigned>(index));
            if (param.type.kind == TypeKind::Named && find_class_def(param.type.name) != nullptr) {
                locals_[param.name].moved_flag = create_moved_flag_if_has_destructor(param.type.name);
            }
        }

        const Type& this_type = fn.params[0].type;
        if (this_type.kind != TypeKind::Reference || this_type.pointee == nullptr || this_type.pointee->kind != TypeKind::Named) {
            throw CodegenError("defaulted destructor '" + fn.name + "' has an invalid this parameter", fn.loc);
        }
        const std::string& class_name = this_type.pointee->name;
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            throw CodegenError("defaulted destructor '" + fn.name + "' names unknown class '" + class_name + "'", fn.loc);
        }

        llvm::Type* this_llvm_type = to_llvm_type(fn.params[0].type);
        llvm::Value* this_ptr = builder_->CreateLoad(this_llvm_type, locals_.at("this").alloca, "thisptr");
        const StructInfo& info = info_it->second;
        for (std::size_t i = info.field_types.size(); i > 0; --i) {
            const Type& field_type = info.field_types[i - 1];
            if (field_type.kind == TypeKind::Named && structs_.contains(field_type.name)) {
                llvm::Value* field_ptr =
                    builder_->CreateStructGEP(info.llvm_type, this_ptr, info.physical_field_index(i - 1),
                                              info.field_names[i - 1]);
                codegen_destroy_old_class_state_for_move_assign(field_ptr, field_type.name);
            }
        }

        builder_->CreateRetVoid();
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }


    void Codegen::define_forwarding_function(const Function& fn)
{
        llvm::Function* llvm_fn = module_->getFunction(overload_names_.at(&fn));
        if (llvm_fn == nullptr) {
            throw CodegenError("function '" + fn.name + "' was not declared before definition",
                current_loc_);
        }
        // Finds the exact base method this stub forwards to: `name`
        // alone isn't necessarily unique (ch05 §5.10 method
        // overloading), but this stub's own params[1:] were copied
        // verbatim from that exact overload at synthesis time, so
        // matching on both name and every non-`this` parameter's type
        // is unambiguous.
        const Function* target = nullptr;
        for (const Function& candidate : program_->functions) {
            if (candidate.name != fn.forwards_to || candidate.params.size() != fn.params.size()) continue;
            bool params_match = true;
            for (std::size_t i = 1; i < fn.params.size() && params_match; i++) {
                params_match = types_equal(candidate.params[i].type, fn.params[i].type);
            }
            if (params_match) {
                target = &candidate;
                break;
            }
        }
        if (target == nullptr) {
            throw CodegenError("forwarding stub '" + fn.name + "' names an unknown target '" + fn.forwards_to + "'",
                current_loc_);
        }
        llvm::Function* target_llvm = module_->getFunction(overload_names_.at(target));

        attach_debug_subprogram(llvm_fn, fn);
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context_, "entry", llvm_fn);
        builder_->SetInsertPoint(entry);
        current_loc_ = fn.loc;
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        std::vector<llvm::Value*> args;
        args.reserve(llvm_fn->arg_size());
        for (auto& arg : llvm_fn->args()) args.push_back(&arg);
        llvm::Value* call_result = nullptr;
        if (!fn.params.empty() && is_interface_reference_type(fn.params.front().type)) {
            std::optional<std::size_t> slot_index = interface_method_slot_index(fn.member_owner_class, fn);
            if (!slot_index.has_value()) {
                throw CodegenError("missing interface dispatch slot for forwarding stub '" + fn.name + "'", current_loc_);
            }
            llvm::Value* receiver_value = args.front();
            llvm::Value* dispatch_ptr = extract_interface_dispatch_ptr(receiver_value);
            llvm::ArrayType* table_type = interface_dispatch_table_type(fn.member_owner_class);
            llvm::Value* table_ptr = builder_->CreateBitCast(dispatch_ptr, llvm::PointerType::get(*context_, 0),
                                                             "ifacetable");
            llvm::Value* slot_ptr =
                builder_->CreateGEP(table_type, table_ptr,
                                    {llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0),
                                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_),
                                                           static_cast<unsigned>(*slot_index))},
                                    "ifaceslot");
            llvm::Value* target_ptr =
                create_load(llvm::PointerType::getUnqual(*context_), slot_ptr, std::nullopt, "ifacemethod");
            std::vector<llvm::Value*> dispatch_args;
            dispatch_args.reserve(args.size());
            dispatch_args.push_back(extract_interface_object_ptr(receiver_value));
            for (std::size_t i = 1; i < args.size(); ++i) dispatch_args.push_back(args[i]);
            call_result = builder_->CreateCall(interface_dispatch_function_type(*target), target_ptr, dispatch_args);
        } else if (!fn.params.empty() && !target->params.empty() && is_interface_reference_type(target->params.front().type)) {
            const std::string& concrete_class_name = fn.params.front().type.pointee->name;
            const std::string& target_interface_name = target->params.front().type.pointee->name;
            llvm::Value* fat_receiver =
                build_interface_value(args.front(), get_or_create_interface_dispatch_table(concrete_class_name,
                                                                                           target_interface_name));
            std::vector<llvm::Value*> direct_args;
            direct_args.reserve(args.size());
            direct_args.push_back(fat_receiver);
            for (std::size_t i = 1; i < args.size(); ++i) direct_args.push_back(args[i]);
            call_result = builder_->CreateCall(target_llvm, direct_args);
        } else {
            call_result = builder_->CreateCall(target_llvm, args);
        }
        if (is_bare_void(fn.return_type)) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(call_result);
        }
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        current_debug_scope_ = nullptr;
        current_subprogram_ = nullptr;
    }

} // namespace scpp
