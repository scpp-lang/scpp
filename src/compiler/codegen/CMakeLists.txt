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
# Every one of these eleven partition files that needs any LLVM-C surface
# at all (api.cppm, orchestration.cppm, debug.cppm, layout.cppm,
# expressions.cppm, functions.cppm, object_model.cppm, lifetime.cppm,
# statements.cppm) reaches it via a single `import llvm;` -- the one,
# consolidated module covering every piece of LLVM-C surface this
# project's codegen needs (see libs/llvm/CMakeLists.txt) -- so the single
# `llvm` target is linked here explicitly, alongside ${LLVM_LIBS} for the
# real LLVM-C symbols every partition's own declarations ultimately
# resolve to.
target_link_libraries(scpp_codegen PUBLIC scpp_ast scpp_constexpr llvm ${LLVM_LIBS})
target_compile_features(scpp_codegen PRIVATE cxx_std_23)
