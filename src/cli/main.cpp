#include "CLI11.hpp"
#include "mbtiles.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    CLI::App app{"libmbtiles command line interface"};
    app.require_subcommand(1);

    auto extract_cmd = app.add_subcommand("extract", "Extract tiles from an MBTiles archive");

    std::string extract_input;
    std::string extract_output = ".";
    std::string extract_pattern = "{z}/{x}/{y}.jpg";
    bool extract_verbose = false;

    extract_cmd->add_option("mbtiles", extract_input, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    extract_cmd->add_option("-o,--output-dir", extract_output, "Destination directory for the extracted tiles")
        ->default_val(".");
    extract_cmd->add_option("-p,--pattern", extract_pattern,
                             "Output filename pattern using placeholders like {z}, {x}, {y}, {t}, {n}, {XX}.")
        ->default_val("{z}/{x}/{y}.jpg");
    extract_cmd->add_flag("-v,--verbose", extract_verbose, "Enable verbose logging during extraction");

    auto grayscale_cmd = app.add_subcommand("convert-gray", "Convert a directory of tiles to grayscale");
    std::string gray_input;
    std::string gray_output;
    bool gray_no_recursive = false;
    bool gray_verbose = false;

    grayscale_cmd->add_option("input", gray_input, "Input directory containing image tiles")
        ->required()
        ->check(CLI::ExistingDirectory);
    grayscale_cmd->add_option("output", gray_output, "Directory where grayscale tiles will be written")
        ->required();
    grayscale_cmd->add_flag("--no-recursive", gray_no_recursive, "Only process files in the top-level directory");
    grayscale_cmd->add_flag("-v,--verbose", gray_verbose, "Print each converted file");

    auto decrease_cmd = app.add_subcommand("decrease-zoom", "Down-sample the highest zoom level into the next level");
    std::string decrease_input;
    std::string decrease_output;
    bool decrease_grayscale = false;
    bool decrease_force_png = false;
    bool decrease_verbose = false;

    decrease_cmd->add_option("input", decrease_input, "Input directory arranged as z/x/y image tiles")
        ->required()
        ->check(CLI::ExistingDirectory);
    decrease_cmd->add_option("output", decrease_output, "Directory to store the down-sampled tiles")
        ->required();
    decrease_cmd->add_flag("--grayscale", decrease_grayscale, "Convert the output tiles to grayscale");
    decrease_cmd->add_flag("--force-png", decrease_force_png, "Force PNG output irrespective of source format");
    decrease_cmd->add_flag("-v,--verbose", decrease_verbose, "Print progress information");

    auto metadata_cmd = app.add_subcommand("metadata", "Inspect and update MBTiles metadata");
    metadata_cmd->require_subcommand(1);

    auto metadata_list_cmd = metadata_cmd->add_subcommand("list", "List all metadata key/value pairs");
    std::string metadata_list_path;
    metadata_list_cmd->add_option("mbtiles", metadata_list_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);

    auto metadata_get_cmd = metadata_cmd->add_subcommand("get", "Read a metadata value by key");
    std::string metadata_get_path;
    std::string metadata_get_key;
    metadata_get_cmd->add_option("mbtiles", metadata_get_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    metadata_get_cmd->add_option("key", metadata_get_key, "Metadata key to retrieve")
        ->required();

    auto metadata_set_cmd = metadata_cmd->add_subcommand("set", "Write a metadata entry");
    std::string metadata_set_path;
    std::string metadata_set_key;
    std::string metadata_set_value;
    bool metadata_no_overwrite = false;

    metadata_set_cmd->add_option("mbtiles", metadata_set_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    metadata_set_cmd->add_option("key", metadata_set_key, "Metadata key to write")
        ->required();
    metadata_set_cmd->add_option("value", metadata_set_value, "Metadata value to write")
        ->required();
    metadata_set_cmd->add_flag("--no-overwrite", metadata_no_overwrite,
                                "Fail if the key already exists instead of overwriting");

    CLI11_PARSE(app, argc, argv);

    try {
        if (*extract_cmd) {
            mbtiles::ExtractOptions options;
            options.output_directory = extract_output;
            options.pattern = extract_pattern;
            options.verbose = extract_verbose;

            const auto count = mbtiles::extract(extract_input, options);
            std::cout << "Extracted " << count << " tiles to '" << options.output_directory << "'" << std::endl;
            return EXIT_SUCCESS;
        }

        if (*grayscale_cmd) {
            mbtiles::GrayscaleOptions options;
            options.recursive = !gray_no_recursive;
            options.verbose = gray_verbose;
            mbtiles::convert_directory_to_grayscale(gray_input, gray_output, options);
            return EXIT_SUCCESS;
        }

        if (*decrease_cmd) {
            mbtiles::DecreaseZoomOptions options;
            options.grayscale = decrease_grayscale;
            options.force_png = decrease_force_png;
            options.verbose = decrease_verbose;
            mbtiles::decrease_zoom_level(decrease_input, decrease_output, options);
            return EXIT_SUCCESS;
        }

        if (*metadata_list_cmd) {
            const auto metadata = mbtiles::read_metadata(metadata_list_path);
            for (const auto &entry : metadata) {
                std::cout << entry.first << "=" << entry.second << '\n';
            }
            return EXIT_SUCCESS;
        }

        if (*metadata_get_cmd) {
            const auto metadata = mbtiles::read_metadata(metadata_get_path);
            auto it = metadata.find(metadata_get_key);
            if (it == metadata.end()) {
                std::cerr << "Metadata key '" << metadata_get_key << "' not found" << std::endl;
                return EXIT_FAILURE;
            }
            std::cout << it->second << std::endl;
            return EXIT_SUCCESS;
        }

        if (*metadata_set_cmd) {
            mbtiles::write_metadata_entry(metadata_set_path, metadata_set_key, metadata_set_value,
                                          !metadata_no_overwrite);
            return EXIT_SUCCESS;
        }
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

