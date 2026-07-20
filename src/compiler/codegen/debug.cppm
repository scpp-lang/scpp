module;

// LLVM's stable llvm-c surface still defines every operation this file
// performs -- including pointer ABI alignment (see
// pointer_abi_alignment_for_as below) and every DIBuilder debug-info
// operation (compile unit/file/type/variable/subprogram construction,
// source locations) -- so this file reaches them via the imported
// scpp module's :llvm partition (libs/scpp/llvm/) instead of including
// LLVM headers directly. See libs/README.md for why this project stays
// on that LLVM-C surface wherever it already covers what's needed: a
// rigorous, function-by-function empirical audit found it fully covers
// every LLVM operation this project's codegen needs, and libs/scpp/llvm/
// now covers every one of those in turn.

module scpp.compiler.codegen:debug;

import std;
import scpp;
import :api;

namespace scpp {

namespace {

// Standard DWARF attribute-encoding values (DWARF spec, also mirrored in
// LLVM's own llvm/BinaryFormat/Dwarf.def as DW_ATE_*) -- named locally
// since llvm-c/DebugInfo.h's LLVMDWARFTypeEncoding is a bare `unsigned`
// with no accompanying named enumerators of its own, unlike
// LLVMDWARFSourceLanguage (which does have real enumerators upstream,
// spelled out below as raw literals with an explanatory comment instead,
// per libs/scpp/llvm/'s own one-off-enum-constant convention -- see
// DIBuilder::create_compile_unit_handle's own comment).
constexpr unsigned kDwAteBoolean = 0x02;
constexpr unsigned kDwAteFloat = 0x04;
constexpr unsigned kDwAteSigned = 0x05;
constexpr unsigned kDwAteSignedChar = 0x06;
constexpr unsigned kDwAteUnsigned = 0x07;

void* data_layout_ref(void* module) { return scpp::llvm::Module::data_layout_handle(module); }

// llvm::DataLayout::getPointerABIAlignment(address_space).value() has no
// function in llvm-c/Target.h with this exact shape (a data layout plus a
// bare address space), but LLVMABIAlignmentOfType() queried against an
// opaque pointer type *for that same address space* reads the exact same
// per-address-space entry in the DataLayout's own pointer-alignment
// table -- empirically confirmed identical (via a standalone LLVM-C++ vs.
// LLVM-C comparison program) across several representative data layouts
// and address spaces, including a synthetic one with an unusual, non-
// default alignment. So this composes two already-official LLVM-C calls
// instead of needing any wrapper of our own.
unsigned pointer_abi_alignment_for_as(void* module, unsigned address_space) {
    return scpp::llvm::DataLayout::abi_alignment_of_type_handle(
        scpp::llvm::Module::data_layout_handle(module),
        scpp::llvm::Type::get_pointer_handle(scpp::llvm::Module::context_handle(module), address_space));
}

} // namespace

    [[nodiscard]] std::string Codegen::default_debug_source_path() const
{
        return source_path_.empty() ? (std::filesystem::current_path() / "memory.scpp").string() : source_path_;
    }


    [[nodiscard]] void* Codegen::debug_file_for_path(const std::string& path)
{
        if (!emit_debug_info_) return nullptr;
        auto it = debug_file_cache_.find(path);
        if (it != debug_file_cache_.end()) return it->second;
        std::filesystem::path source(path);
        std::string filename = source.filename().string();
        std::string dir = source.parent_path().string();
        void* file =
            scpp::llvm::DIBuilder::create_file_handle(dibuilder_, filename.c_str(), filename.size(), dir.c_str(), dir.size());
        debug_file_cache_.emplace(path, file);
        return file;
    }


    [[nodiscard]] void* Codegen::debug_file_for_program()
{
        if (!emit_debug_info_) return nullptr;
        if (compile_unit_file_ != nullptr) return compile_unit_file_;
        compile_unit_file_ = debug_file_for_path(default_debug_source_path());
        return compile_unit_file_;
    }


    [[nodiscard]] void* Codegen::debug_file_for_loc(const SourceLocation& loc)
{
        return debug_file_for_path(loc.has_source_path() ? loc.source_path_text() : default_debug_source_path());
    }


    void Codegen::initialize_debug_info()
{
        if (!emit_debug_info_) return;
        dibuilder_ = scpp::llvm::DIBuilder::create_handle(module_);
        void* file = debug_file_for_program();
        static const char* const kProducer = "scpp";
        compile_unit_ = scpp::llvm::DIBuilder::create_compile_unit_handle(
            dibuilder_, /*lang=*/40 /* LLVMDWARFSourceLanguageC_plus_plus_17 */, file, kProducer, std::strlen(kProducer),
            /*isOptimized=*/false, "", 0, /*RuntimeVer=*/0, "", 0, /*kind=*/1 /* LLVMDWARFEmissionFull */,
            /*DWOId=*/0, /*SplitDebugInlining=*/false, /*DebugInfoForProfiling=*/false, "", 0, "", 0);
        scpp::llvm::Module::add_module_flag_handle(
            module_, /*behavior=*/1 /* LLVMModuleFlagBehaviorWarning */, "Debug Info Version",
            sizeof("Debug Info Version") - 1,
            scpp::llvm::Value::as_metadata_handle(scpp::llvm::Value::const_int_handle(
                scpp::llvm::Type::get_int32_handle(context_),
                scpp::llvm::DIBuilder::debug_metadata_version_handle(), false)));
        scpp::llvm::Module::add_module_flag_handle(
            module_, /*behavior=*/1 /* LLVMModuleFlagBehaviorWarning */, "Dwarf Version", sizeof("Dwarf Version") - 1,
            scpp::llvm::Value::as_metadata_handle(
                scpp::llvm::Value::const_int_handle(scpp::llvm::Type::get_int32_handle(context_), 5, false)));
    }


    void Codegen::finalize_debug_info()
{
        if (!emit_debug_info_ || dibuilder_ == nullptr) return;
        scpp::llvm::DIBuilder::finalize_handle(dibuilder_);
    }


    [[nodiscard]] void* Codegen::debug_type_for(const Type& type)
{
        if (!emit_debug_info_) return nullptr;
        std::string key = mangle_type(type);
        auto it = debug_type_cache_.find(key);
        if (it != debug_type_cache_.end()) return it->second;
        void* result = nullptr;
        switch (type.kind) {
            case TypeKind::Named: {
                auto basic = [&](unsigned encoding) -> void* {
                    return scpp::llvm::DIBuilder::create_basic_type_handle(
                        dibuilder_, type.name.c_str(), type.name.size(),
                        scpp::llvm::DataLayout::size_of_type_in_bits_handle(data_layout_ref(module_), to_llvm_type(type)),
                        encoding, /*flags=*/0 /* LLVMDIFlagZero */);
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
                    result = scpp::llvm::DIBuilder::create_unspecified_type_handle(dibuilder_, type.name.c_str(),
                                                                                          type.name.size());
                }
                break;
            }
            case TypeKind::Pointer:
            case TypeKind::Reference: {
                if (is_interface_representation_type(type)) {
                    const std::string name = type.kind == TypeKind::Pointer ? "interface_ptr" : "interface_ref";
                    result = scpp::llvm::DIBuilder::create_unspecified_type_handle(dibuilder_, name.c_str(), name.size());
                    break;
                }
                void* pointee = type.pointee ? debug_type_for(*type.pointee) : nullptr;
                result = scpp::llvm::DIBuilder::create_pointer_type_handle(
                    dibuilder_, pointee,
                    8ULL * scpp::llvm::DataLayout::pointer_size_handle(data_layout_ref(module_), 0),
                    static_cast<unsigned int>(pointer_abi_alignment_for_as(module_, 0) * 8), /*AddressSpace=*/0, "", 0);
                break;
            }
            case TypeKind::Array: {
                void* element = debug_type_for(*type.element);
                void* subrange = scpp::llvm::DIBuilder::get_or_create_subrange_handle(
                    dibuilder_, 0, static_cast<long>(type.array_size));
                void* subscripts = scpp::llvm::DIBuilder::get_or_create_array_handle(dibuilder_, &subrange, 1);
                void* llvm_type = to_llvm_type(type);
                result = scpp::llvm::DIBuilder::create_array_type_handle(
                    dibuilder_, scpp::llvm::DataLayout::size_of_type_in_bits_handle(data_layout_ref(module_), llvm_type),
                    static_cast<unsigned int>(
                        scpp::llvm::DataLayout::abi_alignment_of_type_handle(data_layout_ref(module_), llvm_type) * 8),
                    element, &subscripts, 1);
                break;
            }
            case TypeKind::Span: {
                static const char* const kName = "std::span";
                result = scpp::llvm::DIBuilder::create_unspecified_type_handle(dibuilder_, kName, std::strlen(kName));
                break;
            }
            case TypeKind::Function:
            case TypeKind::FunctionPointer: {
                std::vector<void*> elems;
                elems.push_back(type.function_return ? debug_type_for(*type.function_return) : nullptr);
                for (const Type& param : type.function_params) elems.push_back(debug_type_for(param));
                void* subroutine = scpp::llvm::DIBuilder::create_subroutine_type_handle(
                    dibuilder_, nullptr, elems.data(), static_cast<unsigned>(elems.size()), /*flags=*/0 /* LLVMDIFlagZero */);
                result = type.kind == TypeKind::FunctionPointer
                             ? scpp::llvm::DIBuilder::create_pointer_type_handle(
                                   dibuilder_, subroutine,
                                   8ULL * scpp::llvm::DataLayout::pointer_size_handle(data_layout_ref(module_), 0),
                                   static_cast<unsigned int>(pointer_abi_alignment_for_as(module_, 0) * 8),
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
            scpp::llvm::Builder::set_current_debug_location_handle(builder_, nullptr);
            return;
        }
        scpp::llvm::Builder::set_current_debug_location_handle(
            builder_, scpp::llvm::DIBuilder::create_debug_location_handle(
                          context_, loc.line, std::max(loc.column, 1), current_debug_scope_, nullptr));
    }


    void Codegen::maybe_emit_parameter_debug_decl(const Param& param, void* slot, unsigned index)
{
        if (!emit_debug_info_ || current_subprogram_ == nullptr) return;
        void* type = debug_type_for(param.type);
        if (type == nullptr) return;
        void* var = scpp::llvm::DIBuilder::create_parameter_variable_handle(
            dibuilder_, current_subprogram_, param.name.c_str(), param.name.size(), index,
            debug_file_for_loc(current_function_def_->loc), std::max(current_function_def_->loc.line, 1), type,
            /*AlwaysPreserve=*/true, /*flags=*/0 /* LLVMDIFlagZero */);
        void* expr = scpp::llvm::DIBuilder::create_expression_handle(dibuilder_, nullptr, 0);
        void* loc = scpp::llvm::DIBuilder::create_debug_location_handle(
            context_, std::max(current_function_def_->loc.line, 1), 1, current_subprogram_, nullptr);
        scpp::llvm::DIBuilder::insert_declare_record_at_end_handle(
            dibuilder_, slot, var, expr, loc, scpp::llvm::Builder::insert_block_handle(builder_));
    }


    void Codegen::maybe_emit_local_debug_decl(const std::string& name, const Type& type, void* slot, SourceLocation loc)
{
        if (!emit_debug_info_ || current_debug_scope_ == nullptr) return;
        void* debug_type = debug_type_for(type);
        if (debug_type == nullptr) return;
        void* var = scpp::llvm::DIBuilder::create_auto_variable_handle(
            dibuilder_, current_debug_scope_, name.c_str(), name.size(), debug_file_for_loc(loc),
            std::max(loc.line, 1), debug_type,
            /*AlwaysPreserve=*/true, /*flags=*/0 /* LLVMDIFlagZero */, /*AlignInBits=*/0);
        void* expr = scpp::llvm::DIBuilder::create_expression_handle(dibuilder_, nullptr, 0);
        void* debug_loc = scpp::llvm::DIBuilder::create_debug_location_handle(
            context_, std::max(loc.line, 1), std::max(loc.column, 1), current_debug_scope_, nullptr);
        scpp::llvm::DIBuilder::insert_declare_record_at_end_handle(
            dibuilder_, slot, var, expr, debug_loc, scpp::llvm::Builder::insert_block_handle(builder_));
    }


    void* Codegen::create_entry_block_alloca(void* type, const std::string& name,
                                                std::optional<unsigned> alignment)
{
        void* current_block = scpp::llvm::Builder::insert_block_handle(builder_);
        if (current_block == nullptr) {
            void* slot = scpp::llvm::Builder::alloca(builder_, type, name.c_str());
            if (alignment.has_value()) scpp::llvm::Value::set_alignment_handle(slot, *alignment);
            return slot;
        }
        // Every real call site reaches this function with the builder
        // positioned at the end of whatever block it's currently
        // building (this function is the only place that ever
        // temporarily repositions the builder mid-block, always
        // restoring before returning), so saving just the current block
        // (rather than a full IRBuilderBase::InsertPoint, which
        // llvm-c has no equivalent handle for) and restoring via
        // Builder::position_at_end_handle is equivalent here.
        void* saved_block = current_block;
        void* saved_dbg = scpp::llvm::Builder::current_debug_location_handle(builder_);
        void* entry =
            scpp::llvm::Function::entry_block_handle(scpp::llvm::BasicBlock::parent_handle(current_block));
        // Entry blocks have no predecessors, hence no PHI/landingpad
        // instructions -- so "first insertion point" degenerates to
        // simply the first instruction (or none, if empty), matching
        // getFirstInsertionPt()'s general PHI/landingpad-skipping
        // behavior for this specific (entry-block-only) use.
        void* insert_before = scpp::llvm::BasicBlock::first_instruction_handle(entry);
        while (insert_before != nullptr && scpp::llvm::Value::is_alloca_inst_handle(insert_before) != nullptr) {
            insert_before = scpp::llvm::Value::next_instruction_handle(insert_before);
        }
        if (insert_before != nullptr) {
            scpp::llvm::Builder::position_before_handle(builder_, insert_before);
        } else {
            scpp::llvm::Builder::position_at_end_handle(builder_, entry);
        }
        scpp::llvm::Builder::set_current_debug_location_handle(builder_, nullptr);
        void* slot = scpp::llvm::Builder::alloca(builder_, type, name.c_str());
        if (alignment.has_value()) scpp::llvm::Value::set_alignment_handle(slot, *alignment);
        scpp::llvm::Builder::position_at_end_handle(builder_, saved_block);
        scpp::llvm::Builder::set_current_debug_location_handle(builder_, saved_dbg);
        return slot;
    }


    void Codegen::attach_debug_subprogram(void* llvm_fn, const Function& fn)
{
        if (!emit_debug_info_) return;
        std::vector<void*> type_elems;
        type_elems.push_back(debug_type_for(fn.return_type));
        for (const Param& param : fn.params) type_elems.push_back(debug_type_for(param.type));
        void* fn_type = scpp::llvm::DIBuilder::create_subroutine_type_handle(
            dibuilder_, nullptr, type_elems.data(), static_cast<unsigned>(type_elems.size()), /*flags=*/0 /* LLVMDIFlagZero */);
        void* file = debug_file_for_loc(fn.loc);
        std::size_t linkage_name_len = 0;
        const char* linkage_name = scpp::llvm::Value::name_of_handle(llvm_fn, &linkage_name_len);
        void* subprogram = scpp::llvm::DIBuilder::create_function_handle(
            dibuilder_, file, fn.name.c_str(), fn.name.size(), linkage_name, linkage_name_len, file,
            std::max(fn.loc.line, 1), fn_type, /*IsLocalToUnit=*/false, /*IsDefinition=*/true,
            std::max(fn.loc.line, 1), /*flags=*/256 /* LLVMDIFlagPrototyped */, /*IsOptimized=*/false);
        scpp::llvm::Function::set_subprogram_handle(llvm_fn, subprogram);
        current_subprogram_ = subprogram;
        current_debug_scope_ = subprogram;
    }

} // namespace scpp
