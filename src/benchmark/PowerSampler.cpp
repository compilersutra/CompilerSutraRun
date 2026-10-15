#include "csrun/benchmark/PowerSampler.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>

namespace csrun {
namespace {

namespace fs = std::filesystem;

bool readMicroWatts(const fs::path &path, double &out) {
  std::ifstream in(path);
  if (!in)
    return false;
  long long uw = 0;
  if (!(in >> uw))
    return false;
  out = static_cast<double>(uw) / 1.0e6;
  return true;
}

/// AMD/Intel GPU: /sys/class/drm/cardN/device/hwmon/hwmonM/power1_{average,input}
/// power1_average exists on most amdgpu parts; power1_input is instantaneous and
/// appears on newer parts (e.g. gfx12). Prefer average, fall back to input.
std::optional<fs::path> probeGpuHwmon() {
  static const fs::path kDrmRoot{"/sys/class/drm"};
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(kDrmRoot, ec)) {
    if (!entry.is_directory())
      continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("card", 0) != 0 || name.find('-') != std::string::npos)
      continue;
    const fs::path hwmonDir = entry.path() / "device" / "hwmon";
    std::error_code ec2;
    for (const auto &h : fs::directory_iterator(hwmonDir, ec2)) {
      if (!h.is_directory())
        continue;
      const fs::path avg = h.path() / "power1_average";
      if (fs::exists(avg, ec2))
        return avg;
      const fs::path input = h.path() / "power1_input";
      if (fs::exists(input, ec2))
        return input;
    }
  }
  return std::nullopt;
}

/// CPU package fallback: /sys/class/powercap/intel-rapl:*/energy_uj (micro joules).
std::optional<fs::path> probeRaplaEnergy() {
  static const fs::path kRoot{"/sys/class/powercap"};
  std::error_code ec;
  for (const auto &entry : fs::directory_iterator(kRoot, ec)) {
    if (!entry.is_directory())
      continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("intel-rapl:", 0) != 0)
      continue;
    const fs::path energy = entry.path() / "energy_uj";
    if (fs::exists(energy, ec))
      return energy;
  }
  return std::nullopt;
}

double nowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

PowerSampler::PowerSampler() {
  probe();
  if (!available_)
    return;
  // Measure idle power before the workload starts; stats() subtracts this.
  sampleOnce();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!samples_.empty())
      idleWatts_ = samples_.front().watts;
  }
  // Synchronous first sample so even sub-millisecond regions have a reading.
  sampleOnce();
  thread_ = std::thread([this] { pollLoop(); });
}

PowerSampler::~PowerSampler() {
  if (!available_)
    return;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  if (thread_.joinable())
    thread_.join();
}

void PowerSampler::probe() {
  if (auto gpu = probeGpuHwmon()) {
    available_ = true;
    path_ = gpu->string();
    source_ = "sysfs-hwmon";
    // cardN from .../cardN/device/hwmon/hwmonM/power1_*
    const fs::path p = *gpu;
    device_ = p.parent_path().parent_path().parent_path().parent_path().filename().string();
    return;
  }
  if (auto rapl = probeRaplaEnergy()) {
    available_ = true;
    path_ = rapl->string();
    source_ = "rapl";
    std::ifstream name(rapl->parent_path() / "name");
    std::string n;
    if (name >> n)
      device_ = rapl->parent_path().filename().string() + " (" + n + ")";
    else
      device_ = rapl->parent_path().filename().string();
  }
}

void PowerSampler::sampleOnce() {
  const double t = nowMs();
  if (source_ == "rapl") {
    double energyUj = 0.0;
    if (readMicroWatts(path_, energyUj)) {
      std::lock_guard<std::mutex> lock(mutex_);
      samples_.push_back({t, energyUj / 1.0e6}); // treat first reading as watts-equivalent
    }
    return;
  }
  double watts = 0.0;
  if (readMicroWatts(path_, watts)) {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back({t, watts});
  }
}

void PowerSampler::pollLoop() {
  bool isRapl = (source_ == "rapl");
  double prevEnergyUj = 0.0;
  double prevT = 0.0;
  bool havePrev = false;
  if (isRapl) {
    std::ifstream in(path_);
    if (in >> prevEnergyUj) {
      prevT = nowMs();
      havePrev = true;
    }
  }

  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_)
        return;
    }
    const double t = nowMs();
    if (isRapl) {
      double energyUj = 0.0;
      if (readMicroWatts(path_, energyUj) && havePrev) {
        // RAPL energy is cumulative; convert the delta to average watts over dt.
        const double dt = t - prevT;
        if (dt > 0.0 && energyUj >= prevEnergyUj) {
          const double watts = (energyUj - prevEnergyUj) / 1.0e6 / (dt / 1.0e3);
          std::lock_guard<std::mutex> lock(mutex_);
          samples_.push_back({t, watts});
        }
        prevEnergyUj = energyUj;
        prevT = t;
      }
    } else {
      double watts = 0.0;
      if (readMicroWatts(path_, watts)) {
        std::lock_guard<std::mutex> lock(mutex_);
        samples_.push_back({t, watts});
      }
    }
    std::this_thread::sleep_for(
        std::chrono::duration<double, std::milli>(intervalMs_));
  }
}

PowerStats PowerSampler::stats() const {
  PowerStats out;
  if (!available_)
    return out;
  std::vector<Sample> copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    copy = samples_;
    if (copy.size() < 2) {
      // Region was shorter than the poll interval; take a final synchronous reading.
      // Done under the same lock so the sample order stays monotonic.
      const double t = nowMs();
      if (source_ == "rapl") {
        double energyUj = 0.0;
        if (readMicroWatts(path_, energyUj))
          copy.push_back({t, energyUj / 1.0e6});
      } else {
        double watts = 0.0;
        if (readMicroWatts(path_, watts))
          copy.push_back({t, watts});
      }
    }
  }
  out.available = true;
  out.source = source_;
  out.device = device_;
  out.samples = static_cast<std::uint32_t>(copy.size());
  if (copy.empty()) {
    out.note = "no power samples collected";
    return out;
  }
  out.durationMs = copy.back().timestampMs - copy.front().timestampMs;
  double sum = 0.0;
  out.minWatts = copy.front().watts;
  out.maxWatts = copy.front().watts;
  for (const auto &s : copy) {
    sum += s.watts;
    out.minWatts = std::min(out.minWatts, s.watts);
    out.maxWatts = std::max(out.maxWatts, s.watts);
  }
  out.avgWatts = sum / static_cast<double>(copy.size());
  out.idleWatts = idleWatts_;
  out.activeAvgWatts = out.avgWatts - idleWatts_;
  // Trapezoidal integration for energy.
  double energy = 0.0;
  double activeEnergy = 0.0;
  for (std::size_t i = 1; i < copy.size(); ++i) {
    const double dt = (copy[i].timestampMs - copy[i - 1].timestampMs) / 1.0e3;
    energy += 0.5 * (copy[i].watts + copy[i - 1].watts) * dt;
    activeEnergy += 0.5 * ((copy[i].watts - idleWatts_) + (copy[i - 1].watts - idleWatts_)) * dt;
  }
  out.energyActiveJoules = activeEnergy;
  out.energyJoules = energy;
  return out;
}

} // namespace csrun
