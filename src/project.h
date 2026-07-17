#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace scpp {

struct ProjectBuildOptions {
    bool build_lib_only = false;
    std::optional<std::string> selected_bin;
    std::optional<std::string> selected_profile;
    std::optional<std::string> selected_package;
    bool release = false;
    bool build_workspace = false;
};

std::optional<std::filesystem::path> find_project_manifest(const std::filesystem::path& start_dir);
int build_manifest_project(const std::filesystem::path& start_dir, const ProjectBuildOptions& options);

} // namespace scpp
