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
    std::float_t min = std::numeric_limits<float>::max();
    std::float_t max = std::numeric_limits<float>::lowest();
    double sum = 0;
    int freq = 0;
};

int main() {
    std::ifstream f("../data/measurements.txt");

    std::map<std::string, LocationStats> m;
    std::string s;
    while (std::getline(f, s)) {
        std::istringstream ss(s);

        std::string location;
        std::getline(ss, location, ';');

        std::string tempString;
        std::getline(ss, tempString, ';');
        std::float_t temperature = std::stof(tempString);

        LocationStats* locationStat = &m[location];
        locationStat->min = std::fmin(locationStat->min, temperature);
        locationStat->max = std::fmax(locationStat->max, temperature);
        locationStat->freq++;
        locationStat->sum += temperature;
    }

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

    return EXIT_SUCCESS;
}
