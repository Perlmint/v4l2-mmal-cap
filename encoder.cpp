#include "./encoder.h"

#include <exception>

#include <bcm_host.h>
#include <interface/mmal/mmal.h>
#include <interface/mmal/mmal_queue.h>
#include <interface/mmal/util/mmal_connection.h>
#include <interface/mmal/util/mmal_default_components.h>
#include <interface/mmal/util/mmal_util.h>
#include <interface/mmal/util/mmal_util_params.h>
#include <interface/vcos/vcos.h>

struct EncoderContext {
  MMAL_COMPONENT_T *component = nullptr;
  MMAL_POOL_T *pool_in = nullptr, *pool_out = nullptr;
  MMAL_QUEUE_T *queue = nullptr;
  VCOS_SEMAPHORE_T semaphore;
  MMAL_STATUS_T mmal_status = MMAL_SUCCESS;
};

std::atomic<bool> Encoder::initialized = false;

constexpr uint32_t MAX_BUFFERS = 2;

static void log_format(MMAL_ES_FORMAT_T *format, MMAL_PORT_T *port) {
  const char *name_type;

  if (port)
    fprintf(stderr, "%s:%s:%i", port->component->name,
            port->type == MMAL_PORT_TYPE_CONTROL
                ? "ctr"
                : port->type == MMAL_PORT_TYPE_INPUT
                      ? "in"
                      : port->type == MMAL_PORT_TYPE_OUTPUT ? "out" : "invalid",
            (int)port->index);

  switch (format->type) {
  case MMAL_ES_TYPE_AUDIO:
    name_type = "audio";
    break;
  case MMAL_ES_TYPE_VIDEO:
    name_type = "video";
    break;
  case MMAL_ES_TYPE_SUBPICTURE:
    name_type = "subpicture";
    break;
  default:
    name_type = "unknown";
    break;
  }

  fprintf(stderr, "type: %s, fourcc: %4.4s\n", name_type,
          (char *)&format->encoding);
  fprintf(stderr, " bitrate: %i, framed: %i\n", format->bitrate,
          !!(format->flags & MMAL_ES_FORMAT_FLAG_FRAMED));
  fprintf(stderr, " extra data: %i, %p\n", format->extradata_size,
          format->extradata);
  switch (format->type) {
  case MMAL_ES_TYPE_AUDIO:
    fprintf(stderr, " samplerate: %i, channels: %i, bps: %i, block align: %i\n",
            format->es->audio.sample_rate, format->es->audio.channels,
            format->es->audio.bits_per_sample, format->es->audio.block_align);
    break;

  case MMAL_ES_TYPE_VIDEO:
    fprintf(stderr, " width: %i, height: %i, (%i,%i,%i,%i)\n",
            format->es->video.width, format->es->video.height,
            format->es->video.crop.x, format->es->video.crop.y,
            format->es->video.crop.width, format->es->video.crop.height);
    fprintf(stderr, " pixel aspect ratio: %i/%i, frame rate: %i/%i\n",
            format->es->video.par.num, format->es->video.par.den,
            format->es->video.frame_rate.num, format->es->video.frame_rate.den);
    break;

  case MMAL_ES_TYPE_SUBPICTURE:
    break;

  default:
    break;
  }

  if (!port)
    return;

  fprintf(stderr,
          " buffers num: %i(opt %i, min %i), size: %i(opt %i, min: %i), align: "
          "%i\n",
          port->buffer_num, port->buffer_num_recommended, port->buffer_num_min,
          port->buffer_size, port->buffer_size_recommended,
          port->buffer_size_min, port->buffer_alignment_min);
}

static void control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
  auto &ctx = *reinterpret_cast<EncoderContext *>(port->userdata);

  switch (buffer->cmd) {
  case MMAL_EVENT_EOS:
    /* Only sink component generate EOS events */
    break;
  case MMAL_EVENT_ERROR:
    /* Something went wrong. Signal this to the application */
    ctx.mmal_status = *reinterpret_cast<MMAL_STATUS_T *>(buffer->data);
    break;
  default:
    break;
  }

  /* Done with the event, recycle it */
  mmal_buffer_header_release(buffer);

  /* Kick the processing thread */
  vcos_semaphore_post(&ctx.semaphore);
}

static void input_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
  auto &ctx = *reinterpret_cast<EncoderContext *>(port->userdata);

  /* The component is done with the data, just recycle the buffer header into
   * its pool */
  mmal_buffer_header_release(buffer);

  /* Kick the processing thread */
  vcos_semaphore_post(&ctx.semaphore);
}

static void output_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer) {
  auto &ctx = *reinterpret_cast<EncoderContext *>(port->userdata);

  /* Queue the decoded video frame */
  mmal_queue_put(ctx.queue, buffer);

  /* Kick the processing thread */
  vcos_semaphore_post(&ctx.semaphore);
}

inline void check_status(int32_t status) {
  if (status != MMAL_SUCCESS) {
    throw std::runtime_error("");
  }
}

void Encoder::Init() {
  if (initialized.load()) {
    return;
  }

  bcm_host_init();

  initialized.store(true);
}

Encoder::Encoder(uint32_t input_four_cc, uint32_t input_width,
                 uint32_t input_height, uint32_t output_four_cc) {
  Encoder::Init();

  context.reset(new EncoderContext());

  vcos_semaphore_create(&context->semaphore, "encoder", 1);

  auto &component = context->component;

  check_status(
      mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &component));

  component->control->userdata =
      reinterpret_cast<MMAL_PORT_USERDATA_T *>(context.get());
  check_status(mmal_port_enable(component->control, control_callback));

  check_status(mmal_port_parameter_set_boolean(
      component->output[0], MMAL_PARAMETER_ZERO_COPY, MMAL_TRUE));

  auto &format_in = *component->input[0]->format;
  format_in.type = MMAL_ES_TYPE_VIDEO;
  format_in.encoding = input_four_cc;
  format_in.es->video.width = input_width;
  format_in.es->video.height = input_height;
  format_in.es->video.frame_rate.num = 0;
  format_in.es->video.frame_rate.den = 1;
  format_in.es->video.par.num = 1;
  format_in.es->video.par.den = 1;
  format_in.es->video.crop.x = format_in.es->video.crop.y =
      format_in.es->video.crop.width = format_in.es->video.crop.height = 0;

  check_status(mmal_port_format_commit(component->input[0]));

  fprintf(stderr, "%s\n", component->input[0]->name);
  fprintf(stderr, " type: %i, fourcc: %4.4s\n", format_in.type,
          (char *)&format_in.encoding);
  fprintf(stderr, " bitrate: %i, framed: %i\n", format_in.bitrate,
          !!(format_in.flags & MMAL_ES_FORMAT_FLAG_FRAMED));
  fprintf(stderr, " extra data: %i, %p\n", format_in.extradata_size,
          format_in.extradata);
  fprintf(stderr, " width: %i, height: %i, (%i,%i,%i,%i)\n",
          format_in.es->video.width, format_in.es->video.height,
          format_in.es->video.crop.x, format_in.es->video.crop.y,
          format_in.es->video.crop.width, format_in.es->video.crop.height);

  auto &format_out = *component->output[0]->format;
  format_out.encoding = output_four_cc;

  check_status(mmal_port_format_commit(component->output[0]));

  fprintf(stderr, "%s\n", component->output[0]->name);
  fprintf(stderr, " type: %i, fourcc: %4.4s\n", format_out.type,
          (char *)&format_out.encoding);
  fprintf(stderr, " bitrate: %i, framed: %i\n", format_out.bitrate,
          !!(format_out.flags & MMAL_ES_FORMAT_FLAG_FRAMED));
  fprintf(stderr, " extra data: %i, %p\n", format_out.extradata_size,
          format_out.extradata);
  fprintf(stderr, " width: %i, height: %i, (%i,%i,%i,%i)\n",
          format_out.es->video.width, format_out.es->video.height,
          format_out.es->video.crop.x, format_out.es->video.crop.y,
          format_out.es->video.crop.width, format_out.es->video.crop.height);

  component->input[0]->buffer_num = component->input[0]->buffer_num_recommended;
  component->input[0]->buffer_size =
      component->input[0]->buffer_size_recommended;
  component->output[0]->buffer_num =
      component->output[0]->buffer_num_recommended;
  component->output[0]->buffer_size =
      component->output[0]->buffer_size_recommended;
  context->pool_in = mmal_port_pool_create(component->input[0],
                                           component->input[0]->buffer_num,
                                           component->input[0]->buffer_size);

  context->queue = mmal_queue_create();

  component->input[0]->userdata =
      reinterpret_cast<MMAL_PORT_USERDATA_T *>(context.get());
  component->output[0]->userdata =
      reinterpret_cast<MMAL_PORT_USERDATA_T *>(context.get());

  check_status(mmal_port_enable(component->input[0], input_callback));
  check_status(mmal_port_enable(component->output[0], output_callback));

  context->pool_out = mmal_port_pool_create(component->output[0],
                                            component->output[0]->buffer_num,
                                            component->output[0]->buffer_size);

  MMAL_BUFFER_HEADER_T *buffer = nullptr;
  while ((buffer = mmal_queue_get(context->pool_out->queue)) != nullptr) {
    // printf("Sending buf %p\n", buffer);
    check_status(mmal_port_send_buffer(component->output[0], buffer));
  }

  mmal_component_enable(component);
}

std::vector<uint8_t> Encoder::encode(const uint8_t *input, uint32_t length) {
  bool in_eos = false;
  bool out_eos = false;
  std::vector<uint8_t> ret;
  auto &component = context->component;
  auto &pool_out = context->pool_out;
  auto &pool_in = context->pool_in;

  while (!out_eos) {
    MMAL_BUFFER_HEADER_T *buffer = nullptr;

    auto vcos_status = vcos_semaphore_wait_timeout(&context->semaphore, 2000);
    if (vcos_status != VCOS_SUCCESS) {
      fprintf(stderr, "vcos_semaphore_wait_timeout failed - status %d\n",
              vcos_status);
    }

    if (context->mmal_status != MMAL_SUCCESS) {
      fprintf(stderr, "mmal error - %u\n", context->mmal_status);
      break;
    }

    while (!in_eos && (buffer = mmal_queue_get(pool_in->queue)) != nullptr) {
      const auto copy_len = std::min(buffer->alloc_size - 128, length);
      if (copy_len > 0) {
        memcpy(buffer->data, input, copy_len);
        buffer->offset = 0;
        length -= copy_len;
        buffer->flags = 0;
        input += copy_len;
      } else {
        buffer->flags = MMAL_BUFFER_HEADER_FLAG_EOS;
        in_eos = true;
      }
      buffer->length = copy_len;

      buffer->pts = buffer->dts = MMAL_TIME_UNKNOWN;
      check_status(mmal_port_send_buffer(component->input[0], buffer));
    }

    while ((buffer = mmal_queue_get(context->queue)) != nullptr) {
      out_eos = (buffer->flags & MMAL_BUFFER_HEADER_FLAG_EOS) != 0;

      if (buffer->cmd != 0) {
        fprintf(stderr, "received event length %d, %4.4s\n", buffer->length,
                (char *)&buffer->cmd);
        if (buffer->cmd == MMAL_EVENT_FORMAT_CHANGED) {
          auto *event = mmal_event_format_changed_get(buffer);
          if (event) {
            fprintf(stderr, "----------Port format changed----------\n");
            log_format(component->output[0]->format, component->output[0]);
            fprintf(stderr, "-----------------to---------------------\n");
            log_format(event->format, 0);
            fprintf(stderr,
                    " buffers num (opt %i, min %i), size (opt %i, min: %i)\n",
                    event->buffer_num_recommended, event->buffer_num_min,
                    event->buffer_size_recommended, event->buffer_size_min);
            fprintf(stderr, "----------------------------------------\n");
          }
          mmal_buffer_header_release(buffer);
          mmal_port_disable(component->output[0]);

          // Clear out the queue and release the buffers.
          while (mmal_queue_length(pool_out->queue) < pool_out->headers_num) {
            buffer = mmal_queue_wait(context->queue);
            mmal_buffer_header_release(buffer);
            fprintf(stderr, "Retrieved buffer %p\n", buffer);
          }

          // Assume we can't reuse the output buffers, so have to disable,
          // destroy pool, create new pool, enable port, feed in buffers.
          mmal_port_pool_destroy(component->output[0], pool_out);

          check_status(mmal_format_full_copy(component->output[0]->format,
                                             event->format));
          component->output[0]->format->encoding = MMAL_ENCODING_I420;
          component->output[0]->buffer_num = MAX_BUFFERS;
          component->output[0]->buffer_size =
              component->output[0]->buffer_size_recommended;

          check_status(mmal_port_format_commit(component->output[0]));

          mmal_port_enable(component->output[0], output_callback);
          pool_out = mmal_port_pool_create(component->output[0],
                                           component->output[0]->buffer_num,
                                           component->output[0]->buffer_size);
        } else {
          mmal_buffer_header_release(buffer);
        }
        continue;
      } else {
        auto *begin = buffer->data + buffer->offset;
        ret.insert(ret.end(), begin, begin + buffer->length);
        mmal_buffer_header_release(buffer);
      }
    }

    while ((buffer = mmal_queue_get(pool_out->queue)) != NULL) {
      check_status(mmal_port_send_buffer(component->output[0], buffer));
    }
  }

  return ret;
}

Encoder::~Encoder() {
  auto &component = context->component;
  mmal_port_disable(component->input[0]);
  mmal_port_disable(component->output[0]);
  mmal_component_disable(component);
}