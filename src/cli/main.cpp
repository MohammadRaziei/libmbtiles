#include "CLI11.hpp"
#include "mbtiles.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

int main(int argc, char **argv) {
    CLI::App app{"libmbtiles command line interface"};
    app.require_subcommand(1);

    auto extract_cmd = app.add_subcommand("extract", "Extract tiles from an MBTiles archive");

    std::string extract_input;
    std::string extract_output = ".";
    std::string extract_pattern = "{z}/{x}/{y}.{ext}";
    bool extract_verbose = false;

    extract_cmd->add_option("mbtiles", extract_input, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    extract_cmd->add_option("-o,--output-dir", extract_output, "Destination directory for the extracted tiles")
        ->default_val(".");
    extract_cmd->add_option("-p,--pattern", extract_pattern,
                             "Output filename pattern using placeholders like {z}, {x}, {y}, {t}, {n}, {XX}, {ext}.")
        ->default_val("{z}/{x}/{y}.{ext}");
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

    auto decrease_cmd = app.add_subcommand("decrease-zoom",
                                           "Generate lower zoom levels from an MBTiles archive or copy specific levels");
    std::string decrease_input;
    std::string decrease_output;
    std::vector<int> decrease_levels;
    std::string decrease_pattern = "{z}/{x}/{y}.{ext}";
    bool decrease_yes = false;
    bool decrease_verbose = false;
    bool decrease_grayscale = false;

    decrease_cmd->add_option("mbtiles", decrease_input, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    decrease_cmd->add_option("output", decrease_output, "Directory or .mbtiles file for the results")
        ->required();
    CLI::Option *pattern_option = decrease_cmd
                                       ->add_option("-p,--pattern", decrease_pattern,
                                                    "Output filename pattern when writing to a directory. Uses placeholders like {z}, {x}, {y}, {ext}.")
                                       ->default_val("{z}/{x}/{y}.{ext}");
    decrease_cmd->add_option("--levels", decrease_levels,
                             "Zoom levels to include. Negative values are relative to the maximum zoom (e.g. -1 -> max-1).")
        ->expected(-1);
    decrease_cmd->add_flag("-y,--yes", decrease_yes, "Overwrite the output if it exists without prompting");
    decrease_cmd->add_flag("-v,--verbose", decrease_verbose, "Print progress information");
    decrease_cmd->add_flag("--grayscale", decrease_grayscale,
                           "Convert copied and generated tiles to grayscale before writing");

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

    auto viewer_cmd = app.add_subcommand("view", "Launch a local web viewer for an MBTiles archive");
    std::string viewer_path;
    std::string viewer_host = "127.0.0.1";
    std::uint16_t viewer_port = 8080;

    viewer_cmd->add_option("mbtiles", viewer_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    viewer_cmd->add_option("--host", viewer_host, "Host/IP address to bind the viewer server")
        ->default_val("127.0.0.1");
    viewer_cmd->add_option("-p,--port", viewer_port, "Port to bind the viewer server")
        ->default_val(8080);

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
            namespace fs = std::filesystem;

            const auto existing_levels = mbtiles::list_zoom_levels(decrease_input);
            if (existing_levels.empty()) {
                std::cerr << "No tiles found in the source archive." << std::endl;
                return EXIT_FAILURE;
            }

            const int max_zoom = *std::max_element(existing_levels.begin(), existing_levels.end());

            std::vector<int> target_levels;
            std::vector<int> generated_levels;
            std::unordered_set<int> seen_levels;

            if (decrease_levels.empty()) {
                if (max_zoom <= 0) {
                    std::cerr << "Cannot decrease zoom level because the maximum zoom is " << max_zoom << std::endl;
                    return EXIT_FAILURE;
                }
                target_levels.push_back(max_zoom - 1);
                generated_levels.push_back(max_zoom - 1);
            } else {
                for (int raw_level : decrease_levels) {
                    int resolved_level = raw_level;
                    if (raw_level < 0) {
                        resolved_level = max_zoom + raw_level;
                        if (resolved_level < 0) {
                            std::cerr << "Requested level " << raw_level
                                      << " is below zero after applying the relative offset." << std::endl;
                            return EXIT_FAILURE;
                        }
                        generated_levels.push_back(resolved_level);
                    }

                    if (seen_levels.insert(resolved_level).second) {
                        target_levels.push_back(resolved_level);
                    }
                }
            }

            for (int level : target_levels) {
                if (level > max_zoom &&
                    std::find(generated_levels.begin(), generated_levels.end(), level) == generated_levels.end()) {
                    std::cerr << "Warning: requested zoom level " << level
                              << " is above the source maximum " << max_zoom << std::endl;
                }
            }

            fs::path output_path = decrease_output;
            const bool output_exists = fs::exists(output_path);
            bool output_is_directory = false;
            if (output_exists) {
                output_is_directory = fs::is_directory(output_path);
            } else {
                output_is_directory = !output_path.has_extension();
            }

            if (output_exists && !decrease_yes) {
                std::cout << "Output path '" << output_path.string() << "' exists. Overwrite? [y/N] ";
                std::string response;
                if (!std::getline(std::cin, response)) {
                    std::cerr << "Aborted." << std::endl;
                    return EXIT_FAILURE;
                }
                const bool accepted = !response.empty() && (response[0] == 'y' || response[0] == 'Y');
                if (!accepted) {
                    std::cerr << "Aborted." << std::endl;
                    return EXIT_FAILURE;
                }
                if (!output_is_directory && fs::is_regular_file(output_path)) {
                    std::error_code remove_ec;
                    fs::remove(output_path, remove_ec);
                    if (remove_ec) {
                        std::cerr << "Failed to remove existing file: " << remove_ec.message() << std::endl;
                        return EXIT_FAILURE;
                    }
                }
            }

            if (!output_is_directory) {
                if (!output_path.has_extension() || output_path.extension() != ".mbtiles") {
                    std::cerr << "Output path must be a directory or end with .mbtiles" << std::endl;
                    return EXIT_FAILURE;
                }
                if (pattern_option != nullptr && pattern_option->count() > 0) {
                    std::cerr << "Warning: the output pattern is ignored when writing to an MBTiles file." << std::endl;
                }
            }

            mbtiles::DecreaseZoomOptions options;
            options.target_levels = target_levels;
            options.generated_levels = generated_levels;
            options.pattern = decrease_pattern;
            options.verbose = decrease_verbose;
            options.grayscale = decrease_grayscale;

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

        if (*viewer_cmd) {
            mbtiles::ViewerOptions options;
            options.host = viewer_host;
            options.port = viewer_port;
            std::cout << "Launching viewer for '" << viewer_path << "' at http://" << options.host << ":"
                      << options.port << std::endl;
            std::cout << "Press Ctrl+C to stop the server." << std::endl;
            mbtiles::serve_viewer(viewer_path, options);
            return EXIT_SUCCESS;
        }
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

