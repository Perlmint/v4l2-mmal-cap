#include <cstring>
#include <filesystem>

#include "./camera.h"
#include "./encoder.h"

#include <interface/mmal/mmal_encodings.h>

uint32_t fourcc_from_path(const std::filesystem::path &p) {
  if (!p.has_extension()) {
    throw std::invalid_argument("Output path doesn't have extension. extension is required.");
  }
  const auto s = p.extension().string();

  if (s == ".jpg" || s == ".jpeg") {
    return MMAL_ENCODING_JPEG;
  }
  if (s == ".gif") {
    return MMAL_ENCODING_GIF;
  }
  if (s == ".png") {
    return MMAL_ENCODING_PNG;
  }
  if (s == ".tga") {
    return MMAL_ENCODING_TGA;
  }
  if (s == ".bmp") {
    return MMAL_ENCODING_BMP;
  }

  throw std::invalid_argument("Can't specify output encoder from extension of output path");
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: %s INPUT_DEVICE OUTPUT_PATH\n", argv[0]);
    return -1;
  }

  std::filesystem::path input_path = argv[1];
  std::filesystem::path output_path = argv[2];

  Camera camera{input_path, IOMethod::MMAP};

  Encoder encoder{camera.fourcc(), camera.width(), camera.height(), fourcc_from_path(output_path)};

  camera.start_capturing();

  while (true) {
    const auto frame = camera.read_frame();
    if (frame.length() == 0) {
      fprintf(stderr, "Read 0 sized frame. retry\n");
    } else {
      auto out = fopen(output_path.c_str(), "wb");
      fprintf(stderr, "Read raw input: %lu bytes\n", frame.length());
      const auto encoded = encoder.encode(frame.data(), frame.length());
      fprintf(stderr, "Encoded : %lu bytes\n", encoded.size());
      fwrite(reinterpret_cast<const char *>(encoded.data()), 1, encoded.size(),
             out);
      fclose(out);
      break;
    }
  }

  camera.stop_capturing();

  return 0;
}
