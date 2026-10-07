// SPDX-License-Identifier: GPL-2.0-or-later
// Replaces shadPS4's common/path_util.cpp, which creates a whole user directory tree
// (in %APPDATA% or next to the executable) at static-init time and needs core/file_sys.
// Here every PathType is a sub-directory of SotcGpu::g_user_dir (sotcgpu_config::cache_dir,
// default "sotcgpu"), created only when first asked for.
// Sub-directory names and the API follow shadPS4 (14ada48).

#include <map>
#include <mutex>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "sotcgpu_internal.h"

namespace Common::FS {

namespace fs = std::filesystem;

static std::mutex g_paths_mutex;
static std::map<PathType, fs::path> g_paths;

static const char* SubDir(PathType type) {
    switch (type) {
    case PathType::UserDir:
        return "";
    case PathType::LogDir:
        return LOG_DIR;
    case PathType::ScreenshotsDir:
        return SCREENSHOTS_DIR;
    case PathType::ShaderDir:
        return SHADER_DIR;
    case PathType::GameDataDir:
        return GAMEDATA_DIR;
    case PathType::TempDataDir:
        return TEMPDATA_DIR;
    case PathType::SysModuleDir:
        return SYSMODULES_DIR;
    case PathType::DownloadDir:
        return DOWNLOAD_DIR;
    case PathType::CapturesDir:
        return CAPTURES_DIR;
    case PathType::CheatsDir:
        return CHEATS_DIR;
    case PathType::PatchesDir:
        return PATCHES_DIR;
    case PathType::MetaDataDir:
        return METADATA_DIR;
    case PathType::CustomTrophy:
        return CUSTOM_TROPHY;
    case PathType::CustomConfigs:
        return CUSTOM_CONFIGS;
    case PathType::CacheDir:
        return CACHE_DIR;
    case PathType::FontsDir:
        return FONTS_DIR;
    case PathType::TrophyDir:
        return TROPHY_DIR;
    case PathType::HomeDir:
        return HOME_DIR;
    case PathType::CustomModulesDir:
        return CUSTOM_MODULES_DIR;
    case PathType::LicensesDir:
        return LICENSES_DIR;
    }
    return "";
}

bool ValidatePath(const fs::path& path) {
    if (path.empty()) {
        LOG_ERROR(Common_Filesystem, "Input path is empty");
        return false;
    }
    return true;
}

std::string PathToUTF8String(const fs::path& path) {
    const auto u8_string = path.u8string();
    return std::string{u8_string.begin(), u8_string.end()};
}

const fs::path& GetUserPath(PathType type) {
    std::scoped_lock lk{g_paths_mutex};
    auto it = g_paths.find(type);
    if (it == g_paths.end()) {
        fs::path p = SotcGpu::g_user_dir / SubDir(type);
        std::error_code ec;
        fs::create_directories(p, ec);
        it = g_paths.emplace(type, std::move(p)).first;
    }
    return it->second; // std::map nodes are stable
}

std::string GetUserPathString(PathType type) {
    return PathToUTF8String(GetUserPath(type));
}

void SetUserPath(PathType type, const fs::path& new_path) {
    std::scoped_lock lk{g_paths_mutex};
    g_paths.insert_or_assign(type, new_path);
}

std::optional<fs::path> FindGameByID(const fs::path&, const std::string&, int) {
    return std::nullopt;
}

} // namespace Common::FS
