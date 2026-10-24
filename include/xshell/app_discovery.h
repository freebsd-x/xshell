#pragma once

#include <string>
#include <vector>
#include <expected>
#include <system_error>

namespace fbx::xshell {

struct AppMetadata {
    std::string name;
    std::string version;
    std::string identifier;
    std::string executable_path;
    std::string bundle_path;
    std::string icon_path;
    std::vector<std::string> mime_types; // MIME types this app can open, e.g. "text/plain"
};

class AppDiscoveryService {
public:
    static std::vector<AppMetadata> discoverApps(const std::string& apps_dir) noexcept;
};

} // namespace fbx::xshell
