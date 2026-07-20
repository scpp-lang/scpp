add_library(scpp_codegen)
target_sources(scpp_codegen
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES FILES
            codegen.cppm
            errors.cppm
            api.cppm
            orchestration.cppm
            debug.cppm
            semantics.cppm
            layout.cppm
            object_model.cppm
            functions.cppm
            expressions.cppm
            lifetime.cppm
            statements.cppm)
target_include_directories(scpp_codegen SYSTEM PUBLIC ${LLVM_INCLUDE_DIRS})
target_compile_definitions(scpp_codegen PUBLIC ${LLVM_DEFINITIONS_LIST})
# api.cppm itself `import llvm.types;` directly (for LLVMDIBuilderRef,
# not otherwise reachable through llvm_core), so llvm_types is linked
# here explicitly too, alongside llvm_core -- not relying solely on
# llvm_core's own PUBLIC link to llvm_types (libs/llvm/CMakeLists.txt),
# so this target's own CMake dependency graph accurately reflects its
# real, direct module dependency. Likewise, orchestration.cppm and
# debug.cppm themselves `import llvm.debug_info;` directly, so llvm_debug_info
# is linked here explicitly too, for the same reason.
target_link_libraries(scpp_codegen PUBLIC scpp_ast scpp_constexpr llvm_core llvm_types llvm_debug_info ${LLVM_LIBS})
target_compile_features(scpp_codegen PRIVATE cxx_std_23)
