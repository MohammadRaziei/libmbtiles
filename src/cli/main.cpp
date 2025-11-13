#include "CLI11.hpp"
#include "mbtiles.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

int main(int argc, char **argv) {
    CLI::App app{"libmbtiles command line interface"};
    app.require_subcommand(1);

    int verbosity = 0;
    auto add_logging_flags = [&](CLI::App *cmd) {
        cmd->add_flag("-v,--verbose", verbosity, "Increase logging verbosity");
        cmd->add_flag_function("--verbose-extra", [&](int count) { verbosity += count * 2; },
                               "Enable extra verbose logging");
    };

    auto extract_cmd = app.add_subcommand("extract", "Extract tiles from an MBTiles archive");
    add_logging_flags(extract_cmd);

    std::string extract_input;
    std::string extract_output = ".";
    std::string extract_pattern = "{z}/{x}/{y}.{ext}";

    extract_cmd->add_option("mbtiles", extract_input, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    extract_cmd->add_option("-o,--output-dir", extract_output, "Destination directory for the extracted tiles")
        ->default_val(".");
    extract_cmd->add_option("-p,--pattern", extract_pattern,
                             "Output filename pattern using placeholders like {z}, {x}, {y}, {t}, {n}, {XX}, {ext}.")
        ->default_val("{z}/{x}/{y}.{ext}");

    auto grayscale_cmd = app.add_subcommand("convert-gray", "Convert a directory of tiles to grayscale");
    add_logging_flags(grayscale_cmd);
    std::string gray_input;
    std::string gray_output;
    bool gray_no_recursive = false;

    grayscale_cmd->add_option("input", gray_input, "Input directory containing image tiles")
        ->required()
        ->check(CLI::ExistingDirectory);
    grayscale_cmd->add_option("output", gray_output, "Directory where grayscale tiles will be written")
        ->required();
    grayscale_cmd->add_flag("--no-recursive", gray_no_recursive, "Only process files in the top-level directory");

    auto resize_cmd = app.add_subcommand("resize",
                                         "Resize tiles to generate additional zoom levels or copy existing ones");
    add_logging_flags(resize_cmd);
    std::string resize_input;
    std::string resize_output;
    std::vector<std::string> resize_levels_raw;
    std::string resize_pattern = "{z}/{x}/{y}.{ext}";
    bool resize_yes = false;
    bool resize_grayscale = false;

    resize_cmd->add_option("mbtiles", resize_input, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    resize_cmd->add_option("output", resize_output, "Directory or .mbtiles file for the results")
        ->required();
    CLI::Option *pattern_option = resize_cmd
                                       ->add_option("-p,--pattern", resize_pattern,
                                                    "Output filename pattern when writing to a directory. Uses placeholders like {z}, {x}, {y}, {ext}.")
                                       ->default_val("{z}/{x}/{y}.{ext}");
    resize_cmd->add_option("--levels", resize_levels_raw,
                           "Zoom levels to include. Prefix values with '-' to request levels below the minimum zoom and with '+' to request levels above the maximum zoom. Unprefixed values are treated as absolute zoom levels.")
        ->expected(-1);
    resize_cmd->add_flag("-y,--yes", resize_yes, "Overwrite the output if it exists without prompting");
    resize_cmd->add_flag("--grayscale", resize_grayscale,
                         "Convert copied and generated tiles to grayscale before writing");

    auto metadata_cmd = app.add_subcommand("metadata", "Inspect and update MBTiles metadata");
    metadata_cmd->require_subcommand(1);

    auto metadata_list_cmd = metadata_cmd->add_subcommand("list", "List all metadata key/value pairs");
    add_logging_flags(metadata_list_cmd);
    std::string metadata_list_path;
    metadata_list_cmd->add_option("mbtiles", metadata_list_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);

    auto metadata_get_cmd = metadata_cmd->add_subcommand("get", "Read a metadata value by key");
    add_logging_flags(metadata_get_cmd);
    std::string metadata_get_path;
    std::string metadata_get_key;
    metadata_get_cmd->add_option("mbtiles", metadata_get_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    metadata_get_cmd->add_option("key", metadata_get_key, "Metadata key to retrieve")
        ->required();

    auto metadata_set_cmd = metadata_cmd->add_subcommand("set", "Write a metadata entry");
    add_logging_flags(metadata_set_cmd);
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
    add_logging_flags(viewer_cmd);
    std::string viewer_path;
    std::string viewer_host = "127.0.0.1";
    std::uint16_t viewer_port = 8080;

    viewer_cmd->add_option("mbtiles", viewer_path, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    viewer_cmd->add_option("--host", viewer_host, "Host/IP address to bind the viewer server")
        ->default_val("0.0.0.0");
    viewer_cmd->add_option("-p,--port", viewer_port, "Port to bind the viewer server")
        ->default_val(8080);

    CLI11_PARSE(app, argc, argv);

    if (verbosity >= 2) {
        mbtiles::Logger::set_level(mbtiles::LogLevel::DEBUG);
    } else if (verbosity == 1) {
        mbtiles::Logger::set_level(mbtiles::LogLevel::INFO);
    } else {
        mbtiles::Logger::set_level(mbtiles::LogLevel::WARNING);
    }

    try {
        if (*extract_cmd) {
            mbtiles::MBTiles mb(extract_input);
            const auto count = mb.extract(extract_output, extract_pattern);
            std::cout << "Extracted " << count << " tiles to '" << extract_output << "'" << std::endl;
            return EXIT_SUCCESS;
        }

        if (*grayscale_cmd) {
            mbtiles::GrayscaleOptions options;
            options.recursive = !gray_no_recursive;
            // mbtiles::convert_directory_to_grayscale(gray_input, gray_output, options);
            return EXIT_SUCCESS;
        }

        if (*resize_cmd) {
            // namespace fs = std::filesystem;

            // const auto existing_levels = mbtiles::list_zoom_levels(resize_input);
            // if (existing_levels.empty()) {
            //     std::cerr << "No tiles found in the source archive." << std::endl;
            //     return EXIT_FAILURE;
            // }

            // const int min_zoom = *std::min_element(existing_levels.begin(), existing_levels.end());
            // const int max_zoom = *std::max_element(existing_levels.begin(), existing_levels.end());

            // std::vector<int> target_levels;
            // std::unordered_set<int> seen_levels;

            // auto add_level = [&](int level) {
            //     if (level < 0) {
            //         std::cerr << "Requested zoom level " << level << " is below zero." << std::endl;
            //         throw std::runtime_error("invalid zoom");
            //     }
            //     if (seen_levels.insert(level).second) {
            //         target_levels.push_back(level);
            //     }
            // };

            // try {
            //     if (resize_levels_raw.empty()) {
            //         if (min_zoom <= 0) {
            //             std::cerr << "Cannot generate a lower zoom level because the minimum zoom is " << min_zoom
            //                       << " and zoom levels cannot be negative." << std::endl;
            //             return EXIT_FAILURE;
            //         }
            //         add_level(min_zoom - 1);
            //     }
            //     for (const auto &token : resize_levels_raw) {
            //         if (token.empty()) {
            //             continue;
            //         }
            //         if (token.front() == '+') {
            //             if (token.size() == 1) {
            //                 throw std::invalid_argument("+");
            //             }
            //             const int offset = std::stoi(token.substr(1));
            //             const int resolved = max_zoom + offset;
            //             add_level(resolved);
            //         } else if (token.front() == '-') {
            //             if (token.size() == 1) {
            //                 throw std::invalid_argument("-");
            //             }
            //             const int offset = std::stoi(token.substr(1));
            //             const int resolved = min_zoom - offset;
            //             if (resolved < 0) {
            //                 std::cerr << "Requested level " << token
            //                           << " is below zero after applying the relative offset." << std::endl;
            //                 return EXIT_FAILURE;
            //             }
            //             add_level(resolved);
            //         } else {
            //             const int resolved = std::stoi(token);
            //             add_level(resolved);
            //         }
            //     }
            // } catch (const std::invalid_argument &) {
            //     std::cerr << "Invalid zoom level specified in --levels." << std::endl;
            //     return EXIT_FAILURE;
            // } catch (const std::out_of_range &) {
            //     std::cerr << "Zoom level value is out of range." << std::endl;
            //     return EXIT_FAILURE;
            // } catch (const std::runtime_error &) {
            //     return EXIT_FAILURE;
            // }

            // for (int level : target_levels) {
            //     if (level > max_zoom) {
            //         std::cerr << "Warning: requested zoom level " << level
            //                   << " is above the source maximum " << max_zoom
            //                   << ". Output quality may be reduced." << std::endl;
            //     }
            // }

            // fs::path output_path = resize_output;
            // const bool output_exists = fs::exists(output_path);
            // bool output_is_directory = false;
            // if (output_exists) {
            //     output_is_directory = fs::is_directory(output_path);
            // } else {
            //     output_is_directory = !output_path.has_extension();
            // }

            // if (output_exists && !resize_yes) {
            //     std::cout << "Output path '" << output_path.string() << "' exists. Overwrite? [y/N] ";
            //     std::string response;
            //     if (!std::getline(std::cin, response)) {
            //         std::cerr << "Aborted." << std::endl;
            //         return EXIT_FAILURE;
            //     }
            //     const bool accepted = !response.empty() && (response[0] == 'y' || response[0] == 'Y');
            //     if (!accepted) {
            //         std::cerr << "Aborted." << std::endl;
            //         return EXIT_FAILURE;
            //     }
            //     if (!output_is_directory && fs::is_regular_file(output_path)) {
            //         std::error_code remove_ec;
            //         fs::remove(output_path, remove_ec);
            //         if (remove_ec) {
            //             std::cerr << "Failed to remove existing file: " << remove_ec.message() << std::endl;
            //             return EXIT_FAILURE;
            //         }
            //     }
            // }

            // if (!output_is_directory) {
            //     if (!output_path.has_extension() || output_path.extension() != ".mbtiles") {
            //         std::cerr << "Output path must be a directory or end with .mbtiles" << std::endl;
            //         return EXIT_FAILURE;
            //     }
            //     if (pattern_option != nullptr && pattern_option->count() > 0) {
            //         std::cerr << "Warning: the output pattern is ignored when writing to an MBTiles file." << std::endl;
            //     }
            // }

            // mbtiles::ResizeOptions options;
            // options.target_levels = target_levels;
            // options.pattern = resize_pattern;
            // options.grayscale = resize_grayscale;

            // mbtiles::resize_zoom_levels(resize_input, resize_output, options);
            // return EXIT_SUCCESS;
        }

        if (*metadata_list_cmd) {
            const auto metadata = mbtiles::MBTiles(metadata_list_path).metadata();
            for (const auto &entry : metadata) {
                std::cout << entry.first << "=" << entry.second << '\n';
            }
            return EXIT_SUCCESS;
        }

        if (*metadata_get_cmd) {
            const auto metadata = mbtiles::MBTiles(metadata_get_path).metadata();
            auto it = metadata.find(metadata_get_key);
            if (it == metadata.end()) {
                std::cerr << "Metadata key '" << metadata_get_key << "' not found" << std::endl;
                return EXIT_FAILURE;
            }
            std::cout << it->second << std::endl;
            return EXIT_SUCCESS;
        }

        if (*metadata_set_cmd) {
            mbtiles::MBTiles(metadata_set_path).setMetadata(metadata_set_key, metadata_set_value,
                                          !metadata_no_overwrite);
            return EXIT_SUCCESS;
        }

        if (*viewer_cmd) {
            std::cout << "Launching viewer for '" << viewer_path << "' at http://" << viewer_host << ":"
                      << viewer_port << std::endl;
            std::cout << "Press Ctrl+C to stop the server." << std::endl;
            mbtiles::MBTiles(viewer_path).view(viewer_port, viewer_host);
            return EXIT_SUCCESS;
        }
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

