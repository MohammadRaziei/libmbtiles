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
    bool verbose = false;
};

struct GrayscaleOptions {
    bool recursive = true;
    bool verbose = false;
};

struct DecreaseZoomOptions {
    std::vector<int> target_levels;
    std::vector<int> generated_levels;
    std::string pattern = "{z}/{x}/{y}.{ext}";
    bool verbose = false;
    bool grayscale = false;
};

std::size_t extract(const std::string &mbtiles_path, const ExtractOptions &options = {});

void convert_directory_to_grayscale(const std::string &input_directory, const std::string &output_directory,
                                    const GrayscaleOptions &options = {});

std::vector<int> list_zoom_levels(const std::string &mbtiles_path);

void decrease_zoom_level(const std::string &input_mbtiles, const std::string &output_path,
                         const DecreaseZoomOptions &options = {});

std::map<std::string, std::string> read_metadata(const std::string &mbtiles_path);

void write_metadata_entries(const std::string &mbtiles_path,
                            const std::map<std::string, std::string> &entries,
                            bool overwrite_existing = true);

void write_metadata_entry(const std::string &mbtiles_path, const std::string &key, const std::string &value,
                          bool overwrite_existing = true);

std::vector<std::string> metadata_keys(const std::string &mbtiles_path);

struct ViewerOptions {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;
};

void serve_viewer(const std::string &mbtiles_path, const ViewerOptions &options = {});

}  // namespace mbtiles

