#include <iostream>
#include <experimental/filesystem>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <cstdlib>
#include <chrono>
#include <iomanip>
#include <immintrin.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image_resize2.h"

namespace fs = std::experimental::filesystem;

void printHelp() {
    std::cout << "Usage: decrease_zoom [options] <input_directory> <output_directory>\n"
              << "Options:\n"
              << "  --grayscale    Convert tiles to grayscale\n"
              << "  --force-png    Force output as PNG (lossless)\n"
              << "  --help         Show this help message\n";
}

unsigned char toGray(unsigned char r, unsigned char g, unsigned char b) {
    return static_cast<unsigned char>(0.299*r + 0.587*g + 0.114*b);
}

inline uint64_t packXY(int x, int y) {
    return (static_cast<uint64_t>(x) << 32) | (uint32_t)y;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printHelp();
        return 1;
    }

    bool grayscale = false;
    bool forcePng = false;
    std::string inputDir, outputDir;

    for (int i=1; i<argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            printHelp();
            return 0;
        } else if (arg == "--grayscale") {
            grayscale = true;
        } else if (arg == "--force-png") {
            forcePng = true;
        } else if (inputDir.empty()) {
            inputDir = arg;
        } else if (outputDir.empty()) {
            outputDir = arg;
        }
    }

    if (inputDir.empty() || outputDir.empty()) {
        printHelp();
        return 1;
    }

    // Detect max zoom level
    int maxZoom = -1;
    for (auto& entry : fs::directory_iterator(inputDir)) {
        if (fs::is_directory(entry)) {
            int z = std::stoi(entry.path().filename().string());
            if (z > maxZoom) maxZoom = z;
        }
    }
    if (maxZoom < 1) {
        std::cerr << "No valid zoom level directories found." << std::endl;
        return 1;
    }

    int newZoom = maxZoom - 1;
    fs::create_directories(outputDir + "/" + std::to_string(newZoom));

    // Preload available files into a hash set (avoid fs::exists calls)
    std::unordered_map<std::string, std::string> availableFiles; // key: x/y, value: extension
    for (auto& xDir : fs::directory_iterator(inputDir + "/" + std::to_string(maxZoom))) {
        if (!fs::is_directory(xDir)) continue;
        std::string xStr = xDir.path().filename().string();
        for (auto& yFile : fs::directory_iterator(xDir.path())) {
            if (fs::is_directory(yFile)) continue;
            std::string stem = yFile.path().stem().string();
            std::string ext  = yFile.path().extension().string();
            availableFiles[xStr + "/" + stem] = ext;
        }
    }

    int totalPotentialTiles = (int)availableFiles.size();
    int estimatedTotalParentTiles = totalPotentialTiles / 4;

    std::unordered_set<uint64_t> processed;

    int totalTilesProcessed = 0;
    auto startTime = std::chrono::steady_clock::now();
    auto lastReportTime = startTime;
    int tilesSinceLastReport = 0;

    std::cout << "Starting tile processing from zoom " << maxZoom << " to " << newZoom << std::endl;
    std::cout << "Estimated total parent tiles: " << estimatedTotalParentTiles << std::endl;
    std::cout << "Progress will be reported every second..." << std::endl;

    for (auto& kv : availableFiles) {
        // Parse x/y
        size_t slashPos = kv.first.find('/');
        int x = std::stoi(kv.first.substr(0, slashPos));
        int y = std::stoi(kv.first.substr(slashPos+1));

        int X = x / 2;
        int Y = y / 2;

        uint64_t key = packXY(X,Y);
        if (processed.count(key)) continue;

        // Child coords
        std::vector<std::pair<int,int>> children = {
            {2*X, 2*Y}, {2*X+1, 2*Y}, {2*X, 2*Y+1}, {2*X+1, 2*Y+1}
        };

        bool allExist = true;
        std::vector<unsigned char*> childData(4, nullptr);
        std::vector<int> childW(4), childH(4);

        for (int i=0; i<4; i++) {
            std::string keyStr = std::to_string(children[i].first) + "/" + std::to_string(children[i].second);
            if (availableFiles.find(keyStr) == availableFiles.end()) {
                allExist = false;
                break;
            }
            std::string childPath = inputDir + "/" + std::to_string(maxZoom) + "/" +
                                    std::to_string(children[i].first) + "/" + std::to_string(children[i].second) + availableFiles[keyStr];
            int n;
            childData[i] = stbi_load(childPath.c_str(), &childW[i], &childH[i], &n, 4);
            if (!childData[i]) {
                allExist = false;
                break;
            }
        }

        if (!allExist) {
            for (auto ptr : childData) if (ptr) stbi_image_free(ptr);
            continue;
        }

        // Prepare final parent
        std::vector<unsigned char> finalImg(256*256*4, 0);

        for (int i=0; i<4; i++) {
            int dx = (i % 2) * (childW[i]/2);
            int dy = (i / 2) * (childH[i]/2);

            // Resize directly into final image position
            stbir_resize_uint8_linear(
                childData[i], childW[i], childH[i], 0,
                finalImg.data() + (dy*256 + dx)*4,
                childW[i]/2, childH[i]/2, 256*4, STBIR_RGBA);

            if (grayscale) {
                for (int yy=0; yy<childH[i]/2; yy++) {
                    for (int xx=0; xx<childW[i]/2; xx++) {
                        int idx = ((dy+yy)*256 + (dx+xx))*4;
                        unsigned char g = toGray(finalImg[idx], finalImg[idx+1], finalImg[idx+2]);
                        finalImg[idx] = finalImg[idx+1] = finalImg[idx+2] = g;
                    }
                }
            }
        }

        for (auto ptr : childData) if (ptr) stbi_image_free(ptr);

        // Write output
        std::string outDir = outputDir + "/" + std::to_string(newZoom) + "/" + std::to_string(X);
        fs::create_directories(outDir);
        std::string outFile = outDir + "/" + std::to_string(Y) + (forcePng ? ".png" : ".jpg");

        if (forcePng) {
            stbi_write_png(outFile.c_str(), 256, 256, 4, finalImg.data(), 256*4);
        } else {
            stbi_write_jpg(outFile.c_str(), 256, 256, 4, finalImg.data(), 100);
        }

        processed.insert(key);
        totalTilesProcessed++;
        tilesSinceLastReport++;

        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSinceLastReport = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastReportTime).count();

        if (elapsedSinceLastReport >= 1000) {
            auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();
            double tilesPerSecond = (tilesSinceLastReport * 1000.0) / elapsedSinceLastReport;
            double overallTilesPerSecond = (totalTilesProcessed * 1000.0) / totalElapsed;
            double progressPercent = (estimatedTotalParentTiles > 0) ? 
                (static_cast<double>(totalTilesProcessed) / estimatedTotalParentTiles) * 100.0 : 0.0;

            std::cout << "\rProgress: " << std::fixed << std::setprecision(1) << progressPercent << "% | "
                      << "Processed: " << totalTilesProcessed << "/~" << estimatedTotalParentTiles << " tiles | "
                      << "Current: " << std::fixed << std::setprecision(1) << tilesPerSecond << " tiles/s | "
                      << "Average: " << std::fixed << std::setprecision(1) << overallTilesPerSecond << " tiles/s"
                      << std::flush;

            FILE *fp = fopen("progress.txt", "w");  // open file for writing
            if (fp) {
            fprintf(fp, "%i", (int)progressPercent + 100);  // write progressPercent to file
                fclose(fp);  // close file
            }

            lastReportTime = currentTime;
            tilesSinceLastReport = 0;
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count();
    double overallTilesPerSecond = (totalTilesProcessed * 1000.0) / totalElapsed;
    double progressPercent = (estimatedTotalParentTiles > 0) ? 
        (static_cast<double>(totalTilesProcessed) / estimatedTotalParentTiles) * 100.0 : 100.0;

    std::cout << "\rProgress: " << std::fixed << std::setprecision(1) << progressPercent << "% | "
              << "Processed: " << totalTilesProcessed << "/~" << estimatedTotalParentTiles << " tiles | "
              << "Average: " << std::fixed << std::setprecision(1) << overallTilesPerSecond << " tiles/s | "
              << "Total time: " << std::fixed << std::setprecision(1) << (totalElapsed / 1000.0) << " seconds"
              << std::endl;

    std::cout << "Tiles downscaled to zoom " << newZoom << " successfully." << std::endl;
    return 0;
}
