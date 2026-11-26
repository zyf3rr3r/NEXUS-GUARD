#include "../../firmware/NexusS3/guard.h"
#include "../../firmware/NexusS3/stop_job.h"
#include "../../firmware/NexusS3/media_arm.h"
#include "../../firmware/NexusS3/radio_policy.h"

#include <cstdio>
#include <cstring>
#include <limits>

using namespace nexus;

namespace {
struct Driver : ServiceDriver {
  bool startOk = true;
  bool stopOk = true;
  unsigned starts = 0;
  unsigned stops = 0;
  bool start() override { ++starts; return startOk; }
  bool stop() override { ++stops; return stopOk; }
};

unsigned failures = 0;
void expect(bool correct, const char* description) {
  std::printf("[%s] %s\n", correct ? "PASS" : "FAIL", description);
  failures += !correct;
}
}  // namespace

int main() {
  const GuardSample healthy{100, 28.3f, true, 300000, 100000, 8000000};
  auto sample = healthy;
  Guard guard;
  sample.temperatureC = 33.4f;
  expect(guard.admit(sample) == Reason::WarmStart, "warm start denied");
  sample.temperatureC = std::numeric_limits<float>::quiet_NaN();
  expect(guard.admit(sample) == Reason::TemperatureUnavailable, "invalid sample denied");
  sample = healthy;
  sample.internalFree = kInternalReserve + Guard::kManagementHeadroom - 1;
  expect(guard.admit(sample) == Reason::HeapReserve, "management headroom reserved");
  sample = healthy;
  sample.internalLargest = Guard::kManagementLargest - 1;
  expect(guard.admit(sample) == Reason::BlockTooSmall, "fragmented internal heap denied");
  sample = healthy;
  sample.psramFree = kPsramReserve - 1;
  expect(guard.admit(sample) == Reason::PsramReserve, "PSRAM reserve enforced");

  Driver driver;
  ManagementService service(driver);
  expect(service.start(healthy) == Reason::None && driver.starts == 1 && service.guard().leased(),
         "start acquired a lease");
  expect(service.start(healthy) == Reason::Busy && driver.starts == 1, "conflicting start rejected");
  sample = healthy;
  sample.temperatureC = 36.4f;
  service.tick(sample);
  expect(service.warning() == Reason::ThermalWarning &&
         service.state() == ServiceState::Running && service.guard().cutoff() == healthy.temperatureC + 10,
         "early warning without a raised cutoff");
  sample.temperatureC = 35.5f;
  service.tick(sample);
  expect(service.warning() == Reason::ThermalWarning, "warning hysteresis retained");
  sample.temperatureC = 34;
  service.tick(sample);
  expect(service.warning() == Reason::None, "warning clears below hysteresis margin");
  sample.temperatureC = 38.4f;
  service.tick(sample);
  expect(service.state() == ServiceState::Stopped && !service.guard().leased() && driver.stops == 1 &&
         service.lastReason() == Reason::ThermalCutoff, "cutoff stops and releases only on acknowledgment");
  expect(service.stop(101, Reason::StopRequested) == Reason::None && driver.stops == 1, "stop is idempotent");
  expect(service.start(healthy) == Reason::Cooldown, "cooldown denies restart");
  sample = healthy;
  sample.nowMs += Guard::kCooldownMs;
  expect(service.start(sample) == Reason::None, "restart after cooldown is possible");
  sample.nowMs += Guard::kSessionMs;
  service.tick(sample);
  expect(service.lastReason() == Reason::SessionExpired && driver.stops == 2, "management lease expires");

  Driver failedStart;
  failedStart.startOk = false;
  ManagementService partial(failedStart);
  expect(partial.start(healthy) == Reason::DriverStart && failedStart.stops == 1 &&
         !partial.guard().leased() && partial.state() == ServiceState::Faulted,
         "partial start rolls back and latches fault");
  expect(partial.start(healthy) == Reason::DriverStart && failedStart.starts == 1, "fault never auto-retries");
  Driver failedStop;
  failedStop.stopOk = false;
  ManagementService retained(failedStop);
  retained.start(healthy);
  sample = healthy;
  sample.temperatureValid = false;
  retained.tick(sample);
  expect(retained.state() == ServiceState::Faulted && retained.guard().leased() &&
         retained.lastReason() == Reason::DriverStop, "failed release retains ownership");
  retained.latchIntegrityFault();
  expect(retained.lastReason() == Reason::HeapIntegrity && failedStop.stops == 1, "heap fault avoids teardown");

  StopJob job;
  const auto first = job.prepare();
  expect(first.created && !job.take(60000), "a job cannot start before the response is sent");
  job.arm(first.id, 100);
  const auto duplicate = job.prepare();
  job.arm(duplicate.id, 500);
  expect(!duplicate.created && duplicate.id == first.id && job.remainingMs(first.id, 15099) == 1 &&
         !job.take(15099), "duplicates preserve their id and original deadline");
  expect(!job.cancel(first.id + 1, 15100) && job.cancel(first.id, 15099) &&
         job.cancel(first.id, 15100) && !job.take(15100), "cancellation is valid up to the deadline and repeatable");

  const auto second = job.prepare();
  job.arm(second.id, 15200);
  expect(second.created && second.id != first.id && !job.cancel(second.id, 30200) &&
         job.take(30200) && !job.cancel(second.id, 30201) && !job.take(30201),
         "at takeover, cancellation and duplicate execution are denied");
  job.clear();
  expect(job.state(second.id) == StopJob::State::None && job.prepare().id != second.id,
         "released service retires job identifiers");
  const auto unsent = job.prepare();
  expect(job.cancel(unsent.id, 40000), "failed response can discard its unarmed job");
  job.arm(unsent.id, 40001);
  expect(!job.take(60000), "a cancelled unarmed job cannot be reactivated");
  MediaArm media;
  expect(!media.consume(0), "native media output is disarmed on boot");
  media.arm(100);
  expect(media.consume(14999) && !media.consume(15000), "native media arming permits one action");
  media.arm(200);
  expect(!media.consume(15200), "native media arming expires at its deadline");
  media.arm(300);
  media.clear();
  expect(!media.consume(301), "native USB disarm and disconnect revoke the action");

  Telemetry radioSample{};
  radioSample.temperatureValid = true;
  radioSample.temperatureC = 34.3f;
  radioSample.internal.freeBytes = 230296;
  radioSample.internal.largestBlockBytes = 188404;
  radioSample.psram.freeBytes = 8369192;
  const auto admitted = [&radioSample] {
    return optionalRadioBlock(ServiceState::Running, Reason::None, 38.3f, radioSample);
  };
  expect(admitted() == nullptr, "radio admits a cool sample with headroom");
  radioSample.temperatureC = 35.3f;
  expect(std::strcmp(admitted(), "RADIO_THERMAL_MARGIN") == 0, "radio rejects at the existing three-degree margin");
  radioSample.temperatureC = 34.3f;
  radioSample.internal.freeBytes = 152520;
  expect(admitted() == nullptr, "radio admits observed Wi-Fi+NimBLE internal heap floor");
  radioSample.internal.freeBytes = 128 * 1024 - 1;
  expect(std::strcmp(admitted(), "RADIO_MEMORY_MARGIN") == 0, "radio reports memory denial separately");
  radioSample.temperatureValid = false;
  expect(std::strcmp(admitted(), "RADIO_TEMP_UNAVAILABLE") == 0, "radio reports sensor denial separately");
  return failures == 0 ? 0 : 1;
}
