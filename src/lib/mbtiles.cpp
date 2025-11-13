#include "mbtiles.h"

#include "aixlog.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image_resize2.h"


namespace fs = std::filesystem;

namespace mbtiles {

AixLog::Severity to_aixlog_severity(LogLevel level) {
    switch (level) {
        case LogLevel::Trace:
            return AixLog::Severity::trace;
        case LogLevel::DEBUG:
            return AixLog::Severity::debug;
        case LogLevel::INFO:
            return AixLog::Severity::info;
        case LogLevel::WARNING:
            return AixLog::Severity::warning;
        case LogLevel::ERROR:
            return AixLog::Severity::error;
        case LogLevel::FATAL:
            return AixLog::Severity::fatal;
    }
    return AixLog::Severity::warning;
}

struct Logger::Impl {
    LogLevel level = LogLevel::WARNING;

    void apply() {
        AixLog::Filter filter;
        filter.add_filter(to_aixlog_severity(level));
        auto sink = std::make_shared<AixLog::SinkCout>(filter, "[#severity] #message");
        AixLog::Log::init({sink});
    }
};

Logger::Impl &Logger::impl() {
    static Logger::Impl instance;
    static bool initialized = false;
    if (!initialized) {
        instance.apply();
        initialized = true;
    }
    return instance;
}

void Logger::set_level(LogLevel level) {
    auto &state = impl();
    if (state.level == level) {
        return;
    }
    state.level = level;
    state.apply();
}

LogLevel Logger::level() {
    return impl().level;
}



MBTiles::MBTiles() : _db(nullptr), _name("") {
    
}

MBTiles::~MBTiles() {
    close();
}


MBTiles::MBTiles(const std::string& path) : _db(nullptr), _name("") {
    open(path);
}

void MBTiles::close() {
    if (_db != nullptr) {
        sqlite3_close(_db);
    }
    _db = nullptr;
}

void MBTiles::open(const std::string& path) {
    if (path.empty()) {
        throw std::invalid_argument("MBTiles path must not be empty");
    }
    close();
    if (sqlite3_open(path.c_str(), &_db) != SQLITE_OK) {
        std::string message = "Unable to open MBTiles file: " + path;
        if (_db != nullptr) {
            message += ": ";
            message += sqlite3_errmsg(_db);
            sqlite3_close(_db);
        }
        throw mbtiles_error(message);
    }

    const std::filesystem::path file_path = std::filesystem::absolute(path);
    _name = file_path.filename().string();
}

struct stmt_deleter {
    void operator()(sqlite3_stmt *stmt) const noexcept {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }
};



bool equals_ignore_case(const std::string &lhs, const std::string &rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::string detect_extension(const void *data, int size) {
    if (data == nullptr || size <= 0) {
        return ".bin";
    }

    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    if (size >= 8) {
        if (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
            return ".png";
        }
        if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
            return ".jpg";
        }
        if (size >= 12 && bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 && bytes[3] == 0x46 &&
            bytes[8] == 0x57 && bytes[9] == 0x45 && bytes[10] == 0x42 && bytes[11] == 0x50) {
            return ".webp";
        }
    }

    return ".bin";
}

std::string extension_without_dot(const std::string &ext) {
    if (!ext.empty() && ext[0] == '.') {
        return ext.substr(1);
    }
    return ext;
}

std::string ensure_dot_prefixed(const std::string &ext) {
    if (ext.empty() || ext[0] == '.') {
        return ext;
    }
    return "." + ext;
}

std::string normalize_extension_token(std::string value) {
    auto is_not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());

    if (!value.empty() && value.front() == '.') {
        value.erase(value.begin());
    }

    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }

    if (value == "jpeg") {
        return "jpg";
    }
    return value;
}

std::string read_metadata_format_extension(sqlite3 *db) {
    if (db == nullptr) {
        return {};
    }

    const char *query = "SELECT value FROM metadata WHERE name='format' LIMIT 1";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        if (raw_stmt != nullptr) {
            sqlite3_finalize(raw_stmt);
        }
        return {};
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt.get(), 0);
        if (text != nullptr) {
            return normalize_extension_token(reinterpret_cast<const char *>(text));
        }
    }

    return {};
}

double tile_x_to_lon(int x, int z) {
    const double n = static_cast<double>(x) / static_cast<double>(1LL << z);
    return n * 360.0 - 180.0;
}

double tile_y_to_lat(int y, int z) {
    constexpr double kPi = 3.141592653589793238462643383279502884;
    const double n = kPi - 2.0 * kPi * static_cast<double>(y) / static_cast<double>(1LL << z);
    return 180.0 / kPi * std::atan(0.5 * (std::exp(n) - std::exp(-n)));
}

std::string format_decimal(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    return stream.str();
}

std::string leading_digits(long long value, std::size_t count) {
    std::string digits = std::to_string(std::llabs(value));
    if (digits.size() < count) {
        digits = std::string(count - digits.size(), '0') + digits;
    }
    return digits.substr(0, count);
}

std::string leading_digits_from_double(double value, std::size_t count) {
    const long long integer_part = static_cast<long long>(std::floor(std::fabs(value)));
    return leading_digits(integer_part, count);
}

bool token_is(const std::string &token, char expected) {
    return token.size() == 1 && token[0] == expected;
}

bool token_is_repeat_of(const std::string &token, char expected) {
    if (token.empty()) {
        return false;
    }
    for (char ch : token) {
        if (ch != expected) {
            return false;
        }
    }
    return true;
}

std::string format_pattern(int z, int x, int y, const std::string &pattern, const std::string &extension) {
    const double lon = tile_x_to_lon(x, z);
    const double lat = tile_y_to_lat(y, z);

    std::string result;
    result.reserve(pattern.size() + 32);

    for (std::size_t i = 0; i < pattern.size();) {
        const char ch = pattern[i];
        if (ch != '{') {
            result.push_back(ch);
            ++i;
            continue;
        }

        const std::size_t closing = pattern.find('}', i + 1);
        if (closing == std::string::npos) {
            throw mbtiles_error("Unclosed placeholder in pattern: " + pattern);
        }

        const std::string token = pattern.substr(i + 1, closing - i - 1);
        if (token.empty()) {
            throw mbtiles_error("Empty placeholder in pattern: " + pattern);
        }

        std::string replacement;
        if (token_is(token, 'z')) {
            replacement = std::to_string(z);
        } else if (token_is(token, 'x')) {
            replacement = std::to_string(x);
        } else if (token_is(token, 'y')) {
            replacement = std::to_string(y);
        } else if (token_is(token, 'a')) {
            replacement = format_decimal(lat);
        } else if (token_is(token, 'o')) {
            replacement = format_decimal(lon);
        } else if (token_is_repeat_of(token, 'Z')) {
            replacement = leading_digits(z, token.size());
        } else if (token_is_repeat_of(token, 'X')) {
            replacement = leading_digits(x, token.size());
        } else if (token_is_repeat_of(token, 'Y')) {
            replacement = leading_digits(y, token.size());
        } else if (token_is_repeat_of(token, 'A')) {
            replacement = leading_digits_from_double(lat, token.size());
        } else if (token_is_repeat_of(token, 'O')) {
            replacement = leading_digits_from_double(lon, token.size());
        } else if (token == "ext") {
            replacement = extension;
        } else {
            throw mbtiles_error("Unknown placeholder '{" + token + "}' in pattern: " + pattern);
        }

        result += replacement;
        i = closing + 1;
    }

    return result;
}

bool is_supported_image_extension(const fs::path &path) {
    const std::string ext = path.extension().string();
    return equals_ignore_case(ext, ".png") || equals_ignore_case(ext, ".jpg") ||
           equals_ignore_case(ext, ".jpeg");
}





RGBAImage::RGBAImage() {

}

RGBAImage::RGBAImage(const fs::path &path) {
    load(path);
}

RGBAImage::RGBAImage(const unsigned char *data, int size) {
    loadFromMemory(data, size);
}

void RGBAImage::load(const fs::path &path) {
    int components = 0;
    unsigned char *raw = stbi_load(path.string().c_str(), &this->width, &this->height, &components, 4);
    if (!raw) {
        throw mbtiles_error("Failed to load image '" + path.string() + "'");
    }
    this->pixels.assign(raw, raw + static_cast<std::size_t>(this->width) * this->height * 4);
    stbi_image_free(raw);
}

void RGBAImage::loadFromMemory(const unsigned char *data, int size) {
    if (data == nullptr || size <= 0) {
        throw mbtiles_error("Tile image data is empty");
    }

    int components = 0;
    unsigned char *raw = stbi_load_from_memory(data, size, &this->width, &this->height, &components, 4);
    if (raw == nullptr) {
        throw mbtiles_error("Failed to decode image from MBTiles blob");
    }
    this->pixels.assign(raw, raw + static_cast<std::size_t>(this->width) * this->height * 4);
    stbi_image_free(raw);
}

void RGBAImage::save(const fs::path &path) const {
    const std::string ext = path.extension().string();
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            throw mbtiles_error("Failed to create directory '" + path.parent_path().string() + "': " + ec.message());
        }
    }

    /// save png
    if (equals_ignore_case(ext, ".png")) {
        if (stbi_write_png(path.string().c_str(), this->width, this->height, 4, 
                this->pixels.data(), width * 4) == 0) {
            throw mbtiles_error("Failed to write PNG file '" + path.string() + "'");
        }
        return;
    }


    /// save jpg
    if (equals_ignore_case(ext, ".jpg") || equals_ignore_case(ext, ".jpeg")) {
        std::vector<unsigned char> rgb(static_cast<std::size_t>(this->width) * this->height * 3);
        for (int i = 0; i < this->width * this->height; ++i) {
            rgb[i * 3 + 0] = pixels[i * 4 + 0];
            rgb[i * 3 + 1] = pixels[i * 4 + 1];
            rgb[i * 3 + 2] = pixels[i * 4 + 2];
        }
        if (stbi_write_jpg(path.string().c_str(), this->width, this->height, 3, rgb.data(), 90) == 0) {
            throw mbtiles_error("Failed to write JPEG file '" + path.string() + "'");
        }
        return;
    }

    fs::path png_path = path;
    png_path.replace_extension(".png");
    if (stbi_write_png(png_path.string().c_str(), this->width, this->height, 4, 
            this->pixels.data(), width * 4) == 0) {
        throw mbtiles_error("Failed to write PNG file '" + png_path.string() + "'");
    }
}

std::vector<unsigned char> RGBAImage::encodePng() {
    std::vector<unsigned char> buffer;
    buffer.reserve(static_cast<std::size_t>(this->width) * this->height);

    auto callback = [](void *context, void *data_ptr, int size) {
        auto *destination = static_cast<std::vector<unsigned char> *>(context);
        const auto *bytes = static_cast<unsigned char *>(data_ptr);
        destination->insert(destination->end(), bytes, bytes + size);
    };

    if (stbi_write_png_to_func(callback, &buffer, this->width, this->height, 4, this->pixels.data(), this->width * 4) == 0) {
        throw mbtiles_error("Failed to encode tile as PNG");
    }

    return buffer;
}

void RGBAImage::toGrayScale() {
    if (pixels.empty()) {
        return;
    }
    const std::size_t total_pixels = pixels.size() / 4;
    for (std::size_t i = 0; i < total_pixels; ++i) {
        const std::size_t index = i * 4;
        const unsigned char r = pixels[index + 0];
        const unsigned char g = pixels[index + 1];
        const unsigned char b = pixels[index + 2];
        const unsigned char gray = static_cast<unsigned char>(0.299 * r + 0.587 * g + 0.114 * b);
        pixels[index + 0] = gray;
        pixels[index + 1] = gray;
        pixels[index + 2] = gray;
    }
}

int tms_to_xyz_y(int tms_y, int z) {
    const auto value = static_cast<long long>((1LL << z) - 1) - static_cast<long long>(tms_y);
    if (value < 0 || value > std::numeric_limits<int>::max()) {
        throw mbtiles_error("Tile row is outside representable range for zoom level " + std::to_string(z));
    }
    return static_cast<int>(value);
}

int xyz_to_tms_y(int xyz_y, int z) {
    const long long max_value = (1LL << z) - 1;
    const long long result = max_value - static_cast<long long>(xyz_y);
    if (result < 0 || result > std::numeric_limits<int>::max()) {
        throw mbtiles_error("Tile row is outside representable range for zoom level " + std::to_string(z));
    }
    return static_cast<int>(result);
}

std::vector<int> collect_zoom_levels(sqlite3 *db) {
    const char *query = "SELECT DISTINCT zoom_level FROM tiles ORDER BY zoom_level";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db, query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to enumerate zoom levels: " + std::string(sqlite3_errmsg(db)));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    std::vector<int> levels;
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading zoom levels: " + std::string(sqlite3_errmsg(db)));
        }
        levels.push_back(sqlite3_column_int(stmt.get(), 0));
    }
    return levels;
}


/*
TileMap MBTiles::loadLevelRgba(int zoom) {
    const char *query = "SELECT tile_column, tile_row, tile_data FROM tiles WHERE zoom_level=?";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(_db, query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to read tiles for zoom level " + std::to_string(zoom) + ": " +
        std::string(sqlite3_errmsg(_db)));
    }
    
    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    TileMap tiles;
    
    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading tiles: " + std::string(sqlite3_errmsg(_db)));
        }
        
        const int x = sqlite3_column_int(stmt.get(), 0);
        const int tms_y = sqlite3_column_int(stmt.get(), 1);
        const void *blob = sqlite3_column_blob(stmt.get(), 2);
        const int blob_size = sqlite3_column_bytes(stmt.get(), 2);
        
        const int y = tms_to_xyz_y(tms_y, zoom);
        RGBAImage image(static_cast<const unsigned char *>(blob), blob_size);
        tiles.emplace(make_tile_key(x, y), std::move(image));
    }
    
    return tiles;
}
*/

// TileMap downsample_level(const TileMap &source_tiles, int source_zoom) {
    //     struct ParentGroup {
        //         std::array<bool, 4> present = {false, false, false, false};
        //         std::array<image_data, 4> images;
        //     };
        
//     std::unordered_map<TileKey, ParentGroup> groups;
//     for (const auto &entry : source_tiles) {
//         const int x = tile_key_x(entry.first);
//         const int y = tile_key_y(entry.first);
//         const int parent_x = x / 2;
//         const int parent_y = y / 2;
//         const int idx = (y % 2) * 2 + (x % 2);

//         auto &group = groups[make_tile_key(parent_x, parent_y)];
//         group.present[idx] = true;
//         group.images[idx] = entry.second;
//     }

//     TileMap result;
//     for (const auto &item : groups) {
//         const ParentGroup &group = item.second;
//         if (!std::all_of(group.present.begin(), group.present.end(), [](bool value) { return value; })) {
//             continue;
//         }

//         const int child_width = group.images[0].width;
//         const int child_height = group.images[0].height;
//         if (child_width <= 0 || child_height <= 0) {
//             continue;
//         }

//         bool consistent = true;
//         for (const auto &img : group.images) {
//             if (img.width != child_width || img.height != child_height) {
//                 consistent = false;
//                 break;
//             }
//         }
//         if (!consistent) {
//             continue;
//         }

//         const int canvas_width = child_width * 2;
//         const int canvas_height = child_height * 2;
//         std::vector<unsigned char> canvas(static_cast<std::size_t>(canvas_width) * canvas_height * 4, 0);

//         for (std::size_t idx = 0; idx < group.images.size(); ++idx) {
//             const int offset_x = static_cast<int>(idx % 2) * child_width;
//             const int offset_y = static_cast<int>(idx / 2) * child_height;
//             const image_data &img = group.images[idx];

//             for (int row = 0; row < child_height; ++row) {
//                 unsigned char *dest = canvas.data() + ((offset_y + row) * canvas_width + offset_x) * 4;
//                 const unsigned char *src = img.pixels.data() + static_cast<std::size_t>(row) * child_width * 4;
//                 std::memcpy(dest, src, static_cast<std::size_t>(child_width) * 4);
//             }
//         }

//         std::vector<unsigned char> resized(static_cast<std::size_t>(child_width) * child_height * 4, 0);
//         if (!stbir_resize_uint8_linear(canvas.data(), canvas_width, canvas_height, canvas_width * 4, resized.data(),
//                                        child_width, child_height, child_width * 4, STBIR_RGBA)) {
//             throw mbtiles_error("Failed to downsample tiles at zoom " + std::to_string(source_zoom));
//         }

//         image_data parent_image;
//         parent_image.width = child_width;
//         parent_image.height = child_height;
//         parent_image.pixels = std::move(resized);

//         result.emplace(item.first, std::move(parent_image));
//     }

//     return result;
// }

// }  // namespace

std::size_t MBTiles::extract(const std::string& output_directory, const std::string& pattern) const {
    namespace fs = std::filesystem;

    // Resolve output root directory
    fs::path output_root = output_directory.empty() ? fs::current_path() : fs::path(output_directory);
    std::error_code ec;
    fs::create_directories(output_root, ec);
    if (ec) {
        throw mbtiles_error("Failed to create output directory '" + output_root.string() + "': " + ec.message());
    }

    // Create tile iterator
    TileIterator iter(_db);

    std::size_t count = 0;
    while (auto tile = iter.next()) {
        // Determine file extension for output
        std::string extension_token = tile->extension; // already normalized (no dot)

        // Format output path using pattern (e.g., "{z}/{x}/{y}.png")
        std::string relative_path = format_pattern(
            tile->zoom,
            tile->x,
            tile->y,           // XYZ Y (already converted from TMS)
            pattern,
            extension_token
        );
        fs::path output_path = output_root / fs::path(relative_path);

        // Ensure file has proper extension if missing
        if (output_path.extension().empty() && !extension_token.empty()) {
            // Reconstruct dot-prefixed extension (e.g., "png" → ".png")
            std::string dot_ext = "." + extension_token;
            output_path += dot_ext;
        }

        // Create parent directories
        fs::create_directories(output_path.parent_path(), ec);
        if (ec) {
            throw mbtiles_error("Failed to create directory '" + output_path.parent_path().string() + "': " + ec.message());
        }

        // Write tile data to file
        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            throw mbtiles_error("Failed to open output file '" + output_path.string() + "'");
        }

        if (!tile->data.empty()) {
            file.write(
                reinterpret_cast<const char*>(tile->data.data()),
                static_cast<std::streamsize>(tile->data.size())
            );
        }

        if (!file) {
            throw mbtiles_error("Failed to write tile to '" + output_path.string() + "'");
        }

        ++count;
        if (count % 100 == 0) {
            LOG(INFO) << "Extracted " << count << " tiles...";
        }
    }

    LOG(INFO) << "Extraction completed. Total tiles: " << count;
    return count;
}

// void convert_directory_to_grayscale(const std::string &input_directory, const std::string &output_directory,
//                                     const GrayscaleOptions &options) {
//     fs::path input_root = input_directory;
//     fs::path output_root = output_directory;

//     if (!fs::exists(input_root)) {
//         throw mbtiles_error("Input directory does not exist: " + input_root.string());
//     }
//     if (!fs::is_directory(input_root)) {
//         throw mbtiles_error("Input path is not a directory: " + input_root.string());
//     }

//     std::error_code ec;
//     fs::create_directories(output_root, ec);
//     if (ec) {
//         throw mbtiles_error("Failed to create output directory '" + output_root.string() + "': " + ec.message());
//     }

//     auto process_entry = [&](const fs::directory_entry &entry) {
//         if (!entry.is_regular_file()) {
//             return;
//         }
//         if (!is_supported_image_extension(entry.path())) {
//             return;
//         }

//         fs::path relative = entry.path().lexically_relative(input_root);
//         if (relative.empty()) {
//             std::error_code rel_ec;
//             relative = fs::relative(entry.path(), input_root, rel_ec);
//             if (rel_ec) {
//                 throw mbtiles_error("Failed to determine relative path for '" + entry.path().string() + "': " + rel_ec.message());
//             }
//         }

//         fs::path destination = output_root / relative;

//         image_data image = load_image_rgba(entry.path());
//         convert_pixels_to_grayscale(image.pixels);
//         save_image_rgba(destination, image.pixels.data(), image.width, image.height);

//         LOG(INFO) << "Converted " << entry.path() << " -> " << destination;
//     };

//     if (options.recursive) {
//         for (const auto &entry : fs::recursive_directory_iterator(input_root)) {
//             process_entry(entry);
//         }
//     } else {
//         for (const auto &entry : fs::directory_iterator(input_root)) {
//             process_entry(entry);
//         }
//     }
// }

// std::vector<int> list_zoom_levels(const std::string &mbtiles_path) {
//     auto db = open_database(mbtiles_path);
//     return collect_zoom_levels(db.get());
// }

// TileMap upsample_level(const TileMap &source_tiles, int source_zoom) {
//     TileMap result;
//     for (const auto &entry : source_tiles) {
//         const int parent_x = tile_key_x(entry.first);
//         const int parent_y = tile_key_y(entry.first);
//         const image_data &image = entry.second;

//         if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
//             continue;
//         }

//         const int child_width = image.width;
//         const int child_height = image.height;
//         const int expanded_width = child_width * 2;
//         const int expanded_height = child_height * 2;

//         std::vector<unsigned char> expanded(static_cast<std::size_t>(expanded_width) * expanded_height * 4, 0);
//         if (!stbir_resize_uint8_linear(image.pixels.data(), child_width, child_height, child_width * 4, expanded.data(),
//                                        expanded_width, expanded_height, expanded_width * 4, STBIR_RGBA)) {
//             throw mbtiles_error("Failed to upsample tiles at zoom " + std::to_string(source_zoom));
//         }

//         for (int dy = 0; dy < 2; ++dy) {
//             for (int dx = 0; dx < 2; ++dx) {
//                 image_data child;
//                 child.width = child_width;
//                 child.height = child_height;
//                 child.pixels.resize(static_cast<std::size_t>(child_width) * child_height * 4, 0);

//                 for (int row = 0; row < child_height; ++row) {
//                     const unsigned char *src =
//                         expanded.data() + ((dy * child_height + row) * expanded_width + dx * child_width) * 4;
//                     unsigned char *dest =
//                         child.pixels.data() + static_cast<std::size_t>(row) * child_width * 4;
//                     std::memcpy(dest, src, static_cast<std::size_t>(child_width) * 4);
//                 }

//                 const int child_x = parent_x * 2 + dx;
//                 const int child_y = parent_y * 2 + dy;
//                 result.emplace(make_tile_key(child_x, child_y), std::move(child));
//             }
//         }
//     }

//     return result;
// }

// void resize_zoom_levels(const std::string &input_mbtiles, const std::string &output_path,
//                         const ResizeOptions &options) {
//     auto db = open_database(input_mbtiles);
//     const std::vector<int> available_levels = collect_zoom_levels(db.get());
//     if (available_levels.empty()) {
//         throw mbtiles_error("No tiles found in MBTiles archive: " + input_mbtiles);
//     }

//     std::vector<int> requested_levels = options.target_levels;
//     const int min_zoom_available = *std::min_element(available_levels.begin(), available_levels.end());
//     const int max_zoom_available = *std::max_element(available_levels.begin(), available_levels.end());

//     if (requested_levels.empty()) {
//         if (min_zoom_available <= 0) {
//             throw mbtiles_error("Cannot generate a lower zoom level because minimum zoom is " +
//                                 std::to_string(min_zoom_available));
//         }
//         requested_levels.push_back(min_zoom_available - 1);
//     }

//     std::vector<int> unique_levels;
//     std::unordered_set<int> seen_levels;
//     for (int level : requested_levels) {
//         if (level < 0) {
//             throw mbtiles_error("Zoom levels must be non-negative after CLI normalization");
//         }
//         if (seen_levels.insert(level).second) {
//             unique_levels.push_back(level);
//         }
//     }
//     requested_levels = std::move(unique_levels);

//     std::unordered_set<int> available_set(available_levels.begin(), available_levels.end());

//     std::vector<int> copy_levels;
//     std::vector<int> generated_levels;
//     for (int level : requested_levels) {
//         if (available_set.count(level) > 0) {
//             copy_levels.push_back(level);
//         } else {
//             generated_levels.push_back(level);
//         }
//     }

//     std::sort(copy_levels.begin(), copy_levels.end());
//     std::sort(generated_levels.begin(), generated_levels.end());

//     fs::path destination_path = output_path;
//     bool output_is_directory = false;
//     if (fs::exists(destination_path)) {
//         if (fs::is_directory(destination_path)) {
//             output_is_directory = true;
//         } else {
//             if (!equals_ignore_case(destination_path.extension().string(), ".mbtiles")) {
//                 throw mbtiles_error("Output path must be a directory or a .mbtiles file");
//             }
//         }
//     } else {
//         if (destination_path.has_extension()) {
//             if (!equals_ignore_case(destination_path.extension().string(), ".mbtiles")) {
//                 throw mbtiles_error("Only directories or .mbtiles files are supported as resize outputs");
//             }
//         } else {
//             output_is_directory = true;
//         }
//     }

//     if (output_is_directory) {
//         std::error_code ec;
//         fs::create_directories(destination_path, ec);
//         if (ec) {
//             throw mbtiles_error("Failed to create output directory '" + destination_path.string() + "': " + ec.message());
//         }
//     } else if (destination_path.has_parent_path()) {
//         std::error_code ec;
//         fs::create_directories(destination_path.parent_path(), ec);
//         if (ec) {
//             throw mbtiles_error("Failed to create parent directory '" + destination_path.parent_path().string() + "': " +
//                                 ec.message());
//         }
//     }

//     const std::string metadata_extension = read_metadata_format_extension(db.get());

//     std::unordered_map<int, TileMap> tile_cache;

//     auto apply_grayscale_to_tiles = [&](TileMap &tiles) {
//         if (!options.grayscale) {
//             return;
//         }
//         for (auto &entry : tiles) {
//             convert_pixels_to_grayscale(entry.second.pixels);
//         }
//     };

//     auto ensure_level_tiles = [&](auto &&self, int level) -> TileMap & {
//         auto cache_it = tile_cache.find(level);
//         if (cache_it != tile_cache.end()) {
//             return cache_it->second;
//         }

//         if (available_set.count(level) > 0) {
//             TileMap tiles = load_level_rgba(db.get(), level);
//             auto inserted = tile_cache.emplace(level, std::move(tiles));
//             return inserted.first->second;
//         }

//         TileMap generated;
//         if (level < min_zoom_available) {
//             TileMap &parent_tiles = self(self, level + 1);
//             generated = downsample_level(parent_tiles, level + 1);
//         } else if (level > max_zoom_available) {
//             TileMap &source_tiles = self(self, level - 1);
//             generated = upsample_level(source_tiles, level - 1);
//         } else {
//             bool created = false;
//             if (level + 1 <= max_zoom_available || available_set.count(level + 1) > 0 ||
//                 tile_cache.count(level + 1) > 0) {
//                 TileMap &parent_tiles = self(self, level + 1);
//                 generated = downsample_level(parent_tiles, level + 1);
//                 created = true;
//             }
//             if (!created) {
//                 if (level == 0) {
//                     throw mbtiles_error("Unable to generate zoom level " + std::to_string(level));
//                 }
//                 TileMap &source_tiles = self(self, level - 1);
//                 generated = upsample_level(source_tiles, level - 1);
//             }
//         }

//         apply_grayscale_to_tiles(generated);
//         auto inserted = tile_cache.emplace(level, std::move(generated));
//         return inserted.first->second;
//     };

//     for (int level : generated_levels) {
//         ensure_level_tiles(ensure_level_tiles, level);
//     }

//     if (!generated_levels.empty()) {
//         std::ostringstream message;
//         message << "Generated zoom levels:";
//         for (int level : generated_levels) {
//             message << ' ' << level;
//         }
//         LOG(INFO) << message.str();
//     }

//     if (output_is_directory) {
//         const fs::path base = destination_path;

//         auto write_blob_tile = [&](int z, int x, int tms_y, const void *blob, int blob_size) {
//             const int y = tms_to_xyz_y(tms_y, z);
//             const bool has_metadata_extension = !metadata_extension.empty();
//             std::string detected_extension;
//             if (!has_metadata_extension) {
//                 detected_extension = detect_extension(blob, blob_size);
//             }
//             const std::string extension_token = has_metadata_extension ? metadata_extension
//                                                                        : extension_without_dot(detected_extension);
//             std::string relative_path = format_pattern(z, x, y, options.pattern, extension_token);
//             fs::path output_file = base / fs::path(relative_path);

//             const std::string extension = has_metadata_extension ? ensure_dot_prefixed(metadata_extension)
//                                                                  : detected_extension;
//             if (output_file.extension().empty() && !extension.empty()) {
//                 output_file += extension;
//             }

//             if (options.grayscale) {
//                 if (blob == nullptr || blob_size <= 0) {
//                     throw mbtiles_error("Tile data is empty; cannot convert to grayscale");
//                 }
//                 image_data image =
//                     load_image_rgba_from_memory(static_cast<const unsigned char *>(blob), blob_size);
//                 convert_pixels_to_grayscale(image.pixels);
//                 fs::path destination = output_file;
//                 const std::string current_extension = destination.extension().string();
//                 if (current_extension.empty() ||
//                     (!equals_ignore_case(current_extension, ".png") && !equals_ignore_case(current_extension, ".jpg") &&
//                      !equals_ignore_case(current_extension, ".jpeg"))) {
//                     destination.replace_extension(".png");
//                 }
//                 save_image_rgba(destination, image.pixels.data(), image.width, image.height);
//                 return;
//             }

//             std::error_code ec;
//             fs::create_directories(output_file.parent_path(), ec);
//             if (ec) {
//                 throw mbtiles_error("Failed to create directory '" + output_file.parent_path().string() + "': " +
//                                     ec.message());
//             }

//             std::ofstream file(output_file, std::ios::binary);
//             if (!file) {
//                 throw mbtiles_error("Failed to open output file '" + output_file.string() + "'");
//             }
//             if (blob != nullptr && blob_size > 0) {
//                 file.write(static_cast<const char *>(blob), blob_size);
//             }
//             if (!file) {
//                 throw mbtiles_error("Failed to write tile to '" + output_file.string() + "'");
//             }
//         };

//         for (int level : copy_levels) {
//             sqlite3_stmt *raw_stmt = nullptr;
//             const char *select_sql = "SELECT tile_column, tile_row, tile_data FROM tiles WHERE zoom_level=?";
//             if (sqlite3_prepare_v2(db.get(), select_sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
//                 throw mbtiles_error("Failed to read tiles for zoom level " + std::to_string(level) + ": " +
//                                     std::string(sqlite3_errmsg(db.get())));
//             }

//             std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
//             while (true) {
//                 const int rc = sqlite3_step(stmt.get());
//                 if (rc == SQLITE_DONE) {
//                     break;
//                 }
//                 if (rc != SQLITE_ROW) {
//                     throw mbtiles_error("SQLite error while reading tiles: " + std::string(sqlite3_errmsg(db.get())));
//                 }

//                 const int x = sqlite3_column_int(stmt.get(), 0);
//                 const int tms_y = sqlite3_column_int(stmt.get(), 1);
//                 const void *blob = sqlite3_column_blob(stmt.get(), 2);
//                 const int blob_size = sqlite3_column_bytes(stmt.get(), 2);
//                 write_blob_tile(level, x, tms_y, blob, blob_size);
//             }
//         }

//         for (int level : generated_levels) {
//             auto &tiles = ensure_level_tiles(ensure_level_tiles, level);
//             for (const auto &entry : tiles) {
//                 const int x = tile_key_x(entry.first);
//                 const int y = tile_key_y(entry.first);
//                 const std::string extension_token = "png";
//                 std::string relative_path = format_pattern(level, x, y, options.pattern, extension_token);
//                 fs::path output_file = base / fs::path(relative_path);
//                 if (output_file.extension().empty()) {
//                     output_file += ".png";
//                 }
//                 save_image_rgba(output_file, entry.second.pixels.data(), entry.second.width, entry.second.height);
//             }
//         }
//         return;
//     }

//     // MBTiles output
//     sqlite3 *raw_out_db = nullptr;
//     if (sqlite3_open(destination_path.string().c_str(), &raw_out_db) != SQLITE_OK) {
//         std::string message = "Unable to create output MBTiles file: " + destination_path.string();
//         if (raw_out_db != nullptr) {
//             message += ": ";
//             message += sqlite3_errmsg(raw_out_db);
//             sqlite3_close(raw_out_db);
//         }
//         throw mbtiles_error(message);
//     }
//     std::unique_ptr<sqlite3, sqlite_deleter> out_db(raw_out_db);

//     const char *create_tiles_sql =
//         "CREATE TABLE IF NOT EXISTS tiles (zoom_level INTEGER, tile_column INTEGER, tile_row INTEGER, tile_data BLOB)";
//     if (sqlite3_exec(out_db.get(), create_tiles_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
//         throw mbtiles_error("Failed to create tiles table: " + std::string(sqlite3_errmsg(out_db.get())));
//     }
//     const char *index_sql =
//         "CREATE UNIQUE INDEX IF NOT EXISTS tiles_index ON tiles (zoom_level, tile_column, tile_row)";
//     if (sqlite3_exec(out_db.get(), index_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
//         throw mbtiles_error("Failed to create tiles index: " + std::string(sqlite3_errmsg(out_db.get())));
//     }

//     if (sqlite3_exec(out_db.get(), "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK) {
//         throw mbtiles_error("Failed to begin transaction: " + std::string(sqlite3_errmsg(out_db.get())));
//     }

//     sqlite3_stmt *insert_stmt_raw = nullptr;
//     const char *insert_sql = "INSERT OR REPLACE INTO tiles(zoom_level, tile_column, tile_row, tile_data) VALUES(?1, ?2, ?3, ?4)";
//     if (sqlite3_prepare_v2(out_db.get(), insert_sql, -1, &insert_stmt_raw, nullptr) != SQLITE_OK) {
//         sqlite3_exec(out_db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
//         throw mbtiles_error("Failed to prepare insert statement: " + std::string(sqlite3_errmsg(out_db.get())));
//     }
//     std::unique_ptr<sqlite3_stmt, stmt_deleter> insert_stmt(insert_stmt_raw);

//     auto insert_tile = [&](int z, int x, int tms_y, const void *data_ptr, int data_size) {
//         sqlite3_reset(insert_stmt.get());
//         sqlite3_clear_bindings(insert_stmt.get());
//         sqlite3_bind_int(insert_stmt.get(), 1, z);
//         sqlite3_bind_int(insert_stmt.get(), 2, x);
//         sqlite3_bind_int(insert_stmt.get(), 3, tms_y);
//         sqlite3_bind_blob(insert_stmt.get(), 4, data_ptr, data_size, SQLITE_TRANSIENT);
//         const int rc = sqlite3_step(insert_stmt.get());
//         if (rc != SQLITE_DONE) {
//             throw mbtiles_error("Failed to write tile to output MBTiles: " + std::string(sqlite3_errmsg(out_db.get())));
//         }
//     };

//     for (int level : copy_levels) {
//         sqlite3_stmt *raw_stmt = nullptr;
//         const char *select_sql = "SELECT tile_column, tile_row, tile_data FROM tiles WHERE zoom_level=?";
//         if (sqlite3_prepare_v2(db.get(), select_sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
//             sqlite3_exec(out_db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
//             throw mbtiles_error("Failed to read tiles for zoom level " + std::to_string(level) + ": " +
//                                 std::string(sqlite3_errmsg(db.get())));
//         }

//         std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
//         while (true) {
//             const int rc = sqlite3_step(stmt.get());
//             if (rc == SQLITE_DONE) {
//                 break;
//             }
//             if (rc != SQLITE_ROW) {
//                 sqlite3_exec(out_db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
//                 throw mbtiles_error("SQLite error while reading tiles: " + std::string(sqlite3_errmsg(db.get())));
//             }

//             const int x = sqlite3_column_int(stmt.get(), 0);
//             const int tms_y = sqlite3_column_int(stmt.get(), 1);
//             const void *blob = sqlite3_column_blob(stmt.get(), 2);
//             const int blob_size = sqlite3_column_bytes(stmt.get(), 2);
//             if (options.grayscale) {
//                 if (blob == nullptr || blob_size <= 0) {
//                     throw mbtiles_error("Tile data is empty; cannot convert to grayscale");
//                 }
//                 image_data image =
//                     load_image_rgba_from_memory(static_cast<const unsigned char *>(blob), blob_size);
//                 convert_pixels_to_grayscale(image.pixels);
//                 const std::vector<unsigned char> png =
//                     encode_png_rgba(image.pixels.data(), image.width, image.height);
//                 insert_tile(level, x, tms_y, png.data(), static_cast<int>(png.size()));
//             } else {
//                 insert_tile(level, x, tms_y, blob, blob_size);
//             }
//         }
//     }

//     for (int level : generated_levels) {
//         auto &tiles = ensure_level_tiles(ensure_level_tiles, level);
//         for (const auto &entry : tiles) {
//             const int x = tile_key_x(entry.first);
//             const int y = tile_key_y(entry.first);
//             const int tms_y = xyz_to_tms_y(y, level);
//             const std::vector<unsigned char> png =
//                 encode_png_rgba(entry.second.pixels.data(), entry.second.width, entry.second.height);
//             insert_tile(level, x, tms_y, png.data(), static_cast<int>(png.size()));
//         }
//     }

//     if (sqlite3_exec(out_db.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
//         throw mbtiles_error("Failed to commit tile data: " + std::string(sqlite3_errmsg(out_db.get())));
//     }

//     out_db.reset();

//     auto metadata = read_metadata(input_mbtiles);
//     if (!requested_levels.empty()) {
//         const int min_level = *std::min_element(requested_levels.begin(), requested_levels.end());
//         const int max_level_out = *std::max_element(requested_levels.begin(), requested_levels.end());
//         metadata["minzoom"] = std::to_string(min_level);
//         metadata["maxzoom"] = std::to_string(max_level_out);
//     }
//     if (options.grayscale) {
//         metadata["format"] = "png";
//     }
//     write_metadata_entries(destination_path.string(), metadata, true);
// }

std::map<std::string, std::string> MBTiles::metadata() const{
    const char *query = "SELECT name, value FROM metadata ORDER BY name";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(_db, query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to read metadata: " + std::string(sqlite3_errmsg(_db)));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    std::map<std::string, std::string> result;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading metadata: " + std::string(sqlite3_errmsg(_db)));
        }

        const unsigned char *name_text = sqlite3_column_text(stmt.get(), 0);
        const unsigned char *value_text = sqlite3_column_text(stmt.get(), 1);
        const std::string name = name_text != nullptr ? reinterpret_cast<const char *>(name_text) : std::string();
        const std::string value = value_text != nullptr ? reinterpret_cast<const char *>(value_text) : std::string();
        result.emplace(name, value);
    }

    return result;
}

void MBTiles::setMetadata(const std::map<std::string, std::string> &entries,
                            bool overwrite_existing) {
    if (entries.empty()) {
        return;
    }

    const char *create_sql = "CREATE TABLE IF NOT EXISTS metadata (name TEXT PRIMARY KEY, value TEXT)";
    if (sqlite3_exec(_db, create_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to ensure metadata table exists: " + std::string(sqlite3_errmsg(_db)));
    }

    if (sqlite3_exec(_db, "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to begin transaction: " + std::string(sqlite3_errmsg(_db)));
    }

    const char *insert_sql = overwrite_existing
                                 ? "INSERT INTO metadata(name, value) VALUES(?1, ?2) ON CONFLICT(name) DO UPDATE SET value=excluded.value"
                                 : "INSERT INTO metadata(name, value) VALUES(?1, ?2)";

    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(_db, insert_sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(_db, "ROLLBACK", nullptr, nullptr, nullptr);
        throw mbtiles_error("Failed to prepare metadata statement: " + std::string(sqlite3_errmsg(_db)));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);

    for (const auto &entry : entries) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        sqlite3_bind_text(stmt.get(), 1, entry.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.get(), 2, entry.second.c_str(), -1, SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            sqlite3_exec(_db, "ROLLBACK", nullptr, nullptr, nullptr);
            throw mbtiles_error("Failed to write metadata entry '" + entry.first + "': " +
                                std::string(sqlite3_errmsg(_db)));
        }
    }

    if (sqlite3_exec(_db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to commit metadata changes: " + std::string(sqlite3_errmsg(_db)));
    }
}

void MBTiles::setMetadata(const std::string &key, const std::string &value,
                    bool overwrite_existing) {
    std::map<std::string, std::string> entries({{key, value}});
    setMetadata(entries, overwrite_existing);
}

std::vector<std::string> MBTiles::metadataKeys() const {
    const char *query = "SELECT name FROM metadata ORDER BY name";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(_db, query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to read metadata keys: " + std::string(sqlite3_errmsg(_db)));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    std::vector<std::string> keys;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading metadata keys: " + std::string(sqlite3_errmsg(_db)));
        }

        const unsigned char *name_text = sqlite3_column_text(stmt.get(), 0);
        keys.emplace_back(name_text != nullptr ? reinterpret_cast<const char *>(name_text) : "");
    }

    return keys;
}




// Helper: convert Web Mercator tile (z, x, y) to latitude/longitude of NW corner
// Returns (lat, lon) for the top-left (northwest) corner of the tile
std::pair<double, double> tile2latlon(const TileInfo& tile) {
    const double n = std::pow(2.0, tile.zoom);
    const double lon_deg = tile.x / n * 360.0 - 180.0;
    const double lat_rad = std::atan(std::sinh(M_PI * (1 - 2.0 * tile.y / n)));
    const double lat_deg = lat_rad * 180.0 / M_PI;
    return {lat_deg, lon_deg};
}
std::pair<double, double> tile2latlon(int z, int x, int y) {
    TileInfo tile;
    tile.zoom = z;
    tile.x = x;
    tile.x = y;
    return tile2latlon(tile);
} // anonymous namespace

double TileInfo::latMin() const {
    // Bottom edge = y+1
    return tile2latlon(zoom, x, y + 1).first;
}

double TileInfo::latMax() const {
    // Top edge = y

    return tile2latlon(zoom, x, y).first;
}

double TileInfo::lonMin() const {
    // Left edge = x
    return tile2latlon(zoom, x, y).second;
}

double TileInfo::lonMax() const {
    // Right edge = x+1
    return tile2latlon(zoom, x + 1, y).second;
}




TileIterator::TileIterator(sqlite3* db) : _db(db) {
    if (!_db) {
        throw std::invalid_argument("Database handle is null");
    }
    _metadata_ext = read_metadata_format_extension(_db);
}

TileIterator::~TileIterator() {
    if (_stmt) {
        sqlite3_finalize(_stmt);
    }
}

std::optional<TileInfo> TileIterator::next() {
    if (!_started) {
        const char* query = "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles";
        int rc = sqlite3_prepare_v2(_db, query, -1, &_stmt, nullptr);
        if (rc != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare tile query: " + std::string(sqlite3_errmsg(_db)));
        }
        _started = true;
    }

    int rc = sqlite3_step(_stmt);
    if (rc == SQLITE_DONE) {
        return std::nullopt;
    }
    if (rc != SQLITE_ROW) {
        throw std::runtime_error("SQLite error during step: " + std::string(sqlite3_errmsg(_db)));
    }

    const int z = sqlite3_column_int(_stmt, 0);
    const int x = sqlite3_column_int(_stmt, 1);
    const int tms_y = sqlite3_column_int(_stmt, 2);

    if (z < 0 || z >= 63) {
        throw std::runtime_error("Unsupported zoom level: " + std::to_string(z));
    }

    const long long xyz_y_ll = (1LL << z) - 1 - static_cast<long long>(tms_y);
    if (xyz_y_ll < std::numeric_limits<int>::min() || xyz_y_ll > std::numeric_limits<int>::max()) {
        throw std::runtime_error("Y coordinate out of int range at zoom " + std::to_string(z));
    }
    const int xyz_y = static_cast<int>(xyz_y_ll);

    const void* blob = sqlite3_column_blob(_stmt, 3);
    const int blob_size = sqlite3_column_bytes(_stmt, 3);

    std::string ext;
    const bool has_meta_ext = !_metadata_ext.empty();
    if (has_meta_ext) {
        ext = _metadata_ext;
    } else {
        ext = detect_extension(blob, blob_size);
        ext = extension_without_dot(ext);
    }

    std::vector<std::byte> data;
    if (blob && blob_size > 0) {
        data.resize(blob_size);
        std::memcpy(data.data(), blob, blob_size);
    }

    return TileInfo{
        z,
        x,
        xyz_y,
        tms_y,
        std::move(data),
        ext
    };
}

}  // namespace mbtiles

