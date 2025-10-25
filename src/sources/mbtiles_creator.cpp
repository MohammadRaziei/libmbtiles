#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sqlite3.h>
#include <cstring>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <experimental/filesystem>
#include <cmath>
#include <limits>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <iomanip>

namespace fs = std::experimental::filesystem;

// Global variables for progress tracking
std::atomic<int> total_tiles_processed(0);
std::atomic<int> total_tiles_count(0);
std::chrono::steady_clock::time_point start_time;
std::mutex progress_mutex;

// Function to read directory contents
std::vector<std::string> read_directory(const std::string& path) {
    std::vector<std::string> result;
    DIR* dir = opendir(path.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
                result.push_back(entry->d_name);
            }
        }
        closedir(dir);
    }
    return result;
}

// Count total tiles for progress tracking
int countTotalTiles(const std::string& inputDir, int zoom) {
    int count = 0;
    try {
        // Iterate over zoom levels (z)
        auto z_dirs = read_directory(inputDir);
        for (const auto& z_str : z_dirs) {
            int z = atoi(z_str.c_str());
            if(z!=zoom) continue;

            std::string z_path = inputDir + "/" + z_str;
            
            // Iterate over x directories
            auto x_dirs = read_directory(z_path);
            for (const auto& x_str : x_dirs) {
                std::string x_path = z_path + "/" + x_str;
                
                // Iterate over y files
                auto y_files = read_directory(x_path);
                for (const auto& y_file : y_files) {
                    if (y_file.size() < 5 || y_file.substr(y_file.size() - 4) != ".jpg") {
                        continue; // Skip non-JPG files
                    }
                    count++;
                }
            }
        }
        return count;
    } catch (const std::exception& e) {
        std::cerr << "Error counting tiles: " << e.what() << std::endl;
        return -1;
    }
}

// Create MBTiles database and tables
bool createMBTilesDB(sqlite3* db, const char* description, double min_lon, double min_lat, double max_lon, double max_lat, int min_zoom, int max_zoom) {
    const char* sql = 
        "CREATE TABLE metadata (name text, value text);"
        "CREATE TABLE tiles (zoom_level integer, tile_column integer, tile_row integer, tile_data blob);"
        "CREATE UNIQUE INDEX tile_index on tiles (zoom_level, tile_column, tile_row);";
    
    char* errMsg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &errMsg)) {
        std::cerr << "Error creating tables: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }
    
    // Add required metadata
    char metadata_sql[4000]; // Increased buffer size to prevent overflow
    sprintf(metadata_sql,
        "INSERT INTO metadata (name, value) VALUES ('name', 'Tiles');"
        "INSERT INTO metadata (name, value) VALUES ('type', 'baselayer');"
        "INSERT INTO metadata (name, value) VALUES ('version', '1.3');"
        "INSERT INTO metadata (name, value) VALUES ('description', '%s');"
        "INSERT INTO metadata (name, value) VALUES ('format', 'jpg');"
        "INSERT INTO metadata (name, value) VALUES ('bounds', '%.7f,%.7f,%.7f,%.7f');"
        "INSERT INTO metadata (name, value) VALUES ('center', '%.7f,%.7f,%i');"
        "INSERT INTO metadata (name, value) VALUES ('minzoom', '%i');"
        "INSERT INTO metadata (name, value) VALUES ('maxzoom', '%i');",
        description, 
        min_lon, min_lat, max_lon, max_lat,
        (min_lon+max_lon)/2., (min_lat+max_lat)/2., min_zoom,
        min_zoom, max_zoom
    );
    
    if (sqlite3_exec(db, (const char*)metadata_sql, nullptr, nullptr, &errMsg)) {
        std::cerr << "Error inserting metadata: " << errMsg << std::endl;
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

sqlite3_stmt* prepareInsertStatement(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO tiles (zoom_level, tile_column, tile_row, tile_data) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare insert statement: " << sqlite3_errmsg(db) << std::endl;
        return nullptr;
    }
    return stmt;
}

// Check if a tile exists in the database and remove it if found
bool checkAndRemoveTile(sqlite3* db, int z, int x, int y) {
    int y_mbtiles = (1 << z) - 1 - y;
    
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM tiles WHERE zoom_level = ? AND tile_column = ? AND tile_row = ?;";
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "Failed to prepare delete statement: " << sqlite3_errmsg(db) << std::endl;
        return false;
    }
    
    sqlite3_bind_int(stmt, 1, z);
    sqlite3_bind_int(stmt, 2, x);
    sqlite3_bind_int(stmt, 3, y_mbtiles);
    
    int result = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    
    // SQLITE_DONE means the delete was successful (even if no rows were deleted)
    return result == SQLITE_DONE;
}

// Insert a tile into the database
bool insertTile(sqlite3_stmt* stmt, int z, int x, int y, const std::vector<char>& data) {
    int y_mbtiles = (1 << z) - 1 - y;

    sqlite3_bind_int(stmt, 1, z);
    sqlite3_bind_int(stmt, 2, x);
    sqlite3_bind_int(stmt, 3, y_mbtiles);
    sqlite3_bind_blob(stmt, 4, data.data(), data.size(), SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE) {
        std::cerr << "Failed to insert tile: " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
        return false;
    }

    sqlite3_reset(stmt); // Reset for next use
    return true;
}

// Function to display progress report
void displayProgress() {
    while (total_tiles_processed < total_tiles_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // Update more frequently
        
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();
        
        std::lock_guard<std::mutex> lock(progress_mutex);
        double progress = (static_cast<double>(total_tiles_processed) / total_tiles_count) * 100.0;
        
        if (elapsed_seconds > 0) {
            double tps = static_cast<double>(total_tiles_processed) / elapsed_seconds;
            
            std::cout << "\rProgress: " << total_tiles_processed << "/" << total_tiles_count 
                      << " tiles (" << std::fixed << std::setprecision(1) << progress << "%)"
                      << " | tiles/s: " << std::fixed << std::setprecision(1) << tps
                      << " | Elapsed: " << elapsed_seconds << "s"
                      << std::flush;

            // Progress file
            FILE *fp = fopen("progress.txt", "w");  // open file for writing
            fprintf(fp, "%i", (int)progress + 200);  // write progressPercent to file
            fclose(fp);  // close file
            
        } else {
            std::cout << "\rProgress: " << total_tiles_processed << "/" << total_tiles_count 
                      << " tiles (" << std::fixed << std::setprecision(1) << progress << "%)"
                      << " | Starting..."
                      << std::flush;
        }
    }
}

// Process all tiles in the directory
bool processTiles(sqlite3_stmt* stmt, sqlite3* db, const std::string& inputDir, int min_zoom, int max_zoom, bool augment_mode) {
    try {
        // Count total tiles first
        total_tiles_count = 0;
        for(int zoom = min_zoom; zoom <= max_zoom; zoom++) {
            int count = countTotalTiles(inputDir, zoom);
            if (count > 0) {
                total_tiles_count += count;
            }
        }
        
        if (total_tiles_count <= 0) {
            std::cerr << "No tiles found or error counting tiles" << std::endl;
            return false;
        }
        
        std::cout << "Total tiles to process: " << total_tiles_count << std::endl;
        
        // Start progress display thread
        start_time = std::chrono::steady_clock::now();
        std::thread progress_thread(displayProgress);
        
        // Iterate over zoom levels (z)
        auto z_dirs = read_directory(inputDir);
        for (const auto& z_str : z_dirs) {
            int zoom = atoi(z_str.c_str());
            if(zoom < min_zoom || zoom > max_zoom) continue;

            std::string z_path = inputDir + "/" + z_str;
            
            // Iterate over x directories
            auto x_dirs = read_directory(z_path);
            for (const auto& x_str : x_dirs) {
                int x = atoi(x_str.c_str());
                std::string x_path = z_path + "/" + x_str;
                
                // Iterate over y files
                auto y_files = read_directory(x_path);
                for (const auto& y_file : y_files) {
                    if (y_file.size() < 5 || y_file.substr(y_file.size() - 4) != ".jpg") {
                        continue; // Skip non-JPG files
                    }
                    
                    int y = atoi(y_file.substr(0, y_file.size() - 4).c_str());
                    std::string y_path = x_path + "/" + y_file;

                    // Read tile data
                    std::ifstream file(y_path, std::ios::binary);
                    if (!file) {
                        std::cerr << "Failed to open tile file: " << y_path << std::endl;
                        continue;
                    }

                    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

                    // In augment mode, check and remove existing tile first
                    if (augment_mode) {
                        if (!checkAndRemoveTile(db, zoom, x, y)) {
                            std::cerr << "Failed to check/remove existing tile zoom=" << zoom << ", x=" << x << ", y=" << y << std::endl;
                            continue;
                        }
                    }

                    // Insert into database
                    if (!insertTile(stmt, zoom, x, y, data)) {
                        std::cerr << "Failed to insert tile zoom=" << zoom << ", x=" << x << ", y=" << y << std::endl;
                        continue;
                    }

                    // Update progress counter
                    total_tiles_processed++;
                }
            }
        }
        
        // Wait for progress thread to finish
        if (progress_thread.joinable()) {
            progress_thread.join();
        }
        
        // Final progress report
        auto end_time = std::chrono::steady_clock::now();
        auto total_seconds = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
        double avg_tps = static_cast<double>(total_tiles_count) / total_seconds;
        
        std::cout << "\rCompleted: " << total_tiles_count << "/" << total_tiles_count 
                  << " tiles (100.0%)"
                  << " | Avg tiles/s: " << std::fixed << std::setprecision(1) << avg_tps
                  << " | Total time: " << total_seconds << "s"
                  << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error processing tiles: " << e.what() << std::endl;
        return false;
    }
}

// Convert tile X,Y,Z to lon/lat bounds
void tile2lonlat(int x, int y, int z, double &min_lon, double &min_lat, double &max_lon, double &max_lat) {
    double n = std::pow(2.0, z);
    min_lon = x / n * 360.0 - 180.0;
    max_lon = (x + 1) / n * 360.0 - 180.0;

    auto lat_deg = [](double y, double n) {
        double lat_rad = std::atan(std::sinh(M_PI * (1 - 2 * y / n)));
        return lat_rad * 180.0 / M_PI;
    };

    min_lat = lat_deg(y + 1, n);
    max_lat = lat_deg(y, n);
}

int checkDataDir(const std::string &inputDir,
                 double &min_lon, double &min_lat,
                 double &max_lon, double &max_lat,
                 int &min_zoom, int &max_zoom)
{
    min_zoom = std::numeric_limits<int>::max();
    max_zoom = std::numeric_limits<int>::min();

    // Step 1: find zoom levels (directories under inputDir)
    for (auto &zdir : fs::directory_iterator(inputDir)) {
        if (is_directory(zdir)) {
            try {
                int z = std::stoi(zdir.path().filename().string());
                if (z < min_zoom) min_zoom = z;
                if (z > max_zoom) max_zoom = z;
            } catch (...) { /* skip non-numeric */ }
        }
    }

    if (max_zoom == std::numeric_limits<int>::min()) {
        return -1; // no valid zooms
    }

    // Step 2: find min/max X inside max_zoom directory
    std::string maxZoomPath = (fs::path(inputDir) / std::to_string(max_zoom)).string();
    int min_x = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();

    for (auto &xdir : fs::directory_iterator(maxZoomPath)) {
        if (is_directory(xdir)) {
            try {
                int x = std::stoi(xdir.path().filename().string());
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
            } catch (...) { /* skip non-numeric */ }
        }
    }

    if (min_x == std::numeric_limits<int>::max()) {
        return -2; // no x folders
    }

    // Step 3: find min/max Y inside min_x and max_x folders
    auto find_y_bounds = [](const fs::path &xdir, int &min_y, int &max_y) {
        for (auto &file : fs::directory_iterator(xdir)) {
            if (is_regular_file(file)) {
                std::string stem = file.path().stem().string(); // filename without extension
                try {
                    int y = std::stoi(stem);
                    if (y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                } catch (...) { /* skip non-numeric */ }
            }
        }
    };

    int min_y = std::numeric_limits<int>::max();
    int max_y = std::numeric_limits<int>::min();

    find_y_bounds(fs::path(maxZoomPath) / std::to_string(min_x), min_y, max_y);
    find_y_bounds(fs::path(maxZoomPath) / std::to_string(max_x), min_y, max_y);

    if (min_y == std::numeric_limits<int>::max()) {
        return -3; // no y files
    }

    // Step 4: compute geographic bounding box (corners)
    double lon, lat, lon2, lat2;

    // bottom-left corner (min_x, max_y)
    tile2lonlat(min_x, max_y, max_zoom, lon, lat, lon2, lat2);
    min_lon = lon;
    min_lat = lat;

    // top-right corner (max_x, min_y)
    tile2lonlat(max_x, min_y, max_zoom, lon, lat, lon2, lat2);
    max_lon = lon2;
    max_lat = lat2;

    return 0;
}

int main(int argc, char* argv[]) 
{
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] 
                  << " input_directory [-z zoom_level] [-d description] [-a|--augment [file_name]]" 
                  << std::endl;
        return 1;
    }

    std::string inputDir = argv[1];
    int user_specified_zoom = -1;                   // default value (unset)
    std::string description = "";    // empty if not provided
    bool augment_mode = false;
    std::string augment_file_name = "";

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-z" && i + 1 < argc) {
            try {
                user_specified_zoom = std::stoi(argv[++i]);
            } catch (std::invalid_argument& e) {
                std::cerr << "Invalid zoom value: " << argv[i] << std::endl;
                return 1;
            } catch (std::out_of_range& e) {
                std::cerr << "Zoom value out of range: " << argv[i] << std::endl;
                return 1;
            }
        } else if (arg == "-d" && i + 1 < argc) {
            description = argv[++i];
        } else if (arg == "-a" || arg == "--augment") {
            augment_mode = true;
            // Check if next argument is a file name (not another flag)
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                augment_file_name = argv[++i];
            }
        } else {
            std::cerr << "Unknown or misplaced argument: " << arg << std::endl;
            return 1;
        }
    }

    std::cout << "Input directory: " << inputDir << std::endl;
    if (user_specified_zoom != -1)
        std::cout << "User specified zoom level: " << user_specified_zoom << std::endl;
    else
        std::cout << "Zoom level: (not set)" << std::endl;

    std::cout << "Description: " << description << std::endl;
    
    // Inspect tiles data in dir
    int min_zoom, max_zoom;
    double min_lon, min_lat, max_lon, max_lat;
    checkDataDir(inputDir, min_lon, min_lat, max_lon, max_lat, min_zoom, max_zoom);
    printf("min_lon:%f, min_lat:%f, max_lon:%f, max_lat:%f, min_zoom:%i, max_zoom:%i\n", min_lon, min_lat, max_lon, max_lat, min_zoom, max_zoom);
    if(min_zoom > max_zoom) {
        std::cerr << " No zoom levels found!" << std::endl;
        return -1;
    }

    // Limit zoom level
    if(user_specified_zoom != -1) {
        if(user_specified_zoom < min_zoom || user_specified_zoom > max_zoom) {
            std::cerr << " Specified zoom level not in valid range!" << std::endl;
            return -1;
        }
        min_zoom = user_specified_zoom;
        max_zoom = user_specified_zoom;
    }

    // Determine file name
    char file_name[300]; // Increased buffer size to prevent overflow
    if (augment_mode && !augment_file_name.empty()) {
        // Use user-specified file name for augment mode
        strcpy(file_name, augment_file_name.c_str());
    } else {
        // Generate file name based on coordinates and zoom levels
        #define nsrFloor(var) ((int)((var>=0)?var:(var-1)))
        int lat = nsrFloor((min_lat+max_lat)/2.);
        int lon = nsrFloor((min_lon+max_lon)/2.);
        if(min_zoom == max_zoom) {
            sprintf(file_name, "%s_%c%03i%c%03i_z%02i.mbtiles", description.c_str(), lat >= 0?'N':'S', lat >= 0?lat:-lat, lon >= 0?'E':'W', lon >= 0?lon:-lon, max_zoom);
        } else {
            sprintf(file_name, "%s_%c%03i%c%03i_z%02i-%02i.mbtiles", description.c_str(), lat >= 0?'N':'S', lat >= 0?lat:-lat, lon >= 0?'E':'W', lon >= 0?lon:-lon, min_zoom, max_zoom);
        }
    }
    printf(" Using file: %s\n", file_name);

    // Check if file exists for augment mode
    bool file_exists = false;
    std::ifstream test_file(file_name);
    if (test_file.good()) {
        file_exists = true;
        test_file.close();
    }

    // If in augment mode but file doesn't exist, fall back to normal mode
    if (augment_mode && !file_exists) {
        std::cout << "File " << file_name << " does not exist, falling back to normal mode" << std::endl;
        augment_mode = false;
    }

    if (augment_mode) {
        std::cout << "Augment mode: using existing file " << file_name << std::endl;
    } else {
        // Remove existing output file in normal mode
        remove(file_name);
    }
    
    // Open database
    sqlite3* db = nullptr;
    if (sqlite3_open(file_name, &db)) {
        std::cerr << "Can't open database: " << sqlite3_errmsg(db) << std::endl;
        return 1;
    }

    // Performance
    sqlite3_exec(db, "PRAGMA journal_mode = MEMORY;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA synchronous = OFF;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA temp_store = MEMORY;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA cache_size = 10000;", nullptr, nullptr, nullptr);
    
    // Create MBTiles structure only if not in augment mode
    if (!augment_mode) {
        if (!createMBTilesDB(db, description.c_str(), min_lon, min_lat, max_lon, max_lat, min_zoom, max_zoom)) {
            sqlite3_close(db);
            return 1;
        }
    } else {
        std::cout << "Using existing database structure" << std::endl;
    }
    
    // Begin bulk insert transaction
    sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);

    sqlite3_stmt* stmt = prepareInsertStatement(db);

    // Process tiles
    if (!processTiles(stmt, db, inputDir, min_zoom, max_zoom, augment_mode)) {
        sqlite3_close(db);
        return 1;
    }

    sqlite3_finalize(stmt);
    // Commit transaction
    sqlite3_exec(db, "END TRANSACTION;", nullptr, nullptr, nullptr);

    // Optimize database
    sqlite3_exec(db, "VACUUM;", nullptr, nullptr, nullptr);
    
    sqlite3_close(db);
    std::cout << "Successfully created MBTiles file" << std::endl;
    return 0;
}
