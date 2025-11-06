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
#include <set>
#include <map>

namespace fs = std::experimental::filesystem;

class MBTilesAnalyzer {
private:
    sqlite3* db;
    std::atomic<int> tilesProcessed;
    std::mutex coutMutex;
    std::ofstream outputFile;
    
public:
    MBTilesAnalyzer() : db(nullptr), tilesProcessed(0) {}
    
    ~MBTilesAnalyzer() {
        if (db) {
            sqlite3_close(db);
        }
        if (outputFile.is_open()) {
            outputFile.close();
        }
    }
    
    bool openDatabase(const std::string& dbPath) {
        int rc = sqlite3_open(dbPath.c_str(), &db);
        if (rc != SQLITE_OK) {
            std::cerr << "Cannot open database: " << sqlite3_errmsg(db) << std::endl;
            return false;
        }
        return true;
    }
    
    bool openOutputFile(const std::string& outputPath) {
        outputFile.open(outputPath);
        if (!outputFile.is_open()) {
            std::cerr << "Error: Could not open output file '" << outputPath << "'" << std::endl;
            return false;
        }
        return true;
    }
    
    void analyzeMissingTiles(bool verbose = false, bool inverse = false, bool upper_zoom = false) {
        if (!db) {
            std::cerr << "Database not opened!" << std::endl;
            return;
        }
        
        if (!outputFile.is_open()) {
            std::cerr << "Output file not opened!" << std::endl;
            return;
        }
        
        // First, get all available zoom levels
        std::vector<int> zoomLevels = getZoomLevels();
        if (zoomLevels.empty()) {
            std::cerr << "No zoom levels found in database!" << std::endl;
            return;
        }
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        // Analyze each zoom level
        for (int z : zoomLevels) {
            analyzeZoomLevel(z, verbose, inverse, upper_zoom);
        }
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        std::cout << "\nAnalysis completed!" << std::endl;
        std::cout << "Total tiles processed: " << tilesProcessed << std::endl;
        std::cout << "Time taken: " << duration.count() << " ms" << std::endl;
        std::cout << "Missing tiles written to output file" << std::endl;
    }
    
private:
    std::vector<int> getZoomLevels() {
        std::vector<int> zoomLevels;
        std::string query = "SELECT DISTINCT zoom_level FROM tiles ORDER BY zoom_level";
        sqlite3_stmt* stmt;
        
        int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return zoomLevels;
        }
        
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int z = sqlite3_column_int(stmt, 0);
            zoomLevels.push_back(z);
        }
        
        sqlite3_finalize(stmt);
        return zoomLevels;
    }
    
    void analyzeZoomLevel(int z, bool verbose, bool inverse, bool upper_zoom) {
        // Get all tiles for this zoom level
        std::string query = "SELECT tile_column, tile_row FROM tiles WHERE zoom_level = ? ORDER BY tile_column, tile_row";
        sqlite3_stmt* stmt;
        
        int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return;
        }
        
        sqlite3_bind_int(stmt, 1, z);
        
        std::set<int> xValues;
        std::set<int> yValues;
        std::set<std::pair<int, int>> existingTiles;
        
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int x = sqlite3_column_int(stmt, 0);
            int y = sqlite3_column_int(stmt, 1);
            
            xValues.insert(x);
            yValues.insert(y);
            existingTiles.insert({x, y});
            
            tilesProcessed++;
            
            if (verbose && tilesProcessed % 100 == 0) {
                std::lock_guard<std::mutex> lock(coutMutex);
                std::cout << "Processed " << tilesProcessed << " tiles..." << std::endl;
            }
        }
        
        sqlite3_finalize(stmt);
        
        if (xValues.empty() || yValues.empty()) {
            std::cout << "Zoom level " << z << ": No tiles found" << std::endl;
            return;
        }
        
        // Calculate min/max ranges
        int x_min = *xValues.begin();
        int x_max = *xValues.rbegin();
        int y_min = *yValues.begin();
        int y_max = *yValues.rbegin();
        
        std::cout << "\nZoom level " << z << ":" << std::endl;
        std::cout << "X range: " << x_min << " to " << x_max << std::endl;
        std::cout << "Y range: " << y_min << " to " << y_max << std::endl;
        std::cout << "Total expected tiles in range: " << (x_max - x_min + 1) * (y_max - y_min + 1) << std::endl;
        std::cout << "Actual tiles present: " << existingTiles.size() << std::endl;
        
        // Find missing tiles and write to file
        int missingCount = 0;
        for (int x = x_min; x <= x_max; x++) {
            for (int y = y_min; y <= y_max; y++) {
                if (existingTiles.find({x, y}) == existingTiles.end()) {
                    if (upper_zoom) {
                        // Write the 4 equivalent tiles in the higher zoom level
                        int next_z = z + 1;
                        for (int dx = 0; dx <= 1; dx++) {
                            for (int dy = 0; dy <= 1; dy++) {
                                int child_x = 2 * x + dx;
                                int child_y = 2 * y + dy;
                                
                                if (inverse) {
                                    int child_y_xyz = (1 << next_z) - 1 - child_y;
                                    outputFile << "/" << next_z << "/" << child_x << "/" << child_y_xyz << "\n";
                                } else {
                                    outputFile << "/" << next_z << "/" << child_x << "/" << child_y << "\n";
                                }
                            }
                        }
                    } else {
                        // Convert TMS Y to XYZ Y (flip the y coordinate)
                        if(inverse) {
                            int y_xyz = (1 << z) - 1 - y;
                            outputFile << "/" << z << "/" << x << "/" << y_xyz << "\n";
                        } else {
                            outputFile << "/" << z << "/" << x << "/" << y << "\n";
                        }
                    }

                    missingCount++;
                }
            }
        }
        
        std::cout << "Missing tiles: " << missingCount << std::endl;
        
        // Print sample of missing tiles to console
        if (missingCount > 0) {
            if(inverse) {
                std::cout << "Sample missing tiles format(XYZ): /" << z << "/" << x_min << "/" 
                      << ((1 << z) - 1 - y_min) << " ..." << std::endl;
            } else {
                std::cout << "Sample missing tiles format(TMS): /" << z << "/" << x_min << "/" 
                      << y_min << " ..." << std::endl;                
            }

        } else {
            std::cout << "No missing tiles found in this zoom level!" << std::endl;
        }
    }
};

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <mbtiles_file> <output_txt_file> [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -v, --verbose    Enable verbose output" << std::endl;
    std::cout << "  -h, --help       Show this help message" << std::endl;
    std::cout << "  -i, --inverse    Convert missing files to xyz format" << std::endl;
    std::cout << "  -u, --upper-zoom Report 4 equivalent tiles in higher zoom level instead of missing tiles" << std::endl;
    std::cout << "\nThis tool analyzes MBTiles files to find x,y min/max ranges" << std::endl;
    std::cout << "and writes all missing tiles to a text file" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string mbtilesPath = argv[1];
    std::string outputPath = argv[2];
    bool verbose = false;
    bool inverse = false;
    bool upper_zoom = false;
    
    // Parse command line options
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-i" || arg == "--inverse") {
            inverse = true;
        } else if (arg == "-u" || arg == "--upper-zoom") {
            upper_zoom = true;
        }
    }
    
    // Check if input file exists
    if (!fs::exists(mbtilesPath)) {
        std::cerr << "Error: MBTiles file '" << mbtilesPath << "' does not exist!" << std::endl;
        return 1;
    }
    
    MBTilesAnalyzer analyzer;
    
    std::cout << "Opening MBTiles database: " << mbtilesPath << std::endl;
    if (!analyzer.openDatabase(mbtilesPath)) {
        return 1;
    }
    
    std::cout << "Creating output file: " << outputPath << std::endl;
    if (!analyzer.openOutputFile(outputPath)) {
        return 1;
    }
    
    std::cout << "Analyzing tile ranges and writing missing tiles..." << std::endl;
    analyzer.analyzeMissingTiles(verbose, inverse, upper_zoom);
    
    return 0;
}
