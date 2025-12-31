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
#include <sys/cdefs.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

constexpr size_t NUM_THREADS = 16;

class FastMap {
  private:
    struct LocationEntry_ {
        int32_t min;
        int32_t max;
        size_t freq;
        int64_t sum;
        std::string location;
        LocationEntry_() {
            min = std::numeric_limits<int32_t>::max();
            max = std::numeric_limits<int32_t>::min();
            freq = 0;
            sum = 0;
        }
    };
    std::vector<LocationEntry_> data;
    size_t capacity_;
    size_t mask_;

    size_t fast_hash(std::string_view sv) {
        assert(!sv.empty());

        uint64_t hash = 0;
        for (size_t index = 0; index < sv.size(); ++index)
            hash = hash * 1315423911u + static_cast<unsigned char>(sv[index]);
        return hash;
    }

    size_t getIdx(std::string_view queryLocation) {
        assert(!queryLocation.empty());

        size_t hash = fast_hash(queryLocation);
        size_t idx = hash & this->mask_;
        while (true) {
            const auto& location = this->data[idx].location;
            if (location.size() <= 0 || location == queryLocation) {
                return idx;
            }
            idx = (idx + 1) & mask_;
        }
    }

    double roundTowardsINF(double value) const { return std::round(value * 10.0) / 10.0; }

    bool sortInplace() {
        assert(this->data.size() > 0);

        std::sort(this->data.begin(), this->data.end(),
                  [](const LocationEntry_& a, const LocationEntry_& b) {
                      return a.location < b.location;
                  });

        return true;
    }

  public:
    FastMap(size_t initCap = 1 << 14) : capacity_(initCap), mask_(initCap - 1) {
        this->data.resize(initCap, LocationEntry_());
    }

    size_t size() const { return this->capacity_; }

    __attribute__((hot)) bool update(std::string_view location, int32_t temperature) {
        assert(!location.empty());

        size_t idx = this->getIdx(location);

        assert(idx >= 0 && idx < this->capacity_);

        if (this->data[idx].location.size() <= 0) {
            this->data[idx].location = std::string(location);
        }
        this->data[idx].freq += 1;
        this->data[idx].sum += temperature;
        this->data[idx].min = std::min(this->data[idx].min, temperature);
        this->data[idx].max = std::max(this->data[idx].max, temperature);

        return true;
    }

    bool update(const FastMap& other) {
        for (size_t i = 0; i < other.size(); ++i) {
            const auto& otherLocation = other.data[i].location;

            if (otherLocation.size() <= 0) continue;

            size_t thisIdx = this->getIdx(otherLocation);

            if (this->data[thisIdx].location.size() <= 0) {
                this->data[thisIdx].location = otherLocation;
            }
            this->data[thisIdx].freq += other.data[i].freq;
            this->data[thisIdx].sum += other.data[i].sum;
            this->data[thisIdx].min = std::min(this->data[thisIdx].min, other.data[i].min);
            this->data[thisIdx].max = std::max(this->data[thisIdx].max, other.data[i].max);
        }

        return true;
    }

    void printSorted() {
        this->sortInplace();

        std::string outBuffer;
        outBuffer.reserve(2 * 1024 * 1024);

        for (const auto& locEntry : this->data) {
            const auto& location = locEntry.location;

            if (location.size() <= 0) continue;

            if (outBuffer.size() > 1) outBuffer += ", ";

            outBuffer += location;
            outBuffer += "=";

            double min_val = locEntry.min / 10.0;
            double max_val = locEntry.max / 10.0;
            double avg = roundTowardsINF(locEntry.sum / static_cast<double>(locEntry.freq * 10));

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
int32_t parseInt32(const char* start) {
    int32_t value = 0;
    bool isNegative = false;

    if (start[0] == '-') {
        isNegative = true;
        start++;
    }

    value = (start[0] - '0') * 10 + start[2] - '0';

    if (start[2] == '.') {
        value = (start[0] - '0') * 100 + (start[1] - '0') * 10 + start[3] - '0';
    }

    return isNegative ? -value : value;
}

std::pair<std::string_view, int32_t> parseLine(std::string_view line) {
    assert(!line.empty());

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
    int32_t temp_int = parseInt32(start);

    return std::make_pair(locationView, temp_int);
}

size_t skipTillNextLine(size_t startPos, MMAPFile f) {
    assert(startPos > 0);

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

void processLinesInCurrBatch(size_t startPos, size_t batchEndPos, MMAPFile f, FastMap& m) {
    size_t pos = startPos;

    while (pos < batchEndPos && pos < f.fileSize) {
        size_t newlinePos = pos;
        while (newlinePos < f.fileSize && *(f.filePtr + newlinePos) != '\n') {
            ++newlinePos;
        }

        std::string_view line(f.filePtr + pos, newlinePos - pos);

        assert(!line.empty());

        auto result = parseLine(line);
        auto [location, temperature] = result;
        m.update(location, temperature);

        pos = newlinePos + 1;
    }
}

void accumulateBatch(size_t threadIndex, size_t startPos, size_t batchSizeBytes, FastMap& m,
                     MMAPFile f) {
    assert(batchSizeBytes > 0);

    size_t batchEnd = startPos + batchSizeBytes;

    // Skip to the next line boundary at the start for non-zero threads
    if (threadIndex > 0) {
        size_t bytesSkipped = skipTillNextLine(startPos, f);
        startPos += bytesSkipped;
    }

    processLinesInCurrBatch(startPos, batchEnd, f, m);
}

void accumulateThreadResults(const std::vector<FastMap>& maps, FastMap& finalMap) {
    for (const auto& m : maps) {
        finalMap.update(m);
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

void processLinesInSingleBatch(MMAPFile f, FastMap& finalMap) {
    accumulateBatch(0, 0, f.fileSize, finalMap, f);
}

void accumulate(MMAPFile f, FastMap& finalMap) {
    size_t batchSize = getBatchSize(f.fileSize);

    assert(batchSize > 0);

    if (batchSize > 4 * 1024) {
        processInBatches(f, batchSize, finalMap);
    } else {
        processLinesInSingleBatch(f, finalMap);
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
