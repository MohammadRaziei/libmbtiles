# libMBTILES

![libmbtiles-logo](docs/images/libmbtiles.svg)


libMBTILES is a lightweight C++ utility library and command-line toolset for
working with [MBTiles](https://github.com/mapbox/mbtiles-spec) archives and tile
pyramids stored on disk. It provides:

- A static C++ library that exposes high-level helpers for extracting tiles,
  converting raster tiles to grayscale, down-sampling zoom levels, and reading
  or writing metadata inside an MBTiles database.
- A CLI built on top of the same primitives for quick one-off operations from
the shell.

This document walks through the available interfaces, explains how to build the
project, and demonstrates common usage patterns.

## Build and installation

### Prerequisites

- A C++17 compatible compiler (GCC ≥ 9, Clang ≥ 10, or MSVC ≥ 2019).
- CMake 3.15 or newer.
- A POSIX-like environment (macOS, Linux, WSL) is recommended. Windows builds
  are supported through CMake/Visual Studio, but the CLI examples below use
  POSIX-style shell syntax.

All third-party dependencies (SQLite3 and the stb image headers) are vendored in
`src/lib`, so no additional packages are required.

### Configure & build

```bash
cmake -S . -B build
cmake --build build
```

The build produces two artefacts:

- `build/libmbtiles/libmbtiles.a` – the static library.
- `build/mbtiles-cli` (or `mbtiles-cli.exe` on Windows) – the CLI frontend.

### Installing

To install the library, headers, and CLI binary into your CMake prefix (defaults
to `/usr/local` on Unix-like systems):

```bash
cmake --install build --prefix <install-prefix>
```

This copies `libmbtiles.a` to `<prefix>/lib`, installs the public headers under
`<prefix>/include`, and places the CLI executable in `<prefix>/bin`.

## Command-line interface

The CLI exposes several subcommands. Run `mbtiles-cli --help` to view the
top-level usage information, and append `--help` to individual subcommands for
detailed options.

```text
Usage: mbtiles-cli [OPTIONS] SUBCOMMAND
```

### `extract`

Extracts all tiles from an MBTiles SQLite database to a directory on disk.

```
mbtiles-cli extract <mbtiles> [--output-dir <dir>] [--pattern <pattern>] [--verbose]
```

| Option | Description |
| --- | --- |
| `mbtiles` | Required path to the MBTiles file. Must exist. |
| `-o`, `--output-dir` | Destination directory. Defaults to the current directory. |
| `-p`, `--pattern` | Filename pattern for each tile. Defaults to `{z}/{x}/{y}.jpg`. |
| `-v`, `--verbose` | Print progress information every 100 tiles. |

Patterns may reference placeholders to build informative paths:

| Placeholder | Meaning |
| --- | --- |
| `{z}`, `{x}`, `{y}` | Zoom level, X coordinate, and XYZ-formatted Y coordinate. |
| `{t}`, `{n}` | Latitude and longitude at the top-left corner of the tile (decimal degrees, 6 decimals). |
| `{Z}`, `{X}`, `{Y}` | Zero-padded zoom/X/Y. Repeat the letter to control the width (`{ZZZ}` → 3 digits). |
| `{T}`, `{N}` | Zero-padded leading digits of latitude/longitude. |

If the computed filename has no extension, libMBTILES detects the tile type and
adds `.png`, `.jpg`, or `.webp` automatically.

### `convert-gray`

Converts a directory tree of raster tiles to grayscale and writes the result to
a separate directory while preserving the directory structure.

```
mbtiles-cli convert-gray <input> <output> [--no-recursive] [--verbose]
```

- `input` – Source directory containing tile images (`.png`, `.jpg`, `.jpeg`).
- `output` – Target directory for grayscale tiles. Created if missing.
- `--no-recursive` – Only process files in the top-level directory. By default,
  subdirectories are traversed recursively.
- `-v`, `--verbose` – Print each converted file pair.

### `decrease-zoom`

Down-samples the highest zoom level (e.g. `z=15`) into the next level down (`z=14`).
The input directory must be structured as `root/<zoom>/<x>/<y>.<ext>`.

```
mbtiles-cli decrease-zoom <input> <output> [--grayscale] [--force-png] [--verbose]
```

- `--grayscale` – Convert the resampled tiles to grayscale before saving.
- `--force-png` – Always write PNG output even if the source tiles are JPEG.
- `-v`, `--verbose` – Print progress details as tiles are generated.

The command reads all tiles at the highest zoom level, identifies parent tiles,
combines the four children for each parent, and saves the averaged result to the
output directory under the new zoom level.

### `metadata`

Inspect or modify the `metadata` table within an MBTiles archive.

```
mbtiles-cli metadata <list|get|set> ...
```

- `metadata list <mbtiles>` – Print all metadata key/value pairs.
- `metadata get <mbtiles> <key>` – Print the value for a specific key. Exits with
  status 1 if the key does not exist.
- `metadata set <mbtiles> <key> <value> [--no-overwrite]` – Insert or update a
  metadata entry. With `--no-overwrite`, the command fails if the key already
  exists.

### Exit codes

All subcommands return `0` on success. Non-zero exit codes are accompanied by an
error message on `stderr`. Invalid input, missing files, or SQLite failures are
reported via descriptive errors from the underlying library.

## C library interface

Include the main header to access the APIs:

```cpp
#include <mbtiles.h>
```

All symbols live in the `mbtiles` namespace. Errors are reported by throwing
`mbtiles::mbtiles_error`, which derives from `std::runtime_error` and can be
caught alongside other standard exceptions.

### Tile extraction

```cpp
std::size_t extract(const std::string &mbtiles_path,
                    const ExtractOptions &options = {});
```

`ExtractOptions` fields:

- `output_directory` – Root directory for extracted tiles (default: current
  directory).
- `pattern` – Output filename pattern (default: `{z}/{x}/{y}.jpg`). Supports the
  placeholders listed earlier in the CLI section.
- `verbose` – Print progress every 100 tiles to `std::cout`.

The function returns the number of tiles written. The MBTiles archive must use
the standard schema (`tiles` and `metadata` tables).

### Grayscale conversion

```cpp
void convert_directory_to_grayscale(const std::string &input_directory,
                                    const std::string &output_directory,
                                    const GrayscaleOptions &options = {});
```

`GrayscaleOptions` fields:

- `recursive` – Traverse subdirectories when `true` (default). Only the top
  level is processed when `false`.
- `verbose` – Emit a log line for each converted file.

The helper loads PNG or JPEG tiles into memory, converts pixels using the
standard luminance formula (`0.299 * R  0.587 * G  0.114 * B`), and writes the
result back using stb_image_write. Directory structures are mirrored into the
output tree.

### Zoom level down-sampling

```cpp
void decrease_zoom_level(const std::string &input_directory,
                         const std::string &output_directory,
                         const DecreaseZoomOptions &options = {});
```

`DecreaseZoomOptions` fields:

- `grayscale` – Convert output to grayscale before writing (default: `false`).
- `force_png` – Force PNG output even when input tiles are JPEG (default: `false`).
- `verbose` – Print progress for each generated tile.

The function scans the input tree to determine the highest available zoom level,
calculates parent tile coordinates, stitches together the four child tiles for
each parent, and resizes them to the appropriate dimensions using
`stb_image_resize2`. Missing child tiles trigger an exception.

### Metadata helpers

```cpp
std::map<std::string, std::string> read_metadata(const std::string &mbtiles_path);

void write_metadata_entries(const std::string &mbtiles_path,
                            const std::map<std::string, std::string> &entries,
                            bool overwrite_existing = true);

void write_metadata_entry(const std::string &mbtiles_path,
                          const std::string &key,
                          const std::string &value,
                          bool overwrite_existing = true);

std::vector<std::string> metadata_keys(const std::string &mbtiles_path);
```

- `read_metadata` returns a map of metadata entries sorted by key.
- `write_metadata_entries` inserts or updates multiple entries inside a single
  transaction. When `overwrite_existing` is `false`, existing keys cause the
  operation to fail.
- `write_metadata_entry` is a convenience wrapper for inserting a single key.
- `metadata_keys` reads only the key names, ordered alphabetically.

These helpers automatically create the `metadata` table if it does not exist and
wrap modifications in a transaction to keep the database consistent.

### Error handling

All operations may throw `mbtiles_error` when encountering filesystem problems,
invalid patterns, unsupported zoom levels, or SQLite errors. Catch the exception
in client code to surface a descriptive message to end users:

```cpp
try {
    const auto count = mbtiles::extract("tiles.mbtiles");
    std::cout << "Extracted " << count << " tiles\n";
} catch (const mbtiles::mbtiles_error &ex) {
    std::cerr << "Extraction failed: " << ex.what() << std::endl;
}
```

## Contributing

Bug reports and pull requests are welcome. Please format new C code to match
the existing style and include tests or reproducible scenarios for functional
changes.


## License

MIT License
