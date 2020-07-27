#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

struct EncoderContext;

class Encoder {
public:
  static void Init();
  Encoder(uint32_t input_four_cc, uint32_t input_width, uint32_t input_height,
          uint32_t output_four_cc);
  ~Encoder();

  std::vector<uint8_t> encode(const uint8_t *input, uint32_t length);

protected:
  static std::atomic<bool> initialized;
  std::unique_ptr<EncoderContext> context;
};