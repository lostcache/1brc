#include <algorithm>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

constexpr char NUM_THREADS = 8;
constexpr int64_t MAX_FILE_READ_BYTES = 1 * 1024 * 1024 * 512; // 512MB

struct LocationStats {
    int32_t min = std::numeric_limits<int32_t>::max();
    int32_t max = std::numeric_limits<int32_t>::min();
    int64_t sum = 0;
    int32_t freq = 0;
};

int64_t getFileSize(const std::string& fileName) {
    return static_cast<int64_t>(std::filesystem::file_size(fileName));
}

int64_t getBatchSize(const std::string& fileName) {
    int64_t fileSize = getFileSize(fileName);
    return (fileSize + (NUM_THREADS - 1)) / NUM_THREADS;
}

double round1(double value) { return std::round(value * 10.0) / 10.0; }

void printResults(const std::map<std::string, LocationStats>& m) {
    std::string outBuffer;
    outBuffer.reserve(8 * 1024 * 1024);

    outBuffer += "{";
    for (const auto& [location, stat] : m) {
        if (outBuffer.size() > 1) outBuffer += ", ";

        outBuffer += location;
        outBuffer += "=";

        double min_val = stat.min / 10.0;
        double max_val = stat.max / 10.0;
        double avg = round1(stat.sum / static_cast<double>(stat.freq * 10));

        size_t pos = outBuffer.size();
        outBuffer.resize(pos + 32);
        int writtenBytes =
            std::snprintf(&outBuffer[pos], 32, "%.1f/%.1f/%.1f", min_val, avg, max_val);
        outBuffer.resize(pos + writtenBytes);
    }
    outBuffer += "}\n";

    std::cout << outBuffer;
}

std::optional<std::pair<std::string_view, int32_t>> parseLine(std::string_view line) {
    size_t semicolonPos = line.find(';');
    if (semicolonPos == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view locationView = line.substr(0, semicolonPos);

    // Find start and end of temperature
    size_t tempStart = semicolonPos + 1;
    if (tempStart >= line.size()) {
        return std::nullopt;
    }

    // Trim trailing whitespace
    size_t tempEnd = line.size();
    while (tempEnd > tempStart && (line[tempEnd - 1] == ' ' || line[tempEnd - 1] == '\t' ||
                                   line[tempEnd - 1] == '\n' || line[tempEnd - 1] == '\r')) {
        --tempEnd;
    }

    if (tempEnd <= tempStart) {
        return std::nullopt;
    }

    double temperature;
    const char* start = line.data() + tempStart;
    const char* end = line.data() + tempEnd;
    auto [ptr, ec] = std::from_chars(start, end, temperature);

    if (ec != std::errc{}) {
        return std::nullopt;
    }

    int32_t temp_int = static_cast<int32_t>(std::round(temperature * 10));
    return std::make_pair(locationView, temp_int);
}

void updateStats(std::string_view location, int32_t temperature,
                 std::map<std::string, LocationStats>& m) {
    auto& stats = m[std::string(location)];
    stats.min = std::min(stats.min, temperature);
    stats.max = std::max(stats.max, temperature);
    stats.freq++;
    stats.sum += temperature;
}

int64_t skipTillNextLine(int threadIndex, int64_t startPos, std::ifstream& f) {
    if (startPos == 0) return 0;

    // No need to skip if alredy at start of a new line
    f.seekg(startPos - 1);
    char prevChar;
    if (f.get(prevChar) && prevChar == '\n') {
        return 0;
    }

    f.seekg(startPos);
    int64_t bytesSkipped = 0;
    char c;
    while (f.get(c) && c != '\n') {
        bytesSkipped++;
    }
    if (f.good()) bytesSkipped++; // skip '\n'
    return bytesSkipped;
}

int64_t readTillEndOfLine(int64_t bytesRead, std::string& fileReadBuffer, std::ifstream& f) {
    if (bytesRead > 0 && fileReadBuffer[bytesRead - 1] != '\n') {
        while (f.get(fileReadBuffer[bytesRead]) && fileReadBuffer[bytesRead] != '\n') {
            bytesRead++;
        }
    }
    return bytesRead;
}

void processLines(std::span<const char> buffer, std::map<std::string, LocationStats>& m) {
    size_t pos = 0;
    size_t validBytes = buffer.size();

    while (pos < validBytes) {
        size_t newlinePos = pos;
        while (newlinePos < validBytes && buffer[newlinePos] != '\n') {
            ++newlinePos;
        }

        if (newlinePos > pos) {
            std::string_view line(buffer.data() + pos, newlinePos - pos);

            if (!line.empty()) {
                auto result = parseLine(line);
                if (result) {
                    auto [location, temperature] = *result;
                    updateStats(location, temperature, m);
                }
            }
        }

        pos = newlinePos + 1;
    }
}

void accumulateBatch(int threadIndex, int64_t startPos, int64_t batchBytes,
                     std::map<std::string, LocationStats>& m, const std::string& fileName) {
    std::ifstream f(fileName, std::ios::binary);

    if (!f.is_open()) {
        std::cerr << "Failed to open file: " << fileName << std::endl;
        return;
    }

    // Skip to the next line boundary at the start for non-zero threads
    int64_t processedBytes = 0;
    if (threadIndex) {
        processedBytes = skipTillNextLine(threadIndex, startPos, f);
    }

    // Read mini-batches of size MAX_FILE_READ_BYTES to avoid Out of Memory Error
    while (processedBytes < batchBytes) {
        std::string miniBatchBuffer;
        // extra 128 bytes to read till next delimiter char even if not part of the batch.
        miniBatchBuffer.resize(MAX_FILE_READ_BYTES + 128);

        f.read(miniBatchBuffer.data(), std::min(MAX_FILE_READ_BYTES, batchBytes - processedBytes));
        std::streamsize bytesRead = f.gcount();
        bytesRead = readTillEndOfLine(bytesRead, miniBatchBuffer, f);

        if (bytesRead <= 0) break;

        processLines(std::span<const char>(miniBatchBuffer.data(), bytesRead), m);

        processedBytes += bytesRead;
    }
}

void accumulateThreadResults(const std::vector<std::map<std::string, LocationStats>>& maps,
                             std::map<std::string, LocationStats>& finalMap) {
    for (const auto& m : maps) {
        for (const auto& [location, stats] : m) {
            auto& finalStats = finalMap[location];
            finalStats.freq += stats.freq;
            finalStats.sum += stats.sum;
            finalStats.max = std::max(finalStats.max, stats.max);
            finalStats.min = std::min(finalStats.min, stats.min);
        }
    }
}

void processInBatches(int64_t batchSize, const std::string& fileName,
                      std::map<std::string, LocationStats>& finalMap) {
    std::vector<std::map<std::string, LocationStats>> maps(NUM_THREADS);
    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i) {
        int64_t startPos = i * batchSize;
        threads.emplace_back(accumulateBatch, i, startPos, batchSize, std::ref(maps[i]),
                             std::cref(fileName));
    }

    for (auto& t : threads) {
        t.join();
    }

    accumulateThreadResults(maps, finalMap);
}

void processInSingleBatch(const std::string& fileName,
                          std::map<std::string, LocationStats>& finalMap) {
    int64_t fileSize = getFileSize(fileName);
    accumulateBatch(0, 0, fileSize, finalMap, fileName);
}

void accumulate(const std::string& fileName, std::map<std::string, LocationStats>& finalMap) {
    int64_t batchSize = getBatchSize(fileName);

    assert(batchSize > 0);

    if (batchSize > 4 * 1024) {
        processInBatches(batchSize, fileName, finalMap);
    } else {
        processInSingleBatch(fileName, finalMap);
    }
}

void oneBrc(const char* filename) {
    std::map<std::string, LocationStats> finalMap;
    accumulate(filename, finalMap);

    printResults(finalMap);
}

int main(int argc, char* argv[]) {
    const char* filename = argc > 1 ? argv[1] : "../data/measurements.txt";
    oneBrc(filename);

    return EXIT_SUCCESS;
}
