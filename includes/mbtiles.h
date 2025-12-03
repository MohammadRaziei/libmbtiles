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

constexpr int ITER_ALL_ZOOMS {-1};

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
    std::vector<std::string> zoom_levels = {"0"};
    bool grayscale = false;
    Format format = Format::DEFAULT;
};






class Logger {
public:
    enum class Level {
        TRACE,
        DEBUG,
        INFO,
        WARNING,
        ERROR,
        FATAL
    };
    static void setLevel(Level level);
    static void setLevel(const std::string& level);
    static Level level();


    static void info(const std::string &message);
    static void error(const std::string &message);
    static void warn(const std::string &message);
    static void debug(const std::string &message);
    static void trace(const std::string &message);

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

    std::vector<unsigned char> encodePng() const;
    std::vector<unsigned char> encodeJpg(int quality = 90) const;

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
    std::vector<unsigned char> data;  // raw tile blob (e.g., PNG/JPG/PBF)
    std::string extension;        // "png", "jpg", "pbf", etc.

    // Compute latitude/longitude bounds on demand (no storage overhead)
    double latMin() const;
    double latMax() const;
    double lonMin() const;
    double lonMax() const;

    RGBAImage image() const;
};

std::pair<double, double> tile2latlon(int zoom, int x, int y);
std::pair<double, double> tile2latlon(const TileInfo& tile);



class TileIterator {
public:
    explicit TileIterator(sqlite3* db, int zoom = ITER_ALL_ZOOMS);
    ~TileIterator();

    // Returns the next tile, or std::nullopt if done.
    // Throws on database error.
    std::optional<TileInfo> next();

    // Reset iterator to start from the beginning.
    void reset();

private:
    sqlite3* _db;
    int _zoom = -1;
    sqlite3_stmt* _stmt = nullptr;
    bool _started = false;
    std::string _metadata_ext;
};

class MBTiles {
  public:
    MBTiles();
    MBTiles(const std::string& path);
    MBTiles(MBTiles&& other) noexcept;
    MBTiles& operator=(MBTiles&& other) noexcept;
    MBTiles(const MBTiles&) = delete;
    MBTiles& operator=(const MBTiles&) = delete;
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

    TileIterator tiles(int zoom = ITER_ALL_ZOOMS) const;


    void view(std::uint16_t port = 8080, std::string host = "0.0.0.0");

    std::vector<int> zoomLevels() const;
    std::optional<int> minZoomLevel() const;
    std::optional<int> maxZoomLevel() const;
    std::optional<std::string> tileData(int zoom, int x, int y) const;
    void writeTileData(int zoom, int x, int y, const unsigned char* data, int size);


    void convert(const ConvertOptions& options);
    void save(const std::string &path) const;

  private:
    std::string _name;
    sqlite3 *_db;

    std::optional<int> queryZoomValue(const char *sql) const;
    std::optional<std::string> fetchTileBlob(int zoom, int x, int y) const;
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
