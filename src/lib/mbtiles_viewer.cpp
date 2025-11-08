#include "httplib.h"
#include "mbtiles.h"
#include "sqlite3.h"

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

std::string json_escape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                std::ostringstream oss;
                oss << "\\u" << std::hex << std::uppercase << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(ch));
                escaped += oss.str();
            } else {
                escaped += ch;
            }
            break;
        }
    }
    return escaped;
}

std::string build_metadata_json(const std::map<std::string, std::string> &metadata,
                                std::uint64_t tile_count,
                                int min_zoom,
                                int max_zoom,
                                const std::string &file_name,
                                const std::string &file_path) {
    std::string json = "{\"metadata\":{";
    bool first = true;
    for (const auto &entry : metadata) {
        if (!first) {
            json += ',';
        }
        first = false;
        json += '\"' + json_escape(entry.first) + '\"';
        json += ':';
        json += '\"' + json_escape(entry.second) + '\"';
    }
    json += "},\"stats\":{";
    json += "\"tile_count\":" + std::to_string(tile_count) + ',';
    json += "\"min_zoom\":" + std::to_string(min_zoom) + ',';
    json += "\"max_zoom\":" + std::to_string(max_zoom) + ',';
    json += "\"file_name\":\"" + json_escape(file_name) + "\",";
    json += "\"file_path\":\"" + json_escape(file_path) + "\"";
    json += "}}";
    return json;
}

std::string build_viewer_page(const std::string &title) {
    std::ostringstream html;
    html << "<!DOCTYPE html>\n";
    html << "<html lang=\"en\">\n";
    html << "<head>\n";
    html << "<meta charset=\"utf-8\">\n";
    html << "<title>" << title << " - MBTiles Viewer</title>\n";
    html << "<style>\n";
    html << "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;padding:0;background:#f5f5f5;color:#222;}\n";
    html << "header{background:#0d47a1;color:#fff;padding:1rem 2rem;}\n";
    html << "main{display:flex;flex-wrap:wrap;padding:1rem 2rem;gap:1.5rem;}\n";
    html << "section{background:#fff;border-radius:8px;box-shadow:0 2px 6px rgba(0,0,0,0.1);padding:1rem;}\n";
    html << "#metadata{flex:1 1 260px;max-width:360px;}\n";
    html << "#viewer{flex:3 1 360px;min-width:320px;display:flex;flex-direction:column;}\n";
    html << "h1{margin:0;font-size:1.6rem;}\n";
    html << "h2{margin-top:0;font-size:1.1rem;}\n";
    html << "dl{margin:0;}\n";
    html << "dt{font-weight:600;margin-top:0.75rem;}\n";
    html << "dd{margin:0.25rem 0 0;font-family:monospace;font-size:0.95rem;}\n";
    html << "#controls{display:flex;flex-wrap:wrap;gap:1rem;margin-bottom:1rem;}\n";
    html << "#controls label{display:flex;flex-direction:column;font-size:0.85rem;}\n";
    html << "input[type=number]{padding:0.3rem;border:1px solid #bbb;border-radius:4px;width:7rem;}\n";
    html << "button{background:#1976d2;color:#fff;border:none;border-radius:4px;padding:0.5rem 0.8rem;font-size:0.9rem;cursor:pointer;}\n";
    html << "button:hover{background:#1256a0;}\n";
    html << "#tile-container{flex:1;display:flex;align-items:center;justify-content:center;background:#e0e0e0;border-radius:6px;min-height:320px;overflow:hidden;}\n";
    html << "#tile-image{max-width:100%;image-rendering:pixelated;}\n";
    html << "#navigation{display:grid;grid-template-columns:repeat(3,auto);gap:0.3rem;justify-content:center;margin-bottom:0.8rem;}\n";
    html << "#status{margin-top:0.5rem;font-size:0.9rem;color:#444;}\n";
    html << "#error{color:#c62828;margin-top:0.5rem;font-weight:600;}\n";
    html << "@media(max-width:768px){main{flex-direction:column;}#metadata{max-width:none;width:100%;}}\n";
    html << "</style>\n";
    html << "</head>\n";
    html << "<body>\n";
    html << "<header><h1>MBTiles Viewer</h1><p>" << title << "</p></header>\n";
    html << "<main>\n";
    html << "<section id=\"metadata\">\n";
    html << "<h2>Metadata</h2>\n";
    html << "<dl id=\"metadata-list\"></dl>\n";
    html << "</section>\n";
    html << "<section id=\"viewer\">\n";
    html << "<h2>Tile Inspector</h2>\n";
    html << "<div id=\"controls\">\n";
    html << "<label>Zoom<input type=\"number\" id=\"zoom\" min=\"0\" value=\"0\"></label>\n";
    html << "<label>X<input type=\"number\" id=\"x\" min=\"0\" value=\"0\"></label>\n";
    html << "<label>Y<input type=\"number\" id=\"y\" min=\"0\" value=\"0\"></label>\n";
    html << "<div style=\"align-self:flex-end;display:flex;gap:0.5rem;\"><button id=\"load\">Load tile</button><button id=\"reset\">Reset</button></div>\n";
    html << "</div>\n";
    html << "<div id=\"navigation\">\n";
    html << "<span></span><button data-dx=\"0\" data-dy=\"-1\">▲</button><span></span>\n";
    html << "<button data-dx=\"-1\" data-dy=\"0\">◀</button><span></span><button data-dx=\"1\" data-dy=\"0\">▶</button>\n";
    html << "<span></span><button data-dx=\"0\" data-dy=\"1\">▼</button><span></span>\n";
    html << "</div>\n";
    html << "<div id=\"tile-container\"><img id=\"tile-image\" alt=\"Tile image\"></div>\n";
    html << "<div id=\"status\"></div>\n";
    html << "<div id=\"error\"></div>\n";
    html << "</section>\n";
    html << "</main>\n";
    html << "<script>\n";
    html << "(function(){\n";
    html << "const metadataList=document.getElementById('metadata-list');\n";
    html << "const zoomInput=document.getElementById('zoom');\n";
    html << "const xInput=document.getElementById('x');\n";
    html << "const yInput=document.getElementById('y');\n";
    html << "const tileImage=document.getElementById('tile-image');\n";
    html << "const status=document.getElementById('status');\n";
    html << "const errorBox=document.getElementById('error');\n";
    html << "const navigation=document.getElementById('navigation');\n";
    html << "const loadButton=document.getElementById('load');\n";
    html << "const resetButton=document.getElementById('reset');\n";
    html << "let minZoom=0;\n";
    html << "let maxZoom=18;\n";
    html << "let currentZoom=0;\n";
    html << "let currentX=0;\n";
    html << "let currentY=0;\n";
    html << "function updateStatus(message){status.textContent=message;}\n";
    html << "function showError(message){errorBox.textContent=message||'';}\n";
    html << "function clampCoordinates(){\n";
    html << "const maxIndex=Math.max(0,(1<<currentZoom)-1);\n";
    html << "currentX=Math.min(Math.max(0,currentX),maxIndex);\n";
    html << "currentY=Math.min(Math.max(0,currentY),maxIndex);\n";
    html << "xInput.value=currentX;\n";
    html << "yInput.value=currentY;\n";
    html << "}\n";
    html << "function loadMetadata(){\n";
    html << "fetch('metadata').then(r=>{if(!r.ok){throw new Error('Failed to load metadata');}return r.json();}).then(data=>{\n";
    html << "metadataList.innerHTML='';\n";
    html << "if(data && data.metadata){\n";
    html << "for(const key of Object.keys(data.metadata)){\n";
    html << "const dt=document.createElement('dt');dt.textContent=key;\n";
    html << "const dd=document.createElement('dd');dd.textContent=data.metadata[key];\n";
    html << "metadataList.appendChild(dt);metadataList.appendChild(dd);\n";
    html << "}}\n";
    html << "if(data && data.stats){\n";
    html << "minZoom=data.stats.min_zoom||0;\n";
    html << "maxZoom=data.stats.max_zoom||minZoom;\n";
    html << "zoomInput.min=minZoom;\n";
    html << "zoomInput.max=maxZoom;\n";
    html << "zoomInput.value=minZoom;\n";
    html << "currentZoom=minZoom;\n";
    html << "updateStatus('Tiles: '+data.stats.tile_count+' | Zoom '+currentZoom);\n";
    html << "}\n";
    html << "loadTile();\n";
    html << "}).catch(err=>{showError(err.message);});\n";
    html << "}\n";
    html << "function loadTile(){\n";
    html << "currentZoom=parseInt(zoomInput.value,10)||0;\n";
    html << "currentZoom=Math.min(Math.max(currentZoom,minZoom),maxZoom);\n";
    html << "const maxIndex=Math.max(0,(1<<currentZoom)-1);\n";
    html << "currentX=parseInt(xInput.value,10)||0;\n";
    html << "currentY=parseInt(yInput.value,10)||0;\n";
    html << "currentX=Math.min(Math.max(0,currentX),maxIndex);\n";
    html << "currentY=Math.min(Math.max(0,currentY),maxIndex);\n";
    html << "zoomInput.value=currentZoom;\n";
    html << "xInput.value=currentX;\n";
    html << "yInput.value=currentY;\n";
    html << "const url='tiles/'+currentZoom+'/'+currentX+'/'+currentY+'?t='+(Date.now());\n";
    html << "tileImage.src=url;\n";
    html << "showError('');\n";
    html << "updateStatus('Loading tile z='+currentZoom+' x='+currentX+' y='+currentY+' ...');\n";
    html << "}\n";
    html << "tileImage.addEventListener('load',()=>{updateStatus('Showing tile z='+currentZoom+' x='+currentX+' y='+currentY);});\n";
    html << "tileImage.addEventListener('error',()=>{showError('Tile not found for the current coordinates.');updateStatus('');});\n";
    html << "loadButton.addEventListener('click',loadTile);\n";
    html << "resetButton.addEventListener('click',()=>{zoomInput.value=minZoom;currentZoom=minZoom;currentX=0;currentY=0;xInput.value=0;yInput.value=0;loadTile();});\n";
    html << "zoomInput.addEventListener('change',loadTile);\n";
    html << "navigation.querySelectorAll('button').forEach(btn=>{\n";
    html << "btn.addEventListener('click',()=>{const dx=parseInt(btn.getAttribute('data-dx'),10)||0;const dy=parseInt(btn.getAttribute('data-dy'),10)||0;currentX+=dx;currentY+=dy;clampCoordinates();loadTile();});\n";
    html << "});\n";
    html << "loadMetadata();\n";
    html << "})();\n";
    html << "</script>\n";
    html << "</body>\n";
    html << "</html>\n";
    return html.str();
}

int query_zoom_value(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return sqlite3_column_int(stmt, 0);
    }
    return 0;
}

std::uint64_t query_tile_count(sqlite3 *db) {
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(1) FROM tiles", -1, &stmt, nullptr) != SQLITE_OK) {
        return 0;
    }
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> guard(stmt, sqlite3_finalize);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 0));
    }
    return 0;
}

}  // namespace

void serve_viewer(const std::string &mbtiles_path, const ViewerOptions &options) {
    if (mbtiles_path.empty()) {
        throw std::invalid_argument("MBTiles path must not be empty");
    }

    auto db = open_database(mbtiles_path);
    std::mutex db_mutex;

    const auto metadata = read_metadata(mbtiles_path);
    const std::uint64_t tile_count = [&]() {
        std::lock_guard<std::mutex> lock(db_mutex);
        return query_tile_count(db.get());
    }();
    const int min_zoom = [&]() {
        std::lock_guard<std::mutex> lock(db_mutex);
        return query_zoom_value(db.get(), "SELECT MIN(zoom_level) FROM tiles");
    }();
    const int max_zoom = [&]() {
        std::lock_guard<std::mutex> lock(db_mutex);
        return query_zoom_value(db.get(), "SELECT MAX(zoom_level) FROM tiles");
    }();

    const std::filesystem::path file_path = std::filesystem::absolute(mbtiles_path);
    const std::string file_name = file_path.filename().string();

    const std::string metadata_json = build_metadata_json(metadata, tile_count, min_zoom, max_zoom, file_name,
                                                          file_path.string());
    const std::string viewer_page = build_viewer_page(file_name);

    httplib::Server server;

    server.Get("/view", [viewer_page](const httplib::Request &, httplib::Response &res) {
        res.set_content(viewer_page, "text/html; charset=utf-8");
    });

    server.Get("/view/metadata", [metadata_json](const httplib::Request &, httplib::Response &res) {
        res.set_content(metadata_json, "application/json; charset=utf-8");
    });

    server.Get(R"(/view/tiles/(\d+)/(\d+)/(\d+))",
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

