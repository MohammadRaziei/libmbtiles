#include "httplib.h"
#include "mbtiles.h"
#include "sqlite3.h"
#include "mustache.hpp"


#include "templates/view_mustache_html.h"
#include "templates/assets/leaflet_css.h"
#include "templates/assets/leaflet_js.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cctype>
#include <cstdint>

namespace mbtiles {
namespace {

struct sqlite_deleter {
    void operator()(sqlite3 *db) const noexcept {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

std::unique_ptr<sqlite3, sqlite_deleter> open_database(const std::string &path) {
    sqlite3 *raw_db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &raw_db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        std::string message = "Unable to open MBTiles file: " + path;
        if (raw_db != nullptr) {
            message += ": ";
            message += sqlite3_errmsg(raw_db);
            sqlite3_close(raw_db);
        }
        throw mbtiles_error(message);
    }
    return std::unique_ptr<sqlite3, sqlite_deleter>(raw_db);
}

std::string detect_content_type(const std::string &payload) {
    if (payload.size() >= 8) {
        const unsigned char *bytes = reinterpret_cast<const unsigned char *>(payload.data());
        if (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
            return "image/png";
        }
        if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
            return "image/jpeg";
        }
        if (payload.size() >= 12 && bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 &&
            bytes[3] == 0x46 && bytes[8] == 0x57 && bytes[9] == 0x45 && bytes[10] == 0x42 && bytes[11] == 0x50) {
            return "image/webp";
        }
    }
    return "application/octet-stream";
}

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = value.find_last_not_of(" \t\n\r\f\v");
    return value.substr(first, last - first + 1);
}

std::optional<std::string> find_metadata_value(const std::map<std::string, std::string> &metadata,
                                              const std::string &key) {
    const auto direct = metadata.find(key);
    if (direct != metadata.end()) {
        return direct->second;
    }

    std::string lowered_key = key;
    std::transform(lowered_key.begin(), lowered_key.end(), lowered_key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (const auto &entry : metadata) {
        std::string lowered_entry_key = entry.first;
        std::transform(lowered_entry_key.begin(), lowered_entry_key.end(), lowered_entry_key.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lowered_entry_key == lowered_key) {
            return entry.second;
        }
    }

    return std::nullopt;
}

std::optional<int> parse_int(const std::string &value) {
    try {
        std::size_t processed = 0;
        const int parsed = std::stoi(value, &processed);
        if (processed == value.size()) {
            return parsed;
        }
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

std::optional<double> parse_double(const std::string &value) {
    try {
        std::size_t processed = 0;
        const double parsed = std::stod(value, &processed);
        if (processed == value.size()) {
            return parsed;
        }
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

std::optional<int> query_zoom_value(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return std::nullopt;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return std::nullopt;
}

struct CenterInfo {
    double lat = 0.0;
    double lon = 0.0;
    std::optional<int> zoom;
};

std::optional<CenterInfo> parse_center(const std::string &value) {
    std::stringstream ss(value);
    std::string token;
    std::vector<std::string> parts;
    while (std::getline(ss, token, ',')) {
        parts.emplace_back(trim(token));
    }
    if (parts.size() < 2) {
        return std::nullopt;
    }

    const auto lon = parse_double(parts[0]);
    const auto lat = parse_double(parts[1]);
    if (!lon || !lat) {
        return std::nullopt;
    }

    CenterInfo info;
    info.lon = *lon;
    info.lat = *lat;
    if (parts.size() >= 3) {
        info.zoom = parse_int(parts[2]);
    }
    return info;
}

struct BoundsInfo {
    double min_lon = 0.0;
    double min_lat = 0.0;
    double max_lon = 0.0;
    double max_lat = 0.0;
};

std::optional<BoundsInfo> parse_bounds(const std::string &value) {
    std::stringstream ss(value);
    std::string token;
    std::vector<std::string> parts;
    while (std::getline(ss, token, ',')) {
        parts.emplace_back(trim(token));
    }
    if (parts.size() != 4) {
        return std::nullopt;
    }

    const auto min_lon = parse_double(parts[0]);
    const auto min_lat = parse_double(parts[1]);
    const auto max_lon = parse_double(parts[2]);
    const auto max_lat = parse_double(parts[3]);
    if (!min_lon || !min_lat || !max_lon || !max_lat) {
        return std::nullopt;
    }

    BoundsInfo bounds;
    bounds.min_lon = *min_lon;
    bounds.min_lat = *min_lat;
    bounds.max_lon = *max_lon;
    bounds.max_lat = *max_lat;
    return bounds;
}

std::string format_double(double value) {
    std::ostringstream oss;
    oss.imbue(std::locale::classic());
    oss << std::setprecision(12) << value;
    std::string result = oss.str();
    const auto dot = result.find('.');
    if (dot != std::string::npos) {
        while (!result.empty() && result.back() == '0') {
            result.pop_back();
        }
        if (!result.empty() && result.back() == '.') {
            result.pop_back();
        }
    }
    if (result.empty()) {
        result = "0";
    }
    return result;
}

}  // namespace

void serve_viewer(const std::string &mbtiles_path, const ViewerOptions &options) {
    if (mbtiles_path.empty()) {
        throw std::invalid_argument("MBTiles path must not be empty");
    }

    auto db = open_database(mbtiles_path);
    std::mutex db_mutex;

    const auto metadata = read_metadata(mbtiles_path);

    std::optional<int> min_zoom_value;
    if (const auto min_zoom_str = find_metadata_value(metadata, "minzoom")) {
        min_zoom_value = parse_int(*min_zoom_str);
    }
    if (!min_zoom_value) {
        min_zoom_value = [&]() -> std::optional<int> {
            std::lock_guard<std::mutex> lock(db_mutex);
            return query_zoom_value(db.get(), "SELECT MIN(zoom_level) FROM tiles");
        }();
    }

    std::optional<int> max_zoom_value;
    if (const auto max_zoom_str = find_metadata_value(metadata, "maxzoom")) {
        max_zoom_value = parse_int(*max_zoom_str);
    }
    if (!max_zoom_value) {
        max_zoom_value = [&]() -> std::optional<int> {
            std::lock_guard<std::mutex> lock(db_mutex);
            return query_zoom_value(db.get(), "SELECT MAX(zoom_level) FROM tiles");
        }();
    }

    int min_zoom = min_zoom_value.value_or(0);
    int max_zoom = max_zoom_value.value_or(min_zoom);
    if (max_zoom < min_zoom) {
        max_zoom = min_zoom;
    }

    const std::filesystem::path file_path = std::filesystem::absolute(mbtiles_path);
    const std::string file_name = file_path.filename().string();

    std::optional<CenterInfo> center_info;
    if (const auto center_value = find_metadata_value(metadata, "center")) {
        center_info = parse_center(*center_value);
    }

    double center_lat = 0.0;
    double center_lon = 0.0;
    if (center_info) {
        center_lat = center_info->lat;
        center_lon = center_info->lon;
    } else if (const auto bounds_value = find_metadata_value(metadata, "bounds")) {
        if (const auto bounds = parse_bounds(*bounds_value)) {
            center_lat = (bounds->min_lat + bounds->max_lat) / 2.0;
            center_lon = (bounds->min_lon + bounds->max_lon) / 2.0;
        }
    }

    int initial_zoom = min_zoom;
    if (center_info && center_info->zoom) {
        initial_zoom = *center_info->zoom;
    }
    initial_zoom = std::clamp(initial_zoom, min_zoom, max_zoom);

    kainjow::mustache::data context;
    context.set("title", file_name);
    context.set("tile_path", std::string{"/tiles"});
    context.set("min_zoom", std::to_string(min_zoom));
    context.set("max_zoom", std::to_string(max_zoom));
    context.set("initial_zoom", std::to_string(initial_zoom));
    context.set("center_lat", format_double(center_lat));
    context.set("center_lng", format_double(center_lon));

    kainjow::mustache::mustache view_template{templates::view_mustache_html};
    const std::string viewer_page = view_template.render(context);

    httplib::Server server;

    server.Get("/view", [viewer_page](const httplib::Request &, httplib::Response &res) {
        res.set_content(viewer_page, "text/html; charset=utf-8");
    });

    server.Get("/assets/leaflet.js", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(templates::assets::leaflet_js, "application/javascript; charset=utf-8");
    });

    server.Get("/assets/leaflet.css", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(templates::assets::leaflet_css, "text/css; charset=utf-8");
    });

    server.Get(R"(/tiles/(\d+)/(\d+)/(\d+)\.png)",
               [&db, &db_mutex](const httplib::Request &req, httplib::Response &res) {
                   const int zoom = std::stoi(req.matches[1]);
                   const int column = std::stoi(req.matches[2]);
                   const int row = std::stoi(req.matches[3]);

                   if (zoom < 0 || column < 0 || row < 0) {
                       res.status = 400;
                       res.set_content("Invalid tile coordinates", "text/plain; charset=utf-8");
                       return;
                   }

                   const std::int64_t max_index = (static_cast<std::int64_t>(1) << zoom) - 1;
                   if (column > max_index || row > max_index) {
                       res.status = 404;
                       res.set_content("Tile coordinates exceed range for zoom level", "text/plain; charset=utf-8");
                       return;
                   }

                   std::string tile_data;
                   {
                       std::lock_guard<std::mutex> lock(db_mutex);
                       sqlite3_stmt *stmt = nullptr;
                       const char *sql =
                           "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=? LIMIT 1";
                       if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
                           res.status = 500;
                           res.set_content("Failed to prepare tile query", "text/plain; charset=utf-8");
                           return;
                       }

                       std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);

                       const std::int64_t tms_row = max_index - row;
                       sqlite3_bind_int(stmt, 1, zoom);
                       sqlite3_bind_int(stmt, 2, column);
                       sqlite3_bind_int64(stmt, 3, tms_row);

                       if (sqlite3_step(stmt) != SQLITE_ROW) {
                           res.status = 404;
                           res.set_content("Tile not found", "text/plain; charset=utf-8");
                           return;
                       }

                       const void *blob = sqlite3_column_blob(stmt, 0);
                       const int size = sqlite3_column_bytes(stmt, 0);
                       if (blob == nullptr || size <= 0) {
                           res.status = 404;
                           res.set_content("Tile is empty", "text/plain; charset=utf-8");
                           return;
                       }
                       tile_data.assign(static_cast<const char *>(blob), static_cast<std::size_t>(size));
                   }

                   const std::string content_type = detect_content_type(tile_data);
                   res.set_header("Cache-Control", "no-store, max-age=0");
                   res.set_content(tile_data, content_type.c_str());
               });

    std::cout << "Serving MBTiles viewer for '" << file_name << "' on http://" << options.host << ':'
              << options.port << "/view" << std::endl;

    if (!server.listen(options.host.c_str(), options.port)) {
        throw std::runtime_error("Failed to start HTTP server. Ensure the port is available.");
    }
}

}  // namespace mbtiles

