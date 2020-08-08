#include "steam_utils.hpp"

#include <exception>
#include <stdexcept>

#include <fmt/format.h>

#include "filesystem_utils.hpp"
#include "std_utils.hpp"
#include "string_utils.hpp"
#include "vdf.hpp"

#include "exceptions/directory_not_found.hpp"
#include "exceptions/steam_install_not_found.hpp"
#include "exceptions/steam_workshop_directory_not_found.hpp"

using namespace StringUtils;

using std::filesystem::exists;
using std::filesystem::path;

namespace fs = FilesystemUtils;

SteamUtils::SteamUtils(std::vector<path> const &search_paths)
{
    for (auto const &search_path : search_paths)
    {
        path replace_var = Replace(search_path.c_str(), "$HOME", getenv("HOME"));
        std::string final_path = replace_var / config_path_;
        if (fs::Exists(final_path))
        {
            steam_path_ = replace_var;
            break;
        }
    }
    if (steam_path_.empty())
        throw SteamInstallNotFoundException();
}

path const &SteamUtils::GetSteamPath() const noexcept
{
    return steam_path_;
}

std::vector<path> SteamUtils::GetInstallPaths() const
{
    std::vector<std::filesystem::path> ret;
    ret.emplace_back(steam_path_);

    VDF vdf;
    vdf.LoadFromText(StdUtils::FileReadAllText(steam_path_ / config_path_));

    for (auto const &key : vdf.GetValuesWithFilter("BaseInstallFolder"))
        ret.emplace_back(key);

    return ret;
}

path SteamUtils::GetGamePathFromInstallPath(path const &install_path, std::string_view const appid) const
{
    std::filesystem::path manifest_file = install_path / "steamapps" / fmt::format("appmanifest_{}.acf", appid.data());

    VDF vdf;
    vdf.LoadFromText(StdUtils::FileReadAllText(manifest_file));
    return install_path / "steamapps/common" / vdf.KeyValue.at("AppState/installdir");
}

path SteamUtils::GetWorkshopPath(path const &install_path, std::string const &appid) const
{
    path proposed_path = install_path / "steamapps/workshop/content" / appid;
    if (fs::Exists(proposed_path))
        return proposed_path;

    throw SteamWorkshopDirectoryNotFoundException(appid);
}

std::pair<std::filesystem::path, std::string> SteamUtils::GetCompatibilityToolForAppId(std::uint64_t const app_id)
{
    auto const config_vdf_path = steam_path_ / "config/config.vdf";
    auto const key = fmt::format("InstallConfigStore/Software/Valve/Steam/CompatToolMapping/{}/name", app_id);

    VDF config_vdf;
    config_vdf.LoadFromText(StdUtils::FileReadAllText(config_vdf_path));
    if (!StdUtils::ContainsKey(config_vdf.KeyValue, key))
        throw std::runtime_error("compatibility tool entry not found");

    auto const compatibility_tool_path = get_compatibility_tool_path(config_vdf.KeyValue[key]);

    std::string const command_line_key = "manifest/commandline";
    auto const tool_manifest = compatibility_tool_path / "toolmanifest.vdf";
    VDF tool_manifest_vdf;
    tool_manifest_vdf.LoadFromText(StdUtils::FileReadAllText(tool_manifest));
    if (!StdUtils::ContainsKey(tool_manifest_vdf.KeyValue, command_line_key))
        throw std::runtime_error(fmt::format("cannot read '{}' from '{}'", command_line_key, tool_manifest.string()));

    auto const tool_name = tool_manifest_vdf.KeyValue.at("manifest/commandline");
    auto const separator = tool_name.find(' ');
    auto const tool_args = StringUtils::trim(tool_name.substr(separator));
    auto const tool_path = StringUtils::trim(tool_name.substr(0, separator));
    auto full_path = compatibility_tool_path;
    full_path += tool_path;
    return std::pair<std::filesystem::path, std::string>(full_path, tool_args);
}

std::filesystem::path SteamUtils::GetInstallPathFromGamePath(std::filesystem::path const &game_path)
{
    //game_path == "~/.local/share/Steam/steamapps/common/Arma 3"
    return std::filesystem::weakly_canonical(game_path / "../../../");
}

std::filesystem::path SteamUtils::get_compatibility_tool_path(std::string const &shortname) const
{
    auto const log_file_path = steam_path_ / "logs/compat_log.txt";
    auto const log_content = StdUtils::FileReadAllText(log_file_path);

    // we need to scan compat_log.txt for something like "proton_5" and get its appid
    auto const text_to_look_for = fmt::format("Registering tool {}, AppID ", shortname);
    auto const position = log_content.rfind(text_to_look_for);
    if (position == std::string::npos)
        throw std::runtime_error(fmt::format("cannot find entry for '{}' compatibility tool", shortname));

    // expecting something like "Registering tool proton_5, AppID 12345678\n<nextlogline>"
    auto const app_id_start = position + text_to_look_for.length();
    auto const app_id_end = log_content.find_first_of("\r\n[", app_id_start);
    auto const app_id_str = log_content.substr(app_id_start, app_id_end - app_id_start);

    if (app_id_str == "0") // custom compatibility tool installed in ~Steam/compatibilitytools.d/
        return get_user_compatibility_tool(shortname);
    else // compatibility tool provided by Steam
        return get_builtin_compatibility_tool(shortname, app_id_str);
}

std::filesystem::path SteamUtils::get_user_compatibility_tool(std::string const &shortname) const
{
    auto compatibility_tool_path = GetSteamPath() / "compatibilitytools.d" / shortname;
    if (!fs::Exists(compatibility_tool_path))
        throw DirectoryNotFoundException(compatibility_tool_path);
    return compatibility_tool_path;
}

std::filesystem::path SteamUtils::get_builtin_compatibility_tool(std::string const &shortname,
        std::string_view const app_id_str) const
{
    for (auto const &install_path : GetInstallPaths())
    {
        try
        {
            return GetGamePathFromInstallPath(install_path, app_id_str);
        }
        catch (std::exception const &)
        {
        }
    }
    throw std::runtime_error(fmt::format("cannot find tool with appid '{}', name '{}'. Is it installed in Steam?",
                                         app_id_str.data(), shortname));
}
