#ifndef MBTILES_H
#define MBTILES_H
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <optional>

class sqlite3;
class sqlite3_stmt;

namespace mbtiles {

class mbtiles_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ExtractOptions {
    ExtractOptions(const std::string& output_directory = ".", 
        const std::string& pattern = "{z}/{x}/{y}.{ext}") : 
        output_directory(output_directory), pattern(pattern) {}
    std::string output_directory;
    std::string pattern;
};

struct GrayscaleOptions {
    bool recursive = true;
};

enum class Format {
    DEFAULT,
    JPG,
    PNG,
};

struct ConvertOptions {
    std::vector<int> target_levels = {0};
    std::string output_mbtiles = "";
    ExtractOptions extract_options = {}; // only if output_mbtiles is empty  
    bool grayscale = false;
    Format format = Format::DEFAULT;
};


enum class LogLevel {
    Trace,
    DEBUG,
    INFO,
    WARNING,
    ERROR,
    FATAL
};

class Logger {
public:
    static void set_level(LogLevel level);
    static LogLevel level();

private:
    struct Impl;
    static Impl &impl();
};

class RGBAImage {
  public:
    RGBAImage();
    RGBAImage(const std::filesystem::path& path);
    RGBAImage(const unsigned char *data, int size);

    void load(const std::filesystem::path& path);
    void loadFromMemory(const unsigned char *data, int size);

    void save(const std::filesystem::path& path) const;

    std::vector<unsigned char> encodePng();

    void toGrayScale();

    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};


struct TileInfo {
    int zoom = 0;
    int x = 0;
    int y = 0;          // XYZ / Web Mercator Y
    int tms_y = 0;      // TMS Y as stored in MBTiles DB
    std::vector<std::byte> data;  // raw tile blob (e.g., PNG/JPG/PBF)
    std::string extension;        // "png", "jpg", "pbf", etc.

    // Compute latitude/longitude bounds on demand (no storage overhead)
    double latMin() const;
    double latMax() const;
    double lonMin() const;
    double lonMax() const;
};

std::pair<double, double> tile2latlon(int zoom, int x, int y);
std::pair<double, double> tile2latlon(const TileInfo& tile);



class TileIterator {
public:
    explicit TileIterator(sqlite3* db);
    ~TileIterator();

    // Returns the next tile, or std::nullopt if done.
    // Throws on database error.
    std::optional<TileInfo> next();

private:
    sqlite3* _db;
    sqlite3_stmt* _stmt = nullptr;
    bool _started = false;
    std::string _metadata_ext;
};

class MBTiles {
  public:
    MBTiles();
    MBTiles(const std::string& path);
    ~MBTiles();
    void open(const std::string& path);
    void close();
    size_t extract(const std::string& output_directory = ".", 
            const std::string& pattern = "{z}/{x}/{y}.{ext}") const;
    std::map<std::string, std::string> metadata() const;
    const std::string& metadata(const std::string& key) const;
    std::vector<std::string> metadataKeys() const;
    void setMetadata(const std::map<std::string, std::string> &entries, 
        bool overwrite_existing = true);
    void setMetadata(const std::string &key, const std::string &value,
        bool overwrite_existing = true);

    TileIterator tiles() const;



    // TileMap loadLevelRgba(int zoom);


    void view(std::uint16_t port = 8080, std::string host = "0.0.0.0");

  private:
    std::string _name;
    sqlite3 *_db;
};

// std::size_t extract(const std::string &mbtiles_path, const ExtractOptions &options = {});

// void convert_directory_to_grayscale(const std::string &input_directory, const std::string &output_directory,
                                    // const GrayscaleOptions &options = {});

// std::vector<int> list_zoom_levels(const std::string &mbtiles_path);

// void resize_zoom_levels(const std::string &input_mbtiles, const std::string &output_path,
                        // const ResizeOptions &options = {});

// std::map<std::string, std::string> read_metadata(const std::string &mbtiles_path);

// void write_metadata_entries(const std::string &mbtiles_path,
                            // const std::map<std::string, std::string> &entries,
                            // bool overwrite_existing = true);

// void write_metadata_entry(const std::string &mbtiles_path, const std::string &key, const std::string &value,
                        //   bool overwrite_existing = true);

// std::vector<std::string> metadata_keys(const std::string &mbtiles_path);

// struct ViewerOptions {
//     std::string host = "0.0.0.0";
//     std::uint16_t port = 8080;
// };

// void serve_viewer(const std::string &mbtiles_path, const ViewerOptions &options = {});

}  // namespace mbtiles

#endif // MBTILES_H