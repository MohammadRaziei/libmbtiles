#include "httplib.h"
#include "mbtiles.h"
#include "sqlite3.h"
#include "mustache.hpp"


#include "templates/view_mustache_html.h"
#include "templates/assets/leaflet_css.h"
#include "templates/assets/leaflet_js.h"

#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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

// std::string json_escape(const std::string &value) {
//     std::string escaped;
//     escaped.reserve(value.size() + 8);
//     for (char ch : value) {
//         switch (ch) {
//         case '\\':
//             escaped += "\\\\";
//             break;
//         case '"':
//             escaped += "\\\"";
//             break;
//         case '\b':
//             escaped += "\\b";
//             break;
//         case '\f':
//             escaped += "\\f";
//             break;
//         case '\n':
//             escaped += "\\n";
//             break;
//         case '\r':
//             escaped += "\\r";
//             break;
//         case '\t':
//             escaped += "\\t";
//             break;
//         default:
//             if (static_cast<unsigned char>(ch) < 0x20) {
//                 std::ostringstream oss;
//                 oss << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
//                     << static_cast<int>(static_cast<unsigned char>(ch));
//                 escaped += oss.str();
//             } else {
//                 escaped += ch;
//             }
//             break;
//         }
//     }
//     return escaped;
// }

// std::string build_metadata_json(const std::map<std::string, std::string> &metadata,
//                                 std::uint64_t tile_count,
//                                 int min_zoom,
//                                 int max_zoom,
//                                 const std::string &file_name,
//                                 const std::string &file_path) {
//     std::string json = "{\"metadata\":{";
//     bool first = true;
//     for (const auto &entry : metadata) {
//         if (!first) {
//             json += ',';
//         }
//         first = false;
//         json += '\"' + json_escape(entry.first) + '\"';
//         json += ':';
//         json += '\"' + json_escape(entry.second) + '\"';
//     }
//     json += "},\"stats\":{";
//     json += "\"tile_count\":" + std::to_string(tile_count) + ',';
//     json += "\"min_zoom\":" + std::to_string(min_zoom) + ',';
//     json += "\"max_zoom\":" + std::to_string(max_zoom) + ',';
//     json += "\"file_name\":\"" + json_escape(file_name) + "\",";
//     json += "\"file_path\":\"" + json_escape(file_path) + "\"";
//     json += "}}";
//     return json;
// }

// std::string build_viewer_page(const std::string &title) {
   
//     return html.str();
// }

// int query_zoom_value(sqlite3 *db, const char *sql) {
//     sqlite3_stmt *stmt = nullptr;
//     if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
//         return 0;
//     }
//     std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
//     if (sqlite3_step(stmt) == SQLITE_ROW) {
//         return sqlite3_column_int(stmt, 0);
//     }
//     return 0;
// }

// std::uint64_t query_tile_count(sqlite3 *db) {
//     sqlite3_stmt *stmt = nullptr;
//     if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM tiles", -1, &stmt, nullptr) != SQLITE_OK) {
//         return 0;
//     }
//     std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
//     if (sqlite3_step(stmt) == SQLITE_ROW) {
//         return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
//     }
//     return 0;
// }

}  // namespace

void serve_viewer(const std::string &mbtiles_path, const ViewerOptions &options) {
    if (mbtiles_path.empty()) {
        throw std::invalid_argument("MBTiles path must not be empty");
    }

    // auto db = open_database(mbtiles_path);
    int db;
    std::mutex db_mutex;

    // const auto metadata = read_metadata(mbtiles_path);
    // const std::uint64_t tile_count = [&]() {
    //     std::lock_guard<std::mutex> lock(db_mutex);
    //     return query_tile_count(db.get());
    // }();
    // const int min_zoom = [&]() {
    //     std::lock_guard<std::mutex> lock(db_mutex);
    //     return query_zoom_value(db.get(), "SELECT MIN(zoom_level) FROM tiles");
    // }();
    // const int max_zoom = [&]() {
    //     std::lock_guard<std::mutex> lock(db_mutex);
    //     return query_zoom_value(db.get(), "SELECT MAX(zoom_level) FROM tiles");
    // }();

    const std::filesystem::path file_path = std::filesystem::absolute(mbtiles_path);
    const std::string file_name = file_path.filename().string();

    // const std::string metadata_json = build_metadata_json(metadata, tile_count, min_zoom, max_zoom, file_name,
                                                        //   file_path.string());
    // const std::string viewer_page = build_viewer_page(file_name);


    const std::string viewer_page = templates::view_mustache_html; // use mustache for 

    httplib::Server server;

    server.Get("/view", [viewer_page](const httplib::Request &, httplib::Response &res) {
        res.set_content(viewer_page, "text/html; charset=utf-8");
    });

    server.Get("/assets/leaflet.js", [viewer_page](const httplib::Request &, httplib::Response &res) {
        res.set_content(templates::assets::leaflet_js, "text/html; charset=utf-8");
    });

    server.Get("/assets/leaflet.css", [viewer_page](const httplib::Request &, httplib::Response &res) {
        res.set_content(templates::assets::leaflet_css, "text/html; charset=utf-8");
    });

    // server.Get("/view/metadata", [metadata_json](const httplib::Request &, httplib::Response &res) {
    //     res.set_content(metadata_json, "application/json; charset=utf-8");
    // });

    server.Get(R"(/view/tiles/(\d+)/(\d+)/(\d+))",
               [&db, &db_mutex](const httplib::Request &req, httplib::Response &res) {
                   const int zoom = std::stoi(req.matches[1]);
                   const int column = std::stoi(req.matches[2]);
                   const int row = std::stoi(req.matches[3]);

    //                if (zoom < 0 || column < 0 || row < 0) {
    //                    res.status = 400;
    //                    res.set_content("Invalid tile coordinates", "text/plain; charset=utf-8");
    //                    return;
    //                }

    //                const std::int64_t max_index = (static_cast<std::int64_t>(1) << zoom) - 1;
    //                if (column > max_index || row > max_index) {
    //                    res.status = 404;
    //                    res.set_content("Tile coordinates exceed range for zoom level", "text/plain; charset=utf-8");
    //                    return;
    //                }

    //                std::string tile_data;
    //                {
    //                    std::lock_guard<std::mutex> lock(db_mutex);
    //                    sqlite3_stmt *stmt = nullptr;
    //                    const char *sql =
    //                        "SELECT tile_data FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=? LIMIT 1";
    //                    if (sqlite3_prepare_v2(db.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
    //                        res.status = 500;
    //                        res.set_content("Failed to prepare tile query", "text/plain; charset=utf-8");
    //                        return;
    //                    }

    //                    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);

    //                    const std::int64_t tms_row = max_index - row;
    //                    sqlite3_bind_int(stmt, 1, zoom);
    //                    sqlite3_bind_int(stmt, 2, column);
    //                    sqlite3_bind_int64(stmt, 3, tms_row);

    //                    if (sqlite3_step(stmt) != SQLITE_ROW) {
    //                        res.status = 404;
    //                        res.set_content("Tile not found", "text/plain; charset=utf-8");
    //                        return;
    //                    }

    //                    const void *blob = sqlite3_column_blob(stmt, 0);
    //                    const int size = sqlite3_column_bytes(stmt, 0);
    //                    if (blob == nullptr || size <= 0) {
    //                        res.status = 404;
    //                        res.set_content("Tile is empty", "text/plain; charset=utf-8");
    //                        return;
    //                    }
    //                    tile_data.assign(static_cast<const char *>(blob), static_cast<std::size_t>(size));
    //                }

    //                const std::string content_type = detect_content_type(tile_data);
    //                res.set_header("Cache-Control", "no-store, max-age=0");
    //                res.set_content(tile_data, content_type.c_str());
               });

    std::cout << "Serving MBTiles viewer for '" << file_name << "' on http://" << options.host << ':'
              << options.port << "/view" << std::endl;

    if (!server.listen(options.host.c_str(), options.port)) {
        throw std::runtime_error("Failed to start HTTP server. Ensure the port is available.");
    }
}

}  // namespace mbtiles

