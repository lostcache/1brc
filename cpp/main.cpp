#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

// Linux-specific headers
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <unistd.h>

// Branch prediction hints
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

constexpr size_t NUM_THREADS = 8;
constexpr size_t BUFFER_SIZE = 64 * 1024;

class Arena {
  private:
    static constexpr size_t CHUNK_SIZE = 1024 * 1024;
    std::vector<char*> chunks;
    char* current;
    size_t remaining;

  public:
    Arena() : current(nullptr), remaining(0) {}

    ~Arena() {
        for (char* chunk : chunks) {
            delete[] chunk;
        }
    }

    char* allocate(size_t size) {
        if (size > remaining) {
            size_t allocSize = std::max(CHUNK_SIZE, size);
            current = new char[allocSize];
            chunks.push_back(current);
            remaining = allocSize;
        }
        char* result = current;
        current += size;
        remaining -= size;
        return result;
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
};

class FastMap {
  private:
    struct LocationEntry_ {
        int32_t min;
        int32_t max;
        size_t freq;
        int64_t sum;
        const char* location;
        size_t location_len;
        LocationEntry_() {
            min = std::numeric_limits<int32_t>::max();
            max = std::numeric_limits<int32_t>::min();
            freq = 0;
            sum = 0;
            location = nullptr;
            location_len = 0;
        }
    };
    std::vector<LocationEntry_> data;
    size_t capacity_;
    size_t mask_;
    Arena* arena;

    __attribute__((hot)) inline size_t fast_hash(const char* data, size_t len) const {
        uint64_t hash = 0;
        for (size_t index = 0; index < len; ++index)
            hash = hash * 1315423911u + static_cast<unsigned char>(data[index]);
        return hash;
    }

    __attribute__((hot)) inline bool str_equal(const char* a, size_t a_len, const char* b, size_t b_len) const {
        return a_len == b_len && memcmp(a, b, a_len) == 0;
    }

    __attribute__((hot)) size_t getIdx(const char* queryLocation, size_t len) {
        size_t hash = fast_hash(queryLocation, len);
        size_t idx = hash & this->mask_;
        while (true) {
            auto& entry = this->data[idx];
            // Most lookups find the entry on first try (empty or match)
            if (likely(entry.freq == 0 || str_equal(entry.location, entry.location_len, queryLocation, len))) {
                return idx;
            }
            idx = (idx + 1) & mask_;
        }
    }

    // Optimized rounding: avoid std::round overhead by using direct arithmetic
    // This is equivalent to round(value * 10.0) / 10.0 but faster
    // Handles both positive and negative numbers correctly
    double roundTowardsINF(double value) const {
        double scaled = value * 10.0;
        // Use conditional to handle negative numbers correctly (round away from zero)
        return (scaled >= 0.0) ? std::floor(scaled + 0.5) / 10.0 : std::ceil(scaled - 0.5) / 10.0;
    }

    bool sortInplace() {
        assert(this->data.size() > 0);

        std::sort(this->data.begin(), this->data.end(),
                  [](const LocationEntry_& a, const LocationEntry_& b) {
                      if (a.freq == 0 && b.freq == 0) return false;
                      if (a.freq == 0) return false;
                      if (b.freq == 0) return true;
                      int cmp = memcmp(a.location, b.location, std::min(a.location_len, b.location_len));
                      if (cmp != 0) return cmp < 0;
                      return a.location_len < b.location_len;
                  });

        return true;
    }

  public:
    FastMap(Arena* arena, size_t initCap = 1 << 14)
        : capacity_(initCap), mask_(initCap - 1), arena(arena) {
        this->data.resize(initCap, LocationEntry_());
    }

    size_t size() const { return this->capacity_; }

    __attribute__((hot)) inline void update(const char* location, size_t len, int32_t temperature) {
        size_t idx = this->getIdx(location, len);

        auto& entry = this->data[idx];
        if (likely(entry.freq != 0)) {
            // Existing entry - just update (most common case in 1BRC)
            entry.min = std::min(entry.min, temperature);
            entry.max = std::max(entry.max, temperature);
            entry.sum += temperature;
            entry.freq += 1;
        } else {
            // New entry - allocate from arena (rare after warmup)
            char* key_copy = arena->allocate(len);
            memcpy(key_copy, location, len);
            entry.location = key_copy;
            entry.location_len = len;
            entry.min = temperature;
            entry.max = temperature;
            entry.sum = temperature;
            entry.freq = 1;
        }
    }

    void mergeFrom(const FastMap& other) {
        for (size_t i = 0; i < other.size(); ++i) {
            const auto& otherEntry = other.data[i];

            if (otherEntry.freq == 0) continue;

            size_t idx = this->getIdx(otherEntry.location, otherEntry.location_len);
            auto& thisEntry = this->data[idx];

            if (thisEntry.freq == 0) {
                char* key_copy = arena->allocate(otherEntry.location_len);
                memcpy(key_copy, otherEntry.location, otherEntry.location_len);
                thisEntry.location = key_copy;
                thisEntry.location_len = otherEntry.location_len;
                thisEntry.min = otherEntry.min;
                thisEntry.max = otherEntry.max;
                thisEntry.sum = otherEntry.sum;
                thisEntry.freq = otherEntry.freq;
            } else {
                thisEntry.freq += otherEntry.freq;
                thisEntry.sum += otherEntry.sum;
                thisEntry.min = std::min(thisEntry.min, otherEntry.min);
                thisEntry.max = std::max(thisEntry.max, otherEntry.max);
            }
        }
    }

    void printSorted() {
        this->sortInplace();

        std::string outBuffer;
        outBuffer.reserve(2 * 1024 * 1024);

        for (const auto& locEntry : this->data) {
            if (locEntry.freq == 0) break; // All remaining are empty

            if (outBuffer.size() > 1) outBuffer += ", ";

            outBuffer.append(locEntry.location, locEntry.location_len);
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

size_t getFileSize(const std::string& fileName) { return std::filesystem::file_size(fileName); }

size_t getBatchSize(size_t fileSize) { return (fileSize + (NUM_THREADS - 1)) / NUM_THREADS; }

// Assumes format: [-]D[D].D where D is digit
__attribute__((hot)) inline int32_t parseInt32(const char* start) {
    int32_t value = 0;
    bool isNegative = false;

    if (start[0] == '-') {
        isNegative = true;
        start++;
    }

    if (start[1] == '.') {
        value = (start[0] - '0') * 10 + start[2] - '0';
    } else {
        value = (start[0] - '0') * 100 + (start[1] - '0') * 10 + start[3] - '0';
    }

    return isNegative ? -value : value;
}

// Semicolon is always at len - 4, len - 5, or len - 6
// Temperature format: [-]D[D].D (3-5 chars + semicolon)
__attribute__((hot)) inline std::pair<std::string_view, int32_t> parseLine(std::string_view line) {
    size_t len = line.size();
    
    size_t semicolonPos;
    if (line[len - 4] == ';') {
        semicolonPos = len - 4;
    } else if (line[len - 5] == ';') {
        semicolonPos = len - 5;
    } else {
        semicolonPos = len - 6;
    }

    std::string_view locationView(line.data(), semicolonPos);
    int32_t temp_int = parseInt32(line.data() + semicolonPos + 1);

    return std::make_pair(locationView, temp_int);
}

class BufferedFileReader {
  private:
    int fd;
    char buffer[BUFFER_SIZE];
    char lineBuffer[256];
    size_t lineBufferUsed;
    size_t bufferPos;
    size_t bufferEnd;
    bool eof;

  public:
    BufferedFileReader(int fd, size_t startPos)
        : fd(fd), lineBufferUsed(0), bufferPos(0), bufferEnd(0), eof(false) {
        lseek(fd, startPos, SEEK_SET);
    }

    __attribute__((hot)) bool readLine(std::string_view& line, size_t& bytesRead) {
        lineBufferUsed = 0;
        bytesRead = 0;

        while (true) {
            if (bufferPos >= bufferEnd) {
                ssize_t n = read(fd, buffer, BUFFER_SIZE);
                if (n <= 0) {
                    eof = true;
                    if (lineBufferUsed > 0) {
                        line = std::string_view(lineBuffer, lineBufferUsed);
                        bytesRead = lineBufferUsed;
                        return true;
                    }
                    return false;
                }
                bufferEnd = n;
                bufferPos = 0;
            }

            const char* start = buffer + bufferPos;
            const char* newline_ptr =
                static_cast<const char*>(memchr(start, '\n', bufferEnd - bufferPos));

            if (likely(newline_ptr != nullptr)) {
                size_t chunkSize = newline_ptr - start;

                if (likely(lineBufferUsed == 0)) {
                    line = std::string_view(start, chunkSize);
                    bytesRead = chunkSize + 1;
                    bufferPos = (newline_ptr - buffer) + 1;
                    return true;
                } else {
                    memcpy(lineBuffer + lineBufferUsed, start, chunkSize);
                    lineBufferUsed += chunkSize;
                    line = std::string_view(lineBuffer, lineBufferUsed);
                    bytesRead = lineBufferUsed + 1;
                    bufferPos = (newline_ptr - buffer) + 1;
                    return true;
                }
            } else {
                size_t remaining = bufferEnd - bufferPos;
                if (lineBufferUsed + remaining < sizeof(lineBuffer)) {
                    memcpy(lineBuffer + lineBufferUsed, start, remaining);
                    lineBufferUsed += remaining;
                }
                bytesRead += remaining;
                bufferPos = bufferEnd;
            }
        }
    }

    bool isEof() const { return eof; }
};

size_t skipToNextLine(int fd, size_t startPos) {
    if (startPos == 0) return 0;

    // Check previous character
    char prevChar;
    lseek(fd, startPos - 1, SEEK_SET);
    if (read(fd, &prevChar, 1) != 1) return 0;
    if (prevChar == '\n') return 0;

    // Skip to next newline
    lseek(fd, startPos, SEEK_SET);
    size_t bytesSkipped = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        bytesSkipped++;
        if (c == '\n') break;
    }

    return bytesSkipped;
}

void processBatch(size_t threadIndex, size_t startPos, size_t batchSize, FastMap& m,
                  const char* filename) {
    // Pin thread to CPU core (Linux-specific)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(threadIndex % std::thread::hardware_concurrency(), &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    int fd = open(filename, O_RDONLY);
    if (fd == -1) {
        std::cerr << "Could not open file in thread " << threadIndex << std::endl;
        return;
    }

    // Tell kernel we're reading sequentially
    posix_fadvise(fd, startPos, batchSize, POSIX_FADV_SEQUENTIAL | POSIX_FADV_WILLNEED);

    size_t processedBytes = 0;
    if (threadIndex > 0) {
        processedBytes = skipToNextLine(fd, startPos);
    }

    BufferedFileReader reader(fd, startPos + processedBytes);
    std::string_view line;
    size_t lineBytes;

    while (processedBytes < batchSize) {
        if (!reader.readLine(line, lineBytes)) {
            break;
        }

        if (likely(!line.empty())) {
            auto [location, temperature] = parseLine(line);
            m.update(location.data(), location.size(), temperature);
        }

        processedBytes += lineBytes;
    }

    close(fd);
}

void accumulateThreadResults(const std::vector<FastMap>& maps, FastMap& finalMap) {
    for (const auto& m : maps) {
        finalMap.mergeFrom(m);
    }
}

void processInBatches(const char* filename, size_t fileSize, FastMap& finalMap) {
    size_t batchSize = getBatchSize(fileSize);

    std::vector<Arena> arenas(NUM_THREADS);
    std::vector<FastMap> maps;
    maps.reserve(NUM_THREADS);
    for (size_t i = 0; i < NUM_THREADS; ++i) {
        maps.emplace_back(&arenas[i]);
    }

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (size_t i = 0; i < NUM_THREADS; ++i) {
        size_t startPos = i * batchSize;
        threads.emplace_back(processBatch, i, startPos, batchSize, std::ref(maps[i]), filename);
    }

    for (auto& t : threads) {
        t.join();
    }

    accumulateThreadResults(maps, finalMap);
}

void processInSingleBatch(const char* filename, size_t fileSize, FastMap& finalMap) {
    processBatch(0, 0, fileSize, finalMap, filename);
}

void oneBrc(const char* filename) {
    size_t fileSize = std::filesystem::file_size(filename);

    Arena finalArena;
    FastMap finalMap(&finalArena);

    if (fileSize > 4 * 1024) {
        processInBatches(filename, fileSize, finalMap);
    } else {
        processInSingleBatch(filename, fileSize, finalMap);
    }

    std::cout << '{';
    finalMap.printSorted();
    std::cout << "}\n";
}

int main(int argc, char* argv[]) {
    const char* filename = argc > 1 ? argv[1] : "../data/measurements.txt";
    oneBrc(filename);

    return EXIT_SUCCESS;
}
