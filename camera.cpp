#include "./camera.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h> /* getopt_long() */

#include <errno.h>
#include <fcntl.h> /* low-level i/o */
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

// v4l2
#include <linux/videodev2.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct Buffer {
  void *start;
  size_t length;
};

static void errno_exit(const char *s) {
  fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
  exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg) {
  int r;

  do {
    r = ioctl(fh, request, arg);
  } while (-1 == r && EINTR == errno);

  return r;
}

Camera::Camera(const std::filesystem::path &device, IOMethod method)
    : io_method(method), device(device), v4l2_buf(new v4l2_buffer()) {
  struct stat st;

  if (-1 == stat(device.c_str(), &st)) {
    fprintf(stderr, "Cannot identify '%s': %d, %s\\n", device.c_str(), errno,
            strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!S_ISCHR(st.st_mode)) {
    fprintf(stderr, "%s is no devicen", device.c_str());
    exit(EXIT_FAILURE);
  }

  fd = open(device.c_str(), O_RDWR /* required */ | O_NONBLOCK, 0);

  if (-1 == fd) {
    fprintf(stderr, "Cannot open '%s': %d, %s\\n", device.c_str(), errno,
            strerror(errno));
    exit(EXIT_FAILURE);
  }

  init();
}

void Camera::init() {  
  struct v4l2_capability cap;

  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
    if (EINVAL == errno) {
      fprintf(stderr, "%s is no V4L2 device\\n", device.c_str());
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_QUERYCAP");
    }
  }

  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    fprintf(stderr, "%s is no video capture device\\n", device.c_str());
    exit(EXIT_FAILURE);
  }

  switch (io_method) {
  case IOMethod::READ:
    if (!(cap.capabilities & V4L2_CAP_READWRITE)) {
      fprintf(stderr, "%s does not support read i/o\\n", device.c_str());
      exit(EXIT_FAILURE);
    }
    break;

  case IOMethod::MMAP:
  case IOMethod::USERPTR:
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
      fprintf(stderr, "%s does not support streaming i/o\\n", device.c_str());
      exit(EXIT_FAILURE);
    }
    break;
  }

  struct v4l2_format fmt;
  
  CLEAR(fmt);

  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt)) {
    errno_exit("VIDIOC_G_FMT");
  }

  /* Buggy driver paranoia. */
  auto min = fmt.fmt.pix.width * 2;
  if (fmt.fmt.pix.bytesperline < min) {
    fmt.fmt.pix.bytesperline = min;
  }
  min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
  if (fmt.fmt.pix.sizeimage < min) {
    fmt.fmt.pix.sizeimage = min;
  }
  
  _fourcc = fmt.fmt.pix.pixelformat;
  _width = fmt.fmt.pix.width;
  _height = fmt.fmt.pix.height;

  switch (io_method) {
  case IOMethod::READ:
    init_read(fmt.fmt.pix.sizeimage);
    break;
  case IOMethod::MMAP:
    init_mmap();
    break;
  case IOMethod::USERPTR:
    init_userp(fmt.fmt.pix.sizeimage);
    break;
  }
}

Camera::~Camera() { uninit(); }

void Camera::uninit(void) {
  switch (io_method) {
  case IOMethod::READ:
    free(buffers[0].start);
    break;

  case IOMethod::MMAP:
    for (unsigned int i = 0; i < buffer_count; ++i)
      if (munmap(buffers[i].start, buffers[i].length) == -1) {
        errno_exit("munmap");
      }
    break;

  case IOMethod::USERPTR:
    for (unsigned int i = 0; i < buffer_count; ++i) {
      free(buffers[i].start);
    }
    break;
  }
}

void Camera::start_capturing(void) {
  switch (io_method) {
  case IOMethod::READ:
    /* Nothing to do. */
    return;
  case IOMethod::MMAP:
    for (unsigned int i = 0; i < buffer_count; ++i) {
      enqueue_buffer_mmap(i);
    }
    break;

  case IOMethod::USERPTR:
    for (unsigned int i = 0; i < buffer_count; ++i) {
      enqueue_buffer_userp(i);
    }
    break;
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
    errno_exit("VIDIOC_STREAMON");
  }
}

void Camera::stop_capturing(void) {
  switch (io_method) {
  case IOMethod::READ:
    /* Nothing to do */
    return;
  case IOMethod::MMAP:
  case IOMethod::USERPTR:
    break;
  }

  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (xioctl(fd, VIDIOC_STREAMOFF, &type) == -1) {
    errno_exit("VIDIOC_STREAMOFF");
  }
}

FrameView Camera::read_frame(void) {
  for (;;) {
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    /* Timeout. */
    timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    auto r = select(fd + 1, &fds, NULL, NULL, &tv);

    if (r == -1) {
      if (EINTR == errno) {
        continue;
      }
      errno_exit("select");
    }

    if (r == 0) {
      fprintf(stderr, "select timeout\n");
      exit(EXIT_FAILURE);
    }

    if (FD_ISSET(fd, &fds)) {
      break;
    }
  }

  switch (io_method) {
  case IOMethod::READ:
    if (read(fd, buffers[0].start, buffers[0].length) == -1) {
      switch (errno) {
      case EAGAIN:
        fprintf(stderr, "eagain");
        return FrameView(*this);

      case EIO:
        /* Could ignore EIO, see spec. */

        /* fall through */

      default:
        errno_exit("read");
      }
    }

    return FrameView(*this, {},
                     reinterpret_cast<const uint8_t *>(buffers[0].start),
                     buffers[0].length);
  case IOMethod::MMAP: {
    v4l2_buffer v4l2_buf;
    CLEAR(v4l2_buf);

    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_DQBUF, &v4l2_buf) == -1) {
      switch (errno) {
      case EAGAIN:
        fprintf(stderr, "eagain");
        return FrameView(*this);

      case EIO:
        /* Could ignore EIO, see spec. */

        /* fall through */

      default:
        errno_exit("VIDIOC_DQBUF");
      }
    }

    assert(v4l2_buf.index < buffer_count);

    return FrameView(
        *this, v4l2_buf.index,
        reinterpret_cast<const uint8_t *>(buffers[v4l2_buf.index].start),
        v4l2_buf.bytesused);
  }
  case IOMethod::USERPTR: {
    v4l2_buffer v4l2_buf;
    CLEAR(v4l2_buf);

    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v4l2_buf.memory = V4L2_MEMORY_USERPTR;

    if (xioctl(fd, VIDIOC_DQBUF, &v4l2_buf) == -1) {
      switch (errno) {
      case EAGAIN:
        fprintf(stderr, "eagain");
        return FrameView(*this);

      case EIO:
        /* Could ignore EIO, see spec. */

        /* fall through */

      default:
        errno_exit("VIDIOC_DQBUF");
      }
    }

    for (unsigned int i = 0; i < buffer_count; ++i) {
      if (v4l2_buf.m.userptr == (unsigned long)buffers[i].start &&
          v4l2_buf.length == buffers[i].length) {
        break;
      }
    }

    return FrameView(*this, v4l2_buf.index,
                     reinterpret_cast<const uint8_t *>(v4l2_buf.m.userptr),
                     v4l2_buf.bytesused);
  }
  default:
    throw std::runtime_error("Invalid io method");
  }
}

void Camera::clean_after_read(unsigned int index) {
  switch (io_method) {
  case IOMethod::MMAP:
    enqueue_buffer_mmap(index);
    break;
  case IOMethod::USERPTR:
    enqueue_buffer_userp(index);
    break;
  }
}

void Camera::close(void) {
  if (-1 == ::close(fd))
    errno_exit("close");

  fd = -1;
}

void Camera::init_read(unsigned int buffer_size) {
  buffers.reset(new Buffer[1]);

  if (!buffers) {
    fprintf(stderr, "Out of memory\\n");
    exit(EXIT_FAILURE);
  }

  buffers[0].length = buffer_size;
  buffers[0].start = malloc(buffer_size);

  if (!buffers[0].start) {
    fprintf(stderr, "Out of memory\\n");
    exit(EXIT_FAILURE);
  }
}

void Camera::init_mmap(void) {
  struct v4l2_requestbuffers req;

  CLEAR(req);

  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;

  if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr,
              "%s does not support "
              "memory mappingn",
              device.c_str());
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_REQBUFS");
    }
  }

  if (req.count < 2) {
    fprintf(stderr, "Insufficient buffer memory on %s\\n", device.c_str());
    exit(EXIT_FAILURE);
  }

  buffers.reset(new Buffer[req.count]);

  if (!buffers) {
    fprintf(stderr, "Out of memory\\n");
    exit(EXIT_FAILURE);
  }

  for (buffer_count = 0; buffer_count < req.count; ++buffer_count) {
    struct v4l2_buffer buf;

    CLEAR(buf);

    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buffer_count;

    if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
      errno_exit("VIDIOC_QUERYBUF");

    buffers[buffer_count].length = buf.length;
    buffers[buffer_count].start =
        mmap(NULL /* start anywhere */, buf.length,
             PROT_READ | PROT_WRITE /* required */,
             MAP_SHARED /* recommended */, fd, buf.m.offset);

    if (MAP_FAILED == buffers[buffer_count].start)
      errno_exit("mmap");
  }
}

void Camera::init_userp(unsigned int buffer_size) {
  struct v4l2_requestbuffers req;

  CLEAR(req);

  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;

  if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
    if (EINVAL == errno) {
      fprintf(stderr,
              "%s does not support "
              "user pointer i/on",
              device.c_str());
      exit(EXIT_FAILURE);
    } else {
      errno_exit("VIDIOC_REQBUFS");
    }
  }

  buffers.reset(new Buffer[4]);

  if (!buffers) {
    fprintf(stderr, "Out of memory\\n");
    exit(EXIT_FAILURE);
  }

  for (buffer_count = 0; buffer_count < 4; ++buffer_count) {
    buffers[buffer_count].length = buffer_size;
    buffers[buffer_count].start = malloc(buffer_size);

    if (!buffers[buffer_count].start) {
      fprintf(stderr, "Out of memory\\n");
      exit(EXIT_FAILURE);
    }
  }
}

void Camera::enqueue_buffer_mmap(unsigned int idx) {
  v4l2_buffer v4l2_buf;
  CLEAR(v4l2_buf);
  v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  v4l2_buf.memory = V4L2_MEMORY_MMAP;
  v4l2_buf.index = idx;
  v4l2_buf.bytesused = 0;

  if (-1 == xioctl(fd, VIDIOC_QBUF, &v4l2_buf)) {
    errno_exit("VIDIOC_QBUF start");
  }
}

void Camera::enqueue_buffer_userp(unsigned int idx) {
  v4l2_buffer v4l2_buf;
  CLEAR(v4l2_buf);
  v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  v4l2_buf.memory = V4L2_MEMORY_USERPTR;
  v4l2_buf.index = idx;
  v4l2_buf.m.userptr = (unsigned long)buffers[idx].start;
  v4l2_buf.length = buffers[idx].length;
  v4l2_buf.bytesused = 0;

  if (-1 == xioctl(fd, VIDIOC_QBUF, &v4l2_buf)) {
    errno_exit("VIDIOC_QBUF start");
  }
}

FrameView::FrameView(Camera &camera)
    : std::basic_string_view<uint8_t>(nullptr, 0), camera(camera),
      buffer_index(std::nullopt) {}
FrameView::FrameView(Camera &camera, unsigned int buffer_index,
                     const uint8_t *s, size_t len)
    : std::basic_string_view<uint8_t>(s, len), camera(camera),
      buffer_index(buffer_index) {}

FrameView::~FrameView() {
  if (buffer_index.has_value()) {
    camera.clean_after_read(buffer_index.value());
  }
}