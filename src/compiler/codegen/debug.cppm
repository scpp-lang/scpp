module;

// Official llvm::LLVM-C (llvm-c/*.h) is itself already a stable, extern "C"
// interface -- the DataLayout numeric queries in this file go through
// module `llvm`'s own `:target` partition's functions directly below
// instead of llvm::DataLayout, so this file no longer needs the heavier
// <llvm/IR/DataLayout.h> or any other native llvm::LLVM C++ header. See
// libs/README.md for why this project binds straight to llvm::LLVM-C wherever
// it already covers what's needed -- including pointer ABI alignment (see
// pointer_abi_alignment_for_as below) and every DIBuilder debug-info
// operation this file performs (from the same module's `:debug_info`
// partition; Core.h's own functions come from its `:core` partition
// instead), all reached via the single `import llvm;` below (module
// `llvm` re-exports every partition, see libs/llvm/llvm.cpp): a rigorous,
// function-by-function empirical audit found llvm::LLVM-C fully covers every
// llvm::LLVM operation this project's codegen needs, so there is no custom
// wrapper of any kind here.

module scpp.compiler.codegen:debug;

import std;
import llvm;
import :api;

namespace scpp {

namespace {

// Standard DWARF attribute-encoding values (DWARF spec, also mirrored in
// llvm::LLVM's own llvm/BinaryFormat/Dwarf.def as DW_ATE_*) -- named locally
// since llvm-c/DebugInfo.h's llvm::LLVMDWARFTypeEncoding is a bare `unsigned`
// with no accompanying named enumerators of its own, unlike
// llvm::LLVMDWARFSourceLanguage (which does have real enumerators, used
// directly below in initialize_debug_info).
constexpr unsigned kDwAteBoolean = 0x02;
constexpr unsigned kDwAteFloat = 0x04;
constexpr unsigned kDwAteSigned = 0x05;
constexpr unsigned kDwAteSignedChar = 0x06;
constexpr unsigned kDwAteUnsigned = 0x07;

llvm::LLVMTargetDataRef data_layout_ref(llvm::LLVMModuleRef module) { return llvm::LLVMGetModuleDataLayout(module); }

// llvm::DataLayout::getPointerABIAlignment(address_space).value() has no
// function in llvm-c/Target.h with this exact shape (a data layout plus a
// bare address space), but llvm::LLVMABIAlignmentOfType() queried against an
// opaque pointer type *for that same address space* reads the exact same
// per-address-space entry in the DataLayout's own pointer-alignment
// table -- empirically confirmed identical (via a standalone llvm::LLVM-C++ vs.
// llvm::LLVM-C comparison program) across several representative data layouts
// and address spaces, including a synthetic one with an unusual, non-
// default alignment. So this composes two already-official llvm::LLVM-C calls
// instead of needing any wrapper of our own.
unsigned pointer_abi_alignment_for_as(llvm::LLVMModuleRef module, unsigned address_space) {
    return llvm::LLVMABIAlignmentOfType(llvm::LLVMGetModuleDataLayout(module),
                                  llvm::LLVMPointerTypeInContext(llvm::LLVMGetModuleContext(module), address_space));
}

} // namespace

    [[nodiscard]] std::string Codegen::default_debug_source_path() const
{
        return source_path_.empty() ? (std::filesystem::current_path() / "memory.scpp").string() : source_path_;
    }


    [[nodiscard]] llvm::LLVMMetadataRef Codegen::debug_file_for_path(const std::string& path)
{
        if (!emit_debug_info_) return nullptr;
        auto it = debug_file_cache_.find(path);
        if (it != debug_file_cache_.end()) return it->second;
        std::filesystem::path source(path);
        std::string filename = source.filename().string();
        std::string dir = source.parent_path().string();
        llvm::LLVMMetadataRef file = llvm::LLVMDIBuilderCreateFile(dibuilder_, filename.c_str(), filename.size(), dir.c_str(), dir.size());
        debug_file_cache_.emplace(path, file);
        return file;
    }


    [[nodiscard]] llvm::LLVMMetadataRef Codegen::debug_file_for_program()
{
        if (!emit_debug_info_) return nullptr;
        if (compile_unit_file_ != nullptr) return compile_unit_file_;
        compile_unit_file_ = debug_file_for_path(default_debug_source_path());
        return compile_unit_file_;
    }


    [[nodiscard]] llvm::LLVMMetadataRef Codegen::debug_file_for_loc(const SourceLocation& loc)
{
        return debug_file_for_path(loc.has_source_path() ? loc.source_path_text() : default_debug_source_path());
    }


    void Codegen::initialize_debug_info()
{
        if (!emit_debug_info_) return;
        dibuilder_ = llvm::LLVMCreateDIBuilder(module_);
        llvm::LLVMMetadataRef file = debug_file_for_program();
        static const char* const kProducer = "scpp";
        compile_unit_ = llvm::LLVMDIBuilderCreateCompileUnit(
            dibuilder_, llvm::LLVMDWARFSourceLanguageC_plus_plus_17, file, kProducer, std::strlen(kProducer),
            /*isOptimized=*/0, "", 0, /*RuntimeVer=*/0, "", 0, llvm::LLVMDWARFEmissionFull, /*DWOId=*/0,
            /*SplitDebugInlining=*/0, /*DebugInfoForProfiling=*/0, "", 0, "", 0);
        llvm::LLVMAddModuleFlag(module_, llvm::LLVMModuleFlagBehaviorWarning, "Debug Info Version", sizeof("Debug Info Version") - 1,
                          llvm::LLVMValueAsMetadata(llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), llvm::LLVMDebugMetadataVersion(), 0)));
        llvm::LLVMAddModuleFlag(module_, llvm::LLVMModuleFlagBehaviorWarning, "Dwarf Version", sizeof("Dwarf Version") - 1,
                          llvm::LLVMValueAsMetadata(llvm::LLVMConstInt(llvm::LLVMInt32TypeInContext(context_), 5, 0)));
    }


    void Codegen::finalize_debug_info()
{
        if (!emit_debug_info_ || dibuilder_ == nullptr) return;
        llvm::LLVMDIBuilderFinalize(dibuilder_);
    }


    [[nodiscard]] llvm::LLVMMetadataRef Codegen::debug_type_for(const Type& type)
{
        if (!emit_debug_info_) return nullptr;
        std::string key = mangle_type(type);
        auto it = debug_type_cache_.find(key);
        if (it != debug_type_cache_.end()) return it->second;
        llvm::LLVMMetadataRef result = nullptr;
        switch (type.kind) {
            case TypeKind::Named: {
                auto basic = [&](llvm::LLVMDWARFTypeEncoding encoding) -> llvm::LLVMMetadataRef {
                    return llvm::LLVMDIBuilderCreateBasicType(dibuilder_, type.name.c_str(), type.name.size(),
                        llvm::LLVMSizeOfTypeInBits(data_layout_ref(module_), to_llvm_type(type)), encoding, llvm::LLVMDIFlagZero);
                };
                if (type.name == "bool") result = basic(kDwAteBoolean);
                else if (type.name == "char") result = basic(kDwAteSignedChar);
                else if (is_float_scalar_type_name(type.name)) result = basic(kDwAteFloat);
                else if (is_scalar_type_name(type.name)) {
                    result = basic(is_unsigned_scalar_type_name(type.name) ? kDwAteUnsigned : kDwAteSigned);
                } else if (const EnumDef* enum_def = find_enum_def(program_, type.name)) {
                    const std::string& underlying = enum_def->underlying_type.name;
                    result = basic(underlying == "char"                ? kDwAteSignedChar
                                   : is_unsigned_scalar_type_name(underlying) ? kDwAteUnsigned
                                                                                : kDwAteSigned);
                } else {
                    result = llvm::LLVMDIBuilderCreateUnspecifiedType(dibuilder_, type.name.c_str(), type.name.size());
                }
                break;
            }
            case TypeKind::Pointer:
            case TypeKind::Reference: {
                if (is_interface_representation_type(type)) {
                    const std::string name = type.kind == TypeKind::Pointer ? "interface_ptr" : "interface_ref";
                    result = llvm::LLVMDIBuilderCreateUnspecifiedType(dibuilder_, name.c_str(), name.size());
                    break;
                }
                llvm::LLVMMetadataRef pointee = type.pointee ? debug_type_for(*type.pointee) : nullptr;
                result = llvm::LLVMDIBuilderCreatePointerType(
                    dibuilder_, pointee, 8ULL * llvm::LLVMPointerSizeForAS(data_layout_ref(module_), 0),
                    static_cast<std::uint32_t>(pointer_abi_alignment_for_as(module_, 0) * 8), /*AddressSpace=*/0, "",
                    0);
                break;
            }
            case TypeKind::Array: {
                llvm::LLVMMetadataRef element = debug_type_for(*type.element);
                llvm::LLVMMetadataRef subrange =
                    llvm::LLVMDIBuilderGetOrCreateSubrange(dibuilder_, 0, static_cast<std::int64_t>(type.array_size));
                llvm::LLVMMetadataRef subscripts = llvm::LLVMDIBuilderGetOrCreateArray(dibuilder_, &subrange, 1);
                llvm::LLVMTypeRef llvm_type = to_llvm_type(type);
                result = llvm::LLVMDIBuilderCreateArrayType(
                    dibuilder_, llvm::LLVMSizeOfTypeInBits(data_layout_ref(module_), llvm_type),
                    static_cast<std::uint32_t>(llvm::LLVMABIAlignmentOfType(data_layout_ref(module_), llvm_type) * 8),
                    element, &subscripts, 1);
                break;
            }
            case TypeKind::Span: {
                static const char* const kName = "std::span";
                result = llvm::LLVMDIBuilderCreateUnspecifiedType(dibuilder_, kName, std::strlen(kName));
                break;
            }
            case TypeKind::Function:
            case TypeKind::FunctionPointer: {
                std::vector<llvm::LLVMMetadataRef> elems;
                elems.push_back(type.function_return ? debug_type_for(*type.function_return) : nullptr);
                for (const Type& param : type.function_params) elems.push_back(debug_type_for(param));
                llvm::LLVMMetadataRef subroutine = llvm::LLVMDIBuilderCreateSubroutineType(
                    dibuilder_, nullptr, elems.data(), static_cast<unsigned>(elems.size()), llvm::LLVMDIFlagZero);
                result = type.kind == TypeKind::FunctionPointer
                             ? llvm::LLVMDIBuilderCreatePointerType(
                                   dibuilder_, subroutine, 8ULL * llvm::LLVMPointerSizeForAS(data_layout_ref(module_), 0),
                                   static_cast<std::uint32_t>(pointer_abi_alignment_for_as(module_, 0) * 8),
                                   /*AddressSpace=*/0, "", 0)
                             : subroutine;
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
            llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
            return;
        }
        llvm::LLVMSetCurrentDebugLocation2(builder_, llvm::LLVMDIBuilderCreateDebugLocation(context_, loc.line, std::max(loc.column, 1),
                                                                                current_debug_scope_, nullptr));
    }


    void Codegen::maybe_emit_parameter_debug_decl(const Param& param, llvm::LLVMValueRef slot, unsigned index)
{
        if (!emit_debug_info_ || current_subprogram_ == nullptr) return;
        llvm::LLVMMetadataRef type = debug_type_for(param.type);
        if (type == nullptr) return;
        llvm::LLVMMetadataRef var = llvm::LLVMDIBuilderCreateParameterVariable(
            dibuilder_, current_subprogram_, param.name.c_str(), param.name.size(), index,
            debug_file_for_loc(current_function_def_->loc), std::max(current_function_def_->loc.line, 1), type,
            /*AlwaysPreserve=*/1, llvm::LLVMDIFlagZero);
        llvm::LLVMMetadataRef expr = llvm::LLVMDIBuilderCreateExpression(dibuilder_, nullptr, 0);
        llvm::LLVMMetadataRef loc = llvm::LLVMDIBuilderCreateDebugLocation(context_, std::max(current_function_def_->loc.line, 1), 1,
                                                               current_subprogram_, nullptr);
        llvm::LLVMDIBuilderInsertDeclareRecordAtEnd(dibuilder_, slot, var, expr, loc, llvm::LLVMGetInsertBlock(builder_));
    }


    void Codegen::maybe_emit_local_debug_decl(const std::string& name, const Type& type, llvm::LLVMValueRef slot, SourceLocation loc)
{
        if (!emit_debug_info_ || current_debug_scope_ == nullptr) return;
        llvm::LLVMMetadataRef debug_type = debug_type_for(type);
        if (debug_type == nullptr) return;
        llvm::LLVMMetadataRef var = llvm::LLVMDIBuilderCreateAutoVariable(dibuilder_, current_debug_scope_, name.c_str(), name.size(),
                                                              debug_file_for_loc(loc), std::max(loc.line, 1), debug_type,
                                                              /*AlwaysPreserve=*/1, llvm::LLVMDIFlagZero, /*AlignInBits=*/0);
        llvm::LLVMMetadataRef expr = llvm::LLVMDIBuilderCreateExpression(dibuilder_, nullptr, 0);
        llvm::LLVMMetadataRef debug_loc = llvm::LLVMDIBuilderCreateDebugLocation(context_, std::max(loc.line, 1), std::max(loc.column, 1),
                                                                     current_debug_scope_, nullptr);
        llvm::LLVMDIBuilderInsertDeclareRecordAtEnd(dibuilder_, slot, var, expr, debug_loc, llvm::LLVMGetInsertBlock(builder_));
    }


    llvm::LLVMValueRef Codegen::create_entry_block_alloca(llvm::LLVMTypeRef type, const std::string& name,
                                                std::optional<unsigned> alignment)
{
        llvm::LLVMBasicBlockRef current_block = llvm::LLVMGetInsertBlock(builder_);
        if (current_block == nullptr) {
            llvm::LLVMValueRef slot = llvm::LLVMBuildAlloca(builder_, type, name.c_str());
            if (alignment.has_value()) llvm::LLVMSetAlignment(slot, *alignment);
            return slot;
        }
        // Every real call site reaches this function with the builder
        // positioned at the end of whatever block it's currently
        // building (this function is the only place that ever
        // temporarily repositions the builder mid-block, always
        // restoring before returning), so saving just the current block
        // (rather than a full IRBuilderBase::InsertPoint, which
        // llvm-c has no equivalent handle for) and restoring via
        // llvm::LLVMPositionBuilderAtEnd is equivalent here.
        llvm::LLVMBasicBlockRef saved_block = current_block;
        llvm::LLVMMetadataRef saved_dbg = llvm::LLVMGetCurrentDebugLocation2(builder_);
        llvm::LLVMBasicBlockRef entry = llvm::LLVMGetEntryBasicBlock(llvm::LLVMGetBasicBlockParent(current_block));
        // Entry blocks have no predecessors, hence no PHI/landingpad
        // instructions -- so "first insertion point" degenerates to
        // simply the first instruction (or none, if empty), matching
        // getFirstInsertionPt()'s general PHI/landingpad-skipping
        // behavior for this specific (entry-block-only) use.
        llvm::LLVMValueRef insert_before = llvm::LLVMGetFirstInstruction(entry);
        while (insert_before != nullptr && llvm::LLVMIsAAllocaInst(insert_before) != nullptr) {
            insert_before = llvm::LLVMGetNextInstruction(insert_before);
        }
        if (insert_before != nullptr) {
            llvm::LLVMPositionBuilderBefore(builder_, insert_before);
        } else {
            llvm::LLVMPositionBuilderAtEnd(builder_, entry);
        }
        llvm::LLVMSetCurrentDebugLocation2(builder_, nullptr);
        llvm::LLVMValueRef slot = llvm::LLVMBuildAlloca(builder_, type, name.c_str());
        if (alignment.has_value()) llvm::LLVMSetAlignment(slot, *alignment);
        llvm::LLVMPositionBuilderAtEnd(builder_, saved_block);
        llvm::LLVMSetCurrentDebugLocation2(builder_, saved_dbg);
        return slot;
    }


    void Codegen::attach_debug_subprogram(llvm::LLVMValueRef llvm_fn, const Function& fn)
{
        if (!emit_debug_info_) return;
        std::vector<llvm::LLVMMetadataRef> type_elems;
        type_elems.push_back(debug_type_for(fn.return_type));
        for (const Param& param : fn.params) type_elems.push_back(debug_type_for(param.type));
        llvm::LLVMMetadataRef fn_type = llvm::LLVMDIBuilderCreateSubroutineType(
            dibuilder_, nullptr, type_elems.data(), static_cast<unsigned>(type_elems.size()), llvm::LLVMDIFlagZero);
        llvm::LLVMMetadataRef file = debug_file_for_loc(fn.loc);
        std::size_t linkage_name_len = 0;
        const char* linkage_name = llvm::LLVMGetValueName2(llvm_fn, &linkage_name_len);
        llvm::LLVMMetadataRef subprogram = llvm::LLVMDIBuilderCreateFunction(
            dibuilder_, file, fn.name.c_str(), fn.name.size(), linkage_name, linkage_name_len, file,
            std::max(fn.loc.line, 1), fn_type, /*IsLocalToUnit=*/0, /*IsDefinition=*/1, std::max(fn.loc.line, 1),
            llvm::LLVMDIFlagPrototyped, /*IsOptimized=*/0);
        llvm::LLVMSetSubprogram(llvm_fn, subprogram);
        current_subprogram_ = subprogram;
        current_debug_scope_ = subprogram;
    }

} // namespace scpp
