/* Copyright (c) 2020, Jonas Lauener & Wingtra AG
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <cmath>
#include <functional>
#include <vibration_daq/VibrationSensorModule.hpp>

#include "chrono"
#include "loguru/loguru.hpp"
#include "thread"
#include "vibration_daq/utils/HexUtils.hpp"

namespace vibration_daq {
using namespace std::this_thread;  // sleep_for, sleep_until
using namespace std::chrono_literals;

VibrationSensorModule::VibrationSensorModule(const std::string &name)
    : name(name) {}

const std::string &VibrationSensorModule::getSensorName() const { return name; }

WordBuffer VibrationSensorModule::transfer(WordBuffer sendBuf) const {
  WordBuffer recBuf = {};
  if (spi_transfer(spi, sendBuf.data(), recBuf.data(), 2) < 0) {
    LOG_F(ERROR, "spi_transfer(): %s\n", spi_errmsg(spi));
    exit(1);
  }

  // keep processor busy to increase time of SPI disable to ~40us
  for (int i = 0; i < 2000; ++i) {
    // fixes the bug of shifting MISO 1 byte
  }

  return recBuf;
}

WordBuffer VibrationSensorModule::transferBlocking(WordBuffer sendBuf) const {
  bool notBusy;

  do {
    if (gpio_read(gpioBusy, &notBusy) < 0) {
      LOG_F(ERROR, "gpio_read(): %s\n", gpio_errmsg(gpioBusy));
      exit(1);
    }

    if (notBusy) {
      return transfer(sendBuf);
    } else {
      // DLOG_S(INFO) << name << " is busy.";
      // sleep_for(10ms);
    }
  } while (!notBusy);

  return {};
}

uint16_t VibrationSensorModule::read(vibration_daq::SpiCommand cmd) const {
  if (!cmd.readFlag) {
    LOG_S(ERROR) << name << ": Cannot read SpiCommand (PageID: " << cmd.pageId
                 << ", Address: " << cmd.address << "). Read flag not set.";
    return 0;
  }

  // select page
  transferBlocking({0x80, cmd.pageId});

  transferBlocking({cmd.address, 0});
  WordBuffer resp = transferBlocking({0, 0});

  return convert(resp);
}

void VibrationSensorModule::write(SpiCommand cmd, uint16_t value) const {
  if (!cmd.writeFlag) {
    LOG_S(ERROR) << name << ": Cannot write SpiCommand (PageID: " << cmd.pageId
                 << ", Address: " << cmd.address << "). Write flag not set.";
    return;
  }

  // select page
  transferBlocking({0x80, cmd.pageId});

  transferBlocking({static_cast<unsigned char>(cmd.address | 0x80),
                    static_cast<unsigned char>(value & 0xff)});
  transferBlocking({static_cast<unsigned char>((cmd.address + 1) | 0x80),
                    static_cast<unsigned char>(value >> 8)});
}

bool VibrationSensorModule::setup(unsigned int resetPin, unsigned int busyPin,
                                  std::string spiPath, uint32_t speed) {
  gpioReset = gpio_new();
  gpioBusy = gpio_new();

  if (gpio_open(gpioReset, GPIO_PATH.data(), resetPin, GPIO_DIR_OUT_LOW) < 0) {
    LOG_F(ERROR, "gpio_open(): %s\n", gpio_errmsg(gpioReset));
    return false;
  }
  if (gpio_open(gpioBusy, GPIO_PATH.data(), busyPin, GPIO_DIR_IN) < 0) {
    LOG_F(ERROR, "gpio_open(): %s\n", gpio_errmsg(gpioBusy));
    return false;
  }

  spi = spi_new();
  if (spi_open_advanced(spi, spiPath.data(), 3, speed,
                        spi_bit_order_t::MSB_FIRST, 8, 0) < 0) {
    LOG_F(ERROR, "spi_open(): %s\n", spi_errmsg(spi));
    return false;
  }

  sleep_for(200ms);

  if (gpio_write(gpioReset, true) < 0) {
    LOG_F(ERROR, "gpio_write(): %s\n", gpio_errmsg(gpioReset));
    return false;
  }

  // important for transient behaviour of busy pin on startup!
  sleep_for(500ms);

  // check if the right model (ADcmXL3021) is connected and if the connection
  // works
  uint16_t prodId = read(spi_commands::PROD_ID);
  if (prodId != 0x0BCD) {
    LOG_F(ERROR, "Not getting the right prodId, getting: 0x%04X", prodId);
    return false;
  }

  return true;
}

void VibrationSensorModule::close() {
  gpio_close(gpioBusy);
  gpio_close(gpioReset);

  gpio_free(gpioBusy);
  gpio_free(gpioReset);

  spi_close(spi);
  spi_free(spi);
}

bool VibrationSensorModule::writeRecordingControl(
    const RecordingMode &recordingMode, const WindowSetting &windowSetting) {
  // hard type sample rate option 0
  uint16_t recCtrl = 0x100;

  recCtrl |= (static_cast<uint8_t>(windowSetting) << 12);
  recCtrl |= static_cast<uint8_t>(recordingMode);

  write(spi_commands::REC_CTRL, recCtrl);

  recCtrl = read(spi_commands::REC_CTRL);
  currentRecordingMode = static_cast<RecordingMode>(recCtrl & 0x3);

  return currentRecordingMode == recordingMode;
}

VibrationData VibrationSensorModule::retrieveVibrationData() const {
  int samplesCount = 0;
  float recordStepSize = 0;
  int decimationFactor;

  std::function<float(int16_t)> convertVibrationValue;
  switch (currentRecordingMode) {
    case RecordingMode::RTS:
      samplesCount = 10;
      recordStepSize = 1.f / (220000.f);
      convertVibrationValue = {
          [](int16_t valueRaw) { return static_cast<float>(valueRaw); }};
      break;
    case RecordingMode::MTC:
      decimationFactor = readRecInfoDecimationFactor();
      samplesCount = 4096;
      recordStepSize = 1.f / (220000.f / static_cast<float>(decimationFactor));
      convertVibrationValue = {[](int16_t valueRaw) {
        return static_cast<float>(valueRaw) * 0.001907349;
      }};
      break;
    case RecordingMode::MFFT:
    case RecordingMode::AFFT:
      decimationFactor = readRecInfoDecimationFactor();
      const uint8_t numberOfFFTAvg = readRecInfoFFTAveragesCount();
      samplesCount = 2048;
      recordStepSize = 110000.f / static_cast<float>(decimationFactor) /
                       static_cast<float>(samplesCount);
      convertVibrationValue = {[numberOfFFTAvg](int16_t valueRaw) {
        // handle special case according to
        // https://ez.analog.com/mems/f/q-a/162759/adcmxl3021-fft-conversion/372600#372600
        if (valueRaw == 0) {
          return 0.0;
        }
        return std::pow(2, static_cast<float>(valueRaw) / 2048) /
               numberOfFFTAvg * 0.9535;
      }};
      break;
  }

  VibrationData vibrationData;

  if (currentRecordingMode == RecordingMode::RTS) {
    vibrationData = readRtsSamplesBuffer(samplesCount, convertVibrationValue);
    vibrationData.stepAxis = generateSteps(recordStepSize, samplesCount * 32);
  } else {
    write(spi_commands::BUF_PNTR, 0);
    vibrationData.xAxis = readSamplesBuffer(spi_commands::X_BUF, samplesCount,
                                            convertVibrationValue);
    vibrationData.yAxis = readSamplesBuffer(spi_commands::Y_BUF, samplesCount,
                                            convertVibrationValue);
    vibrationData.zAxis = readSamplesBuffer(spi_commands::Z_BUF, samplesCount,
                                            convertVibrationValue);
    vibrationData.stepAxis = generateSteps(recordStepSize, samplesCount);
  }
  vibrationData.recordingMode = currentRecordingMode;

  return vibrationData;
}

std::vector<float> VibrationSensorModule::generateSteps(float stepSize,
                                                        int samplesCount) {
  std::vector<float> stepAxis;
  stepAxis.reserve(samplesCount);

  for (int i = 0; i < samplesCount; ++i) {
    stepAxis.push_back(stepSize * i);
  }

  return stepAxis;
}

bool VibrationSensorModule::parseRtsData(std::vector<uint16_t> &resp,
                                         VibrationData &vibrationData) const {
  if (resp.size() != 100) {
    LOG_F(ERROR, "RTS data parsing error");
    return false;
  }

  for (int i = 0; i < 100; i++) {
    // sequenceNumber
    if (i == 0) {
      vibrationData.sequenceNumber.push_back(resp[i]);
    }
    // x
    else if (i <= 32) {
      vibrationData.xAxis.push_back(resp[i]);
    }
    // y
    else if (i <= 64) {
      vibrationData.yAxis.push_back(resp[i]);
    }
    // z
    else if (i <= 96) {
      vibrationData.zAxis.push_back(resp[i]);
    }
    // Temperature
    else if (i == 97) {
      vibrationData.temperature.push_back(resp[i]);
    }
    // Status
    else if (i == 98) {
      vibrationData.status.push_back(resp[i]);
    }
    // CRC
    else if (i == 99) {
      vibrationData.crc.push_back(resp[i]);
    }
  }

  return true;
}

VibrationData VibrationSensorModule::readRtsSamplesBuffer(
    int samplesCount,
    const std::function<float(int16_t)> &convertVibrationValue) const {
  std::vector<uint16_t> resp;
  VibrationData vibrationData;

  for (int i = 0; i < samplesCount; i++) {
    resp.clear();

    for (int j = 0; j < 100; ++j) {
      uint16_t raw = convertRTS(transfer({0, 0}));
      resp.push_back(raw);
    }
    parseRtsData(resp, vibrationData);
  }
  return vibrationData;
}

std::vector<float> VibrationSensorModule::readSamplesBuffer(
    SpiCommand cmd, int samplesCount,
    const std::function<float(int16_t)> &convertVibrationValue) const {
  std::vector<float> axisData;
  axisData.reserve(samplesCount);

  // select page
  transferBlocking({0x80, cmd.pageId});
  transferBlocking({cmd.address, 0});

  for (int i = 0; i < (samplesCount - 1); ++i) {
    uint16_t resp = convert(transferBlocking({cmd.address, 0}));
    auto valueRaw = static_cast<int16_t>(resp);
    axisData.push_back(convertVibrationValue(valueRaw));
  }
  int16_t valueRaw = static_cast<int16_t>(convert(transfer({0, 0})));
  axisData.push_back(convertVibrationValue(valueRaw));
  return axisData;
}

int VibrationSensorModule::readRecInfoFFTAveragesCount() const {
  return read(spi_commands::REC_INFO1) & 0xFF;
}

int VibrationSensorModule::readRecInfoDecimationFactor() const {
  int avgCnt = read(spi_commands::REC_INFO2) & 0x7;
  return static_cast<int>(pow(2, avgCnt));
}

bool VibrationSensorModule::activateMode(const RecordingConfig &recordingConfig,
                                         const RecordingMode &recordingMode,
                                         const WindowSetting &windowSetting) {
  // only modify SR0 as we only work with that. keep rest default.
  switch (recordingMode) {
    case RecordingMode::RTS:
      // add decimation factor
      break;
    default:
      uint16_t avgCnt = 0x7420;
      avgCnt |= static_cast<uint8_t>(recordingConfig.decimationFactor);
      write(spi_commands::AVG_CNT, avgCnt);

      if (recordingConfig.firFilter == FIRFilter::CUSTOM) {
        writeCustomFIRFilterTaps(recordingConfig.customFilterTaps);

        // select filter bank F
        writeFIRFilter(FIRFilter::HIGH_PASS_10kHz);
      } else {
        writeFIRFilter(recordingConfig.firFilter);
      }
  }

  return writeRecordingControl(recordingMode, windowSetting);
}

bool VibrationSensorModule::activateMode(const MFFTConfig &mfftConfig) {
  // only modify SR0 as we only work with that. keep rest default.
  uint16_t fftAvg1 = 0x0100;
  fftAvg1 |= mfftConfig.spectralAvgCount;
  write(spi_commands::FFT_AVG1, fftAvg1);

  return activateMode(mfftConfig, RecordingMode::MFFT,
                      mfftConfig.windowSetting);
}

bool VibrationSensorModule::activateMode(const MTCConfig &mtcConfig) {
  return activateMode(mtcConfig, RecordingMode::MTC);
}

bool VibrationSensorModule::activateMode(const RTSConfig &rtsConfig) {
  return activateMode(rtsConfig, RecordingMode::RTS);
}

void VibrationSensorModule::writeFIRFilter(FIRFilter firFilter) {
  uint16_t filtCtrl = 0x0000;
  // set for every axis same filter
  filtCtrl |= static_cast<uint8_t>(firFilter);
  filtCtrl |= (static_cast<uint8_t>(firFilter) << 3);
  filtCtrl |= (static_cast<uint8_t>(firFilter) << 6);
  write(spi_commands::FILT_CTRL, filtCtrl);
}

void VibrationSensorModule::writeCustomFIRFilterTaps(
    std::array<int16_t, 32> customFilterTaps) {
  // store custom values in filter bank F
  for (int i = 0; i < customFilterTaps.size(); ++i) {
    write(spi_commands::FIR_COEFFS_F[i],
          static_cast<uint16_t>(customFilterTaps[i]));
  }
}

void VibrationSensorModule::triggerRecording() const {
  write(spi_commands::GLOB_CMD, 0x0800);
  sleep_for(12ms);  // delay before getting data
}

void VibrationSensorModule::activateExternalTrigger() const {
  write(spi_commands::MISC_CTRL, 0x1000);
}

void VibrationSensorModule::triggerAutonull() const {
  // setting statistic mode
  LOG_S(INFO) << "Autonull - Setting statistic mode";
  write(spi_commands::REC_CTRL, 0x1142);
  sleep_for(10ms);

  // start record
  LOG_S(INFO) << "Autonull - Start record";
  write(spi_commands::GLOB_CMD, 0x0800);
  sleep_for(500ms);

  // stop record
  LOG_S(INFO) << "Autonull - Stop record";
  write(spi_commands::GLOB_CMD, 0x0800);
  sleep_for(10ms);

  // read stat
  uint16_t x_stat = read(spi_commands::X_STATISTIC);
  uint16_t y_stat = read(spi_commands::Y_STATISTIC);
  uint16_t z_stat = read(spi_commands::Z_STATISTIC);
  LOG_S(INFO) << "Autonull - x_stat: " << x_stat;
  LOG_S(INFO) << "Autonull - y_stat: " << y_stat;
  LOG_S(INFO) << "Autonull - z_stat: " << z_stat;

  write(spi_commands::X_ANULL, x_stat);
  write(spi_commands::Y_ANULL, y_stat);
  write(spi_commands::Z_ANULL, z_stat);
  sleep_for(10ms);
}

void VibrationSensorModule::restoreFactorySettings() {
  write(spi_commands::GLOB_CMD, 0x0008);
}

}  // namespace vibration_daq
