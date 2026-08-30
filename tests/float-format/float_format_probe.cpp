// OQ-5 probe: which double->text format is round-trip-exact AND locale-independent
// on the EXT-08 toolchain (VS 2026 / MSVC 14.5x, Release|x64).
//
// Contenders:
//   A  std::to_chars(v)                                  shortest round-trip
//   B  std::to_chars(v, chars_format::scientific, 16)     17 significant digits
//   C  snprintf("%.17g", v)
//
// Each is judged on two independent axes:
//   round-trip  - re-reading the text yields the identical bit pattern
//   locale      - the text is byte-identical under "C" and under a comma-decimal locale
//
// Build (from a VS 2026 x64 developer prompt):
//     cl /std:c++17 /O2 /EHsc /nologo float_format_probe.cpp
// Exit code is 0 only if at least one contender passes both axes.

#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {

double bitsToDouble(std::uint64_t bits) {
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

std::uint64_t doubleToBits(double d) {
    std::uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    return bits;
}

std::string fmtShortest(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

std::string fmtSci17(double v) {
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific, 16);
    return std::string(buf, r.ptr);
}

std::string fmtPrintf17g(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return std::string(buf);
}

// Read back with from_chars, which is itself locale-independent, so the round-trip
// axis measures the writer and not the reader.
bool roundTrips(const std::string& text, double original) {
    double back = 0.0;
    const char* first = text.c_str();
    const char* last = first + text.size();
    const auto r = std::from_chars(first, last, back);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    return doubleToBits(back) == doubleToBits(original);
}

struct Contender {
    const char* name;
    std::string (*fn)(double);
    bool roundTripOk = true;
    bool localeOk = true;
    double firstRoundTripFailure = 0.0;
    std::string localeC;    // first differing rendering under "C"
    std::string localeAlt;  // ... and under the comma-decimal locale
    double firstLocaleFailure = 0.0;
    std::size_t maxLen = 0;
};

std::vector<double> buildCorpus() {
    std::vector<double> v;

    // Values the bus actually carries: simulation clocks accumulated at 20 Hz, which
    // is where the trailing 0.00000000000014 in the observed stream comes from.
    double t = 0.0;
    for (int i = 0; i < 4000; ++i) { v.push_back(t); t += 0.05; }

    // Geodetic positions and NED velocities in the ranges seen on sim/entity/state.
    v.insert(v.end(), {-19.985, 134.947, 2.25, -20.00979873235411, 134.9702583260468,
                       -0.33499930060691635, -0.9104417029137447, 21980.716682});

    // Awkward-but-legal doubles a recorder must not mangle.
    v.insert(v.end(), {0.0, -0.0, 1.0, -1.0, 0.1, 1.0 / 3.0,
                       std::numeric_limits<double>::min(),
                       std::numeric_limits<double>::denorm_min(),
                       std::numeric_limits<double>::max(),
                       -std::numeric_limits<double>::max(),
                       std::numeric_limits<double>::epsilon()});

    // Uniform random bit patterns, finite only. Infinities and NaN are excluded: they
    // are not representable in JSON and the capture path must reject them upstream.
    std::mt19937_64 rng(20260830u);
    while (v.size() < 200000) {
        const double d = bitsToDouble(rng());
        if (std::isfinite(d)) v.push_back(d);
    }
    return v;
}

const char* pickCommaLocale() {
    // Any locale whose LC_NUMERIC decimal point is a comma will expose the fault.
    static const char* candidates[] = {"de-DE", "German_Germany.1252", "tr-TR",
                                       "Turkish_Turkey.1254", "fr-FR", ""};
    for (const char* c : candidates) {
        if (std::setlocale(LC_ALL, c) != nullptr) {
            const std::string probe = fmtPrintf17g(1.5);
            if (probe.find(',') != std::string::npos) {
                std::setlocale(LC_ALL, "C");
                return c;
            }
        }
    }
    std::setlocale(LC_ALL, "C");
    return nullptr;
}

}  // namespace

int main() {
    const std::vector<double> corpus = buildCorpus();

    Contender contenders[] = {
        {"A  to_chars shortest        ", &fmtShortest},
        {"B  to_chars scientific,16   ", &fmtSci17},
        {"C  snprintf \"%.17g\"         ", &fmtPrintf17g},
    };

    const char* commaLocale = pickCommaLocale();
    std::printf("corpus            %zu finite doubles\n", corpus.size());
    std::printf("comma locale      %s\n",
                commaLocale ? commaLocale : "(none available - locale axis not exercised)");
    std::printf("\n");

    // Axis 1: round-trip, measured under the "C" locale.
    std::setlocale(LC_ALL, "C");
    std::vector<std::vector<std::string>> renderedC(std::size(contenders));
    for (std::size_t c = 0; c < std::size(contenders); ++c) {
        renderedC[c].reserve(corpus.size());
        for (const double v : corpus) {
            std::string s = contenders[c].fn(v);
            contenders[c].maxLen = (std::max)(contenders[c].maxLen, s.size());
            if (contenders[c].roundTripOk && !roundTrips(s, v)) {
                contenders[c].roundTripOk = false;
                contenders[c].firstRoundTripFailure = v;
            }
            renderedC[c].push_back(std::move(s));
        }
    }

    // Axis 2: the same corpus rendered again with a comma-decimal locale installed.
    if (commaLocale != nullptr) {
        std::setlocale(LC_ALL, commaLocale);
        for (std::size_t c = 0; c < std::size(contenders); ++c) {
            for (std::size_t i = 0; i < corpus.size(); ++i) {
                const std::string s = contenders[c].fn(corpus[i]);
                if (contenders[c].localeOk && s != renderedC[c][i]) {
                    contenders[c].localeOk = false;
                    contenders[c].firstLocaleFailure = corpus[i];
                    contenders[c].localeC = renderedC[c][i];
                    contenders[c].localeAlt = s;
                }
            }
        }
        std::setlocale(LC_ALL, "C");
    }

    int passing = 0;
    for (const Contender& k : contenders) {
        const bool ok = k.roundTripOk && k.localeOk;
        if (ok) ++passing;
        std::printf("%s  round-trip %-4s  locale %-4s  maxlen %2zu  %s\n", k.name,
                    k.roundTripOk ? "PASS" : "FAIL", k.localeOk ? "PASS" : "FAIL", k.maxLen,
                    ok ? "<= usable" : "");
        if (!k.roundTripOk) {
            std::printf("        first round-trip failure on bits 0x%016llx\n",
                        static_cast<unsigned long long>(doubleToBits(k.firstRoundTripFailure)));
        }
        if (!k.localeOk) {
            std::printf("        first locale divergence: C locale \"%s\"  vs  %s \"%s\"\n",
                        k.localeC.c_str(), commaLocale, k.localeAlt.c_str());
        }
    }

    std::printf("\n%d of %zu contenders pass both axes\n", passing, std::size(contenders));
    return passing > 0 ? 0 : 1;
}
