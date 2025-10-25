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

class MBTilesExtractor {
private:
    sqlite3* db;
    std::string outputDir;
    std::atomic<int> tilesProcessed;
    std::mutex coutMutex;
    
public:
    MBTilesExtractor() : db(nullptr), tilesProcessed(0) {}
    
    ~MBTilesExtractor() {
        if (db) {
            sqlite3_close(db);
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
    
    bool createOutputDirectory(const std::string& dir) {
        outputDir = dir;
        try {
            if (!fs::exists(outputDir)) {
                fs::create_directories(outputDir);
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error creating output directory: " << e.what() << std::endl;
            return false;
        }
    }
    
    void extractTiles(bool verbose = false) {
        if (!db) {
            std::cerr << "Database not opened!" << std::endl;
            return;
        }
        
        std::string query = "SELECT zoom_level, tile_column, tile_row, tile_data FROM tiles";
        sqlite3_stmt* stmt;
        
        int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return;
        }
        
        auto startTime = std::chrono::high_resolution_clock::now();
        
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            int z = sqlite3_column_int(stmt, 0);
            int x = sqlite3_column_int(stmt, 1);
            int y = sqlite3_column_int(stmt, 2);
            
            // Convert TMS Y to XYZ Y (flip the y coordinate)
            int y_xyz = (1 << z) - 1 - y;
            
            const void* tileData = sqlite3_column_blob(stmt, 3);
            int dataSize = sqlite3_column_bytes(stmt, 3);
            
            saveTile(z, x, y_xyz, tileData, dataSize);
            
            tilesProcessed++;
            
            if (verbose && tilesProcessed % 100 == 0) {
                std::lock_guard<std::mutex> lock(coutMutex);
                std::cout << "Processed " << tilesProcessed << " tiles..." << std::endl;
            }
        }
        
        if (rc != SQLITE_DONE) {
            std::cerr << "Error executing query: " << sqlite3_errmsg(db) << std::endl;
        }
        
        sqlite3_finalize(stmt);
        
        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
        
        std::cout << "\nExtraction completed!" << std::endl;
        std::cout << "Total tiles extracted: " << tilesProcessed << std::endl;
        std::cout << "Time taken: " << duration.count() << " ms" << std::endl;
    }
    
private:
    void saveTile(int z, int x, int y, const void* data, int size) {
        // Create directory structure: z/x/
        std::string zDir = outputDir + "/" + std::to_string(z);
        std::string xDir = zDir + "/" + std::to_string(x);
        
        try {
            if (!fs::exists(zDir)) {
                fs::create_directory(zDir);
            }
            if (!fs::exists(xDir)) {
                fs::create_directory(xDir);
            }
        } catch (const std::exception& e) {
            std::cerr << "Error creating directory: " << e.what() << std::endl;
            return;
        }
        
        // Create file path: z/x/y.png (or appropriate extension)
        std::string filePath = xDir + "/" + std::to_string(y);
        
        // Try to determine file extension from data
        std::string extension = determineExtension(data, size);
        filePath += extension;
        
        // Save tile data to file
        std::ofstream file(filePath, std::ios::binary);
        if (file.is_open()) {
            file.write(static_cast<const char*>(data), size);
            file.close();
        } else {
            std::cerr << "Error writing file: " << filePath << std::endl;
        }
    }
    
    std::string determineExtension(const void* data, int size) {
        if (size >= 8) {
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            
            // PNG signature
            if (bytes[0] == 0x89 && bytes[1] == 0x50 && bytes[2] == 0x4E && bytes[3] == 0x47) {
                return ".png";
            }
            // JPEG signature
            else if (bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
                return ".jpg";
            }
            // WebP signature
            else if (bytes[0] == 0x52 && bytes[1] == 0x49 && bytes[2] == 0x46 && bytes[3] == 0x46 &&
                     bytes[8] == 0x57 && bytes[9] == 0x45 && bytes[10] == 0x42 && bytes[11] == 0x50) {
                return ".webp";
            }
        }
        
        // Default to .bin if format is unknown
        return ".bin";
    }
};

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <mbtiles_file> <output_directory> [options]" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -v, --verbose    Enable verbose output" << std::endl;
    std::cout << "  -h, --help       Show this help message" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string mbtilesPath = argv[1];
    std::string outputPath = argv[2];
    bool verbose = false;
    
    // Parse command line options
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }
    
    // Check if input file exists
    if (!fs::exists(mbtilesPath)) {
        std::cerr << "Error: MBTiles file '" << mbtilesPath << "' does not exist!" << std::endl;
        return 1;
    }
    
    MBTilesExtractor extractor;
    
    std::cout << "Opening MBTiles database: " << mbtilesPath << std::endl;
    if (!extractor.openDatabase(mbtilesPath)) {
        return 1;
    }
    
    std::cout << "Creating output directory: " << outputPath << std::endl;
    if (!extractor.createOutputDirectory(outputPath)) {
        return 1;
    }
    
    std::cout << "Starting tile extraction..." << std::endl;
    extractor.extractTiles(verbose);
    
    return 0;
}