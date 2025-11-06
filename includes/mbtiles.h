#pragma once

#include <cstddef>
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
    std::string pattern = "{z}/{x}/{y}.jpg";
    bool verbose = false;
};

struct GrayscaleOptions {
    bool recursive = true;
    bool verbose = false;
};

struct DecreaseZoomOptions {
    bool grayscale = false;
    bool force_png = false;
    bool verbose = false;
};

std::size_t extract(const std::string &mbtiles_path, const ExtractOptions &options = {});

void convert_directory_to_grayscale(const std::string &input_directory, const std::string &output_directory,
                                    const GrayscaleOptions &options = {});

void decrease_zoom_level(const std::string &input_directory, const std::string &output_directory,
                         const DecreaseZoomOptions &options = {});

std::map<std::string, std::string> read_metadata(const std::string &mbtiles_path);

void write_metadata_entries(const std::string &mbtiles_path,
                            const std::map<std::string, std::string> &entries,
                            bool overwrite_existing = true);

void write_metadata_entry(const std::string &mbtiles_path, const std::string &key, const std::string &value,
                          bool overwrite_existing = true);

std::vector<std::string> metadata_keys(const std::string &mbtiles_path);

}  // namespace mbtiles

