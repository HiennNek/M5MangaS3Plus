#pragma once
// Real CPU clock control for the port.
//
// CONFIG_PM_ENABLE + CONFIG_PM_DFS_INIT_AUTO does NOT mean "runs at max".
// esp_pm's auto-init does:
//     max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ   (160 on S3 by default)
//     min_freq_mhz = XTAL                              (40 MHz)
// and the CPU only leaves min_freq while some task holds an
// ESP_PM_CPU_FREQ_MAX lock. Nothing in this app held one, so the JPEG
// decode ran at 40-80 MHz instead of 240 -- the Arduino sketch's
// setCpuFrequencyMhz(240) around drawPage() became a no-op in the port and
// took the decoder's clock with it.
//
// cpuSetFast() latches one shared ESP_PM_CPU_FREQ_MAX lock (main task).
// CpuBoost is the reentrant, exception-safe form for worker tasks --
// esp_pm locks are refcounted, so nesting across tasks is fine.

void cpuSetFast(bool fast);

struct CpuBoost {
  CpuBoost();
  ~CpuBoost();
  CpuBoost(const CpuBoost &) = delete;
  CpuBoost &operator=(const CpuBoost &) = delete;
};
