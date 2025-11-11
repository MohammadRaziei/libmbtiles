#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mbtiles {

class mbtiles_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ExtractOptions {
    std::string output_directory = ".";
    std::string pattern = "{z}/{x}/{y}.{ext}";
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

std::size_t extract(const std::string &mbtiles_path, const ExtractOptions &options = {});

void convert_directory_to_grayscale(const std::string &input_directory, const std::string &output_directory,
                                    const GrayscaleOptions &options = {});

std::vector<int> list_zoom_levels(const std::string &mbtiles_path);

void resize_zoom_levels(const std::string &input_mbtiles, const std::string &output_path,
                        const ResizeOptions &options = {});

std::map<std::string, std::string> read_metadata(const std::string &mbtiles_path);

void write_metadata_entries(const std::string &mbtiles_path,
                            const std::map<std::string, std::string> &entries,
                            bool overwrite_existing = true);

void write_metadata_entry(const std::string &mbtiles_path, const std::string &key, const std::string &value,
                          bool overwrite_existing = true);

std::vector<std::string> metadata_keys(const std::string &mbtiles_path);

struct ViewerOptions {
    std::string host = "0.0.0.0";
    std::uint16_t port = 8080;
};

void serve_viewer(const std::string &mbtiles_path, const ViewerOptions &options = {});

}  // namespace mbtiles

