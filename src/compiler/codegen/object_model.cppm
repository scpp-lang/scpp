module;

module scpp.compiler.codegen:object_model;

import std;
import llvm;
import :api;

namespace scpp {

    [[nodiscard]] bool Codegen::type_names_interface(const std::string& name) const
{
        const ClassDef* def = find_class_def(name);
        return def != nullptr && def->is_interface;
    }


    [[nodiscard]] bool Codegen::is_interface_named_type(const Type& type) const
{
        return type.kind == TypeKind::Named && type_names_interface(type.name);
    }


    [[nodiscard]] bool Codegen::is_interface_pointer_type(const Type& type) const
{
        return type.kind == TypeKind::Pointer && type.pointee != nullptr && is_interface_named_type(*type.pointee);
    }


    [[nodiscard]] bool Codegen::is_interface_reference_type(const Type& type) const
{
        return type.kind == TypeKind::Reference && type.pointee != nullptr && is_interface_named_type(*type.pointee);
    }


    [[nodiscard]] bool Codegen::is_interface_representation_type(const Type& type) const
{
        return is_interface_pointer_type(type) || is_interface_reference_type(type);
    }


    [[nodiscard]] std::string Codegen::current_enclosing_class_name() const
{
        return current_function_def_ == nullptr ? std::string() : current_function_def_->member_owner_class;
    }


    [[nodiscard]] llvm::LLVMTypeRef Codegen::interface_representation_type()
{
        if (interface_representation_llvm_type_ != nullptr) return interface_representation_llvm_type_;
        llvm::LLVMTypeRef ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
        llvm::LLVMTypeRef fields[] = {ptr_type, ptr_type};
        interface_representation_llvm_type_ =
            llvm::LLVMStructTypeInContext(context_, fields, 2, /*Packed=*/0);
        return interface_representation_llvm_type_;
    }


    [[nodiscard]] llvm::LLVMValueRef Codegen::build_interface_value(llvm::LLVMValueRef object_ptr, llvm::LLVMValueRef dispatch_ptr)
{
        llvm::LLVMValueRef value = llvm::LLVMGetUndef(interface_representation_type());
        value = llvm::LLVMBuildInsertValue(builder_, value, object_ptr, 0, "iface.obj");
        value = llvm::LLVMBuildInsertValue(builder_, value, dispatch_ptr, 1, "iface.dispatch");
        return value;
    }


    [[nodiscard]] llvm::LLVMValueRef Codegen::extract_interface_object_ptr(llvm::LLVMValueRef interface_value)
{
        return llvm::LLVMBuildExtractValue(builder_, interface_value, 0, "iface.obj");
    }


    [[nodiscard]] llvm::LLVMValueRef Codegen::extract_interface_dispatch_ptr(llvm::LLVMValueRef interface_value)
{
        return llvm::LLVMBuildExtractValue(builder_, interface_value, 1, "iface.dispatch");
    }


    [[nodiscard]] bool Codegen::has_accessible_base_conversion(const std::string& source_name, const std::string& target_name,
                                                      std::string_view current_class) const
{
        if (source_name == target_name) return true;
        const ClassDef* def = find_class_def(source_name);
        if (def == nullptr) return false;
        for (const BaseSpecifier& base : def->base_specifiers) {
            if (base.access == AccessSpecifier::Private && current_class != source_name) {
                continue;
            }
            if (base.base_type.name == target_name) return true;
            if (has_accessible_base_conversion(base.base_type.name, target_name, current_class)) return true;
        }
        return false;
    }


    [[nodiscard]] bool Codegen::types_compatible_with_base_conversion(const Type& source_type, const Type& target_type,
                                                             std::string_view current_class) const
{
        if (types_equal(source_type, target_type)) return true;
        if (target_type.kind == TypeKind::Reference && source_type.kind == TypeKind::Reference &&
            !target_type.is_rvalue_ref && !source_type.is_rvalue_ref && target_type.pointee && source_type.pointee) {
            if (target_type.is_mutable_ref && !source_type.is_mutable_ref) return false;
            if (types_equal(*source_type.pointee, *target_type.pointee)) return true;
            return target_type.pointee->kind == TypeKind::Named && source_type.pointee->kind == TypeKind::Named &&
                   has_accessible_base_conversion(source_type.pointee->name, target_type.pointee->name, current_class);
        }
        if (target_type.kind == TypeKind::Reference && source_type.kind != TypeKind::Reference && target_type.pointee) {
            if (types_equal(source_type, *target_type.pointee)) return true;
            return target_type.pointee->kind == TypeKind::Named && source_type.kind == TypeKind::Named &&
                   has_accessible_base_conversion(source_type.name, target_type.pointee->name, current_class);
        }
        if (target_type.kind == TypeKind::Pointer && source_type.kind == TypeKind::Pointer && target_type.pointee &&
            source_type.pointee) {
            if (target_type.is_mutable_pointee && !source_type.is_mutable_pointee) return false;
            if (types_equal(*source_type.pointee, *target_type.pointee)) return true;
            return target_type.pointee->kind == TypeKind::Named && source_type.pointee->kind == TypeKind::Named &&
                   has_accessible_base_conversion(source_type.pointee->name, target_type.pointee->name, current_class);
        }
        return false;
    }


    [[nodiscard]] std::expected<const std::vector<const Function*>*, CodegenError> Codegen::interface_dispatch_methods(const std::string& interface_name)
{
        auto cached = interface_dispatch_methods_cache_.find(interface_name);
        if (cached != interface_dispatch_methods_cache_.end()) return &cached->second;
        const ClassDef* def = find_class_def(interface_name);
        if (def == nullptr || !def->is_interface) {
            return std::unexpected(CodegenError("unknown interface '" + interface_name + "' for dispatch table generation", current_loc_));
        }
        std::vector<const Function*> methods;
        std::unordered_map<std::string, std::size_t> slot_indices;
        for (const BaseSpecifier& base : def->base_specifiers) {
            if (base.kind != BaseClassKind::Interface) continue;
            auto base_methods_result = interface_dispatch_methods(base.base_type.name);
            if (!base_methods_result.has_value()) return std::unexpected(std::move(base_methods_result).error());
            for (const Function* method : *base_methods_result.value()) {
                std::string slot_key = interface_method_slot_key(*method);
                if (!slot_indices.contains(slot_key)) {
                    slot_indices.emplace(slot_key, methods.size());
                    methods.push_back(method);
                }
            }
        }
        for (const Function& fn : program_->functions) {
            if (fn.member_owner_class != interface_name || fn.is_static || !fn.is_virtual || !fn.forwards_to.empty()) continue;
            if (fn.name.ends_with("_new")) continue;
            std::string slot_key = interface_method_slot_key(fn);
            auto slot_it = slot_indices.find(slot_key);
            if (slot_it == slot_indices.end()) {
                slot_indices.emplace(slot_key, methods.size());
                methods.push_back(&fn);
            } else {
                methods[slot_it->second] = &fn;
            }
        }
        interface_slot_indices_cache_.emplace(interface_name, std::move(slot_indices));
        auto [it, _] = interface_dispatch_methods_cache_.emplace(interface_name, std::move(methods));
        return &it->second;
    }


    [[nodiscard]] std::expected<llvm::LLVMTypeRef, CodegenError> Codegen::interface_dispatch_table_type(const std::string& interface_name)
{
        auto it = interface_dispatch_table_types_.find(interface_name);
        if (it != interface_dispatch_table_types_.end()) return it->second;
        auto methods_result = interface_dispatch_methods(interface_name);
        if (!methods_result.has_value()) return std::unexpected(std::move(methods_result).error());
        llvm::LLVMTypeRef type =
            llvm::LLVMArrayType2(llvm::LLVMPointerTypeInContext(context_, 0),
                                 methods_result.value()->size());
        interface_dispatch_table_types_.emplace(interface_name, type);
        return type;
    }


    [[nodiscard]] std::expected<std::optional<std::size_t>, CodegenError> Codegen::interface_method_slot_index(const std::string& interface_name,
                                                                    const Function& method)
{
        auto methods_result = interface_dispatch_methods(interface_name);
        if (!methods_result.has_value()) return std::unexpected(std::move(methods_result).error());
        auto cache_it = interface_slot_indices_cache_.find(interface_name);
        if (cache_it == interface_slot_indices_cache_.end()) return std::nullopt;
        auto slot_it = cache_it->second.find(interface_method_slot_key(method));
        if (slot_it == cache_it->second.end()) return std::nullopt;
        return slot_it->second;
    }


    [[nodiscard]] const Function* Codegen::find_direct_method_by_slot(const std::string& class_name, const std::string& slot_key) const
{
        for (const Function& fn : program_->functions) {
            if (fn.member_owner_class != class_name || fn.is_static || !fn.forwards_to.empty()) continue;
            if (interface_method_slot_key(fn) == slot_key) return &fn;
        }
        return nullptr;
    }


    [[nodiscard]] std::expected<const Function*, CodegenError> Codegen::resolve_interface_slot_provider(const std::string& class_name, const std::string& slot_key) const
{
        if (const Function* direct = find_direct_method_by_slot(class_name, slot_key)) return direct;
        const ClassDef* def = find_class_def(class_name);
        if (def == nullptr) return nullptr;
        const Function* chosen = nullptr;
        for (const BaseSpecifier& base : def->base_specifiers) {
            auto candidate_result = resolve_interface_slot_provider(base.base_type.name, slot_key);
            if (!candidate_result.has_value()) return std::unexpected(std::move(candidate_result).error());
            const Function* candidate = std::move(candidate_result).value();
            if (candidate == nullptr) continue;
            if (chosen == nullptr) {
                chosen = candidate;
                continue;
            }
            if (chosen != candidate && overload_names_.at(chosen) != overload_names_.at(candidate)) {
                return std::unexpected(CodegenError("ambiguous interface dispatch provider for slot '" + slot_key + "' in class '" + class_name + "'",
                                   current_loc_));
            }
        }
        return chosen;
    }


    [[nodiscard]] std::expected<llvm::LLVMTypeRef, CodegenError> Codegen::interface_dispatch_function_type(const Function& method)
{
        std::vector<llvm::LLVMTypeRef> params;
        params.reserve(method.params.size());
        params.push_back(llvm::LLVMPointerTypeInContext(context_, 0));
        for (std::size_t i = 1; i < method.params.size(); i++) {
            auto param_result = to_llvm_type(method.params[i].type);
            if (!param_result.has_value()) return std::unexpected(std::move(param_result).error());
            params.push_back(std::move(param_result).value());
        }
        auto return_type_result = to_llvm_type(method.return_type);
        if (!return_type_result.has_value()) return std::unexpected(std::move(return_type_result).error());
        return llvm::LLVMFunctionType(std::move(return_type_result).value(), params.data(), static_cast<unsigned>(params.size()),
                                /*IsVarArg=*/0);
    }


    [[nodiscard]] bool Codegen::interface_destructor_uses_raw_this(const Function& fn) const
{
        return fn.name.ends_with("_delete") && !fn.member_owner_class.empty() && type_names_interface(fn.member_owner_class);
    }


    [[nodiscard]] bool Codegen::class_has_ordinary_vtable(const std::string& class_name) const
{
        const ClassDef* def = find_class_def(class_name);
        return def != nullptr && !def->is_interface;
    }


    [[nodiscard]] std::expected<llvm::LLVMTypeRef, CodegenError> Codegen::llvm_param_type_for_function(const Function& fn, const Param& param, std::size_t index)
{
        if (index == 0 && interface_destructor_uses_raw_this(fn)) {
            return llvm::LLVMPointerTypeInContext(context_, 0);
        }
        if (param.type.kind == TypeKind::Reference && param.type.is_rvalue_ref && param.type.pointee != nullptr) {
            return llvm::LLVMPointerTypeInContext(context_, 0);
        }
        return to_llvm_type(param.type);
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::get_or_create_interface_dispatch_thunk(const std::string& concrete_class_name,
                                                                         const Function& target)
{
        std::string cache_key = concrete_class_name + "|" + overload_names_.at(&target);
        auto it = interface_dispatch_thunks_.find(cache_key);
        if (it != interface_dispatch_thunks_.end()) return it->second;
        const Type& this_type = target.params.front().type;
        const std::string interface_name = this_type.pointee->name;
        auto thunk_type_result = interface_dispatch_function_type(target);
        if (!thunk_type_result.has_value()) return std::unexpected(std::move(thunk_type_result).error());
        llvm::LLVMTypeRef thunk_type = std::move(thunk_type_result).value();
        llvm::LLVMValueRef thunk = llvm::LLVMAddFunction(module_, ("__scpp_iface_thunk." + cache_key).c_str(), thunk_type);
        llvm::LLVMSetLinkage(thunk, llvm::LLVMPrivateLinkage);
        interface_dispatch_thunks_.emplace(cache_key, thunk);
        llvm::LLVMBasicBlockRef saved_block = llvm::LLVMGetInsertBlock(builder_);
        llvm::LLVMMetadataRef saved_dbg = llvm::LLVMGetCurrentDebugLocation2(builder_);
        llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, thunk, "entry");
        llvm::LLVMPositionBuilderAtEnd(builder_, entry);
        llvm::LLVMValueRef raw_this = llvm::LLVMGetParam(thunk, 0);
        auto dispatch_ptr_result = get_or_create_interface_dispatch_table(concrete_class_name, interface_name);
        if (!dispatch_ptr_result.has_value()) return std::unexpected(std::move(dispatch_ptr_result).error());
        llvm::LLVMValueRef dispatch_ptr = std::move(dispatch_ptr_result).value();
        llvm::LLVMValueRef fat_this = build_interface_value(raw_this, dispatch_ptr);
        std::vector<llvm::LLVMValueRef> args;
        args.reserve(target.params.size());
        args.push_back(fat_this);
        for (unsigned i = 1; i < llvm::LLVMCountParams(thunk); ++i) {
            args.push_back(llvm::LLVMGetParam(thunk, i));
        }
        llvm::LLVMValueRef target_fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(&target).c_str());
        llvm::LLVMValueRef result = build_call(target_fn, args);
        if (target.return_type.kind == TypeKind::Named && target.return_type.name == "void") {
            llvm::LLVMBuildRetVoid(builder_);
        } else {
            llvm::LLVMBuildRet(builder_, result);
        }
        llvm::LLVMPositionBuilderAtEnd(builder_, saved_block);
        llvm::LLVMSetCurrentDebugLocation2(builder_, saved_dbg);
        return thunk;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::get_or_create_interface_destructor_thunk(const std::string& concrete_class_name,
                                                                            const Function& interface_destructor)
{
        std::string cache_key = concrete_class_name + "|dtor|" + overload_names_.at(&interface_destructor);
        auto it = interface_dispatch_thunks_.find(cache_key);
        if (it != interface_dispatch_thunks_.end()) return it->second;
        auto thunk_type_result = interface_dispatch_function_type(interface_destructor);
        if (!thunk_type_result.has_value()) return std::unexpected(std::move(thunk_type_result).error());
        llvm::LLVMTypeRef thunk_type = std::move(thunk_type_result).value();
        llvm::LLVMValueRef thunk = llvm::LLVMAddFunction(module_, ("__scpp_iface_dtor_thunk." + cache_key).c_str(), thunk_type);
        llvm::LLVMSetLinkage(thunk, llvm::LLVMPrivateLinkage);
        interface_dispatch_thunks_.emplace(cache_key, thunk);
        llvm::LLVMBasicBlockRef saved_block = llvm::LLVMGetInsertBlock(builder_);
        llvm::LLVMMetadataRef saved_dbg = llvm::LLVMGetCurrentDebugLocation2(builder_);
        llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, thunk, "entry");
        llvm::LLVMPositionBuilderAtEnd(builder_, entry);
        llvm::LLVMValueRef raw_this = llvm::LLVMGetParam(thunk, 0);
        emit_destructor_chain_calls(concrete_class_name, raw_this);
        llvm::LLVMBuildRetVoid(builder_);
        llvm::LLVMPositionBuilderAtEnd(builder_, saved_block);
        llvm::LLVMSetCurrentDebugLocation2(builder_, saved_dbg);
        return thunk;
    }


    [[nodiscard]] std::expected<const std::vector<const Function*>*, CodegenError> Codegen::ordinary_virtual_methods(const std::string& class_name)
{
        auto cached = ordinary_virtual_methods_cache_.find(class_name);
        if (cached != ordinary_virtual_methods_cache_.end()) return &cached->second;
        const ClassDef* def = find_class_def(class_name);
        if (def == nullptr || def->is_interface) {
            return std::unexpected(CodegenError("unknown ordinary class '" + class_name + "' for vtable generation", current_loc_));
        }
        std::vector<const Function*> methods;
        std::unordered_map<std::string, std::size_t> slot_indices;
        if (auto base = def->direct_ordinary_base()) {
            auto base_methods_result = ordinary_virtual_methods(base->get().base_type.name);
            if (!base_methods_result.has_value()) return std::unexpected(std::move(base_methods_result).error());
            for (const Function* method : *base_methods_result.value()) {
                slot_indices.emplace(interface_method_slot_key(*method), methods.size());
                methods.push_back(method);
            }
        }
        for (const Function& fn : program_->functions) {
            if (fn.member_owner_class != class_name || fn.is_static || !fn.forwards_to.empty()) continue;
            if (fn.name.ends_with("_new")) continue;
            if (fn.name.ends_with("_delete")) continue;
            std::string slot_key = interface_method_slot_key(fn);
            auto slot_it = slot_indices.find(slot_key);
            bool is_effectively_virtual = fn.is_virtual || slot_it != slot_indices.end();
            if (!is_effectively_virtual) continue;
            if (slot_it == slot_indices.end()) {
                slot_indices.emplace(slot_key, methods.size());
                methods.push_back(&fn);
            } else {
                methods[slot_it->second] = &fn;
            }
        }
        ordinary_slot_indices_cache_.emplace(class_name, std::move(slot_indices));
        auto [it, _] = ordinary_virtual_methods_cache_.emplace(class_name, std::move(methods));
        return &it->second;
    }


    [[nodiscard]] std::expected<llvm::LLVMTypeRef, CodegenError> Codegen::ordinary_vtable_type(const std::string& class_name)
{
        auto it = ordinary_vtable_types_.find(class_name);
        if (it != ordinary_vtable_types_.end()) return it->second;
        auto methods_result = ordinary_virtual_methods(class_name);
        if (!methods_result.has_value()) return std::unexpected(std::move(methods_result).error());
        llvm::LLVMTypeRef type =
            llvm::LLVMArrayType2(llvm::LLVMPointerTypeInContext(context_, 0), methods_result.value()->size() + 1);
        ordinary_vtable_types_.emplace(class_name, type);
        return type;
    }


    [[nodiscard]] std::expected<std::optional<std::size_t>, CodegenError> Codegen::ordinary_method_slot_index(const std::string& class_name, const Function& method)
{
        if (!class_has_ordinary_vtable(class_name)) return std::nullopt;
        auto methods_result = ordinary_virtual_methods(class_name);
        if (!methods_result.has_value()) return std::unexpected(std::move(methods_result).error());
        auto cache_it = ordinary_slot_indices_cache_.find(class_name);
        if (cache_it == ordinary_slot_indices_cache_.end()) return std::nullopt;
        auto slot_it = cache_it->second.find(interface_method_slot_key(method));
        if (slot_it == cache_it->second.end()) return std::nullopt;
        return slot_it->second + 1;
    }


    [[nodiscard]] llvm::LLVMValueRef Codegen::get_or_create_ordinary_destructor_thunk(const std::string& concrete_class_name)
{
        auto it = ordinary_destructor_thunks_.find(concrete_class_name);
        if (it != ordinary_destructor_thunks_.end()) return it->second;
        llvm::LLVMTypeRef ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
        llvm::LLVMTypeRef thunk_type = llvm::LLVMFunctionType(llvm::LLVMVoidTypeInContext(context_), &ptr_type, 1, /*IsVarArg=*/0);
        llvm::LLVMValueRef thunk = llvm::LLVMAddFunction(module_, ("__scpp_vtable_dtor." + concrete_class_name).c_str(), thunk_type);
        llvm::LLVMSetLinkage(thunk, llvm::LLVMPrivateLinkage);
        ordinary_destructor_thunks_.emplace(concrete_class_name, thunk);
        llvm::LLVMBasicBlockRef saved_block = llvm::LLVMGetInsertBlock(builder_);
        llvm::LLVMMetadataRef saved_dbg = llvm::LLVMGetCurrentDebugLocation2(builder_);
        llvm::LLVMBasicBlockRef entry = llvm::LLVMAppendBasicBlockInContext(context_, thunk, "entry");
        llvm::LLVMPositionBuilderAtEnd(builder_, entry);
        llvm::LLVMValueRef raw_this = llvm::LLVMGetParam(thunk, 0);
        emit_destructor_chain_calls(concrete_class_name, raw_this);
        llvm::LLVMBuildRetVoid(builder_);
        llvm::LLVMPositionBuilderAtEnd(builder_, saved_block);
        llvm::LLVMSetCurrentDebugLocation2(builder_, saved_dbg);
        return thunk;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::get_or_create_ordinary_vtable(const std::string& class_name)
{
        auto it = ordinary_vtables_.find(class_name);
        if (it != ordinary_vtables_.end()) return it->second;
        auto table_type_result = ordinary_vtable_type(class_name);
        if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
        llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
        llvm::LLVMTypeRef ptr_type = llvm::LLVMPointerTypeInContext(context_, 0);
        std::vector<llvm::LLVMValueRef> entries;
        auto methods_result = ordinary_virtual_methods(class_name);
        if (!methods_result.has_value()) return std::unexpected(std::move(methods_result).error());
        entries.reserve(methods_result.value()->size() + 1);
        entries.push_back(
            llvm::LLVMConstBitCast(get_or_create_ordinary_destructor_thunk(class_name), ptr_type));
        for (const Function* method : *methods_result.value()) {
            llvm::LLVMValueRef target_fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(method).c_str());
            if (target_fn == nullptr) {
                return std::unexpected(CodegenError("missing vtable target for ordinary virtual method '" + method->name + "'",
                                   current_loc_));
            }
            entries.push_back(llvm::LLVMConstBitCast(target_fn, ptr_type));
        }
        llvm::LLVMValueRef init = llvm::LLVMConstArray2(ptr_type, entries.data(), entries.size());
        llvm::LLVMValueRef global = llvm::LLVMAddGlobal(module_, table_type, ("__scpp_vtable." + class_name).c_str());
        llvm::LLVMSetLinkage(global, llvm::LLVMPrivateLinkage);
        llvm::LLVMSetGlobalConstant(global, /*IsConstant=*/1);
        llvm::LLVMSetInitializer(global, init);
        ordinary_vtables_.emplace(class_name, global);
        return global;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::initialize_ordinary_vtable_pointer(const std::string& class_name, llvm::LLVMValueRef object_ptr)
{
        if (!class_has_ordinary_vtable(class_name)) return {};
        const StructInfo& info = structs_.at(class_name);
        llvm::LLVMValueRef vptr_slot = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, object_ptr, 0, "vptr");
        auto vtable_result = get_or_create_ordinary_vtable(class_name);
        if (!vtable_result.has_value()) return std::unexpected(std::move(vtable_result).error());
        create_store(std::move(vtable_result).value(), vptr_slot, alignment_for_type(named_type(class_name)));
        return {};
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::interface_dispatch_entry_for(const std::string& concrete_class_name, const Function& method)
{
        if (method.name.ends_with("_delete")) {
            return get_or_create_interface_destructor_thunk(concrete_class_name, method);
        }
        auto provider_result = resolve_interface_slot_provider(concrete_class_name, interface_method_slot_key(method));
        if (!provider_result.has_value()) return std::unexpected(std::move(provider_result).error());
        const Function* provider = std::move(provider_result).value();
        if (provider == nullptr) {
            return std::unexpected(CodegenError("class '" + concrete_class_name + "' has no final overrider for interface method '" +
                               method_lookup_name(method) + "'",
                current_loc_));
        }
        if (is_interface_reference_type(provider->params.front().type)) {
            return get_or_create_interface_dispatch_thunk(concrete_class_name, *provider);
        }
        llvm::LLVMValueRef fn = llvm::LLVMGetNamedFunction(module_, overload_names_.at(provider).c_str());
        if (fn == nullptr) {
            return std::unexpected(CodegenError("missing llvm::LLVM declaration for interface dispatch target '" + provider->name + "'",
                current_loc_));
        }
        return fn;
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::get_or_create_interface_dispatch_table(const std::string& concrete_class_name,
                                                                               const std::string& interface_name)
{
        std::string cache_key = concrete_class_name + "->" + interface_name;
        auto it = interface_dispatch_tables_.find(cache_key);
        if (it != interface_dispatch_tables_.end()) return it->second;
        auto table_type_result = interface_dispatch_table_type(interface_name);
        if (!table_type_result.has_value()) return std::unexpected(std::move(table_type_result).error());
        llvm::LLVMTypeRef table_type = std::move(table_type_result).value();
        llvm::LLVMValueRef global = llvm::LLVMAddGlobal(module_, table_type, ("__scpp_iface_table." + cache_key).c_str());
        llvm::LLVMSetLinkage(global, llvm::LLVMPrivateLinkage);
        llvm::LLVMSetGlobalConstant(global, /*IsConstant=*/1);
        llvm::LLVMSetInitializer(global, llvm::LLVMConstNull(table_type));
        interface_dispatch_tables_.emplace(cache_key, global);
        std::vector<llvm::LLVMValueRef> entries;
        auto methods_result = interface_dispatch_methods(interface_name);
        if (!methods_result.has_value()) return std::unexpected(std::move(methods_result).error());
        for (const Function* method : *methods_result.value()) {
            auto entry_result = interface_dispatch_entry_for(concrete_class_name, *method);
            if (!entry_result.has_value()) return std::unexpected(std::move(entry_result).error());
            entries.push_back(std::move(entry_result).value());
        }
        llvm::LLVMSetInitializer(global, llvm::LLVMConstArray2(llvm::LLVMPointerTypeInContext(context_, 0), entries.data(), entries.size()));
        return global;
    }


    [[nodiscard]] bool Codegen::is_constructor_function(const Function& fn) const
{
        if (fn.member_owner_class.empty() || !fn.name.ends_with("_new") || fn.params.empty()) return false;
        const Type& this_param = fn.params[0].type;
        return this_param.kind == TypeKind::Reference && this_param.pointee != nullptr &&
               this_param.pointee->kind == TypeKind::Named && this_param.pointee->name == fn.member_owner_class;
    }


    [[nodiscard]] bool Codegen::is_destructor_function(const Function& fn) const
{
        if (fn.member_owner_class.empty() || !fn.name.ends_with("_delete") || fn.params.size() != 1) return false;
        const Type& this_param = fn.params[0].type;
        return this_param.kind == TypeKind::Reference && this_param.pointee != nullptr &&
               this_param.pointee->kind == TypeKind::Named && this_param.pointee->name == fn.member_owner_class;
    }


    [[nodiscard]] std::string Codegen::unqualified_template_base_name(std::string_view class_name) const
{
        std::size_t scope = class_name.rfind("::");
        std::string_view tail = scope == std::string_view::npos ? class_name : class_name.substr(scope + 2);
        std::size_t dot = tail.find('.');
        if (dot != std::string_view::npos) tail = tail.substr(0, dot);
        return std::string(tail);
    }


    [[nodiscard]] bool Codegen::names_direct_base(const std::string& member_name, const ClassDef& def) const
{
        auto base = def.direct_ordinary_base();
        if (!base.has_value() || base->get().base_type.name.empty()) return false;
        return member_name == base->get().base_type.name || member_name == unqualified_template_base_name(base->get().base_type.name);
    }


    [[nodiscard]] bool Codegen::names_base(const std::string& member_name, const BaseSpecifier& base) const
{
        return member_name == base.base_type.name || member_name == unqualified_template_base_name(base.base_type.name);
    }


    void Codegen::collect_virtual_interface_bases_in_construction_order(const ClassDef& def, std::vector<const ClassDef*>& out,
                                                               std::unordered_set<std::string>& seen) const
{
        for (const BaseSpecifier& base : def.base_specifiers) {
            const ClassDef* base_def = find_class_def(base.base_type.name);
            if (base_def == nullptr || base_def->is_forward_declaration) continue;
            collect_virtual_interface_bases_in_construction_order(*base_def, out, seen);
            if (base.kind == BaseClassKind::Interface && seen.insert(base_def->name).second) out.push_back(base_def);
        }
    }


    [[nodiscard]] std::vector<const ClassDef*> Codegen::collect_virtual_interface_bases_in_construction_order(const ClassDef& def) const
{
        std::vector<const ClassDef*> out;
        std::unordered_set<std::string> seen;
        collect_virtual_interface_bases_in_construction_order(def, out, seen);
        return out;
    }


    [[nodiscard]] const MemberInitializer* Codegen::find_explicit_interface_initializer(const Function& ctor,
                                                                               const ClassDef& interface_def) const
{
        for (const MemberInitializer& init : ctor.member_initializers) {
            if (init.member_name == interface_def.name ||
                init.member_name == unqualified_template_base_name(interface_def.name)) {
                return &init;
            }
        }
        return nullptr;
    }


    std::expected<std::vector<Codegen::SavedLocalSlot>, CodegenError>
    Codegen::bind_constructor_parameters(const Function& ctor_def, const std::vector<llvm::LLVMValueRef>& args)
{
        std::vector<SavedLocalSlot> saved;
        // params[0] is the constructor's `this`, which the construction
        // site does not supply as an argument.
        for (std::size_t i = 0; i < args.size() && i + 1 < ctor_def.params.size(); i++) {
            const Param& param = ctor_def.params[i + 1];
            if (!has_param_local(param)) {
                return std::unexpected(CodegenError(
                    "internal error: parameter '" + param.name + "' was never resolved to a local", ctor_def.loc));
            }
            LocalId id = param_local(param);
            SavedLocalSlot entry;
            entry.id = id;
            if (auto it = locals_.find(id); it != locals_.end()) {
                entry.was_bound = true;
                entry.slot = it->second;
            }
            saved.push_back(std::move(entry));
            llvm::LLVMValueRef slot = create_entry_block_alloca(llvm::LLVMTypeOf(args[i]), param.name);
            if (std::optional<unsigned> align = alignment_for_type(param.type)) llvm::LLVMSetAlignment(slot, *align);
            llvm::LLVMBuildStore(builder_, args[i], slot);
            locals_[id] = LocalSlot{slot, param.type};
        }
        return saved;
    }

    void Codegen::restore_bound_locals(const std::vector<SavedLocalSlot>& saved)
{
        for (std::size_t i = saved.size(); i > 0; i--) {
            const SavedLocalSlot& entry = saved[i - 1];
            if (entry.was_bound) {
                locals_[entry.id] = entry.slot;
            } else {
                locals_.erase(entry.id);
            }
        }
    }

    std::expected<std::vector<llvm::LLVMValueRef>, CodegenError>
    Codegen::emit_constructor_arguments_and_virtual_bases(const std::string& class_name, const Function* ctor_def,
                                                          const std::vector<ExprPtr>& args,
                                                          llvm::LLVMValueRef object_ptr)
{
        auto args_result = codegen_call_args(args, ctor_def, /*param_offset=*/1);
        if (!args_result.has_value()) return std::unexpected(std::move(args_result).error());
        std::vector<llvm::LLVMValueRef> arg_values = std::move(args_result).value();
        const ClassDef* class_def = find_class_def(class_name);
        if (class_def == nullptr) return arg_values;
        std::vector<SavedLocalSlot> saved;
        if (ctor_def != nullptr) {
            auto saved_result = bind_constructor_parameters(*ctor_def, arg_values);
            if (!saved_result.has_value()) return std::unexpected(std::move(saved_result).error());
            saved = std::move(saved_result).value();
        }
        auto init_result = emit_complete_object_interface_initializers(*class_def, ctor_def, object_ptr);
        restore_bound_locals(saved);
        if (!init_result.has_value()) return std::unexpected(std::move(init_result).error());
        return arg_values;
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_complete_object_interface_initializers(const ClassDef& most_derived_def, const Function* ctor_def,
                                                     llvm::LLVMValueRef object_ptr)
{
        static const std::vector<ExprPtr> no_base_args;
        for (const ClassDef* interface_def : collect_virtual_interface_bases_in_construction_order(most_derived_def)) {
            if (interface_def == nullptr) continue;
            const MemberInitializer* explicit_init =
                ctor_def != nullptr ? find_explicit_interface_initializer(*ctor_def, *interface_def) : nullptr;
            const std::vector<ExprPtr>* init_args =
                explicit_init != nullptr ? &explicit_init->initializer.brace_args : &no_base_args;
            const Function* base_ctor = resolve_constructor_overload_exact(interface_def->name, *init_args);
            if (base_ctor == nullptr) {
                if (explicit_init == nullptr && !class_has_any_constructor(interface_def->name)) continue;
                if (explicit_init == nullptr && init_args->empty()) {
                    return std::unexpected(CodegenError("class '" + most_derived_def.name +
                                           "' cannot be implicitly default-constructed because virtual interface base '" +
                                           interface_def->name + "' has no accessible default constructor",
                                       current_loc_));
                }
                return std::unexpected(CodegenError("base-class initializer for '" + interface_def->name +
                                       "' does not match any constructor of that class",
                                   current_loc_));
            }
            auto ctor_args_result = codegen_call_args(*init_args, base_ctor, /*param_offset=*/1);
            if (!ctor_args_result.has_value()) return std::unexpected(std::move(ctor_args_result).error());
            std::vector<llvm::LLVMValueRef> ctor_args = std::move(ctor_args_result).value();
            auto dispatch_table_result = get_or_create_interface_dispatch_table(most_derived_def.name, interface_def->name);
            if (!dispatch_table_result.has_value()) return std::unexpected(std::move(dispatch_table_result).error());
            llvm::LLVMValueRef fat_this = build_interface_value(object_ptr, std::move(dispatch_table_result).value());
            ctor_args.insert(ctor_args.begin(), fat_this);
            build_call(llvm::LLVMGetNamedFunction(module_, overload_names_.at(base_ctor).c_str()), ctor_args);
        }
        return {};
    }


    [[nodiscard]] std::expected<llvm::LLVMValueRef, CodegenError> Codegen::load_this_object_ptr()
{
        std::optional<LocalId> this_local = this_param_local();
        auto this_it = this_local.has_value() ? locals_.find(*this_local) : locals_.end();
        if (this_it == locals_.end()) {
            return std::unexpected(CodegenError("constructor/member initialization needs 'this' in scope", current_loc_));
        }
        return create_load(llvm::LLVMPointerTypeInContext(context_, 0), this_it->second.alloca, std::nullopt, "this.obj");
    }


    [[nodiscard]] std::expected<Codegen::LValue, CodegenError> Codegen::codegen_raw_member_storage(llvm::LLVMValueRef object_ptr, const std::string& class_name,
                                                    const ClassField& field)
{
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            return std::unexpected(CodegenError("unknown class '" + class_name + "'", current_loc_));
        }
        const StructInfo& info = info_it->second;
        std::optional<std::size_t> field_index = info.find_field_index(field.name);
        if (!field_index.has_value()) {
            return std::unexpected(CodegenError("class '" + class_name + "' has no field '" + field.name + "'", current_loc_));
        }
        llvm::LLVMValueRef field_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, object_ptr,
                                                     info.physical_field_index(*field_index), field.name.c_str());
        return LValue{field_ptr, field.type, alignment_for_type(field.type)};
    }


    [[nodiscard]] std::expected<Codegen::LValue, CodegenError> Codegen::codegen_raw_member_storage(llvm::LLVMValueRef object_ptr, const std::string& class_name,
                                                    const StructField& field)
{
        auto info_it = structs_.find(class_name);
        if (info_it == structs_.end()) {
            return std::unexpected(CodegenError("unknown class '" + class_name + "'", current_loc_));
        }
        const StructInfo& info = info_it->second;
        std::optional<std::size_t> field_index = info.find_field_index(field.name);
        if (!field_index.has_value()) {
            return std::unexpected(CodegenError("class '" + class_name + "' has no field '" + field.name + "'", current_loc_));
        }
        llvm::LLVMValueRef field_ptr = llvm::LLVMBuildStructGEP2(builder_, info.llvm_type, object_ptr,
                                                     info.physical_field_index(*field_index), field.name.c_str());
        return LValue{field_ptr, field.type, alignment_for_type(field.type)};
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_constructor_member_initializers(const Function& fn)
{
        if (!is_constructor_function(fn)) return {};
        const ClassDef* class_def = find_class_def(fn.member_owner_class);
        const StructDef* struct_def = class_def == nullptr ? find_struct_def(fn.member_owner_class) : nullptr;
        if (class_def == nullptr && struct_def == nullptr) {
            return std::unexpected(CodegenError("unknown constructor owner class '" + fn.member_owner_class + "'", current_loc_));
        }
        auto object_ptr_result = load_this_object_ptr();
        if (!object_ptr_result.has_value()) return std::unexpected(std::move(object_ptr_result).error());
        llvm::LLVMValueRef object_ptr = std::move(object_ptr_result).value();
        if (class_def != nullptr) {
            if (auto base = class_def->direct_ordinary_base()) {
                const MemberInitializer* explicit_base_init = nullptr;
                for (const MemberInitializer& init : fn.member_initializers) {
                    if (names_direct_base(init.member_name, *class_def)) {
                        explicit_base_init = &init;
                        break;
                    }
                }
                const ClassDef* base_def = find_class_def(base->get().base_type.name);
                if (base_def == nullptr) {
                    return std::unexpected(CodegenError("unknown base class '" + base->get().base_type.name + "'", current_loc_));
                }
                static const std::vector<ExprPtr> no_base_args;
                const std::vector<ExprPtr>* base_args =
                    explicit_base_init != nullptr ? &explicit_base_init->initializer.brace_args : nullptr;
                const Function* base_ctor =
                    resolve_constructor_overload_exact(base->get().base_type.name, base_args != nullptr ? *base_args : no_base_args);
                if (base_ctor != nullptr) {
                    auto ctor_args_result =
                        codegen_call_args(base_args != nullptr ? *base_args : no_base_args, base_ctor, /*param_offset=*/1);
                    if (!ctor_args_result.has_value()) return std::unexpected(std::move(ctor_args_result).error());
                    std::vector<llvm::LLVMValueRef> ctor_args = std::move(ctor_args_result).value();
                    ctor_args.insert(ctor_args.begin(), object_ptr);
                    build_call(llvm::LLVMGetNamedFunction(module_, overload_names_.at(base_ctor).c_str()), ctor_args);
                } else if (base_args == nullptr || base_args->empty()) {
                    if (auto default_init_result = emit_default_initializers_for_class_storage(object_ptr, *base_def,
                                                                /*initialize_virtual_interface_bases=*/false);
                        !default_init_result.has_value()) {
                        return std::unexpected(std::move(default_init_result).error());
                    }
                } else {
                    return std::unexpected(CodegenError("base-class initializer for '" + base->get().base_type.name +
                                           "' does not match any constructor of that class",
                                       current_loc_));
                }
            }
            if (auto vtable_result = initialize_ordinary_vtable_pointer(class_def->name, object_ptr); !vtable_result.has_value()) {
                return std::unexpected(std::move(vtable_result).error());
            }
            return emit_record_field_initializers(object_ptr, class_def->name, class_def->fields, &fn.member_initializers);
        }
        return emit_record_field_initializers(object_ptr, struct_def->name, struct_def->fields, &fn.member_initializers);
    }


    [[nodiscard]] bool Codegen::class_has_any_constructor(const std::string& class_name) const
{
        return std::any_of(program_->functions.begin(), program_->functions.end(),
                           [&](const Function& fn) { return is_constructor_function(fn) && fn.member_owner_class == class_name; });
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_default_initializers_for_class_storage(llvm::LLVMValueRef object_ptr, const ClassDef& class_def,
                                                    bool initialize_virtual_interface_bases)
{
        if (initialize_virtual_interface_bases) {
            if (auto init_result = emit_complete_object_interface_initializers(class_def, nullptr, object_ptr); !init_result.has_value()) {
                return std::unexpected(std::move(init_result).error());
            }
        }
        if (auto base = class_def.direct_ordinary_base()) {
            const ClassDef* base_def = find_class_def(base->get().base_type.name);
            if (base_def == nullptr) {
                return std::unexpected(CodegenError("unknown base class '" + base->get().base_type.name + "'", current_loc_));
            }
            const Function* base_ctor = resolve_constructor_overload_exact(base->get().base_type.name, {});
            if (base_ctor != nullptr) {
                build_call(llvm::LLVMGetNamedFunction(module_, overload_names_.at(base_ctor).c_str()), {object_ptr});
            } else if (!class_has_any_constructor(base->get().base_type.name)) {
                if (auto default_init_result = emit_default_initializers_for_class_storage(object_ptr, *base_def, /*initialize_virtual_interface_bases=*/false);
                    !default_init_result.has_value()) {
                    return std::unexpected(std::move(default_init_result).error());
                }
            } else {
                return std::unexpected(CodegenError("class '" + class_def.name + "' cannot be implicitly default-constructed because base class '" +
                                      base->get().base_type.name + "' has no accessible default constructor",
                                   current_loc_));
            }
        }
        if (auto vtable_result = initialize_ordinary_vtable_pointer(class_def.name, object_ptr); !vtable_result.has_value()) {
            return std::unexpected(std::move(vtable_result).error());
        }
        return emit_record_field_initializers(object_ptr, class_def.name, class_def.fields, /*member_initializers=*/nullptr);
    }


    [[nodiscard]] std::expected<void, CodegenError> Codegen::emit_default_initializers_for_record_storage(llvm::LLVMValueRef object_ptr, const std::string& type_name,
                                                     bool initialize_virtual_interface_bases)
{
        if (const ClassDef* class_def = find_class_def(type_name)) {
            return emit_default_initializers_for_class_storage(object_ptr, *class_def, initialize_virtual_interface_bases);
        }
        // A struct has no bases, no vtable and no virtual interface
        // subobjects, so its whole default-initialization is its fields
        // -- through the same field walk the class path ends with, not a
        // second copy of it.
        if (const StructDef* struct_def = find_struct_def(type_name)) {
            return emit_record_field_initializers(object_ptr, struct_def->name, struct_def->fields, /*member_initializers=*/nullptr);
        }
        return {};
    }


    [[nodiscard]] bool Codegen::record_is_implicitly_default_initializable(const std::string& type_name) const
{
        if (class_has_any_constructor(type_name)) return false;
        return find_class_def(type_name) != nullptr || find_struct_def(type_name) != nullptr;
    }

} // namespace scpp
