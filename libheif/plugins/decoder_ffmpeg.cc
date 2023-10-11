/*
 * HEIF codec.
 * Copyright (c) 2023 struktur AG, Dirk Farin <farin@struktur.de>
 *
 * This file is part of libheif.
 *
 * libheif is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libheif is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with libheif.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "libheif/heif.h"
#include "libheif/heif_plugin.h"
#include "decoder_ffmpeg.h"

#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif

#include <assert.h>
#include <memory>
#include <map>

extern "C" 
{
    #include <libavcodec/avcodec.h>
}

class NalUnit {
public:
    NalUnit();
    ~NalUnit();
    bool set_data(const unsigned char* in_data, int n);
    int size() const { return nal_data_size; }
    int unit_type() const { return nal_unit_type;  }
    const unsigned char* data() const { return nal_data_ptr; }
    int bitExtracted(int number, int bits_count, int position_nr)
    {
        return (((1 << bits_count) - 1) & (number >> (position_nr - 1)));
    }
private:
    const unsigned char* nal_data_ptr;
    int nal_unit_type;
    int nal_data_size;
};

struct ffmpeg_decoder
{
    #define NAL_UNIT_VPS_NUT    32
    #define NAL_UNIT_SPS_NUT    33
    #define NAL_UNIT_PPS_NUT    34
    #define NAL_UNIT_IDR_W_RADL 19
    #define NAL_UNIT_IDR_N_LP   20

    std::map<int,NalUnit*> NalMap;

    bool strict_decoding = false;
};

static const char kEmptyString[] = "";
static const char kSuccess[] = "Success";

static const int FFMPEG_DECODER_PLUGIN_PRIORITY = 90;

#define MAX_PLUGIN_NAME_LENGTH 80

static char plugin_name[MAX_PLUGIN_NAME_LENGTH];


static const char* ffmpeg_plugin_name()
{
  snprintf(plugin_name, MAX_PLUGIN_NAME_LENGTH, "FFMPEG HEVC decoder %s", av_version_info());
  plugin_name[MAX_PLUGIN_NAME_LENGTH - 1] = 0; //null-terminated

  return plugin_name;
}


static void ffmpeg_init_plugin()
{

}


static void ffmpeg_deinit_plugin()
{

}


static int ffmpeg_does_support_format(enum heif_compression_format format)
{
  if (format == heif_compression_HEVC) {
    return FFMPEG_DECODER_PLUGIN_PRIORITY;
  }
  else {
    return 0;
  }
}


static struct heif_error ffmpeg_new_decoder(void** dec)
{
  struct ffmpeg_decoder* decoder = new ffmpeg_decoder();

  *dec = decoder;
  return heif_error_success;
}

static void ffmpeg_free_decoder(void* decoder_raw)
{
  struct ffmpeg_decoder* decoder = (struct ffmpeg_decoder*) decoder_raw;

  delete decoder;
}


void ffmpeg_set_strict_decoding(void* decoder_raw, int flag)
{
  struct ffmpeg_decoder* decoder = (ffmpeg_decoder*) decoder_raw;

  decoder->strict_decoding = flag;
}

NalUnit::NalUnit()
{
    nal_data_ptr = NULL;
    nal_unit_type = 0;
    nal_data_size = 0;
}

NalUnit::~NalUnit()
{

}

bool NalUnit::set_data(const unsigned char* in_data, int n)
{
    nal_data_ptr = in_data;
    nal_unit_type = bitExtracted(nal_data_ptr[0], 6, 2);
    nal_data_size = n;
    return true;
}

static struct heif_error ffmpeg_v1_push_data(void* decoder_raw, const void* data, size_t size)
{

  struct ffmpeg_decoder* decoder = (struct ffmpeg_decoder*) decoder_raw;

  const uint8_t* cdata = (const uint8_t*) data;

  size_t ptr = 0;
  while (ptr < size) {
      if (4 > size - ptr) {
          struct heif_error err = { heif_error_Decoder_plugin_error,
                                   heif_suberror_End_of_data,
                                   kEmptyString };
          return err;
      }

      uint32_t nal_size = (cdata[ptr] << 24) | (cdata[ptr + 1] << 16) | (cdata[ptr + 2] << 8) | (cdata[ptr + 3]);
      ptr += 4;

      if (nal_size > size - ptr) {
          struct heif_error err = { heif_error_Decoder_plugin_error,
                                   heif_suberror_End_of_data,
                                   kEmptyString };
          return err;
      }

      NalUnit* nal_unit = new NalUnit();
      nal_unit->set_data(cdata + ptr, nal_size);

      decoder->NalMap[nal_unit->unit_type()] = nal_unit;

      ptr += nal_size;
  }

  return heif_error_success;
}

static struct heif_error hevc_decode(AVCodecContext* hevc_dec_ctx, AVFrame* hevc_frame, AVPacket* hevc_pkt, struct heif_image** image)
{
    int ret;

    ret = avcodec_send_packet(hevc_dec_ctx, hevc_pkt);
    if (ret < 0) {
        struct heif_error err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "Error in avcodec_send_packet" };
        return err;
    }

    ret = avcodec_receive_frame(hevc_dec_ctx, hevc_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
    {
        struct heif_error err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "avcodec_receive_frame returned EAGAIN or ERROR_EOF" };
        return err;
    }
    else if (ret < 0) {
        struct heif_error err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "Error in avcodec_receive_frame" };
        return err;
    }


    if ((hevc_dec_ctx->pix_fmt == AV_PIX_FMT_YUV420P) || (hevc_dec_ctx->pix_fmt == AV_PIX_FMT_YUVJ420P) || //planar YUV 4:2:0, 12bpp, (1 Cr & Cb sample per 2x2 Y samples)
        (hevc_dec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE))
    {
        heif_error err;
        err = heif_image_create(hevc_frame->width,
            hevc_frame->height,
            heif_colorspace_YCbCr,
            heif_chroma_420,
            image);
        if (err.code) {
            return err;
        }

        heif_channel channel2plane[3] = {
            heif_channel_Y,
            heif_channel_Cb,
            heif_channel_Cr
        };

        int nPlanes = 3;

        for (int channel = 0; channel < nPlanes; channel++) {

            int bpp = (hevc_dec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE) ? 10 : 8;
            int stride = hevc_frame->linesize[channel];
            const uint8_t* data = hevc_frame->data[channel];

            int w = (channel == 0) ? hevc_frame->width : hevc_frame->width >> 1;
            int h = (channel == 0) ? hevc_frame->height : hevc_frame->height >> 1;
            if (w <= 0 || h <= 0) {
                heif_image_release(*image);
                err = { heif_error_Decoder_plugin_error,
                       heif_suberror_Invalid_image_size,
                       kEmptyString };
                return err;
            }

            err = heif_image_add_plane(*image, channel2plane[channel], w, h, bpp);
            if (err.code) {
                heif_image_release(*image);
                return err;
            }

            int dst_stride;
            uint8_t* dst_mem = heif_image_get_plane(*image, channel2plane[channel], &dst_stride);

            int bytes_per_pixel = (bpp + 7) / 8;

            for (int y = 0; y < h; y++) {
                memcpy(dst_mem + y * dst_stride, data + y * stride, w * bytes_per_pixel);
            }
        }

        return heif_error_success;
    }
    else
    {
        struct heif_error err = { heif_error_Unsupported_feature,
                                 heif_suberror_Unsupported_color_conversion,
                                 "Pixel format not implemented" };
        return err;
    }
}

static struct heif_error ffmpeg_v1_decode_image(void* decoder_raw,
                                                  struct heif_image** out_img)
{
  struct ffmpeg_decoder* decoder = (struct ffmpeg_decoder*) decoder_raw;

  int heif_idrpic_size;
  int heif_vps_size;
  int heif_sps_size;
  int heif_pps_size;
  const unsigned char* heif_vps_data;
  const unsigned char* heif_sps_data;
  const unsigned char* heif_pps_data;
  const unsigned char* heif_idrpic_data;

  if ((decoder->NalMap.count(NAL_UNIT_VPS_NUT) > 0)
      && (decoder->NalMap.count(NAL_UNIT_SPS_NUT) > 0)
      && (decoder->NalMap.count(NAL_UNIT_PPS_NUT) > 0)
      )
  {
      heif_vps_size = decoder->NalMap[NAL_UNIT_VPS_NUT]->size();
      heif_vps_data = decoder->NalMap[NAL_UNIT_VPS_NUT]->data();

      heif_sps_size = decoder->NalMap[NAL_UNIT_SPS_NUT]->size();
      heif_sps_data = decoder->NalMap[NAL_UNIT_SPS_NUT]->data();

      heif_pps_size = decoder->NalMap[NAL_UNIT_PPS_NUT]->size();
      heif_pps_data = decoder->NalMap[NAL_UNIT_PPS_NUT]->data();
  }
  else
  {
      struct heif_error err = { heif_error_Decoder_plugin_error,
                                heif_suberror_End_of_data,
                                "Unexpected end of data" };
      return err;
  }

  if ((decoder->NalMap.count(NAL_UNIT_IDR_W_RADL) > 0) || (decoder->NalMap.count(NAL_UNIT_IDR_N_LP) > 0))
  {
      if (decoder->NalMap.count(NAL_UNIT_IDR_W_RADL) > 0)
      {
          heif_idrpic_data = decoder->NalMap[NAL_UNIT_IDR_W_RADL]->data();
          heif_idrpic_size = decoder->NalMap[NAL_UNIT_IDR_W_RADL]->size();
      }
      else
      {
          heif_idrpic_data = decoder->NalMap[NAL_UNIT_IDR_N_LP]->data();
          heif_idrpic_size = decoder->NalMap[NAL_UNIT_IDR_N_LP]->size();
      }
  }
  else
  {
      struct heif_error err = { heif_error_Decoder_plugin_error,
                                heif_suberror_End_of_data,
                                "Unexpected end of data" };
      return err;
  }

  const char hevc_AnnexB_StartCode[] = { 0x00, 0x00, 0x00, 0x01 };
  int hevc_AnnexB_StartCode_size = 4;

  size_t hevc_data_size = heif_vps_size + heif_sps_size + heif_pps_size + heif_idrpic_size + 4 * hevc_AnnexB_StartCode_size;
  uint8_t* hevc_data = (uint8_t*)malloc(hevc_data_size);

  //Copy hevc pps data
  uint8_t* hevc_data_ptr = hevc_data;
  memcpy(hevc_data_ptr, hevc_AnnexB_StartCode, hevc_AnnexB_StartCode_size);
  hevc_data_ptr += hevc_AnnexB_StartCode_size;
  memcpy(hevc_data_ptr, heif_vps_data, heif_vps_size);
  hevc_data_ptr += heif_vps_size;

  //Copy hevc sps data
  memcpy(hevc_data_ptr, hevc_AnnexB_StartCode, hevc_AnnexB_StartCode_size);
  hevc_data_ptr += hevc_AnnexB_StartCode_size;
  memcpy(hevc_data_ptr, heif_sps_data, heif_sps_size);
  hevc_data_ptr += heif_sps_size;

  //Copy hevc pps data
  memcpy(hevc_data_ptr, hevc_AnnexB_StartCode, hevc_AnnexB_StartCode_size);
  hevc_data_ptr += hevc_AnnexB_StartCode_size;
  memcpy(hevc_data_ptr, heif_pps_data, heif_pps_size);
  hevc_data_ptr += heif_pps_size;

  //Copy hevc idrpic data
  memcpy(hevc_data_ptr, hevc_AnnexB_StartCode, hevc_AnnexB_StartCode_size);
  hevc_data_ptr += hevc_AnnexB_StartCode_size;
  memcpy(hevc_data_ptr, heif_idrpic_data, heif_idrpic_size);

  //decoder->NalMap not needed anymore
  for (auto current = decoder->NalMap.begin(); current != decoder->NalMap.end(); ++current) {
      delete current->second;
  }
  decoder->NalMap.clear();

  const AVCodec* hevc_codec = NULL;
  AVCodecParserContext* hevc_parser = NULL;
  AVCodecContext* hevc_codecContext = NULL;
  AVPacket* hevc_pkt = NULL;
  AVFrame* hevc_frame = NULL;
  AVCodecParameters* hevc_codecParam = NULL;
  struct heif_color_profile_nclx* nclx = NULL;
  int ret = 0;

  struct heif_error err = heif_error_success;

  uint8_t* parse_hevc_data = NULL;
  int parse_hevc_data_size = 0;

  uint8_t video_full_range_flag = 0;
  uint8_t color_primaries = 0;
  uint8_t transfer_characteristics = 0;
  uint8_t matrix_coefficients = 0;

  hevc_pkt = av_packet_alloc();
  if (!hevc_pkt) {
    err = { heif_error_Memory_allocation_error, heif_suberror_Unspecified, "av_packet_alloc returned error" };
    goto errexit;
  }

  // Find HEVC video decoder
  hevc_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);

  if (!hevc_codec) {
    err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "avcodec_find_decoder(AV_CODEC_ID_HEVC) returned error" };
    goto errexit;
  }

  hevc_parser = av_parser_init(hevc_codec->id);
  if (!hevc_parser) {
    err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "av_parser_init returned error" };
    goto errexit;
  }

  hevc_codecContext = avcodec_alloc_context3(hevc_codec);
  if (!hevc_codecContext) {
    err = { heif_error_Memory_allocation_error, heif_suberror_Unspecified, "avcodec_alloc_context3 returned error" };
    goto errexit;
  }

  /* open it */
  if (avcodec_open2(hevc_codecContext, hevc_codec, NULL) < 0) {
    err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "avcodec_open2 returned error" };
    goto errexit;
  }

  hevc_frame = av_frame_alloc();
  if (!hevc_frame) {
    err = { heif_error_Memory_allocation_error, heif_suberror_Unspecified, "av_frame_alloc returned error" };
    goto errexit;
  }

  parse_hevc_data = hevc_data;
  parse_hevc_data_size = (int)hevc_data_size;
  while (parse_hevc_data_size > 0) {
      hevc_parser->flags = PARSER_FLAG_COMPLETE_FRAMES;
      ret = av_parser_parse2(hevc_parser, hevc_codecContext, &hevc_pkt->data, &hevc_pkt->size, parse_hevc_data, parse_hevc_data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
 
      if (ret < 0) {
	err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "av_parser_parse2 returned error" };
	goto errexit;
      }
      parse_hevc_data += ret;
      parse_hevc_data_size -= ret;

      if (hevc_pkt->size)
      {
	err = hevc_decode(hevc_codecContext, hevc_frame, hevc_pkt, out_img);
	if (err.code != heif_error_Ok)
	  goto errexit;
      }
  }

  hevc_codecParam = avcodec_parameters_alloc();
  if (!hevc_codecParam) {
    err = { heif_error_Memory_allocation_error, heif_suberror_Unspecified, "avcodec_parameters_alloc returned error" };
    goto errexit;
  }
  if (avcodec_parameters_from_context(hevc_codecParam, hevc_codecContext) < 0)
  {
    err = { heif_error_Decoder_plugin_error, heif_suberror_Unspecified, "avcodec_parameters_from_context returned error" };
    goto errexit;
  }

  video_full_range_flag = (hevc_codecParam->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
  color_primaries = hevc_codecParam->color_primaries;
  transfer_characteristics = hevc_codecParam->color_trc;
  matrix_coefficients = hevc_codecParam->color_space;

  nclx = heif_nclx_color_profile_alloc();
  heif_nclx_color_profile_set_color_primaries(nclx, static_cast<uint16_t>(color_primaries));
  heif_nclx_color_profile_set_transfer_characteristics(nclx, static_cast<uint16_t>(transfer_characteristics));
  heif_nclx_color_profile_set_matrix_coefficients(nclx, static_cast<uint16_t>(matrix_coefficients));
  nclx->full_range_flag = (bool)video_full_range_flag;
  heif_image_set_nclx_color_profile(*out_img, nclx);

errexit:
  if (hevc_codecParam) avcodec_parameters_free(&hevc_codecParam);
  if (hevc_data) free(hevc_data);
  if (hevc_parser) av_parser_close(hevc_parser);
  if (hevc_codecContext) avcodec_free_context(&hevc_codecContext);
  if (hevc_frame) av_frame_free(&hevc_frame);
  if (hevc_pkt) av_packet_free(&hevc_pkt);
  if (nclx) heif_nclx_color_profile_free(nclx);

  return err;
}

static const struct heif_decoder_plugin decoder_ffmpeg
    {
        3,
        ffmpeg_plugin_name,
        ffmpeg_init_plugin,
        ffmpeg_deinit_plugin,
        ffmpeg_does_support_format,
        ffmpeg_new_decoder,
        ffmpeg_free_decoder,
        ffmpeg_v1_push_data,
        ffmpeg_v1_decode_image,
        ffmpeg_set_strict_decoding,
        "ffmpeg"
    };

const struct heif_decoder_plugin* get_decoder_plugin_ffmpeg()
{
  return &decoder_ffmpeg;
}

#if PLUGIN_FFMPEG_DECODER
heif_plugin_info plugin_info{
  1,
  heif_plugin_type_decoder,
  &decoder_ffmpeg
};
#endif
