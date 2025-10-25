#include <iostream>
#include <experimental/filesystem>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::experimental::filesystem;

unsigned char toGray(unsigned char r, unsigned char g, unsigned char b) {
    return static_cast<unsigned char>(0.299*r + 0.587*g + 0.114*b);
}

void processImage(const fs::path& inputFile, const fs::path& outputFile) {
    int w, h, n;
    unsigned char* data = stbi_load(inputFile.string().c_str(), &w, &h, &n, 4);
    if (!data) {
        std::cerr << "Failed to load: " << inputFile << "\n";
        return;
    }

    // Convert to grayscale in place
    for (int i=0; i<w*h; i++) {
        unsigned char g = toGray(data[i*4+0], data[i*4+1], data[i*4+2]);
        data[i*4+0] = data[i*4+1] = data[i*4+2] = g;
    }

    fs::create_directories(outputFile.parent_path());

    // Save based on extension
    std::string ext = inputFile.extension().string();
    if (ext == ".png" || ext == ".PNG") {
        stbi_write_png(outputFile.string().c_str(), w, h, 4, data, w*4);
    } else {
        stbi_write_jpg(outputFile.string().c_str(), w, h, 4, data, 100);
    }

    stbi_image_free(data);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: grayscale_converter <input_directory> <output_directory>\n";
        return 1;
    }

    std::string inputDir = argv[1];
    std::string outputDir = argv[2];

    for (auto& p : fs::recursive_directory_iterator(inputDir)) {
        if (!fs::is_regular_file(p)) continue;

        std::string ext = p.path().extension().string();
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" ||
            ext == ".JPG" || ext == ".JPEG" || ext == ".PNG") {
            
            // compute relative path manually
            std::string rel = p.path().string().substr(inputDir.size());
            fs::path relPath = rel;
            fs::path outPath = fs::path(outputDir) / relPath;

            processImage(p.path(), outPath);
            std::cout << "Converted: " << p.path() << " -> " << outPath << "\n";
        }
    }


    std::cout << "All images converted to grayscale.\n";
    return 0;
}
