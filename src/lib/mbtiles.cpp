#include "mbtiles.h"

#include "sqlite3.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
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

namespace mbtiles {
namespace {
namespace fs = std::filesystem;

struct sqlite_deleter {
    void operator()(sqlite3 *db) const noexcept {
        if (db != nullptr) {
            sqlite3_close(db);
        }
    }
};

std::unique_ptr<sqlite3, sqlite_deleter> open_database(const std::string &path) {
    sqlite3 *raw_db = nullptr;
    if (sqlite3_open(path.c_str(), &raw_db) != SQLITE_OK) {
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

struct stmt_deleter {
    void operator()(sqlite3_stmt *stmt) const noexcept {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
    }
};

struct image_data {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
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

std::string format_pattern(int z, int x, int y, const std::string &pattern) {
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
        } else if (token_is(token, 't')) {
            replacement = format_decimal(lat);
        } else if (token_is(token, 'n')) {
            replacement = format_decimal(lon);
        } else if (token_is_repeat_of(token, 'Z')) {
            replacement = leading_digits(z, token.size());
        } else if (token_is_repeat_of(token, 'X')) {
            replacement = leading_digits(x, token.size());
        } else if (token_is_repeat_of(token, 'Y')) {
            replacement = leading_digits(y, token.size());
        } else if (token_is_repeat_of(token, 'T')) {
            replacement = leading_digits_from_double(lat, token.size());
        } else if (token_is_repeat_of(token, 'N')) {
            replacement = leading_digits_from_double(lon, token.size());
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

image_data load_image_rgba(const fs::path &path) {
    image_data image;
    int components = 0;
    unsigned char *raw = stbi_load(path.string().c_str(), &image.width, &image.height, &components, 4);
    if (!raw) {
        throw mbtiles_error("Failed to load image '" + path.string() + "'");
    }
    image.pixels.assign(raw, raw + static_cast<std::size_t>(image.width) * image.height * 4);
    stbi_image_free(raw);
    return image;
}

void save_image_rgba(const fs::path &path, const unsigned char *data, int width, int height) {
    const std::string ext = path.extension().string();
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            throw mbtiles_error("Failed to create directory '" + path.parent_path().string() + "': " + ec.message());
        }
    }

    if (equals_ignore_case(ext, ".png")) {
        if (stbi_write_png(path.string().c_str(), width, height, 4, data, width * 4) == 0) {
            throw mbtiles_error("Failed to write PNG file '" + path.string() + "'");
        }
        return;
    }

    if (equals_ignore_case(ext, ".jpg") || equals_ignore_case(ext, ".jpeg")) {
        std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * height * 3);
        for (int i = 0; i < width * height; ++i) {
            rgb[i * 3 + 0] = data[i * 4 + 0];
            rgb[i * 3 + 1] = data[i * 4 + 1];
            rgb[i * 3 + 2] = data[i * 4 + 2];
        }
        if (stbi_write_jpg(path.string().c_str(), width, height, 3, rgb.data(), 90) == 0) {
            throw mbtiles_error("Failed to write JPEG file '" + path.string() + "'");
        }
        return;
    }

    fs::path png_path = path;
    png_path.replace_extension(".png");
    if (stbi_write_png(png_path.string().c_str(), width, height, 4, data, width * 4) == 0) {
        throw mbtiles_error("Failed to write PNG file '" + png_path.string() + "'");
    }
}

void convert_pixels_to_grayscale(std::vector<unsigned char> &pixels) {
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

}  // namespace

std::size_t extract(const std::string &mbtiles_path, const ExtractOptions &options) {
    auto db = open_database(mbtiles_path);

    fs::path output_root = options.output_directory.empty() ? fs::current_path() : fs::path(options.output_directory);
    std::error_code ec;
    fs::create_directories(output_root, ec);
    if (ec) {
        throw mbtiles_error("Failed to create output directory '" + output_root.string() + "': " + ec.message());
    }

    sqlite3_stmt *raw_stmt = nullptr;
    const char *query = "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles";
    if (sqlite3_prepare_v2(db.get(), query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to prepare tile query: " + std::string(sqlite3_errmsg(db.get())));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);

    std::size_t count = 0;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading tiles: " + std::string(sqlite3_errmsg(db.get())));
        }

        const int z = sqlite3_column_int(stmt.get(), 0);
        const int x = sqlite3_column_int(stmt.get(), 1);
        const int tms_y = sqlite3_column_int(stmt.get(), 2);

        if (z < 0 || z >= 63) {
            throw mbtiles_error("Unsupported zoom level encountered: " + std::to_string(z));
        }

        const auto xyz_y = static_cast<long long>((1LL << z) - 1) - static_cast<long long>(tms_y);
        if (xyz_y < std::numeric_limits<int>::min() || xyz_y > std::numeric_limits<int>::max()) {
            throw mbtiles_error("Tile row is outside representable range for zoom level " + std::to_string(z));
        }

        const void *blob = sqlite3_column_blob(stmt.get(), 3);
        const int blob_size = sqlite3_column_bytes(stmt.get(), 3);

        std::string relative_path = format_pattern(z, x, static_cast<int>(xyz_y), options.pattern);
        fs::path output_path = output_root / fs::path(relative_path);

        const std::string extension = detect_extension(blob, blob_size);
        if (output_path.extension().empty()) {
            output_path += extension;
        }

        fs::create_directories(output_path.parent_path(), ec);
        if (ec) {
            throw mbtiles_error("Failed to create directory '" + output_path.parent_path().string() + "': " + ec.message());
        }

        std::ofstream file(output_path, std::ios::binary);
        if (!file) {
            throw mbtiles_error("Failed to open output file '" + output_path.string() + "'");
        }

        if (blob != nullptr && blob_size > 0) {
            file.write(static_cast<const char *>(blob), blob_size);
        }

        if (!file) {
            throw mbtiles_error("Failed to write tile to '" + output_path.string() + "'");
        }

        ++count;
        if (options.verbose && count % 100 == 0) {
            std::cout << "Extracted " << count << " tiles..." << std::endl;
        }
    }

    if (options.verbose) {
        std::cout << "Extraction completed. Total tiles: " << count << std::endl;
    }

    return count;
}

void convert_directory_to_grayscale(const std::string &input_directory, const std::string &output_directory,
                                    const GrayscaleOptions &options) {
    fs::path input_root = input_directory;
    fs::path output_root = output_directory;

    if (!fs::exists(input_root)) {
        throw mbtiles_error("Input directory does not exist: " + input_root.string());
    }
    if (!fs::is_directory(input_root)) {
        throw mbtiles_error("Input path is not a directory: " + input_root.string());
    }

    std::error_code ec;
    fs::create_directories(output_root, ec);
    if (ec) {
        throw mbtiles_error("Failed to create output directory '" + output_root.string() + "': " + ec.message());
    }

    auto process_entry = [&](const fs::directory_entry &entry) {
        if (!entry.is_regular_file()) {
            return;
        }
        if (!is_supported_image_extension(entry.path())) {
            return;
        }

        fs::path relative = entry.path().lexically_relative(input_root);
        if (relative.empty()) {
            std::error_code rel_ec;
            relative = fs::relative(entry.path(), input_root, rel_ec);
            if (rel_ec) {
                throw mbtiles_error("Failed to determine relative path for '" + entry.path().string() + "': " + rel_ec.message());
            }
        }

        fs::path destination = output_root / relative;

        image_data image = load_image_rgba(entry.path());
        convert_pixels_to_grayscale(image.pixels);
        save_image_rgba(destination, image.pixels.data(), image.width, image.height);

        if (options.verbose) {
            std::cout << "Converted " << entry.path() << " -> " << destination << std::endl;
        }
    };

    if (options.recursive) {
        for (const auto &entry : fs::recursive_directory_iterator(input_root)) {
            process_entry(entry);
        }
    } else {
        for (const auto &entry : fs::directory_iterator(input_root)) {
            process_entry(entry);
        }
    }
}

void decrease_zoom_level(const std::string &input_directory, const std::string &output_directory,
                         const DecreaseZoomOptions &options) {
    fs::path input_root = input_directory;
    fs::path output_root = output_directory;

    if (!fs::exists(input_root)) {
        throw mbtiles_error("Input directory does not exist: " + input_root.string());
    }
    if (!fs::is_directory(input_root)) {
        throw mbtiles_error("Input path is not a directory: " + input_root.string());
    }

    int max_zoom = -1;
    for (const auto &entry : fs::directory_iterator(input_root)) {
        if (!entry.is_directory()) {
            continue;
        }
        try {
            int z = std::stoi(entry.path().filename().string());
            max_zoom = std::max(max_zoom, z);
        } catch (const std::exception &) {
            continue;
        }
    }

    if (max_zoom <= 0) {
        throw mbtiles_error("Unable to determine maximum zoom level in " + input_root.string());
    }

    const int new_zoom = max_zoom - 1;
    fs::path highest_zoom_dir = input_root / std::to_string(max_zoom);
    if (!fs::exists(highest_zoom_dir) || !fs::is_directory(highest_zoom_dir)) {
        throw mbtiles_error("Missing directory for zoom level " + std::to_string(max_zoom));
    }

    std::error_code ec;
    fs::create_directories(output_root / std::to_string(new_zoom), ec);
    if (ec) {
        throw mbtiles_error("Failed to prepare output directory: " + ec.message());
    }

    std::map<std::pair<int, int>, fs::path> tiles;
    for (const auto &x_entry : fs::directory_iterator(highest_zoom_dir)) {
        if (!x_entry.is_directory()) {
            continue;
        }

        int x_value = 0;
        try {
            x_value = std::stoi(x_entry.path().filename().string());
        } catch (const std::exception &) {
            continue;
        }

        for (const auto &y_entry : fs::directory_iterator(x_entry.path())) {
            if (!y_entry.is_regular_file()) {
                continue;
            }

            int y_value = 0;
            try {
                y_value = std::stoi(y_entry.path().stem().string());
            } catch (const std::exception &) {
                continue;
            }

            tiles.emplace(std::make_pair(x_value, y_value), y_entry.path());
        }
    }

    if (tiles.empty()) {
        throw mbtiles_error("No tiles found at zoom level " + std::to_string(max_zoom));
    }

    std::set<std::pair<int, int>> parents;
    for (const auto &item : tiles) {
        parents.emplace(item.first.first / 2, item.first.second / 2);
    }

    std::size_t processed = 0;
    for (const auto &parent : parents) {
        const std::array<std::pair<int, int>, 4> children = {{{parent.first * 2, parent.second * 2},
                                                              {parent.first * 2 + 1, parent.second * 2},
                                                              {parent.first * 2, parent.second * 2 + 1},
                                                              {parent.first * 2 + 1, parent.second * 2 + 1}}};

        std::array<image_data, 4> images;
        bool missing_child = false;
        std::string extension;

        for (std::size_t idx = 0; idx < children.size(); ++idx) {
            const auto it = tiles.find(children[idx]);
            if (it == tiles.end()) {
                missing_child = true;
                break;
            }

            if (extension.empty()) {
                extension = it->second.extension().string();
            }

            try {
                images[idx] = load_image_rgba(it->second);
            } catch (const std::exception &) {
                missing_child = true;
                break;
            }
        }

        if (missing_child) {
            continue;
        }

        const int child_width = images[0].width;
        const int child_height = images[0].height;
        if (child_width <= 0 || child_height <= 0) {
            continue;
        }

        for (const auto &img : images) {
            if (img.width != child_width || img.height != child_height) {
                missing_child = true;
                break;
            }
        }

        if (missing_child) {
            continue;
        }

        const int canvas_width = child_width * 2;
        const int canvas_height = child_height * 2;
        std::vector<unsigned char> canvas(static_cast<std::size_t>(canvas_width) * canvas_height * 4, 0);

        for (std::size_t idx = 0; idx < images.size(); ++idx) {
            const int offset_x = static_cast<int>(idx % 2) * child_width;
            const int offset_y = static_cast<int>(idx / 2) * child_height;
            const auto &img = images[idx];

            for (int row = 0; row < child_height; ++row) {
                unsigned char *dest = canvas.data() + ((offset_y + row) * canvas_width + offset_x) * 4;
                const unsigned char *src = img.pixels.data() + static_cast<std::size_t>(row) * child_width * 4;
                std::memcpy(dest, src, static_cast<std::size_t>(child_width) * 4);
            }
        }

        std::vector<unsigned char> resized(static_cast<std::size_t>(child_width) * child_height * 4, 0);
        if (!stbir_resize_uint8_linear(canvas.data(), canvas_width, canvas_height, canvas_width * 4, resized.data(),
                                       child_width, child_height, child_width * 4, STBIR_RGBA)) {
            throw mbtiles_error("Failed to downsample tile group at (" + std::to_string(parent.first) + ", " +
                                std::to_string(parent.second) + ")");
        }

        if (options.grayscale) {
            convert_pixels_to_grayscale(resized);
        }

        std::string final_extension = options.force_png ? ".png" : extension;
        if (final_extension.empty()) {
            final_extension = ".png";
        }

        fs::path destination = output_root / std::to_string(new_zoom) / std::to_string(parent.first);
        destination /= std::to_string(parent.second) + final_extension;
        save_image_rgba(destination, resized.data(), child_width, child_height);

        ++processed;
        if (options.verbose && processed % 50 == 0) {
            std::cout << "Generated " << processed << " tiles at zoom " << new_zoom << std::endl;
        }
    }

    if (options.verbose) {
        std::cout << "Finished generating tiles at zoom " << new_zoom << " (" << processed << " total)." << std::endl;
    }
}

std::map<std::string, std::string> read_metadata(const std::string &mbtiles_path) {
    auto db = open_database(mbtiles_path);

    const char *query = "SELECT name, value FROM metadata ORDER BY name";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to read metadata: " + std::string(sqlite3_errmsg(db.get())));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    std::map<std::string, std::string> result;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading metadata: " + std::string(sqlite3_errmsg(db.get())));
        }

        const unsigned char *name_text = sqlite3_column_text(stmt.get(), 0);
        const unsigned char *value_text = sqlite3_column_text(stmt.get(), 1);
        const std::string name = name_text != nullptr ? reinterpret_cast<const char *>(name_text) : std::string();
        const std::string value = value_text != nullptr ? reinterpret_cast<const char *>(value_text) : std::string();
        result.emplace(name, value);
    }

    return result;
}

void write_metadata_entries(const std::string &mbtiles_path, const std::map<std::string, std::string> &entries,
                            bool overwrite_existing) {
    if (entries.empty()) {
        return;
    }

    auto db = open_database(mbtiles_path);

    const char *create_sql = "CREATE TABLE IF NOT EXISTS metadata (name TEXT PRIMARY KEY, value TEXT)";
    if (sqlite3_exec(db.get(), create_sql, nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to ensure metadata table exists: " + std::string(sqlite3_errmsg(db.get())));
    }

    if (sqlite3_exec(db.get(), "BEGIN IMMEDIATE TRANSACTION", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to begin transaction: " + std::string(sqlite3_errmsg(db.get())));
    }

    const char *insert_sql = overwrite_existing
                                 ? "INSERT INTO metadata(name, value) VALUES(?1, ?2) ON CONFLICT(name) DO UPDATE SET value=excluded.value"
                                 : "INSERT INTO metadata(name, value) VALUES(?1, ?2)";

    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), insert_sql, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
        throw mbtiles_error("Failed to prepare metadata statement: " + std::string(sqlite3_errmsg(db.get())));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);

    for (const auto &entry : entries) {
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        sqlite3_bind_text(stmt.get(), 1, entry.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt.get(), 2, entry.second.c_str(), -1, SQLITE_TRANSIENT);

        const int rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE) {
            sqlite3_exec(db.get(), "ROLLBACK", nullptr, nullptr, nullptr);
            throw mbtiles_error("Failed to write metadata entry '" + entry.first + "': " +
                                std::string(sqlite3_errmsg(db.get())));
        }
    }

    if (sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to commit metadata changes: " + std::string(sqlite3_errmsg(db.get())));
    }
}

void write_metadata_entry(const std::string &mbtiles_path, const std::string &key, const std::string &value,
                          bool overwrite_existing) {
    write_metadata_entries(mbtiles_path, {{key, value}}, overwrite_existing);
}

std::vector<std::string> metadata_keys(const std::string &mbtiles_path) {
    auto db = open_database(mbtiles_path);

    const char *query = "SELECT name FROM metadata ORDER BY name";
    sqlite3_stmt *raw_stmt = nullptr;
    if (sqlite3_prepare_v2(db.get(), query, -1, &raw_stmt, nullptr) != SQLITE_OK) {
        throw mbtiles_error("Failed to read metadata keys: " + std::string(sqlite3_errmsg(db.get())));
    }

    std::unique_ptr<sqlite3_stmt, stmt_deleter> stmt(raw_stmt);
    std::vector<std::string> keys;

    while (true) {
        const int rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            throw mbtiles_error("SQLite error while reading metadata keys: " + std::string(sqlite3_errmsg(db.get())));
        }

        const unsigned char *name_text = sqlite3_column_text(stmt.get(), 0);
        keys.emplace_back(name_text != nullptr ? reinterpret_cast<const char *>(name_text) : "");
    }

    return keys;
}

}  // namespace mbtiles

