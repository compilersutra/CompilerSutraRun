//===----------------------------------------------------------------------===//
//
// PowerSampler.h - GPU/CPU power monitoring during timed benchmark regions
//
//===----------------------------------------------------------------------===//

#ifndef CSRUN_BENCHMARK_POWERSAMPLER_H
#define CSRUN_BENCHMARK_POWERSAMPLER_H

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace csrun {

/// Power measurements collected over a timed region (warmup + iterations).
struct PowerStats {
  bool available = false;
  std::string source;    // "sysfs-hwmon" | "rapl"
  std::string device;    // e.g. "card2" or "intel-rapl:0 (package-0)"
  std::uint32_t samples = 0;
  double durationMs = 0.0;
  double avgWatts = 0.0;
  double minWatts = 0.0;
  double maxWatts = 0.0;
  double energyJoules = 0.0; // trapezoidal integral of power over the region
  std::string note;
};

/// RAII power sampler. Probes sysfs at construction; if a source is found,
/// a background thread polls it every few milliseconds until destruction.
/// Read `stats()` after the timed region to get min/max/avg/energy.
class PowerSampler {
public:
  PowerSampler();
  ~PowerSampler();

  PowerSampler(const PowerSampler &) = delete;
  PowerSampler &operator=(const PowerSampler &) = delete;

  bool available() const { return available_; }
  PowerStats stats() const;

private:
  struct Sample {
    double timestampMs;
    double watts;
  };

  void pollLoop();
  void probe();
  void sampleOnce();

  bool available_ = false;
  bool stop_ = false;
  std::string path_;
  std::string source_;
  std::string device_;
  double intervalMs_ = 5.0;
  mutable std::mutex mutex_;
  std::vector<Sample> samples_;
  std::thread thread_;
};

} // namespace csrun

#endif
