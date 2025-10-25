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

struct TileRanges {
    int x_min;
    int x_max;
    int y_min;
    int y_max;
};

class MBTilesAnalyzer {
private:
    sqlite3* db;
    std::atomic<int> tilesProcessed;
    std::mutex coutMutex;
    
public:
    MBTilesAnalyzer() : db(nullptr), tilesProcessed(0) {}
    
    ~MBTilesAnalyzer() {
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
    
    bool databaseIsHealthy() {
        if (!db) {
            std::cerr << "Database not opened!" << std::endl;
            return false;
        }
        
        // First, get all available zoom levels
        std::vector<int> zoomLevels = getZoomLevels();
        if (zoomLevels.empty()) {
            std::cerr << "No zoom levels found in database!" << std::endl;
            return false;
        }

        // Use only the highest zoom level
        int highestZoom = zoomLevels.back();
        std::cout << "Using highest zoom level: " << highestZoom << std::endl;

        // Get tile count for highest zoom level
        int tileCount = getTileCountForZoom(highestZoom);
        if (tileCount == 0) {
            std::cerr << "No tiles found in highest zoom level!" << std::endl;
            return false;
        }

        // Get tile ranges for highest zoom level
        auto ranges = getTileRanges(highestZoom);
        if (ranges.x_min < 0 || ranges.x_max < 0 || ranges.y_min < 0 || ranges.y_max < 0) {
            std::cerr << "Could not calculate tile ranges for highest zoom level!" << std::endl;
            return false;
        }

        // Calculate expected tiles for highest zoom level
        int x_range = ranges.x_max - ranges.x_min + 1;
        int y_range = ranges.y_max - ranges.y_min + 1;
        int expectedTiles = x_range * y_range;

        if (expectedTiles == 0) {
            std::cerr << "Could not calculate expected tile range!" << std::endl;
            return false;
        }

        // Calculate health ratio
        double healthRatio = static_cast<double>(tileCount) / expectedTiles;
        std::cout << "Tiles in zoom " << highestZoom << ": " << tileCount << std::endl;
        std::cout << "X range: " << ranges.x_min << " to " << ranges.x_max << " (range: " << x_range << ")" << std::endl;
        std::cout << "Y range: " << ranges.y_min << " to " << ranges.y_max << " (range: " << y_range << ")" << std::endl;
        std::cout << "Expected tiles: " << expectedTiles << std::endl;
        std::cout << "Health ratio: " << healthRatio << std::endl;

        // Check if database is healthy (health ratio >= 0.25)
        if (healthRatio < 0.25) {
            std::cout << "Database is unhealthy (health ratio < 0.25)" << std::endl;
            return false;
        }

        std::cout << "Database is healthy (health ratio >= 0.25)" << std::endl;
        return true;
    }
    
    bool deleteDatabase(const std::string& dbPath) {
        if (db) {
            sqlite3_close(db);
            db = nullptr;
        }
        
        if (fs::exists(dbPath)) {
            try {
                if (fs::remove(dbPath)) {
                    std::cout << "Successfully deleted corrupt database: " << dbPath << std::endl;
                    return true;
                } else {
                    std::cerr << "Failed to delete database: " << dbPath << std::endl;
                    return false;
                }
            } catch (const std::exception& e) {
                std::cerr << "Error deleting database: " << e.what() << std::endl;
                return false;
            }
        }
        return true; // Database doesn't exist, consider it deleted
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
    
    int getTileCountForZoom(int zoom) {
        std::string query = "SELECT COUNT(*) FROM tiles WHERE zoom_level = ?";
        sqlite3_stmt* stmt;
        
        int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return -1;
        }
        
        sqlite3_bind_int(stmt, 1, zoom);
        
        int count = -1;
        if ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            count = sqlite3_column_int(stmt, 0);
        }
        
        sqlite3_finalize(stmt);
        return count;
    }
    
    TileRanges getTileRanges(int zoom) {
        TileRanges ranges = {-1, -1, -1, -1};
        std::string query = "SELECT MIN(tile_column), MAX(tile_column), MIN(tile_row), MAX(tile_row) FROM tiles WHERE zoom_level = ?";
        sqlite3_stmt* stmt;
        
        int rc = sqlite3_prepare_v2(db, query.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db) << std::endl;
            return ranges;
        }
        
        sqlite3_bind_int(stmt, 1, zoom);
        
        if ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            ranges.x_min = sqlite3_column_int(stmt, 0);
            ranges.x_max = sqlite3_column_int(stmt, 1);
            ranges.y_min = sqlite3_column_int(stmt, 2);
            ranges.y_max = sqlite3_column_int(stmt, 3);
        }
        
        sqlite3_finalize(stmt);
        return ranges;
    }
};

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <mbtiles_file>" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string mbtilesPath = argv[1];
    
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
    
    std::cout << "Analyzing tile ranges and writing missing tiles..." << std::endl;
    bool healthy = analyzer.databaseIsHealthy();
    if(!healthy) {
        std::cout << "Database is corrupt, deleting: " << mbtilesPath << std::endl;
        if (!analyzer.deleteDatabase(mbtilesPath)) {
            std::cerr << "Failed to delete corrupt database!" << std::endl;
            return 1;
        }
    }

    return 0;
}
