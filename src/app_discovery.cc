#include "xshell/app_discovery.h"
#include <cctype>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fbx::xshell {

// Extremely simplified JSON parser for the prototype
static std::string getJsonValue(const std::string& json, const std::string& key) {
    std::string search_key = "\"" + key + "\":";
    size_t pos = json.find(search_key);
    if (pos == std::string::npos) return "";

    size_t start = json.find("\"", pos + search_key.length());
    if (start == std::string::npos) return "";

    size_t end = json.find("\"", start + 1);
    if (end == std::string::npos) return "";

    return json.substr(start + 1, end - start - 1);
}

// Extract a JSON string array: "key": ["a", "b", "c"]
static std::vector<std::string> getJsonArray(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return result;
    size_t bracket = json.find('[', pos + search.size());
    if (bracket == std::string::npos) return result;
    size_t end_bracket = json.find(']', bracket);
    if (end_bracket == std::string::npos) return result;
    std::string arr = json.substr(bracket + 1, end_bracket - bracket - 1);
    size_t i = 0;
    while (i < arr.size()) {
        size_t q1 = arr.find('"', i);
        if (q1 == std::string::npos) break;
        size_t q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

std::vector<AppMetadata> AppDiscoveryService::discoverApps(const std::string& apps_dir) noexcept {
    std::vector<AppMetadata> apps;
    
    try {
        if (!std::filesystem::exists(apps_dir)) {
            std::cerr << "[xshell] Apps directory does not exist: " << apps_dir << std::endl;
            return apps;
        }

        for (const auto& entry : std::filesystem::directory_iterator(apps_dir)) {
            if (entry.is_directory() && entry.path().extension() == ".xapp") {
                std::string stem = entry.path().stem().string();

                // A manifest is optional: most bundles ship only a binary + icon.
                // When present it supplies name/identifier/mime; when absent we
                // synthesize a name from the bundle stem so the app still launches.
                std::filesystem::path manifest_path = entry.path() / "manifest.json";
                std::string content;
                if (std::filesystem::exists(manifest_path)) {
                    std::ifstream file(manifest_path);
                    content.assign((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
                }

                AppMetadata app;
                app.name = getJsonValue(content, "name");
                if (app.name.empty()) app.name = stem;   // manifest-less bundle
                app.version = getJsonValue(content, "version");
                app.identifier = getJsonValue(content, "identifier");

                // Resolve the app's runnable binary. The base name comes from the
                // manifest's "executable" basename (e.g. "bin/settings" →
                // "settings"); without a manifest, from the bundle stem
                // ("Settings.xapp" → "settings").
                std::string exec_field = getJsonValue(content, "executable");
                std::string base = exec_field.empty()
                    ? stem
                    : std::filesystem::path(exec_field).filename().string();
                std::transform(base.begin(), base.end(), base.begin(),
                               [](unsigned char c){ return std::tolower(c); });

                // Prefer the freshly-built binary in the build dir (out/) so a stale
                // binary committed inside the bundle never shadows the current build.
                // Meson names some targets <base> and some <base>_app; try both, then
                // fall back to a binary staged inside the bundle (a packaged install
                // has no out/ dir). An app with no runnable binary is skipped rather
                // than listed as a dead icon.
                std::filesystem::path exec_path;
                std::filesystem::path candidates[] = {
                    std::filesystem::path("out") / base,
                    std::filesystem::path("out") / (base + "_app"),
                    exec_field.empty() ? std::filesystem::path{} : entry.path() / exec_field,
                };
                for (const auto& cand : candidates) {
                    if (cand.empty()) continue;
                    std::error_code ec;
                    if (std::filesystem::exists(cand, ec) &&
                        !std::filesystem::is_directory(cand, ec)) {
                        exec_path = cand;
                        break;
                    }
                }
                app.executable_path = exec_path.string();
                app.bundle_path = entry.path().string();

                // Priority: SVG -> PNG
                std::filesystem::path svg_icon = entry.path() / "Resources/icon.svg";
                if (std::filesystem::exists(svg_icon)) {
                    app.icon_path = svg_icon.string();
                } else {
                    app.icon_path = (entry.path() / "Resources/icon.png").string();
                }

                app.mime_types = getJsonArray(content, "mime_types");

                if (!app.name.empty() && !app.executable_path.empty()) {
                    apps.push_back(app);
                    std::cout << "[xshell] Discovered app: " << app.name
                              << " (" << app.identifier << ")"
                              << (app.mime_types.empty() ? "" : " [MIME handler]") << "\n";
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[xshell] Error during app discovery: " << e.what() << std::endl;
    }

    return apps;
}

} // namespace fbx::xshell
