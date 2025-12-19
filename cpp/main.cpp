#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <limits>
#include <map>
#include <sstream>
#include <string>

struct LocationStats {
    double min = std::numeric_limits<double>::max();
    double max = std::numeric_limits<double>::lowest();
    double sum = 0;
    int freq = 0;
};

void printResults(const std::map<std::string, LocationStats>& m) {
    std::string outBuffer;
    outBuffer.reserve(8 * 1024 * 1024);

    bool first = true;
    outBuffer += "{";
    for (const auto& [loc, stat] : m) {
        char buf[32];
        if (!first) outBuffer += ", ";
        outBuffer += loc;
        outBuffer += "=";
        std::snprintf(buf, sizeof(buf), "%.1f/%.1f/%.1f", stat.min, stat.sum / stat.freq, stat.max);
        outBuffer += buf;
        first = false;
    }
    outBuffer += "}\n";

    std::cout << outBuffer;
}

std::pair<std::string, double> parseLine(const std::string& line) {
    std::istringstream ss(line);

    std::string location, tempString;
    std::getline(ss, location, ';');
    std::getline(ss, tempString, ';');
    double temperature = std::stod(tempString);

    return {location, temperature};
}

void updateStats(const std::string& location, const double& temperature,
                 std::map<std::string, LocationStats>& m) {
    LocationStats* locationStat = &m[location];
    locationStat->min = std::fmin(locationStat->min, temperature);
    locationStat->max = std::fmax(locationStat->max, temperature);
    locationStat->freq++;
    locationStat->sum += temperature;
}

std::map<std::string, LocationStats> accumulate(std::ifstream& f) {
    std::map<std::string, LocationStats> m;
    std::string tempBuffer;
    while (std::getline(f, tempBuffer)) {
        auto [location, temperature] = parseLine(tempBuffer);
        updateStats(location, temperature, m);
    }
    return m;
}

void oneBrc() {
    std::ifstream f("../data/measurements.txt");

    std::map<std::string, LocationStats> m = accumulate(f);

    printResults(m);
}

int main() {
    oneBrc();

    return EXIT_SUCCESS;
}
