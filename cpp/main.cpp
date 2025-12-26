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
#include <numeric>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

constexpr size_t NUM_THREADS = 8;

class FastMap {
  private:
    std::vector<int32_t> min_vec;
    std::vector<int32_t> max_vec;
    std::vector<size_t> freq_vec;
    std::vector<int64_t> sum_vec;
    std::vector<std::string> location_vec;
    size_t capacity;
    size_t mask;

    size_t fast_hash(std::string_view sv) {
        size_t hash = 14695981039346656037ULL;
        for (char c : sv) {
            hash ^= static_cast<size_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    std::string_view getLocation(size_t idx) const { return this->location_vec[idx]; }

    int32_t getMin(size_t idx) const { return this->min_vec[idx]; }

    int32_t getMax(size_t idx) const { return this->max_vec[idx]; }

    int64_t getSum(size_t idx) const { return this->sum_vec[idx]; }

    size_t getFreq(size_t idx) const { return this->freq_vec[idx]; }

    size_t getIdx(std::string_view location) {
        size_t hash = fast_hash(location);
        size_t idx = hash & mask;
        while (true) {
            const auto& locationEntry = this->location_vec[idx];
            if (locationEntry.size() <= 0 || locationEntry == location) {
                return idx;
            }
            idx = (idx + 1) & mask;
        }
    }

    void updateLocationEntry(size_t idx, std::string_view location) {
        const auto& locationEntry = this->location_vec[idx];
        if (locationEntry.size() <= 0) {
            this->location_vec[idx] = std::string(location);
        }
    }

    void updateMin(size_t idx, int32_t temperature) {
        this->min_vec[idx] = std::min(this->min_vec[idx], temperature);
    }

    void updateMax(size_t idx, int32_t temperature) {
        this->max_vec[idx] = std::max(this->max_vec[idx], temperature);
    }

    void updateSum(size_t idx, int64_t value) { this->sum_vec[idx] += value; }

    void updateFreq(size_t idx) { this->freq_vec[idx]++; }

    void updateFreq(size_t idx, size_t value) { this->freq_vec[idx] += value; }

    std::vector<size_t> sortKeyIndices() const {
        std::vector<size_t> indices(this->size());
        std::iota(indices.begin(), indices.end(), 0);

        std::sort(indices.begin(), indices.end(), [&](const auto& a, const auto& b) {
            return this->location_vec[a] < this->location_vec[b];
        });

        return indices;
    }

    double roundTowardsINF(double value) const { return std::round(value * 10.0) / 10.0; }

  public:
    FastMap(size_t initCap = 1 << 14) : capacity(initCap), mask(initCap - 1) {
        min_vec.resize(initCap, std::numeric_limits<int32_t>::max());
        max_vec.resize(initCap, std::numeric_limits<int32_t>::min());
        freq_vec.resize(initCap, 0);
        sum_vec.resize(initCap, 0);
        location_vec.resize(initCap, "");
    }

    size_t size() const { return this->capacity; }

    void updateRunning(std::string_view location, int32_t temperature) {
        size_t idx = this->getIdx(location);
        this->updateLocationEntry(idx, location);
        this->updateMin(idx, temperature);
        this->updateMax(idx, temperature);
        this->updateFreq(idx);
        this->updateSum(idx, temperature);
    }

    void updateBatch(const FastMap& other) {
        for (size_t otherIdx = 0; otherIdx < other.size(); ++otherIdx) {
            const auto& otherLocation = other.getLocation(otherIdx);
            if (otherLocation.size() <= 0) continue;
            size_t thisIdx = this->getIdx(otherLocation);

            this->updateLocationEntry(thisIdx, otherLocation);
            this->updateMin(thisIdx, other.getMin(otherIdx));
            this->updateMax(thisIdx, other.getMax(otherIdx));
            this->updateFreq(thisIdx, other.getFreq(otherIdx));
            this->updateSum(thisIdx, other.getSum(otherIdx));
        }
    }

    void printSorted() const {
        std::vector<size_t> sortedKeyIndices = this->sortKeyIndices();

        std::string outBuffer;
        outBuffer.reserve(2 * 1024 * 1024);

        for (auto idx : sortedKeyIndices) {
            if (outBuffer.size() > 1) outBuffer += ", ";

            const auto& location = this->getLocation(idx);

            if (location.size() <= 0) continue;

            outBuffer += location;
            outBuffer += "=";

            double min_val = this->getMin(idx) / 10.0;
            double max_val = this->getMax(idx) / 10.0;
            double avg =
                roundTowardsINF(this->getSum(idx) / static_cast<double>(this->getFreq(idx) * 10));

            size_t pos = outBuffer.size();
            outBuffer.resize(pos + 32);
            int writtenBytes =
                std::snprintf(&outBuffer[pos], 32, "%.1f/%.1f/%.1f", min_val, avg, max_val);
            outBuffer.resize(pos + writtenBytes);
        }

        std::cout << outBuffer;
    }
};

struct MMAPFile {
    const char* filePtr;
    const size_t fileSize;
};

size_t getFileSize(const std::string& fileName) { return std::filesystem::file_size(fileName); }

MMAPFile getMmappedFile(const char* fileName) {
    int fd = open(fileName, O_RDONLY);

    if (fd == -1) {
        std::cerr << "Could not read input file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    size_t fileSize = getFileSize(fileName);

    char* map = static_cast<char*>(mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE, fd, 0));

    close(fd);

    if (map == MAP_FAILED) {
        std::cerr << "Cound not use mmap on the file" << std::endl;
        std::exit(EXIT_FAILURE);
    }

    return {map, fileSize};
}

void unMapFile(MMAPFile f) { munmap(const_cast<char*>(f.filePtr), f.fileSize); }

size_t getBatchSize(size_t fileSize) { return (fileSize + (NUM_THREADS - 1)) / NUM_THREADS; }

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

size_t skipTillNextLine(size_t threadIndex, size_t startPos, MMAPFile f) {
    assert(startPos > 0);
    assert(threadIndex > 0);

    const char* prevChar = f.filePtr + startPos - 1;
    if (*prevChar == '\n') {
        return 0;
    }

    size_t bytesSkipped = 0;
    while (*(f.filePtr + startPos + bytesSkipped) != '\n') {
        bytesSkipped++;
    }

    bytesSkipped++; // skip '\n'

    assert(bytesSkipped > 0);

    return bytesSkipped;
}

void processLinesInCurrBatch(size_t startPos, size_t batchEnd, MMAPFile f, FastMap& m) {
    size_t pos = startPos;

    while (pos < batchEnd && pos < f.fileSize) {
        size_t newlinePos = pos;
        while (newlinePos < f.fileSize && *(f.filePtr + newlinePos) != '\n') {
            ++newlinePos;
        }

        std::string_view line(f.filePtr + pos, newlinePos - pos);

        assert(!line.empty());

        auto result = parseLine(line);
        auto [location, temperature] = result;
        m.updateRunning(location, temperature);
        assert(m.size() > 0);

        pos = newlinePos + 1;
    }
}

void accumulateBatch(size_t threadIndex, size_t startPos, size_t batchSizeBytes, FastMap& m,
                     MMAPFile f) {
    assert(batchSizeBytes > 0);

    size_t batchEnd = startPos + batchSizeBytes;

    // Skip to the next line boundary at the start for non-zero threads
    if (threadIndex > 0) {
        size_t bytesSkipped = skipTillNextLine(threadIndex, startPos, f);
        startPos += bytesSkipped;
    }

    processLinesInCurrBatch(startPos, batchEnd, f, m);
}

void accumulateThreadResults(const std::vector<FastMap>& maps, FastMap& finalMap) {
    for (const auto& m : maps) {
        finalMap.updateBatch(m);
    }
}

void processInBatches(MMAPFile f, size_t batchSize, FastMap& finalMap) {
    std::vector<FastMap> maps(NUM_THREADS, FastMap());

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (size_t i = 0; i < NUM_THREADS; ++i) {
        size_t startPos = i * batchSize;
        threads.emplace_back(accumulateBatch, i, startPos, batchSize, std::ref(maps[i]), f);
    }

    for (auto& t : threads) {
        t.join();
    }

    accumulateThreadResults(maps, finalMap);
}

void processInOneBatch(MMAPFile f, FastMap& finalMap) {
    accumulateBatch(0, 0, f.fileSize, finalMap, f);
}

void accumulate(MMAPFile f, FastMap& finalMap) {
    size_t batchSize = getBatchSize(f.fileSize);

    assert(batchSize > 0);

    if (batchSize > 4 * 1024) {
        processInBatches(f, batchSize, finalMap);
    } else {
        processInOneBatch(f, finalMap);
    }
}

void oneBrc(const char* filename) {
    MMAPFile f = getMmappedFile(filename);

    FastMap finalMap;
    accumulate(f, finalMap);

    std::cout << '{';
    finalMap.printSorted();
    std::cout << "}\n";

    unMapFile(f);
}

int main(int argc, char* argv[]) {
    const char* filename = argc > 1 ? argv[1] : "../data/measurements.txt";
    oneBrc(filename);

    return EXIT_SUCCESS;
}
