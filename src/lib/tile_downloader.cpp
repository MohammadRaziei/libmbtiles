#include <iostream>
#include <string.h>
#include <fstream>
#include <cmath>
#include <cassert>
#include <curl/curl.h>
#include <sys/stat.h>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <errno.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <random>
#include <chrono>
#include <iomanip>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sstream>

// STB Image libraries for grayscale conversion
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define pi 3.1415926535

struct TileCoord {
    int x;
    int y;
};

// Function declarations
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
std::string tileXYToQuadKey(int x, int y, int zoom);
void tile2lla(double x, double y, int zoom, double &lat, double &lon, bool tms = false);
void lla2tile(double lat, double lon, int zoom, double &x, double &y, bool tms = false);
bool createDirectoryRecursive(const std::string& path);
std::string getRandomBingServer();
bool fileExists(const std::string& path);
bool fileExistsAndValidSize(const std::string& path);
bool convert_image_to_grayscale(const std::string& filename);
std::vector<std::string> getSystemIPs();
void initializeSystemIPs();
std::vector<TileCoord> parseTileCoordinatesFromFile(const std::string& filename);

// Global variables
static double minLat, maxLat, minLon, maxLon;
static int zoom;
static std::string mapSource;
static bool convertToGrayscale = false;
static std::atomic<int> successCount(0);
static std::atomic<int> currentTile(0);
static std::atomic<int> skippedCount(0);
static std::atomic<int> totalTiles(0);
static std::atomic<int> activeThreads(0);
static std::atomic<int> unsuccessfulCount(0);

static std::mutex coutMutex;
static std::mutex statsMutex;
static std::mutex ipMutex;
static std::chrono::steady_clock::time_point programStartTime;
static std::vector<std::string> systemIPs;
static const std::vector<std::string> bingServers = {
    "t0.ssl.ak.tiles.virtualearth.net",
    "t1.ssl.ak.tiles.virtualearth.net",
    "t2.ssl.ak.tiles.virtualearth.net",
    "t3.ssl.ak.tiles.virtualearth.net"
};

// Thread statistics structure
struct ThreadStats {
    int downloadCount;
    long long downloadSize;
};

// Progress tracking structure
struct DownloadProgress {
    std::ofstream* stream;
    long long* downloadSize;
    size_t lastSize;
};

// Custom write callback to track download size
static size_t WriteCallbackWithProgress(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    DownloadProgress* progress = (DownloadProgress*)userp;
    progress->stream->write((char*)contents, realsize);
    *(progress->downloadSize) += realsize;
    return realsize;
}

// Function to display download rates and progress
void displayProgress(int numThreads, const std::vector<ThreadStats>& threadStats) {
    auto lastUpdate = std::chrono::steady_clock::now();
    std::vector<int> lastCounts(numThreads, 0);
    std::vector<long long> lastSizes(numThreads, 0);
    
    std::this_thread::sleep_for(std::chrono::seconds(1));

    while (activeThreads > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastUpdate).count();
        
        if (elapsed >= 1.0) {
            std::lock_guard<std::mutex> lock(coutMutex);
            
            // Clear previous progress lines
            std::cout << "\033[" << (numThreads + 2) << "A\033[0J";
            
            // Get current statistics safely
            int currentSuccess, currentSkipped;
            {
                std::lock_guard<std::mutex> statsLock(statsMutex);
                currentSuccess = successCount;
                currentSkipped = skippedCount;
            }
            
            // Display overall progress
            int completed = currentSuccess + currentSkipped;
            double progressPercent = (totalTiles > 0) ? (completed * 100.0 / totalTiles) : 0.0;
            auto totalElapsed = std::chrono::duration<double>(now - programStartTime).count();
            double overallRate = (totalElapsed > 0) ? (completed / totalElapsed) : 0.0;
            
            std::cout << "Overall Progress: " << completed << "/" << totalTiles 
                     << " (" << std::fixed << std::setprecision(1) << progressPercent << "%)"
                     << " | Rate: " << std::setprecision(2) << overallRate << " tiles/sec"
                     << " | Unsuccessful: " << std::setprecision(0) << unsuccessfulCount << "\n"
                     << " | Elapsed: " << std::setprecision(0) << totalElapsed << "s\n";
            
            // Display thread-specific rates
            for (int i = 0; i < numThreads; i++) {
                int currentCount = threadStats[i].downloadCount;
                long long currentSize = threadStats[i].downloadSize;
                
                int deltaCount = currentCount - lastCounts[i];
                long long deltaSize = currentSize - lastSizes[i];
                
                double rateTiles = (elapsed > 0) ? deltaCount / elapsed : 0.0;
                double rateBytes = (elapsed > 0) ? deltaSize / elapsed : 0.0;
                
                std::cout << "Thread " << (i + 1) << ": " << currentCount << " tiles"
                         << " | " << std::setprecision(1) << rateTiles << " tiles/sec"
                         << " | " << std::setprecision(1) << (rateBytes / 1024.0) << " KB/s\n";
                
                lastCounts[i] = currentCount;
                lastSizes[i] = currentSize;
            }
            std::cout << std::flush;

            // Progress file
            FILE *fp = fopen("progress.txt", "w");  // open file for writing
            if (fp) {
                fprintf(fp, "%i", (int)progressPercent);  // write progressPercent to file
                fclose(fp);  // close file
            }
            
            lastUpdate = now;
        }
    }
}

std::vector<std::string> getSystemIPs() {
    std::vector<std::string> ips;
    struct ifaddrs *ifaddr, *ifa;
    
    if (getifaddrs(&ifaddr) == -1) {
        perror("getifaddrs");
        return ips;
    }

    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
            char *ip = inet_ntoa(sa->sin_addr);
            
            // Skip localhost and invalid addresses
            if (strcmp(ip, "127.0.0.1") == 0 || strcmp(ip, "0.0.0.0") == 0) {
                continue;
            }
            
            // Check if IP is already in the list
            if (std::find(ips.begin(), ips.end(), ip) == ips.end()) {
                ips.push_back(ip);
            }
        }
    }

    freeifaddrs(ifaddr);
    return ips;
}

void initializeSystemIPs() {
    systemIPs = getSystemIPs();
    if (systemIPs.empty()) {
        std::cerr << "Warning: No system IP addresses found. Using default network interface." << std::endl;
    } else {
        std::cout << "Found " << systemIPs.size() << " system IP addresses:" << std::endl;
        for (size_t i = 0; i < systemIPs.size(); i++) {
            std::cout << "  " << (i + 1) << ": " << systemIPs[i] << std::endl;
        }
        std::cout << std::endl;
    }
}

// Thread worker function
void downloadWorker(const std::vector<TileCoord>& tiles, int threadId, const std::string& outputDir, ThreadStats& stats) {
    activeThreads++;
    
    // Initialize CURL for this thread
    CURL* curl = curl_easy_init();
    if (!curl) {
        std::lock_guard<std::mutex> lock(coutMutex);
        std::cerr << "Thread " << threadId << ": Failed to initialize CURL" << std::endl;
        activeThreads--;
        return;
    }

    // Set common CURL options
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L); // Prevent signals from interrupting curl

    auto workStartTime = std::chrono::steady_clock::now();
    int processedInThisWorkPeriod = 0;

    for (size_t i = 0; i < tiles.size(); i++) {
        const auto& tile = tiles[i];
        int tileNumber = ++currentTile;
        
        // Check if we need to take a break (every 5 minutes of work)
        auto now = std::chrono::steady_clock::now();
        auto workDuration = std::chrono::duration_cast<std::chrono::minutes>(now - workStartTime);
        
        if (workDuration.count() >= 5 && processedInThisWorkPeriod > 0) {
            {
                std::lock_guard<std::mutex> lock(coutMutex);
                std::cout << "Thread " << threadId << ": Worked for " << workDuration.count() 
                        << " minutes, taking 1 minute break..." << std::endl;
            }
            
            std::this_thread::sleep_for(std::chrono::minutes(1));
            
            workStartTime = std::chrono::steady_clock::now();
            processedInThisWorkPeriod = 0;
        }

        // Create directory structure
        std::string xDir = outputDir + "/" + std::to_string(zoom) + "/" + std::to_string(tile.x);
        if (!createDirectoryRecursive(xDir)) {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cerr << "Thread " << threadId << ": Failed to create directory: " << xDir << std::endl;
            continue;
        }

        std::string filename = xDir + "/" + std::to_string(tile.y) + ".jpg";
        
        // Check if file already exists
        if (fileExistsAndValidSize(filename)) {
            std::lock_guard<std::mutex> lock(coutMutex);
            //std::cout << "Tile " << tileNumber << " of " << totalTiles 
            //         << " (X=" << tile.x << ", Y=" << tile.y << ") already exists. Skipping.\n";
            skippedCount++;
            continue;
        }

        /*{
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cout << "Thread " << threadId << ": Downloading tile " << tileNumber << " of " << totalTiles 
                     << " (X=" << tile.x << ", Y=" << tile.y << ")...\n";
        }*/
        
        // Apply IP rotation strategy
        if (!systemIPs.empty()) {
            int currentActiveThreads = activeThreads.load();
            int ip_index = (currentActiveThreads - 1) % systemIPs.size();
            
            std::lock_guard<std::mutex> lock(ipMutex);
            curl_easy_setopt(curl, CURLOPT_INTERFACE, systemIPs[ip_index].c_str());
            
            // Optional: Debug output to see which IP is being used
            // std::cout << "Thread " << threadId << " using IP: " << systemIPs[ip_index] << std::endl;
        }
        
        // Download the tile
        std::string url;
        if (mapSource == "bing") {
            std::string quadKey = tileXYToQuadKey(tile.x, tile.y, zoom);
            std::string server = getRandomBingServer();
            url = "https://" + server + "/tiles/a" + quadKey + ".jpeg?g=1398";
        }
        else if (mapSource == "google-sat") {
            url = "http://khm.google.com/kh/v=1000&x=" + std::to_string(tile.x) + 
                  "&y=" + std::to_string(tile.y) + "&z=" + std::to_string(zoom);
        }
        else if (mapSource == "google-hybrid") {
            url = "http://khm.google.com/vt/lbw/lyrs=y&hl=x-local&x=" + std::to_string(tile.x) + 
                  "&y=" + std::to_string(tile.y) + "&z=" + std::to_string(zoom);
        }

        std::ofstream outputFile(filename, std::ios::binary);
        if (!outputFile.is_open()) {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cerr << "Thread " << threadId << ": Failed to open file: " << filename << std::endl;
            continue;
        }

        // Set up progress tracking
        long long downloadSize = 0;
        DownloadProgress progress = {&outputFile, &downloadSize, 0};

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallbackWithProgress);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &progress);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cerr << "Thread " << threadId << ": curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
            unsuccessfulCount++;
            outputFile.close();
            continue;
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code != 200) {
            std::lock_guard<std::mutex> lock(coutMutex);
            std::cerr << "Thread " << threadId << ": HTTP request failed with code: " << http_code << std::endl;
            unsuccessfulCount++;
            outputFile.close();
            continue;
        }

        outputFile.close();

        // Update download statistics
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            stats.downloadCount++;
            stats.downloadSize += downloadSize;
        }

        if (convertToGrayscale) {
            if (!convert_image_to_grayscale(filename)) {
                std::lock_guard<std::mutex> lock(coutMutex);
                std::cerr << "Thread " << threadId << ": Warning: Failed to convert tile to grayscale: " << filename << std::endl;
            }
        }

        successCount++;
        processedInThisWorkPeriod++;
    }

    curl_easy_cleanup(curl);
    activeThreads--;
}

// Function to parse tile coordinates from a file
std::vector<TileCoord> parseTileCoordinatesFromFile(const std::string& filename) {
    std::vector<TileCoord> tiles;
    std::ifstream file(filename);
    
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open tile coordinates file: " + filename);
    }
    
    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;
        
        // Remove any trailing whitespace or carriage return
        line.erase(std::remove(line.begin(), line.end(), '\r'), line.end());
        line.erase(std::remove(line.begin(), line.end(), '\n'), line.end());
        
        // Parse format: /zoom/x/y
        if (line[0] == '/') {
            std::istringstream iss(line);
            char slash;
            int z, x, y;
            
            if (iss >> slash >> z >> slash >> x >> slash >> y) {
                tiles.push_back({x, y});
                // Store the zoom level from the first tile
                if (tiles.size() == 1) {
                    zoom = z;
                }
            } else {
                std::cerr << "Warning: Invalid tile format: " << line << std::endl;
            }
        }
    }
    
    return tiles;
}

int main(int argc, char *argv[]) {
    // Initialize random seed
    std::srand(std::time(0));

    // Check if we're using file mode or lat/lon mode
    bool useFileMode = false;
    std::string tileFile;
    int numThreads = 0;
    
    // Parse command line arguments
    if (argc < 4) {
        std::cout << "Usage:\n"
                  << "  Mode 1 (Lat/Lon bounds): ./tile_downloader minLat maxLat minLon maxLon zoom mapSource numThreads [--grayscale]\n"
                  << "  Mode 2 (Tile file): ./tile_downloader --file tile_file.txt mapSource numThreads [--grayscale]\n"
                  << "Examples:\n"
                  << "  ./tile_downloader 40.7 40.8 -74.0 -73.9 12 bing 4 --grayscale\n"
                  << "  ./tile_downloader --file tiles.txt bing 4 --grayscale\n"
                  << "Supported map sources: bing, google-sat, google-hybrid\n";
        return 1;
    }

    // Check for file mode
    if (strcmp(argv[1], "--file") == 0) {
        useFileMode = true;
        if (argc < 5) {
            std::cout << "File mode requires: ./tile_downloader --file tile_file.txt mapSource numThreads [--grayscale]\n";
            return 1;
        }
        tileFile = argv[2];
        mapSource = argv[3];
        numThreads = std::stoi(argv[4]);
        
        // Check for grayscale flag
        for (int i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--grayscale") == 0) {
                convertToGrayscale = true;
                break;
            }
        }
    } else {
        // Lat/Lon mode (original)
        useFileMode = false;
        
        // Check for grayscale flag
        int grayscaleArgPos = 0;
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--grayscale") == 0) {
                convertToGrayscale = true;
                grayscaleArgPos = i;
                break;
            }
        }

        // Parse required arguments
        if (argc < 8) {
            std::cout << "Lat/Lon mode requires: ./tile_downloader minLat maxLat minLon maxLon zoom mapSource numThreads [--grayscale]\n";
            return 1;
        }
        
        numThreads = std::stoi(argv[7]);
        minLat = std::stod(argv[1]);
        maxLat = std::stod(argv[2]);
        minLon = std::stod(argv[3]);
        maxLon = std::stod(argv[4]);
        zoom = std::stoi(argv[5]);
        mapSource = argv[6];
    }

    // Validate map source
    if (mapSource != "bing" && mapSource != "google-sat" && mapSource != "google-hybrid") {
        std::cerr << "Unsupported map source: " << mapSource << std::endl;
        return 1;
    }

    // Validate number of threads
    if (numThreads < 1) {
        std::cerr << "Number of threads must be at least 1" << std::endl;
        return 1;
    }

    std::cout << "Using " << numThreads << " threads for downloading" << std::endl;

    // Initialize system IP addresses
    initializeSystemIPs();

    // Initialize CURL globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string outputDir = mapSource + "_tiles";
    if (!createDirectoryRecursive(outputDir)) {
        std::cerr << "Failed to create base directory: " << outputDir << std::endl;
        return 1;
    }

    std::vector<TileCoord> allTiles;

    if (useFileMode) {
        // Parse tiles from file
        try {
            allTiles = parseTileCoordinatesFromFile(tileFile);
            totalTiles = allTiles.size();
            std::cout << "Loaded " << totalTiles << " tiles from file: " << tileFile << std::endl;
            std::cout << "Using zoom level: " << zoom << " (from first tile)" << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
            return 1;
        }
    } else {
        // Original lat/lon mode
        bool tms = false;
        double minTileX, minTileY, maxTileX, maxTileY;
        lla2tile(minLat, minLon, zoom, minTileX, minTileY, tms);
        lla2tile(maxLat, maxLon, zoom, maxTileX, maxTileY, tms);

        int minX = static_cast<int>(std::floor(minTileX));
        int maxX = static_cast<int>(std::floor(maxTileX));
        int minY = static_cast<int>(std::floor(minTileY));
        int maxY = static_cast<int>(std::floor(maxTileY));

        if (minX > maxX) std::swap(minX, maxX);
        if (minY > maxY) std::swap(minY, maxY);

        std::cout << "Tile range: X[" << minX << " to " << maxX << "], Y[" << minY << " to " << maxY << "]\n";
        
        totalTiles = (maxX - minX + 1) * (maxY - minY + 1);
        
        for (int x = minX; x <= maxX; x++) {
            for (int y = minY; y <= maxY; y++) {
                allTiles.push_back({x, y});
            }
        }
    }

    if (convertToGrayscale) {
        std::cout << "Grayscale conversion enabled\n";
    }

    // Initialize thread statistics
    std::vector<ThreadStats> threadStats(numThreads);
    for (int i = 0; i < numThreads; i++) {
        threadStats[i] = {0, 0};
    }

    // Shuffle the tile list
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(allTiles.begin(), allTiles.end(), g);
    std::cout << "Shuffled " << allTiles.size() << " tiles for download" << std::endl;

    // Divide tiles among threads
    std::vector<std::vector<TileCoord>> threadTiles(numThreads);
    for (size_t i = 0; i < allTiles.size(); i++) {
        threadTiles[i % numThreads].push_back(allTiles[i]);
    }

    // Create space for progress display
    for (int i = 0; i < numThreads + 2; i++) {
        std::cout << "\n";
    }

    programStartTime = std::chrono::steady_clock::now();

    // Create and start download threads
    std::vector<std::thread> threads;
    for (int i = 0; i < numThreads; i++) {
        threads.emplace_back(downloadWorker, threadTiles[i], i + 1, outputDir, std::ref(threadStats[i]));
    }

    // Start progress display thread
    std::thread progressThread(displayProgress, numThreads, std::ref(threadStats));

    // Wait for all download threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    FILE *fp = fopen("./unsuccessful_count", "w");
    if (fp) {
        fprintf(fp, "%i", (int)unsuccessfulCount);
        fclose(fp);
    }

    // Wait for progress thread to finish
    progressThread.join();

    // Final summary
    auto totalElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - programStartTime).count();
    double overallRate = (totalElapsed > 0) ? (successCount + skippedCount) / totalElapsed : 0.0;
    
    std::cout << "\nDownload complete!\n";
    std::cout << "Successfully downloaded: " << successCount << " tiles\n";
    std::cout << "Skipped (already existed): " << skippedCount << " tiles\n";
    std::cout << "Unsuccessful: " << unsuccessfulCount << " tiles\n";
    std::cout << "Total time: " << std::fixed << std::setprecision(1) << totalElapsed << " seconds\n";
    std::cout << "Average rate: " << std::setprecision(2) << overallRate << " tiles/sec\n";

    curl_global_cleanup();
    return 0;
}

bool convert_image_to_grayscale(const std::string& filename) {
    int width, height, channels;
    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, 0);
    if (!data) {
        std::cerr << "Failed to load image for grayscale conversion: " << filename << std::endl;
        return false;
    }

    std::vector<unsigned char> gray_image(width * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            unsigned char* pixel = data + (y * width + x) * channels;
            unsigned char r = pixel[0];
            unsigned char g = channels > 1 ? pixel[1] : r;
            unsigned char b = channels > 2 ? pixel[2] : r;
            gray_image[y * width + x] = static_cast<unsigned char>(0.299f * r + 0.587f * g + 0.114f * b);
        }
    }

    bool success = stbi_write_jpg(filename.c_str(), width, height, 1, gray_image.data(), 100);
    stbi_image_free(data);
    
    if (!success) {
        std::cerr << "Failed to save grayscale image: " << filename << std::endl;
    }
    
    return success;
}

bool fileExists(const std::string& path) {
    std::ifstream f(path.c_str());
    return f.good();
}

bool fileExistsAndValidSize(const std::string& path) {
    std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
    if (!f.good()) {
        return false;
    }
    
    // Get file size by seeking to end
    std::streamsize size = f.tellg();
    f.close();
    
    return size >= 1536; // 1.5 KB = 1536 bytes
}

bool createDirectoryRecursive(const std::string& path) {
    size_t pos = 0;
    std::string dir;
    int mdret;

    if (path[path.size()-1] != '/') {
        dir = path + "/";
    } else {
        dir = path;
    }

    while ((pos = dir.find_first_of('/', pos)) != std::string::npos) {
        std::string subdir = dir.substr(0, pos);
        if (!subdir.empty()) {
#ifdef _WIN32
            mdret = _mkdir(subdir.c_str());
#else
            mdret = mkdir(subdir.c_str(), 0777);
#endif
            if (mdret && errno != EEXIST) {
                std::cerr << "Error creating directory " << subdir << ": " << strerror(errno) << std::endl;
                return false;
            }
        }
        pos++;
    }

    return true;
}

std::string getRandomBingServer() {
    return bingServers[std::rand() % bingServers.size()];
}

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    std::ofstream* stream = (std::ofstream*)userp;
    stream->write((char*)contents, realsize);
    return realsize;
}

std::string tileXYToQuadKey(int x, int y, int zoom) {
    std::string quadKey;
    for (int i = zoom; i > 0; i--) {
        char digit = '0';
        int mask = 1 << (i - 1);
        if ((x & mask) != 0) {
            digit++;
        }
        if ((y & mask) != 0) {
            digit += 2;
        }
        quadKey.push_back(digit);
    }
    return quadKey;
}

void tile2lla(double x, double y, int zoom, double &lat, double &lon, bool tms) {
    double n, two_pow_zoom;
    two_pow_zoom = 0x1 << zoom;
    lon = x / two_pow_zoom * 2. * pi - pi;

    double y_coord = tms ? (two_pow_zoom - y - 1) : y;
    n = pi - 2 * pi * y_coord / two_pow_zoom;
    lat = atan(0.5 * (exp(n) - exp(-n)));

    lat *= 180 / pi;
    lon *= 180 / pi;
}

void lla2tile(double lat, double lon, int zoom, double &x, double &y, bool tms) {
    assert(lat <= 85.05112878);

    double n, m, exp_n, two_pow_zoom;
    two_pow_zoom = 0x1 << zoom;

    x = (lon / 180 + 1) / 2 * two_pow_zoom;
    m = 2 * tan(lat * pi / 180);
    exp_n = m / 2 + sqrt(m * m + 4) / 2;
    n = log(exp_n);
    
    y = (pi - n) / (2. * pi) * two_pow_zoom;
    if (tms) {
        y = two_pow_zoom - y - 1;
    }
}

bool createDirectory(const std::string& path) {
#ifdef _WIN32
    int result = _mkdir(path.c_str());
#else
    int result = mkdir(path.c_str(), 0777);
#endif
    return (result == 0 || errno == EEXIST);
}