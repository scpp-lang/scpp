module;

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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


module scpp.compiler.codegen:debug;

import :api;

namespace scpp {

    [[nodiscard]] std::string Codegen::default_debug_source_path() const
{
        return source_path_.empty() ? (std::filesystem::current_path() / "memory.scpp").string() : source_path_;
    }


    [[nodiscard]] llvm::DIFile* Codegen::debug_file_for_path(const std::string& path)
{
        if (!emit_debug_info_) return nullptr;
        auto it = debug_file_cache_.find(path);
        if (it != debug_file_cache_.end()) return it->second;
        std::filesystem::path source(path);
        llvm::DIFile* file = dibuilder_->createFile(source.filename().string(), source.parent_path().string());
        debug_file_cache_.emplace(path, file);
        return file;
    }


    [[nodiscard]] llvm::DIFile* Codegen::debug_file_for_program()
{
        if (!emit_debug_info_) return nullptr;
        if (compile_unit_file_ != nullptr) return compile_unit_file_;
        compile_unit_file_ = debug_file_for_path(default_debug_source_path());
        return compile_unit_file_;
    }


    [[nodiscard]] llvm::DIFile* Codegen::debug_file_for_loc(const SourceLocation& loc)
{
        return debug_file_for_path(loc.has_source_path() ? loc.source_path_text() : default_debug_source_path());
    }


    void Codegen::initialize_debug_info()
{
        if (!emit_debug_info_) return;
        dibuilder_ = std::make_unique<llvm::DIBuilder>(*module_);
        llvm::DIFile* file = debug_file_for_program();
        compile_unit_ = dibuilder_->createCompileUnit(llvm::dwarf::DW_LANG_C_plus_plus_17, file, "scpp", false, "", 0,
                                                      "", llvm::DICompileUnit::FullDebug);
        module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version", llvm::DEBUG_METADATA_VERSION);
        module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 5);
    }


    void Codegen::finalize_debug_info()
{
        if (!emit_debug_info_ || !dibuilder_) return;
        dibuilder_->finalize();
    }


    [[nodiscard]] llvm::DIType* Codegen::debug_type_for(const Type& type)
{
        if (!emit_debug_info_) return nullptr;
        std::string key = mangle_type(type);
        auto it = debug_type_cache_.find(key);
        if (it != debug_type_cache_.end()) return it->second;
        llvm::DIType* result = nullptr;
        switch (type.kind) {
            case TypeKind::Named: {
                auto basic = [&](llvm::dwarf::TypeKind encoding) -> llvm::DIType* {
                    return dibuilder_->createBasicType(type.name, module_->getDataLayout().getTypeSizeInBits(to_llvm_type(type)),
                                                       encoding);
                };
                if (type.name == "bool") result = basic(llvm::dwarf::DW_ATE_boolean);
                else if (type.name == "char") result = basic(llvm::dwarf::DW_ATE_signed_char);
                else if (is_float_scalar_type_name(type.name)) result = basic(llvm::dwarf::DW_ATE_float);
                else if (is_scalar_type_name(type.name)) {
                    result = basic(is_unsigned_scalar_type_name(type.name) ? llvm::dwarf::DW_ATE_unsigned
                                                                            : llvm::dwarf::DW_ATE_signed);
                } else if (const EnumDef* enum_def = find_enum_def(program_, type.name)) {
                    const std::string& underlying = enum_def->underlying_type.name;
                    result = basic(underlying == "char"                ? llvm::dwarf::DW_ATE_signed_char
                                   : is_unsigned_scalar_type_name(underlying) ? llvm::dwarf::DW_ATE_unsigned
                                                                               : llvm::dwarf::DW_ATE_signed);
                } else {
                    result = dibuilder_->createUnspecifiedType(type.name);
                }
                break;
            }
            case TypeKind::Pointer:
            case TypeKind::Reference: {
                if (is_interface_representation_type(type)) {
                    result = dibuilder_->createUnspecifiedType(type.kind == TypeKind::Pointer ? "interface_ptr"
                                                                                             : "interface_ref");
                    break;
                }
                llvm::DIType* pointee = type.pointee ? debug_type_for(*type.pointee) : nullptr;
                result = dibuilder_->createPointerType(
                    pointee, module_->getDataLayout().getPointerSizeInBits(),
                    module_->getDataLayout().getPointerABIAlignment(0).value() * 8);
                break;
            }
            case TypeKind::Array: {
                llvm::DIType* element = debug_type_for(*type.element);
                auto subscripts = dibuilder_->getOrCreateArray(
                    {dibuilder_->getOrCreateSubrange(0, type.array_size)});
                llvm::Type* llvm_type = to_llvm_type(type);
                result = dibuilder_->createArrayType(module_->getDataLayout().getTypeSizeInBits(llvm_type),
                                                     module_->getDataLayout().getABITypeAlign(llvm_type).value() * 8,
                                                     element, subscripts);
                break;
            }
            case TypeKind::Span:
                result = dibuilder_->createUnspecifiedType("std::span");
                break;
            case TypeKind::Function:
            case TypeKind::FunctionPointer: {
                std::vector<llvm::Metadata*> elems;
                elems.push_back(type.function_return ? debug_type_for(*type.function_return) : nullptr);
                for (const Type& param : type.function_params) elems.push_back(debug_type_for(param));
                llvm::DISubroutineType* subroutine =
                    dibuilder_->createSubroutineType(dibuilder_->getOrCreateTypeArray(elems));
                result = type.kind == TypeKind::FunctionPointer
                             ? dibuilder_->createPointerType(subroutine, module_->getDataLayout().getPointerSizeInBits(),
                                                            module_->getDataLayout().getPointerABIAlignment(0).value() * 8)
                             : static_cast<llvm::DIType*>(subroutine);
                break;
            }
        }
        debug_type_cache_[key] = result;
        return result;
    }


    void Codegen::refresh_debug_location(SourceLocation loc)
{
        current_loc_ = loc;
        if (!emit_debug_info_ || current_debug_scope_ == nullptr || !loc.is_known()) {
            builder_->SetCurrentDebugLocation(llvm::DebugLoc());
            return;
        }
        builder_->SetCurrentDebugLocation(llvm::DILocation::get(*context_, loc.line, std::max(loc.column, 1),
                                                                current_debug_scope_));
    }


    void Codegen::maybe_emit_parameter_debug_decl(const Param& param, llvm::AllocaInst* slot, unsigned index)
{
        if (!emit_debug_info_ || current_subprogram_ == nullptr) return;
        llvm::DIType* type = debug_type_for(param.type);
        if (type == nullptr) return;
        llvm::DILocalVariable* var =
            dibuilder_->createParameterVariable(current_subprogram_, param.name, index, debug_file_for_loc(current_function_def_->loc),
                                                std::max(current_function_def_->loc.line, 1), type, true);
        dibuilder_->insertDeclare(slot, var, dibuilder_->createExpression(),
                                  llvm::DILocation::get(*context_, std::max(current_function_def_->loc.line, 1), 1,
                                                        current_subprogram_),
                                  builder_->GetInsertBlock());
    }


    void Codegen::maybe_emit_local_debug_decl(const std::string& name, const Type& type, llvm::AllocaInst* slot, SourceLocation loc)
{
        if (!emit_debug_info_ || current_debug_scope_ == nullptr) return;
        llvm::DIType* debug_type = debug_type_for(type);
        if (debug_type == nullptr) return;
        llvm::DILocalVariable* var =
            dibuilder_->createAutoVariable(current_debug_scope_, name, debug_file_for_loc(loc), std::max(loc.line, 1),
                                           debug_type, true);
        dibuilder_->insertDeclare(slot, var, dibuilder_->createExpression(),
                                  llvm::DILocation::get(*context_, std::max(loc.line, 1), std::max(loc.column, 1),
                                                        current_debug_scope_),
                                  builder_->GetInsertBlock());
    }


    llvm::AllocaInst* Codegen::create_entry_block_alloca(llvm::Type* type, const std::string& name,
                                                std::optional<llvm::Align> alignment)
{
        llvm::BasicBlock* current_block = builder_->GetInsertBlock();
        if (current_block == nullptr) {
            llvm::AllocaInst* slot = builder_->CreateAlloca(type, nullptr, name);
            if (alignment.has_value()) slot->setAlignment(*alignment);
            return slot;
        }
        llvm::IRBuilderBase::InsertPoint saved_ip = builder_->saveIP();
        llvm::DebugLoc saved_dbg = builder_->getCurrentDebugLocation();
        llvm::BasicBlock& entry = current_block->getParent()->getEntryBlock();
        llvm::BasicBlock::iterator insert_it = entry.getFirstInsertionPt();
        while (insert_it != entry.end() && llvm::isa<llvm::AllocaInst>(*insert_it)) ++insert_it;
        builder_->SetInsertPoint(&entry, insert_it);
        builder_->SetCurrentDebugLocation(llvm::DebugLoc());
        llvm::AllocaInst* slot = builder_->CreateAlloca(type, nullptr, name);
        if (alignment.has_value()) slot->setAlignment(*alignment);
        builder_->restoreIP(saved_ip);
        builder_->SetCurrentDebugLocation(saved_dbg);
        return slot;
    }


    void Codegen::attach_debug_subprogram(llvm::Function* llvm_fn, const Function& fn)
{
        if (!emit_debug_info_) return;
        std::vector<llvm::Metadata*> type_elems;
        type_elems.push_back(debug_type_for(fn.return_type));
        for (const Param& param : fn.params) type_elems.push_back(debug_type_for(param.type));
        llvm::DISubroutineType* fn_type =
            dibuilder_->createSubroutineType(dibuilder_->getOrCreateTypeArray(type_elems));
        llvm::DISubprogram* subprogram = dibuilder_->createFunction(
            debug_file_for_loc(fn.loc), fn.name, llvm_fn->getName(), debug_file_for_loc(fn.loc),
            std::max(fn.loc.line, 1), fn_type, std::max(fn.loc.line, 1),
            llvm::DINode::FlagPrototyped, llvm::DISubprogram::SPFlagDefinition);
        llvm_fn->setSubprogram(subprogram);
        current_subprogram_ = subprogram;
        current_debug_scope_ = subprogram;
    }

} // namespace scpp
