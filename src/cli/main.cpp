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

    auto convert_cmd = app.add_subcommand("convert", "Convert MBTiles by copying, resizing, and changing formats");
    add_logging_flags(convert_cmd);
    std::string convert_input;
    std::string convert_output;
    std::vector<std::string> convert_levels;
    bool convert_grayscale = false;
    std::string convert_format = "default";
    std::string convert_extract_dir;
    std::string convert_extract_pattern = "{z}/{x}/{y}.{ext}";

    convert_cmd->add_option("mbtiles", convert_input, "Path to the MBTiles file")
        ->required()
        ->check(CLI::ExistingFile);
    CLI::Option *convert_output_opt = convert_cmd->add_option(
        "--output", convert_output, "Output .mbtiles path. Defaults to '<input>_converted.mbtiles'.");
    CLI::Validator require_mbtiles_extension = CLI::Validator(
        [](std::string &value) {
            namespace fs = std::filesystem;
            fs::path candidate(value);
            if (!candidate.has_extension()) {
                return std::string("output must end with .mbtiles; use --extract for directories");
            }
            std::string ext = candidate.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (ext != ".mbtiles") {
                return std::string("output must end with .mbtiles; use --extract for directories");
            }
            return std::string();
        },
        "MBTilesPath");
    convert_output_opt->check(require_mbtiles_extension);
    CLI::Option *convert_levels_opt = convert_cmd->add_option(
        "--zoom-levels", convert_levels,
        "Zoom levels to include. Use 0 to copy existing levels, prefix '-' for levels below the minimum, and '+' for levels above the maximum.");
    convert_levels_opt->expected(-1);
    convert_levels_opt->default_val(std::vector<std::string>{"0"});
    convert_levels_opt->default_str("0");
    convert_cmd->add_flag("--grayscale", convert_grayscale, "Convert tiles to grayscale before encoding");
    convert_cmd->add_option("--format", convert_format, "Output format: default, jpg, or png")
        ->default_val("default")
        ->check(CLI::IsMember({"default", "jpg", "jpeg", "png"}, CLI::ignore_case));
    CLI::Option *convert_extract_opt = convert_cmd->add_option(
        "--extract", convert_extract_dir,
        "Extract the converted archive to this directory after conversion");
    CLI::Option *convert_extract_pattern_opt = convert_cmd->add_option(
        "-p,--pattern", convert_extract_pattern,
        "Filename pattern for extracted tiles (e.g., {z}/{x}/{y}.{ext})")
                                                            ->default_val("{z}/{x}/{y}.{ext}");
    convert_extract_pattern_opt->needs(convert_extract_opt);

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

        if (*convert_cmd) {
            auto normalize_format = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
                if (value == "jpeg") {
                    value = "jpg";
                }
                return value;
            };

            mbtiles::ConvertOptions options;
            if (convert_levels.empty()) {
                options.zoom_levels = {"0"};
            } else {
                options.zoom_levels = convert_levels;
            }
            options.grayscale = convert_grayscale;
            options.run_extract = convert_extract_opt->count() > 0;

            const std::string format_lower = normalize_format(convert_format);
            if (format_lower == "png") {
                options.format = mbtiles::Format::PNG;
            } else if (format_lower == "jpg") {
                options.format = mbtiles::Format::JPG;
            } else {
                options.format = mbtiles::Format::DEFAULT;
            }

            mbtiles::MBTiles mb(convert_input);
            auto converted = mb.convert(options);

            namespace fs = std::filesystem;
            auto build_default_output = [&]() {
                fs::path input_path(convert_input);
                fs::path base_dir = input_path.has_parent_path() ? input_path.parent_path() : fs::current_path();
                std::string stem = input_path.stem().string();
                if (stem.empty()) {
                    stem = "converted";
                }
                fs::path candidate = base_dir / (stem + "_converted.mbtiles");
                int suffix = 1;
                while (fs::exists(candidate)) {
                    candidate = base_dir / (stem + "_converted_" + std::to_string(suffix++) + ".mbtiles");
                }
                return candidate;
            };

            fs::path output_path = convert_output_opt->count() > 0 ? fs::path(convert_output) : build_default_output();
            converted.saveTo(output_path.string());
            std::cout << "Converted MBTiles written to '" << output_path.string() << "'" << std::endl;

            if (convert_extract_opt->count() > 0) {
                const auto extracted = converted.extract(convert_extract_dir, convert_extract_pattern);
                std::cout << "Extracted " << extracted << " tiles to '" << convert_extract_dir << "'" << std::endl;
            }
            return EXIT_SUCCESS;
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

