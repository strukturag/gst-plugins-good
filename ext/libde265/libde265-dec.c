/*
 * GStreamer HEVC/H.265 video codec.
 *
 * Copyright (c) 2014 struktur AG, Joachim Bauch <bauch@struktur.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-libde265dec
 *
 * Decodes HEVC/H.265 video.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 filesrc location=bitstream.hevc ! libde265dec mode=raw framerate=25/1 ! autovideosink
 * ]| The above pipeline decodes the HEVC/H.265 bitstream and renders it to the screen.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "libde265-dec.h"

#if defined(LIBDE265_NUMERIC_VERSION) && LIBDE265_NUMERIC_VERSION >= 0x00050000
#define HAVE_DE265_ERROR_WAITING_FOR_INPUT_DATA
#endif

// use two decoder threads if no information about
// available CPU cores can be retrieved
#define DEFAULT_THREAD_COUNT        2

#define READ_BE32(x)                                 \
    (((uint32_t)((const uint8_t*)(x))[0] << 24) |    \
               (((const uint8_t*)(x))[1] << 16) |    \
               (((const uint8_t*)(x))[2] <<  8) |    \
                ((const uint8_t*)(x))[3])

#define WRITE_BE32(p, darg) do {                \
        unsigned d = (darg);                    \
        ((uint8_t*)(p))[3] = (d);               \
        ((uint8_t*)(p))[2] = (d)>>8;            \
        ((uint8_t*)(p))[1] = (d)>>16;           \
        ((uint8_t*)(p))[0] = (d)>>24;           \
    } while(0)

#define parent_class gst_libde265_dec_parent_class
G_DEFINE_TYPE (GstLibde265Dec, gst_libde265_dec, GST_TYPE_VIDEO_DECODER);

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("video/x-h265")
    );

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE ("{ I420 }"))
    );

enum
{
  PROP_0,
  PROP_MODE,
  PROP_FRAMERATE,
  PROP_LAST
};

#define DEFAULT_MODE        GST_TYPE_LIBDE265_DEC_PACKETIZED
#define DEFAULT_FPS_N       0
#define DEFAULT_FPS_D       1

#define GST_TYPE_LIBDE265_DEC_MODE (gst_libde265_dec_mode_get_type ())
static GType
gst_libde265_dec_mode_get_type (void)
{
  static GType libde265_dec_mode_type = 0;
  static const GEnumValue libde265_dec_mode_types[] = {
    {GST_TYPE_LIBDE265_DEC_PACKETIZED,
        "Packetized H.265 bitstream with packet lengths "
          "instead of startcodes", "packetized"},
    {GST_TYPE_LIBDE265_DEC_RAW,
        "Raw H.265 bitstream including startcodes", "raw"},
    {0, NULL, NULL}
  };

  if (!libde265_dec_mode_type) {
    libde265_dec_mode_type =
        g_enum_register_static ("GstLibde265DecMode", libde265_dec_mode_types);
  }
  return libde265_dec_mode_type;
}

static void gst_libde265_dec_finalize (GObject * object);

static void gst_libde265_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_libde265_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static gboolean gst_libde265_dec_start (GstVideoDecoder * decoder);
static gboolean gst_libde265_dec_stop (GstVideoDecoder * decoder);
static GstFlowReturn gst_libde265_dec_parse (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame, GstAdapter * adapter, gboolean at_eos);
static gboolean gst_libde265_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_libde265_dec_flush (GstVideoDecoder * decoder);
static GstFlowReturn gst_libde265_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);

static void
gst_libde265_dec_class_init (GstLibde265DecClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_libde265_dec_finalize;
  gobject_class->set_property = gst_libde265_dec_set_property;
  gobject_class->get_property = gst_libde265_dec_get_property;

  g_object_class_install_property (gobject_class, PROP_MODE,
      g_param_spec_enum ("mode", "Input mode",
          "Input mode of data to decode", GST_TYPE_LIBDE265_DEC_MODE,
          DEFAULT_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_FRAMERATE,
      gst_param_spec_fraction ("framerate", "Frame Rate",
          "Frame rate of images in raw stream", 0, 1, 100, 1, DEFAULT_FPS_N,
          DEFAULT_FPS_D, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  decoder_class->start = GST_DEBUG_FUNCPTR (gst_libde265_dec_start);
  decoder_class->stop = GST_DEBUG_FUNCPTR (gst_libde265_dec_stop);
  decoder_class->parse = GST_DEBUG_FUNCPTR (gst_libde265_dec_parse);
  decoder_class->set_format = GST_DEBUG_FUNCPTR (gst_libde265_dec_set_format);
  decoder_class->flush = GST_DEBUG_FUNCPTR (gst_libde265_dec_flush);
  decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_libde265_dec_handle_frame);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));
  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&src_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "HEVC/H.265 parser",
      "Codec/Decoder/Video",
      "Decodes HEVC/H.265 video streams using libde265",
      "struktur AG <opensource@struktur.de>");
}

static inline void
_gst_libde265_dec_reset_decoder (GstLibde265Dec * dec)
{
  dec->ctx = NULL;
  dec->width = -1;
  dec->height = -1;
  dec->buffer_full = 0;
  dec->input_state = NULL;
}

static void
gst_libde265_dec_init (GstLibde265Dec * dec)
{
  dec->mode = DEFAULT_MODE;
  dec->fps_n = DEFAULT_FPS_N;
  dec->fps_d = DEFAULT_FPS_D;
  _gst_libde265_dec_reset_decoder (dec);
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (dec), FALSE);
}

static inline void
_gst_libde265_dec_free_decoder (GstLibde265Dec * dec)
{
  if (dec->ctx != NULL) {
    de265_free_decoder (dec->ctx);
  }
  if (dec->input_state != NULL) {
    gst_video_codec_state_unref (dec->input_state);
  }
  _gst_libde265_dec_reset_decoder (dec);
}

static void
gst_libde265_dec_finalize (GObject * object)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (object);

  _gst_libde265_dec_free_decoder (dec);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_libde265_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (object);

  switch (prop_id) {
    case PROP_MODE:
      dec->mode = g_value_get_enum (value);
      GST_DEBUG ("Mode set to %d\n", dec->mode);
      break;
    case PROP_FRAMERATE:
      dec->fps_n = gst_value_get_fraction_numerator (value);
      dec->fps_d = gst_value_get_fraction_denominator (value);
      GST_DEBUG ("Framerate set to %d/%d\n", dec->fps_n, dec->fps_d);
      break;
    default:
      break;
  }
}

static void
gst_libde265_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (object);

  switch (prop_id) {
    case PROP_MODE:
      g_value_set_enum (value, dec->mode);
      break;
    case PROP_FRAMERATE:
      gst_value_set_fraction (value, dec->fps_n, dec->fps_d);
      break;
    default:
      break;
  }
}

static gboolean
gst_libde265_dec_start (GstVideoDecoder * decoder)
{
  int threads;
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);

  _gst_libde265_dec_free_decoder (dec);
  dec->ctx = de265_new_decoder ();
  if (dec->ctx == NULL) {
    return FALSE;
  }
#if defined(_SC_NPROC_ONLN)
  threads = sysconf (_SC_NPROC_ONLN);
#elif defined(_SC_NPROCESSORS_ONLN)
  threads = sysconf (_SC_NPROCESSORS_ONLN);
#else
#warning "Don't know how to get number of CPU cores, will use the default thread count"
  threads = DEFAULT_THREAD_COUNT;
#endif
  if (threads <= 0) {
    threads = DEFAULT_THREAD_COUNT;
  }

  de265_start_worker_threads (dec->ctx, threads);
  GST_INFO ("Starting %d worker threads\n", threads);

  return TRUE;
}

static gboolean
gst_libde265_dec_stop (GstVideoDecoder * decoder)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);

  _gst_libde265_dec_free_decoder (dec);

  return TRUE;
}

static gboolean
gst_libde265_dec_flush (GstVideoDecoder * decoder)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);

  // flush any pending decoded images
  const struct de265_image *img;
  do {
    img = de265_get_next_picture (dec->ctx);
  } while (img != NULL);
  dec->buffer_full = 0;

  return TRUE;
}

static GstFlowReturn
_gst_libde265_image_available (GstVideoDecoder * decoder,
    const struct de265_image *image)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);
  int width = de265_get_image_width (image, 0);
  int height = de265_get_image_height (image, 0);

  if (G_UNLIKELY (width != dec->width || height != dec->height)) {
    GstVideoCodecState *state =
        gst_video_decoder_set_output_state (decoder, GST_VIDEO_FORMAT_I420,
        width,
        height, dec->input_state);
    g_assert (state != NULL);
    if (dec->fps_n > 0) {
      state->info.fps_n = dec->fps_n;
      state->info.fps_d = dec->fps_d;
    }
    gst_video_decoder_negotiate (decoder);
    GST_DEBUG ("Frame dimensions are %d x %d\n", width, height);
    dec->width = width;
    dec->height = height;
  }

  return gst_video_decoder_have_frame (decoder);
}

static GstFlowReturn
gst_libde265_dec_parse (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame, GstAdapter * adapter, gboolean at_eos)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);
  const struct de265_image *img;
  gsize size;
  GstBuffer *buf;
  uint8_t *frame_data;
  uint8_t *end_data;
  GstMapInfo info;
  de265_error ret;

  if (dec->buffer_full) {
    // return any pending images before decoding more data
    if ((img = de265_peek_next_picture (dec->ctx)) != NULL) {
      return _gst_libde265_image_available (decoder, img);
    }
    dec->buffer_full = 0;
  }

  size = gst_adapter_available (adapter);
  if (size == 0) {
    return GST_VIDEO_DECODER_FLOW_NEED_DATA;
  }

  buf = gst_adapter_take_buffer (adapter, size);
  if (!gst_buffer_map (buf, &info, GST_MAP_READWRITE)) {
    return GST_FLOW_ERROR;
  }

  frame_data = info.data;
  end_data = frame_data + size;

  if (dec->mode == GST_TYPE_LIBDE265_DEC_PACKETIZED) {
    // replace 4-byte length fields with NAL start codes
    uint8_t *start_data = frame_data;
    while (start_data + 4 <= end_data) {
      int nal_size = READ_BE32 (start_data);
      WRITE_BE32 (start_data, 0x00000001);
      start_data += 4 + nal_size;
    }
    if (start_data > end_data) {
      GST_ELEMENT_ERROR (decoder, STREAM, DECODE,
          ("Overflow in input data, check data mode"), (NULL));
      return GST_FLOW_ERROR;
    }
  }

  ret = de265_push_data (dec->ctx, frame_data, end_data - frame_data, 0, NULL);
  gst_buffer_unmap (buf, &info);
  gst_buffer_unref (buf);
  if (ret == DE265_OK) {
    ret = de265_decode (dec->ctx, NULL);
  }
  switch (ret) {
    case DE265_OK:
      break;

    case DE265_ERROR_IMAGE_BUFFER_FULL:
      dec->buffer_full = 1;
      if ((img = de265_peek_next_picture (dec->ctx)) == NULL) {
        return GST_VIDEO_DECODER_FLOW_NEED_DATA;
      }
      return _gst_libde265_image_available (decoder, img);
#ifdef HAVE_DE265_ERROR_WAITING_FOR_INPUT_DATA
    case DE265_ERROR_WAITING_FOR_INPUT_DATA:
      return GST_VIDEO_DECODER_FLOW_NEED_DATA;
#endif
    default:
      GST_ELEMENT_ERROR (decoder, STREAM, DECODE,
          ("Error while decoding: %s (code=%d)", de265_get_error_text (ret),
              ret), (NULL));
      return GST_FLOW_ERROR;
  }

  while ((ret = de265_get_warning (dec->ctx)) != DE265_OK) {
    GST_ELEMENT_WARNING (decoder, STREAM, DECODE,
        ("%s (code=%d)", de265_get_error_text (ret), ret), (NULL));
  }

  if ((img = de265_peek_next_picture (dec->ctx)) == NULL) {
    // need more data
    return GST_VIDEO_DECODER_FLOW_NEED_DATA;
  }

  return _gst_libde265_image_available (decoder, img);
}

static gboolean
gst_libde265_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);

  if (dec->input_state != NULL) {
    gst_video_codec_state_unref (dec->input_state);
  }
  dec->input_state = state;
  if (state != NULL) {
    gst_video_codec_state_ref (state);
  }

  return TRUE;
}

static GstFlowReturn
gst_libde265_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstFlowReturn ret = GST_FLOW_OK;
  GstLibde265Dec *dec = GST_LIBDE265_DEC (decoder);
  uint8_t *dest;
  GstMapInfo info;
  int plane;

  const struct de265_image *img = de265_get_next_picture (dec->ctx);
  if (img == NULL) {
    // need more data
    return GST_FLOW_OK;
  }

  ret = gst_video_decoder_allocate_output_frame (decoder, frame);
  if (ret != GST_FLOW_OK) {
    return ret;
  }

  if (!gst_buffer_map (frame->output_buffer, &info, GST_MAP_WRITE)) {
    return GST_FLOW_ERROR;
  }

  dest = info.data;

  for (plane = 0; plane < 3; plane++) {
    int stride;
    int height = de265_get_image_height (img, plane);
    const uint8_t *src = de265_get_image_plane (img, plane, &stride);
    memcpy (dest, src, height * stride);
    dest += (height * stride);
  }
  gst_buffer_unmap (frame->output_buffer, &info);

  return gst_video_decoder_finish_frame (decoder, frame);
}

gboolean
gst_libde265_dec_plugin_init (GstPlugin * plugin)
{
  /* create an elementfactory for the libde265 decoder element */
  if (!gst_element_register (plugin, "libde265dec",
          GST_RANK_PRIMARY, GST_TYPE_LIBDE265_DEC))
    return FALSE;

  return TRUE;
}
