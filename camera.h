#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <filesystem>

// v4l2
struct v4l2_buffer;

enum IOMethod {
  READ,
  MMAP,
  USERPTR,
};

struct Buffer;
class Camera;

class FrameView : public std::basic_string_view<uint8_t> {
public:
  FrameView(Camera &camera);
  FrameView(Camera &camera, unsigned int buffer_index, const uint8_t *s,
            size_t len);
  ~FrameView();

protected:
  Camera &camera;
  std::optional<unsigned int> buffer_index;
};

class Camera {
public:
  Camera(const std::filesystem::path &device, IOMethod method);
  ~Camera();

  void start_capturing(void);
  void stop_capturing(void);
  FrameView read_frame(void);
  void clean_after_read(unsigned int index);

  uint32_t fourcc() const {
    return _fourcc;
  }
  uint32_t width() const {
    return _width;
  }
  uint32_t height() const {
    return _height;
  }

protected:
  void init();
  void uninit(void);
  void close(void);

  void init_read(unsigned int buffer_size);
  void init_mmap(void);
  void init_userp(unsigned int buffer_size);

  void enqueue_buffer_mmap(unsigned int idx);
  void enqueue_buffer_userp(unsigned int idx);

  const IOMethod io_method;
  uint32_t _fourcc;
  uint32_t _width, _height;

  int fd;
  std::unique_ptr<Buffer[]> buffers;
  size_t buffer_count;
  std::filesystem::path device;
  std::unique_ptr<v4l2_buffer> v4l2_buf;
};
