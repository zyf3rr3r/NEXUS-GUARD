#include "hal.h"

#include <cmath>
#include "driver/temperature_sensor.h"
#include "esp32-hal-cpu.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_timer.h"

namespace nexus {
namespace {

temperature_sensor_handle_t sensor = nullptr;

MemorySample sampleMemory(uint32_t capabilities) {
  multi_heap_info_t info{};
  heap_caps_get_info(&info, capabilities);
  return {info.total_free_bytes, info.largest_free_block, info.minimum_free_bytes};
}

struct ExpectedPartition {
  const char* label;
  esp_partition_type_t type;
  esp_partition_subtype_t subtype;
  uint32_t offset;
  uint32_t size;
};

constexpr ExpectedPartition kPartitions[] = {
    {"nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x5000},
    {"otadata", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0xE000, 0x2000},
    {"app0", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x10000, 0x300000},
    {"app1", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x310000, 0x300000},
    {"ffat", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, 0x610000, 0x9E0000},
    {"coredump", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, 0xFF0000, 0x10000},
};

}  // namespace

void initTemperatureSensor() {
  // Use the same 10–50 °C sensor range as the qualified Arduino API.
  const temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
  if (temperature_sensor_install(&config, &sensor) != ESP_OK) {
    sensor = nullptr;
  } else if (temperature_sensor_enable(sensor) != ESP_OK) {
    temperature_sensor_uninstall(sensor);
    sensor = nullptr;
  }
}

Telemetry sampleTelemetry() {
  Telemetry sample{};
  sample.uptimeMs = esp_timer_get_time() / 1000;
  sample.cpuMHz = getCpuFrequencyMhz();
  if (esp_flash_get_physical_size(nullptr, &sample.flashBytes) != ESP_OK) {
    sample.flashBytes = 0;
  }
  sample.psramBytes = esp_psram_is_initialized() ? esp_psram_get_size() : 0;
  sample.internal = sampleMemory(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  sample.psram = sampleMemory(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  sample.temperatureValid =
      sensor != nullptr && temperature_sensor_get_celsius(sensor, &sample.temperatureC) == ESP_OK &&
      std::isfinite(sample.temperatureC) && sample.temperatureC >= 10 && sample.temperatureC <= 50;
  return sample;
}

bool checkPartitionLayout() {
  auto iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  unsigned count = 0;
  while (iterator != nullptr) {
    const auto* partition = esp_partition_get(iterator);
    if (partition->flash_chip == esp_flash_default_chip) {
      ++count;
    }
    iterator = esp_partition_next(iterator);
  }
  if (count != sizeof(kPartitions) / sizeof(kPartitions[0])) {
    return false;
  }
  for (const auto& expected : kPartitions) {
    const auto* partition = esp_partition_find_first(expected.type, expected.subtype, expected.label);
    if (partition == nullptr || partition->flash_chip != esp_flash_default_chip ||
        partition->address != expected.offset || partition->size != expected.size ||
        partition->encrypted || partition->readonly) {
      return false;
    }
  }
  return true;
}

}  // namespace nexus
