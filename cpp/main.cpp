#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

constexpr char NUM_THREADS = 16;

struct LocationStats {
    int32_t min = std::numeric_limits<int32_t>::max();
    int32_t max = std::numeric_limits<int32_t>::min();
    int64_t sum = 0;
    int32_t freq = 0;
};

struct MMAPFile {
    const char* filePtr;
    const int64_t fileSize;
};

int64_t getFileSize(const std::string& fileName) {
    return static_cast<int64_t>(std::filesystem::file_size(fileName));
}

MMAPFile getMmappedFile(const char* fileName) {
    int32_t fd = open(fileName, O_RDONLY);

    if (fd == -1) {
        std::cerr << "Could not read input file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    int64_t fileSize = getFileSize(fileName);

    char* map = static_cast<char*>(mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));

    close(fd);

    if (map == MAP_FAILED) {
        std::cerr << "Cound not use mmap on the file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    return {map, fileSize};
}

void unMapFile(MMAPFile f) { munmap(const_cast<char*>(f.filePtr), f.fileSize); }

int64_t getBatchSize(int64_t fileSize) { return (fileSize + (NUM_THREADS - 1)) / NUM_THREADS; }

double round1(double value) { return std::round(value * 10.0) / 10.0; }

void printResults(const std::unordered_map<std::string, LocationStats>& m) {
    std::vector<std::string> keys;
    keys.reserve(1 * 1024 * 1024);
    for (const auto& it : m) {
        keys.emplace_back(it.first);
    }
    sort(keys.begin(), keys.end());

    std::string outBuffer;
    outBuffer.reserve(2 * 1024 * 1024);

    outBuffer += "{";
    for (const auto& location : keys) {
        auto& stat = m.at(location);
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

// Assumes format: [-]D[D].D where D is digit
int32_t parseInt32(const char* start, const char* end) {
    bool neg = (*start == '-');
    if (neg) start++;
    size_t len = end - start;
    int32_t num = 0;
    if (len == 3) {
        num += (*(start + 0) & 0xF) * 10;
        num += (*(start + 2) & 0xF) * 1;
    } else if (len == 4) {
        num += (*(start + 0) & 0xF) * 100;
        num += (*(start + 1) & 0xF) * 10;
        num += (*(start + 3) & 0xF) * 1;
    }
    return neg ? -num : num;
}

std::pair<std::string_view, int32_t> parseLine(std::string_view line) {
    size_t semicolonPos = line.find(';');
    assert(semicolonPos != std::string_view::npos);
    std::string_view locationView = line.substr(0, semicolonPos);

    // Find start and end of temperature
    size_t tempStart = semicolonPos + 1;
    assert(tempStart < line.size());

    // Trim trailing whitespace
    size_t tempEnd = line.size();
    while (tempEnd > tempStart && (line[tempEnd - 1] == ' ' || line[tempEnd - 1] == '\t' ||
                                   line[tempEnd - 1] == '\n' || line[tempEnd - 1] == '\r')) {
        --tempEnd;
    }

    assert(tempEnd > tempStart);

    const char* start = line.data() + tempStart;
    const char* end = line.data() + tempEnd;
    int32_t temp_int = parseInt32(start, end);

    return std::make_pair(locationView, temp_int);
}

void updateStats(std::string_view location, int32_t temperature,
                 std::unordered_map<std::string, LocationStats>& m) {
    auto& stats = m[std::string(location)];
    stats.min = std::min(stats.min, temperature);
    stats.max = std::max(stats.max, temperature);
    stats.freq++;
    stats.sum += temperature;
}

int64_t skipTillNextLine(int threadIndex, int64_t startPos, MMAPFile f) {
    assert(startPos > 0);
    assert(threadIndex > 0);

    const char* prevChar = f.filePtr + startPos - 1;
    if (*prevChar == '\n') {
        return 0;
    }

    int64_t bytesSkipped = 0;
    while (*(f.filePtr + startPos + bytesSkipped) != '\n') {
        bytesSkipped++;
    }

    bytesSkipped++; // skip '\n'

    assert(bytesSkipped > 0);

    return bytesSkipped;
}

void processLinesInCurrBatch(int64_t startPos, int64_t batchSize, MMAPFile f,
                             std::unordered_map<std::string, LocationStats>& m) {
    size_t pos = startPos;
    size_t batchEnd = startPos + batchSize;

    while (pos < batchEnd && pos < f.fileSize) {
        size_t newlinePos = pos;
        while (pos < batchEnd && pos < f.fileSize && *(f.filePtr + newlinePos) != '\n') {
            ++newlinePos;
        }

        std::string_view line(f.filePtr + pos, newlinePos - pos);

        assert(!line.empty());

        auto result = parseLine(line);
        auto [location, temperature] = result;
        updateStats(location, temperature, m);
        assert(m.size() > 0);

        pos = newlinePos + 1;
    }
}

void accumulateBatch(int threadIndex, int64_t startPos, int64_t batchSizeBytes,
                     std::unordered_map<std::string, LocationStats>& m, MMAPFile f) {
    assert(startPos >= 0);
    assert(batchSizeBytes > 0);

    // Skip to the next line boundary at the start for non-zero threads
    if (threadIndex > 0) {
        int64_t bytesSkipped = skipTillNextLine(threadIndex, startPos, f);
        startPos += bytesSkipped;
    }

    processLinesInCurrBatch(startPos, batchSizeBytes, f, m);
}

void accumulateThreadResults(
    const std::vector<std::unordered_map<std::string, LocationStats>>& maps,
    std::unordered_map<std::string, LocationStats>& finalMap) {
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

void processInBatches(MMAPFile f, int64_t batchSize,
                      std::unordered_map<std::string, LocationStats>& finalMap) {
    std::vector<std::unordered_map<std::string, LocationStats>> maps(NUM_THREADS);
    for (auto& m : maps) {
        m.reserve(2 * 1024 * 1024);
    }

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (int i = 0; i < NUM_THREADS; ++i) {
        int64_t startPos = i * batchSize;
        threads.emplace_back(accumulateBatch, i, startPos, batchSize, std::ref(maps[i]), f);
    }

    for (auto& t : threads) {
        t.join();
    }

    assert(finalMap.empty());

    accumulateThreadResults(maps, finalMap);
}

void processInOneBatch(MMAPFile f, std::unordered_map<std::string, LocationStats>& finalMap) {
    accumulateBatch(0, 0, f.fileSize, finalMap, f);
}

void accumulate(MMAPFile f, std::unordered_map<std::string, LocationStats>& finalMap) {
    int64_t batchSize = getBatchSize(f.fileSize);

    assert(batchSize > 0);

    if (batchSize > 4 * 1024) {
        processInBatches(f, batchSize, finalMap);
    } else {
        processInOneBatch(f, finalMap);
    }
}

void oneBrc(const char* filename) {
    MMAPFile f = getMmappedFile(filename);

    std::unordered_map<std::string, LocationStats> finalMap;
    finalMap.reserve(2 * 1024 * 1024);

    accumulate(f, finalMap);

    printResults(finalMap);

    unMapFile(f);
}

int main(int argc, char* argv[]) {
    const char* filename = argc > 1 ? argv[1] : "../data/measurements.txt";
    oneBrc(filename);

    return EXIT_SUCCESS;
}
