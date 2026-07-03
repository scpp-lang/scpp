module;

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

export module scpp.driver;

import scpp.ast;
import scpp.codegen;
import scpp.parser;

export namespace scpp {

struct DriverError : std::runtime_error {
    explicit DriverError(const std::string& message) : std::runtime_error(message) {}
};

// Compiles scpp source text down to a native object file at `object_path`.
// This is the M1 backend: AST -> LLVM IR -> native object code, with no
// `safe` checks performed yet.
void emit_object_file(std::string_view source, const std::string& object_path) {
    Program program = parse(source);

    Codegen codegen("scpp_module");
    llvm::Module& module = codegen.generate(program);

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();

    std::string triple = llvm::sys::getDefaultTargetTriple();
    llvm::Triple target_triple(triple);
    module.setTargetTriple(target_triple);

    std::string lookup_error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(target_triple, lookup_error);
    if (target == nullptr) {
        throw DriverError("failed to lookup target '" + triple + "': " + lookup_error);
    }

    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> target_machine(target->createTargetMachine(
        target_triple, "generic", "", options, llvm::Reloc::PIC_));
    if (!target_machine) {
        throw DriverError("failed to create target machine for '" + triple + "'");
    }

    module.setDataLayout(target_machine->createDataLayout());

    std::error_code error_code;
    llvm::raw_fd_ostream dest(object_path, error_code, llvm::sys::fs::OF_None);
    if (error_code) {
        throw DriverError("could not open object file '" + object_path + "': " + error_code.message());
    }

    llvm::legacy::PassManager pass_manager;
    if (target_machine->addPassesToEmitFile(pass_manager, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw DriverError("target machine cannot emit an object file of this type");
    }
    pass_manager.run(module);
    dest.flush();
}

// Links a native object file into an executable using the system compiler
// driver (clang/cc); this keeps us out of the business of re-implementing a
// platform linker for M1.
void link_executable(const std::string& object_path, const std::string& executable_path) {
    std::string command = "cc \"" + object_path + "\" -o \"" + executable_path + "\"";
    int result = std::system(command.c_str());
    if (result != 0) {
        throw DriverError("linker command failed: " + command);
    }
}

void compile_to_executable(std::string_view source, const std::string& executable_path) {
    std::string object_path = executable_path + ".o";
    emit_object_file(source, object_path);
    link_executable(object_path, executable_path);
    llvm::sys::fs::remove(object_path);
}

} // namespace scpp
