module;

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module scpp.clicommands:installself;

import scpp.driver;

#ifndef SCPP_VERSION_MAJOR
#define SCPP_VERSION_MAJOR "0"
#endif

namespace {

std::filesystem::path require_home_directory() {
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        throw std::runtime_error("installself requires $HOME to be set");
    }
    return std::filesystem::path(home);
}

std::string compiler_version_slug() { return std::string("scpp") + SCPP_VERSION_MAJOR; }

void copy_file_with_permissions(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    if (ec) {
        throw std::runtime_error("failed to create directory '" + to.parent_path().string() + "': " + ec.message());
    }
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) throw std::runtime_error("failed to copy '" + from.string() + "' to '" + to.string() + "': " + ec.message());
    std::filesystem::permissions(to, std::filesystem::status(from, ec).permissions(), ec);
}

void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!std::filesystem::exists(from)) {
        throw std::runtime_error("cannot copy missing directory '" + from.string() + "'");
    }
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    if (ec) throw std::runtime_error("failed to create directory '" + to.string() + "': " + ec.message());
    for (const auto& entry : std::filesystem::recursive_directory_iterator(from, ec)) {
        if (ec) throw std::runtime_error("failed while walking '" + from.string() + "': " + ec.message());
        std::filesystem::path relative = std::filesystem::relative(entry.path(), from, ec);
        if (ec) throw std::runtime_error("failed to compute relative path under '" + from.string() + "': " + ec.message());
        std::filesystem::path dest = to / relative;
        if (entry.is_directory()) {
            std::filesystem::create_directories(dest, ec);
            if (ec) throw std::runtime_error("failed to create directory '" + dest.string() + "': " + ec.message());
            continue;
        }
        if (entry.is_regular_file()) {
            copy_file_with_permissions(entry.path(), dest);
        }
    }
}

void copy_matching_files(const std::filesystem::path& from, const std::filesystem::path& to, std::string_view extension) {
    if (!std::filesystem::exists(from)) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(from, ec)) {
        if (ec) throw std::runtime_error("failed while walking '" + from.string() + "': " + ec.message());
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != extension) continue;
        std::filesystem::path relative = std::filesystem::relative(entry.path(), from, ec);
        if (ec) throw std::runtime_error("failed to compute relative path under '" + from.string() + "': " + ec.message());
        copy_file_with_permissions(entry.path(), to / relative);
    }
}

void copy_prebuilt_stdlib_artifacts(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (!std::filesystem::exists(from)) return;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(from, ec)) {
        if (ec) throw std::runtime_error("failed while reading '" + from.string() + "': " + ec.message());
        if (!entry.is_regular_file()) continue;
        std::string extension = entry.path().extension().string();
        if (extension != ".a" && extension != ".scppm" && extension != ".scppa") continue;
        copy_file_with_permissions(entry.path(), to / entry.path().filename());
    }
}

std::optional<std::filesystem::path> symlink_target_if_present(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_symlink(path, ec)) return std::nullopt;
    std::filesystem::path target = std::filesystem::read_symlink(path, ec);
    if (ec) return std::nullopt;
    return target;
}

struct InstallSelfResult {
    std::string slug;
    std::filesystem::path toolchain_root;
    std::filesystem::path installed_binary;
    std::filesystem::path installed_stdlib_dir;
    std::vector<std::pair<std::filesystem::path, std::optional<std::filesystem::path>>> symlink_old_targets{};
};

} // namespace

export namespace scpp {

int run_installself() {
    try {
        std::optional<std::filesystem::path> exe = scpp::driver_runtime_current_executable_path();
        if (!exe.has_value()) throw std::runtime_error("installself could not resolve the running compiler path");
        std::optional<std::filesystem::path> installed_stdlib = scpp::driver_runtime_installed_stdlib_dir();
        std::optional<std::filesystem::path> prebuilt_stdlib = scpp::driver_runtime_default_prebuilt_stdlib_dir();
        std::optional<std::filesystem::path> source_stdlib = scpp::driver_runtime_default_source_stdlib_dir();

        bool have_installed_stdlib = installed_stdlib.has_value() && std::filesystem::exists(*installed_stdlib);
        bool have_prebuilt_stdlib = prebuilt_stdlib.has_value() && std::filesystem::exists(*prebuilt_stdlib);
        bool have_source_stdlib = source_stdlib.has_value() && std::filesystem::exists(*source_stdlib);
        if (!have_installed_stdlib && !have_prebuilt_stdlib && !have_source_stdlib) {
            throw std::runtime_error("installself could not find the compiler's accompanying stdlib payload");
        }

        std::filesystem::path home = require_home_directory();
        std::filesystem::path scpp_root = home / ".scpp";
        std::filesystem::path toolchains_root = scpp_root / "toolchains";
        std::string slug = compiler_version_slug();
        std::filesystem::path toolchain_root = toolchains_root / slug;
        std::filesystem::path staging_root = toolchains_root / ("." + slug + ".installing");
        std::filesystem::path staging_bin = staging_root / "bin";
        std::filesystem::path staging_stdlib = staging_root / "share" / "scpp" / "stdlib";

        std::error_code ec;
        std::filesystem::remove_all(staging_root, ec);
        ec.clear();
        std::filesystem::create_directories(staging_bin, ec);
        if (ec) throw std::runtime_error("failed to create '" + staging_bin.string() + "': " + ec.message());
        std::filesystem::create_directories(staging_stdlib, ec);
        if (ec) throw std::runtime_error("failed to create '" + staging_stdlib.string() + "': " + ec.message());

        copy_file_with_permissions(*exe, staging_bin / "scpp");
        if (have_installed_stdlib) {
            copy_tree(*installed_stdlib, staging_stdlib);
        } else {
            if (have_prebuilt_stdlib) copy_prebuilt_stdlib_artifacts(*prebuilt_stdlib, staging_stdlib);
            if (have_source_stdlib) copy_matching_files(*source_stdlib, staging_stdlib, ".scpp");
        }

        std::filesystem::remove_all(toolchain_root, ec);
        ec.clear();
        std::filesystem::rename(staging_root, toolchain_root, ec);
        if (ec) throw std::runtime_error("failed to finalize install into '" + toolchain_root.string() + "': " + ec.message());

        std::filesystem::path bin_root = scpp_root / "bin";
        std::filesystem::create_directories(bin_root, ec);
        if (ec) throw std::runtime_error("failed to create '" + bin_root.string() + "': " + ec.message());
        std::filesystem::path installed_binary = toolchain_root / "bin" / "scpp";
        std::filesystem::path versioned_link = bin_root / slug;
        std::filesystem::path default_link = bin_root / "scpp";

        InstallSelfResult result{slug, toolchain_root, installed_binary, toolchain_root / "share" / "scpp" / "stdlib"};
        result.symlink_old_targets.push_back({versioned_link, symlink_target_if_present(versioned_link)});
        result.symlink_old_targets.push_back({default_link, symlink_target_if_present(default_link)});
        for (const std::filesystem::path& link_path : {versioned_link, default_link}) {
            std::filesystem::remove(link_path, ec);
            ec.clear();
            std::filesystem::create_symlink(installed_binary, link_path, ec);
            if (ec) throw std::runtime_error("failed to create symlink '" + link_path.string() + "': " + ec.message());
        }

        std::cout << "Installed " << result.slug << " to " << result.toolchain_root << "\n";
        std::cout << "  binary: " << result.installed_binary << "\n";
        std::cout << "  stdlib: " << result.installed_stdlib_dir << "\n";
        for (const auto& [link_path, old_target] : result.symlink_old_targets) {
            std::cout << "  symlink: " << link_path << " -> " << result.installed_binary;
            if (old_target.has_value()) std::cout << " (was " << *old_target << ")";
            std::cout << "\n";
        }
        std::cout << "To use this toolchain, add " << (scpp_root / "bin") << " to your PATH manually, for example:\n";
        std::cout << "  export PATH=\"$HOME/.scpp/bin:$PATH\"\n";
        std::cout << "Then restart your shell or re-run that export in the current shell. installself cannot change the current "
                     "parent shell session.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}

} // namespace scpp
