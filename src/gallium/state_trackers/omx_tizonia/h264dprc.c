/**************************************************************************
 *
 * Copyright 2013 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <tizplatform.h>
#include <tizkernel.h>
#include <tizutils.h>

#include "entrypoint.h"
#include "h264d.h"
#include "h264dprc.h"
#include "h264dprc_decls.h"

#include "vl/vl_video_buffer.h"
#include "vl/vl_compositor.h"
#include "util/u_memory.h"
#include "util/u_surface.h"

#include "dri_screen.h"
#include "egl_dri2.h"

#define DPB_MAX_SIZE 5

unsigned dec_frame_delta;

struct dpb_list {
   struct list_head list;
   struct pipe_video_buffer *buffer;
   OMX_TICKS timestamp;
   int poc;
};

static const uint8_t Default_4x4_Intra[16] = {
    6, 13, 20, 28, 13, 20, 28, 32,
   20, 28, 32, 37, 28, 32, 37, 42
};

static const uint8_t Default_4x4_Inter[16] = {
   10, 14, 20, 24, 14, 20, 24, 27,
   20, 24, 27, 30, 24, 27, 30, 34
};

static const uint8_t Default_8x8_Intra[64] = {
    6, 10, 13, 16, 18, 23, 25, 27,
   10, 11, 16, 18, 23, 25, 27, 29,
   13, 16, 18, 23, 25, 27, 29, 31,
   16, 18, 23, 25, 27, 29, 31, 33,
   18, 23, 25, 27, 29, 31, 33, 36,
   23, 25, 27, 29, 31, 33, 36, 38,
   25, 27, 29, 31, 33, 36, 38, 40,
   27, 29, 31, 33, 36, 38, 40, 42
};

static const uint8_t Default_8x8_Inter[64] = {
    9, 13, 15, 17, 19, 21, 22, 24,
   13, 13, 17, 19, 21, 22, 24, 25,
   15, 17, 19, 21, 22, 24, 25, 27,
   17, 19, 21, 22, 24, 25, 27, 28,
   19, 21, 22, 24, 25, 27, 28, 30,
   21, 22, 24, 25, 27, 28, 30, 32,
   22, 24, 25, 27, 28, 30, 32, 33,
   24, 25, 27, 28, 30, 32, 33, 35
};

#define PTR_TO_UINT(x) ((unsigned)((intptr_t)(x)))

static unsigned handle_hash(void *key)
{
   return PTR_TO_UINT(key);
}

static int handle_compare(void *key1, void *key2)
{
   return PTR_TO_UINT(key1) != PTR_TO_UINT(key2);
}

static enum pipe_error hash_table_clear_item_callback(void *key, void *value, void *data)
{
   struct pipe_video_buffer *video_buffer = (struct pipe_video_buffer *)value;
   video_buffer->destroy(video_buffer);
   return PIPE_OK;
}

static void h264d_free_input_port_private(OMX_BUFFERHEADERTYPE *buf)
{
   struct pipe_video_buffer *vbuf = buf->pInputPortPrivate;
   if (!vbuf)
      return;

   vbuf->destroy(vbuf);
   buf->pInputPortPrivate = NULL;
}

static void release_input_headers(h264d_prc_t * p_prc) {
   int i;
   for (i = 0; i < p_prc->num_in_buffers; i++) {
      assert(!p_prc->in_port_disabled_);
      if (p_prc->in_buffers[i]->pInputPortPrivate) {
         h264d_free_input_port_private(p_prc->in_buffers[i]);
      }
      (void) tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)),
                                     OMX_VID_DEC_AVC_INPUT_PORT_INDEX,
                                     p_prc->in_buffers[i]);
      p_prc->in_buffers[i] = NULL;
   }
   p_prc->p_inhdr_ = NULL;
   p_prc->num_in_buffers = 0;
}

static void release_output_header(h264d_prc_t * p_prc) {
   if (p_prc->p_outhdr_) {
      assert(!p_prc->out_port_disabled_);
      (void) tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)),
                                     OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX,
                                     p_prc->p_outhdr_);
      p_prc->p_outhdr_ = NULL;
   }
}

static OMX_ERRORTYPE h264d_release_all_headers(h264d_prc_t * p_prc)
{
   assert(p_prc);
   release_input_headers(p_prc);
   release_output_header(p_prc);

   return OMX_ErrorNone;
}

static void h264d_buffer_emptied(h264d_prc_t * p_prc, OMX_BUFFERHEADERTYPE * p_hdr)
{
   assert(p_prc);
   assert(p_prc->in_buffers[0] == p_hdr);

   if (!p_prc->out_port_disabled_) {
      assert (p_hdr->nFilledLen == 0);
      p_hdr->nOffset = 0;

      if ((p_hdr->nFlags & OMX_BUFFERFLAG_EOS) != 0) {
         p_prc->eos_ = true;
      }

      (void) tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)), 0, p_hdr);
      p_prc->p_inhdr_ = NULL;
      p_prc->in_buffers[0] = NULL;
   }
}

static void h264d_buffer_filled(h264d_prc_t * p_prc, OMX_BUFFERHEADERTYPE * p_hdr)
{
   assert(p_prc);
   assert(p_hdr);
   assert(p_prc->p_outhdr_ == p_hdr);

   if (!p_prc->in_port_disabled_) {
      p_hdr->nOffset = 0;

      if (p_prc->eos_) {
         /* EOS has been received and all the input data has been consumed
          * already, so its time to propagate the EOS flag */
         p_prc->p_outhdr_->nFlags |= OMX_BUFFERFLAG_EOS;
         p_prc->eos_ = false;
      }

      (void) tiz_krn_release_buffer(tiz_get_krn (handleOf (p_prc)),
                                    OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX,
                                    p_hdr);
      p_prc->p_outhdr_ = NULL;
   }
}

static bool h264d_shift_buffers_left(h264d_prc_t * p_prc) {
   if (--p_prc->num_in_buffers) {
      p_prc->in_buffers[0] = p_prc->in_buffers[1];
      p_prc->sizes[0] = p_prc->sizes[1] - dec_frame_delta;
      p_prc->inputs[0] = p_prc->inputs[1] + dec_frame_delta;
      p_prc->timestamps[0] = p_prc->timestamps[1];

      return true;
   }
   return false;
}

static OMX_BUFFERHEADERTYPE * get_input_buffer(h264d_prc_t * p_prc) {
   assert(p_prc);

   if (p_prc->in_port_disabled_) {
      return NULL;
   }

   if (p_prc->num_in_buffers > 1) {
      /* The input buffer wasn't cleared last time. */
      h264d_buffer_emptied(p_prc, p_prc->in_buffers[0]);
      if (p_prc->in_buffers[0]) {
         /* Failed to release buffer */
         return NULL;
      }
      h264d_shift_buffers_left(p_prc);
   }

   /* Decode_frame expects new buffers each time */
   bool can_get_new_header = (!p_prc->p_inhdr_) || p_prc->first_buf_in_frame;
   assert(can_get_new_header);
   tiz_krn_claim_buffer(tiz_get_krn (handleOf (p_prc)),
                        OMX_VID_DEC_AVC_INPUT_PORT_INDEX, 0,
                        &p_prc->p_inhdr_);
   return p_prc->p_inhdr_;
}

static struct pipe_resource * st_omx_pipe_texture_from_eglimage(EGLDisplay egldisplay,
                                                                EGLImage eglimage)
{
   _EGLDisplay *disp = egldisplay;
   struct dri2_egl_display *dri2_egl_dpy = disp->DriverData;
   __DRIscreen *_dri_screen = dri2_egl_dpy->dri_screen;
   struct dri_screen *st_dri_screen = dri_screen(_dri_screen);
   __DRIimage *_dri_image = st_dri_screen->lookup_egl_image(st_dri_screen, eglimage);

   return _dri_image->texture;
}

static void get_eglimage(h264d_prc_t * p_prc) {
   OMX_PTR p_eglimage = NULL;
   OMX_NATIVE_WINDOWTYPE * p_egldisplay = NULL;
   const tiz_port_t * p_port = NULL;
   struct pipe_video_buffer templat = {};
   struct pipe_video_buffer *video_buffer = NULL;
   struct pipe_resource * p_res = NULL;
   struct pipe_resource *resources[VL_NUM_COMPONENTS];

   if (OMX_ErrorNone ==
      tiz_krn_claim_eglimage(tiz_get_krn (handleOf (p_prc)),
                             OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX,
                             p_prc->p_outhdr_, &p_eglimage)) {
      p_prc->use_eglimage = true;
      p_port = tiz_krn_get_port(tiz_get_krn (handleOf (p_prc)),
                                OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX);
      p_egldisplay = p_port->portdef_.format.video.pNativeWindow;

      if (!util_hash_table_get(p_prc->video_buffer_map, p_prc->p_outhdr_)) {
        p_res = st_omx_pipe_texture_from_eglimage(p_egldisplay, p_eglimage);

        assert(p_res);

        memset(&templat, 0, sizeof(templat));
        templat.buffer_format = p_res->format;
        templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_NONE;
        templat.width = p_res->width0;
        templat.height = p_res->height0;
        templat.interlaced = 0;

        memset(resources, 0, sizeof(resources));
        pipe_resource_reference(&resources[0], p_res);

        video_buffer = vl_video_buffer_create_ex2(p_prc->pipe, &templat, resources);

        assert(video_buffer);
        assert(video_buffer->buffer_format == p_res->format);

        util_hash_table_set(p_prc->video_buffer_map, p_prc->p_outhdr_, video_buffer);
      }
   } else {
      (void) tiz_krn_release_buffer(tiz_get_krn (handleOf (p_prc)),
                                    OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX,
                                    p_prc->p_outhdr_);
      p_prc->p_outhdr_ = NULL;
   }
}

static OMX_BUFFERHEADERTYPE * get_output_buffer(h264d_prc_t * p_prc) {
   assert (p_prc);

   if (p_prc->out_port_disabled_) {
      return NULL;
   }

   if (!p_prc->p_outhdr_) {
      if (OMX_ErrorNone
          == tiz_krn_claim_buffer(tiz_get_krn (handleOf (p_prc)),
                                  OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX, 0,
                                  &p_prc->p_outhdr_)) {
         if (p_prc->p_outhdr_) {
            /* Check pBuffer nullity to know if an eglimage has been registered. */
            if (!p_prc->p_outhdr_->pBuffer) {
               get_eglimage(p_prc);
            }
         }
      }
   }
   return p_prc->p_outhdr_;
}

static struct pipe_video_buffer *h264d_flush(h264d_prc_t *p_prc, OMX_TICKS *timestamp)
{
   struct dpb_list *entry, *result = NULL;
   struct pipe_video_buffer *buf;

   /* search for the lowest poc and break on zeros */
   LIST_FOR_EACH_ENTRY(entry, &p_prc->codec_data.dpb_list, list) {

      if (result && entry->poc == 0)
         break;

      if (!result || entry->poc < result->poc)
         result = entry;
   }

   if (!result)
      return NULL;

   buf = result->buffer;
   if (timestamp)
      *timestamp = result->timestamp;

   --p_prc->codec_data.dpb_num;
   LIST_DEL(&result->list);
   FREE(result);

   return buf;
}

static void h264d_end_frame(h264d_prc_t *p_prc)
{
   struct dpb_list *entry;
   struct pipe_video_buffer *tmp;
   bool top_field_first;
   OMX_TICKS timestamp;

   if (!p_prc->frame_started)
      return;

   p_prc->codec->end_frame(p_prc->codec, p_prc->target, &p_prc->picture.base);
   p_prc->frame_started = false;

   // TODO: implement frame number handling
   p_prc->picture.h264.frame_num_list[0] = p_prc->picture.h264.frame_num;
   p_prc->picture.h264.field_order_cnt_list[0][0] = p_prc->picture.h264.frame_num;
   p_prc->picture.h264.field_order_cnt_list[0][1] = p_prc->picture.h264.frame_num;

   top_field_first = p_prc->picture.h264.field_order_cnt[0] <  p_prc->picture.h264.field_order_cnt[1];

   if (p_prc->picture.h264.field_pic_flag && p_prc->picture.h264.bottom_field_flag != top_field_first)
      return;

   /* add the decoded picture to the dpb list */
   entry = CALLOC_STRUCT(dpb_list);
   if (!entry)
      return;

   p_prc->first_buf_in_frame = true;
   entry->buffer = p_prc->target;
   entry->timestamp = p_prc->timestamp;
   entry->poc = MIN2(p_prc->picture.h264.field_order_cnt[0], p_prc->picture.h264.field_order_cnt[1]);
   LIST_ADDTAIL(&entry->list, &p_prc->codec_data.dpb_list);
   ++p_prc->codec_data.dpb_num;
   p_prc->target = NULL;
   p_prc->picture.h264.field_order_cnt[0] = p_prc->picture.h264.field_order_cnt[1] = INT_MAX;

   if (p_prc->codec_data.dpb_num <= DPB_MAX_SIZE)
      return;

   tmp = p_prc->in_buffers[0]->pInputPortPrivate;
   p_prc->in_buffers[0]->pInputPortPrivate = h264d_flush(p_prc, &timestamp);
   p_prc->in_buffers[0]->nTimeStamp = timestamp;
   p_prc->target = tmp;
   p_prc->frame_finished = p_prc->in_buffers[0]->pInputPortPrivate != NULL;
}

static void scaling_list(struct vl_rbsp *rbsp, uint8_t *scalingList, unsigned sizeOfScalingList,
                         const uint8_t *defaultList, const uint8_t *fallbackList)
{
   unsigned lastScale = 8, nextScale = 8;
   const int *list;
   unsigned i;

   /* (pic|seq)_scaling_list_present_flag[i] */
   if (!vl_rbsp_u(rbsp, 1)) {
      if (fallbackList)
         memcpy(scalingList, fallbackList, sizeOfScalingList);
      return;
   }

   list = (sizeOfScalingList == 16) ? vl_zscan_normal_16 : vl_zscan_normal;
   for (i = 0; i < sizeOfScalingList; ++i ) {

      if (nextScale != 0) {
         signed delta_scale = vl_rbsp_se(rbsp);
         nextScale = (lastScale + delta_scale + 256) % 256;
         if (i == 0 && nextScale == 0) {
            memcpy(scalingList, defaultList, sizeOfScalingList);
            return;
         }
      }
      scalingList[list[i]] = nextScale == 0 ? lastScale : nextScale;
      lastScale = scalingList[list[i]];
   }
}

static void vui_parameters(struct vl_rbsp *rbsp)
{
    // TODO
}

static struct pipe_h264_pps *pic_parameter_set_id(h264d_prc_t *p_prc, struct vl_rbsp *rbsp)
{
   unsigned id = vl_rbsp_ue(rbsp);
   if (id >= ARRAY_SIZE(p_prc->codec_data.pps))
      return NULL; /* invalid pic_parameter_set_id */

   return &p_prc->codec_data.pps[id];
}

static struct pipe_h264_sps *seq_parameter_set_id(h264d_prc_t *p_prc, struct vl_rbsp *rbsp)
{
   unsigned id = vl_rbsp_ue(rbsp);
   if (id >= ARRAY_SIZE(p_prc->codec_data.sps))
      return NULL; /* invalid seq_parameter_set_id */

   return &p_prc->codec_data.sps[id];
}

static void seq_parameter_set(h264d_prc_t *p_prc, struct vl_rbsp *rbsp)
{
   struct pipe_h264_sps *sps;
   unsigned profile_idc, level_idc;
   unsigned i;

   /* Sequence parameter set */
   profile_idc = vl_rbsp_u(rbsp, 8);

   /* constraint_set0_flag */
   vl_rbsp_u(rbsp, 1);

   /* constraint_set1_flag */
   vl_rbsp_u(rbsp, 1);

   /* constraint_set2_flag */
   vl_rbsp_u(rbsp, 1);

   /* constraint_set3_flag */
   vl_rbsp_u(rbsp, 1);

   /* constraint_set4_flag */
   vl_rbsp_u(rbsp, 1);

   /* constraint_set5_flag */
   vl_rbsp_u(rbsp, 1);

   /* reserved_zero_2bits */
   vl_rbsp_u(rbsp, 2);

   /* level_idc */
   level_idc = vl_rbsp_u(rbsp, 8);

   sps = seq_parameter_set_id(p_prc, rbsp);
   if (!sps)
      return;

   memset(sps, 0, sizeof(*sps));
   memset(sps->ScalingList4x4, 16, sizeof(sps->ScalingList4x4));
   memset(sps->ScalingList8x8, 16, sizeof(sps->ScalingList8x8));

   sps->level_idc = level_idc;

   if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
       profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
       profile_idc == 128 || profile_idc == 138) {

      sps->chroma_format_idc = vl_rbsp_ue(rbsp);

      if (sps->chroma_format_idc == 3)
         sps->separate_colour_plane_flag = vl_rbsp_u(rbsp, 1);

      sps->bit_depth_luma_minus8 = vl_rbsp_ue(rbsp);

      sps->bit_depth_chroma_minus8 = vl_rbsp_ue(rbsp);
      /* qpprime_y_zero_transform_bypass_flag */
      vl_rbsp_u(rbsp, 1);

      sps->seq_scaling_matrix_present_flag = vl_rbsp_u(rbsp, 1);
      if (sps->seq_scaling_matrix_present_flag) {

         scaling_list (rbsp, sps->ScalingList4x4[0], 16, Default_4x4_Intra, Default_4x4_Intra);
         scaling_list (rbsp, sps->ScalingList4x4[1], 16, Default_4x4_Intra, sps->ScalingList4x4[0]);
         scaling_list (rbsp, sps->ScalingList4x4[2], 16, Default_4x4_Intra, sps->ScalingList4x4[1]);
         scaling_list (rbsp, sps->ScalingList4x4[3], 16, Default_4x4_Inter, Default_4x4_Inter);
         scaling_list (rbsp, sps->ScalingList4x4[4], 16, Default_4x4_Inter, sps->ScalingList4x4[3]);
         scaling_list (rbsp, sps->ScalingList4x4[5], 16, Default_4x4_Inter, sps->ScalingList4x4[4]);

         scaling_list (rbsp, sps->ScalingList8x8[0], 64, Default_8x8_Intra, Default_8x8_Intra);
         scaling_list (rbsp, sps->ScalingList8x8[1], 64, Default_8x8_Inter, Default_8x8_Inter);
         if (sps->chroma_format_idc == 3) {
            scaling_list (rbsp, sps->ScalingList8x8[2], 64, Default_8x8_Intra, sps->ScalingList8x8[0]);
            scaling_list (rbsp, sps->ScalingList8x8[3], 64, Default_8x8_Inter, sps->ScalingList8x8[1]);
            scaling_list (rbsp, sps->ScalingList8x8[4], 64, Default_8x8_Intra, sps->ScalingList8x8[2]);
            scaling_list (rbsp, sps->ScalingList8x8[5], 64, Default_8x8_Inter, sps->ScalingList8x8[3]);
         }
      }
   } else if (profile_idc == 183)
      sps->chroma_format_idc = 0;
   else
      sps->chroma_format_idc = 1;

   sps->log2_max_frame_num_minus4 = vl_rbsp_ue(rbsp);

   sps->pic_order_cnt_type = vl_rbsp_ue(rbsp);

   if (sps->pic_order_cnt_type == 0)
      sps->log2_max_pic_order_cnt_lsb_minus4 = vl_rbsp_ue(rbsp);
   else if (sps->pic_order_cnt_type == 1) {
      sps->delta_pic_order_always_zero_flag = vl_rbsp_u(rbsp, 1);

      sps->offset_for_non_ref_pic = vl_rbsp_se(rbsp);

      sps->offset_for_top_to_bottom_field = vl_rbsp_se(rbsp);

      sps->num_ref_frames_in_pic_order_cnt_cycle = vl_rbsp_ue(rbsp);

      for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; ++i)
         sps->offset_for_ref_frame[i] = vl_rbsp_se(rbsp);
   }

   sps->max_num_ref_frames = vl_rbsp_ue(rbsp);

   /* gaps_in_frame_num_value_allowed_flag */
   vl_rbsp_u(rbsp, 1);

   /* pic_width_in_mbs_minus1 */
   int pic_width_in_samplesl = (vl_rbsp_ue(rbsp) + 1) * 16;
   p_prc->stream_info.width = pic_width_in_samplesl;

   /* pic_height_in_map_units_minus1 */
   int pic_height_in_map_units = vl_rbsp_ue(rbsp) + 1;

   sps->frame_mbs_only_flag = vl_rbsp_u(rbsp, 1);
   if (!sps->frame_mbs_only_flag)
      sps->mb_adaptive_frame_field_flag = vl_rbsp_u(rbsp, 1);

   int frame_height_in_mbs = (2 - sps->frame_mbs_only_flag) * pic_height_in_map_units;
   int pic_height_in_mbs = frame_height_in_mbs / ( 1 + p_prc->picture.h264.field_pic_flag );
   int pic_height_in_samplesl = pic_height_in_mbs * 16;
   p_prc->stream_info.height = pic_height_in_samplesl;

   sps->direct_8x8_inference_flag = vl_rbsp_u(rbsp, 1);

   /* frame_cropping_flag */
   if (vl_rbsp_u(rbsp, 1)) {
      unsigned frame_crop_left_offset = vl_rbsp_ue(rbsp);
      unsigned frame_crop_right_offset = vl_rbsp_ue(rbsp);
      unsigned frame_crop_top_offset = vl_rbsp_ue(rbsp);
      unsigned frame_crop_bottom_offset = vl_rbsp_ue(rbsp);

      p_prc->stream_info.width -= (frame_crop_left_offset + frame_crop_right_offset) * 2;
      p_prc->stream_info.height -= (frame_crop_top_offset + frame_crop_bottom_offset) * 2;
   }

   /* vui_parameters_present_flag */
   if (vl_rbsp_u(rbsp, 1))
      vui_parameters(rbsp);
}

static OMX_ERRORTYPE update_port_parameters(h264d_prc_t * p_prc) {
   OMX_VIDEO_PORTDEFINITIONTYPE * p_def = NULL;   /* Output port info */
   h264d_stream_info_t * i_def = NULL; /* Info read from stream */
   OMX_ERRORTYPE err = OMX_ErrorNone;

   assert(p_prc);

   p_def = &(p_prc->out_port_def_.format.video);
   i_def = &(p_prc->stream_info);

   /* Handle dynamic resolution change */
   if ((p_def->nFrameWidth == i_def->width) && p_def->nFrameHeight == i_def->height)
      return err;

   p_def->nFrameWidth = i_def->width;
   p_def->nFrameHeight = i_def->height;
   p_def->nStride = i_def->width;
   p_def->nSliceHeight = i_def->height;

   err = tiz_krn_SetParameter_internal(tiz_get_krn(handleOf(p_prc)), handleOf(p_prc),
                                       OMX_IndexParamPortDefinition, &(p_prc->out_port_def_));
   if (err == OMX_ErrorNone) {
      tiz_port_t * p_obj = tiz_krn_get_port(tiz_get_krn(handleOf(p_prc)), OMX_VID_DEC_AVC_INPUT_PORT_INDEX);

      /* Set desired buffer size that will be used when allocating input buffers */
      p_obj->portdef_.nBufferSize = p_def->nFrameWidth * p_def->nFrameHeight * 512 / (16*16);

      /* Get a locally copy of port def. Useful for the early return above */
      tiz_check_omx(tiz_api_GetParameter(tiz_get_krn(handleOf(p_prc)), handleOf(p_prc),
                                         OMX_IndexParamPortDefinition, &(p_prc->out_port_def_)));

      tiz_srv_issue_event((OMX_PTR) p_prc, OMX_EventPortSettingsChanged,
                          OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX,
                          OMX_IndexParamPortDefinition,
                          NULL);
   }

   return err;
}

static void picture_parameter_set(h264d_prc_t *p_prc, struct vl_rbsp *rbsp)
{
   struct pipe_h264_sps *sps;
   struct pipe_h264_pps *pps;
   unsigned i;

   pps = pic_parameter_set_id(p_prc, rbsp);
   if (!pps)
      return;

   memset(pps, 0, sizeof(*pps));

   sps = pps->sps = seq_parameter_set_id(p_prc, rbsp);
   if (!sps)
      return;

   memcpy(pps->ScalingList4x4, sps->ScalingList4x4, sizeof(pps->ScalingList4x4));
   memcpy(pps->ScalingList8x8, sps->ScalingList8x8, sizeof(pps->ScalingList8x8));

   pps->entropy_coding_mode_flag = vl_rbsp_u(rbsp, 1);

   pps->bottom_field_pic_order_in_frame_present_flag = vl_rbsp_u(rbsp, 1);

   pps->num_slice_groups_minus1 = vl_rbsp_ue(rbsp);
   if (pps->num_slice_groups_minus1 > 0) {
      pps->slice_group_map_type = vl_rbsp_ue(rbsp);

      if (pps->slice_group_map_type == 0) {

         for (i = 0; i <= pps->num_slice_groups_minus1; ++i)
            /* run_length_minus1[i] */
            vl_rbsp_ue(rbsp);

      } else if (pps->slice_group_map_type == 2) {

         for (i = 0; i <= pps->num_slice_groups_minus1; ++i) {
            /* top_left[i] */
            vl_rbsp_ue(rbsp);

            /* bottom_right[i] */
            vl_rbsp_ue(rbsp);
         }

      } else if (pps->slice_group_map_type >= 3 && pps->slice_group_map_type <= 5) {

         /* slice_group_change_direction_flag */
         vl_rbsp_u(rbsp, 1);

         pps->slice_group_change_rate_minus1 = vl_rbsp_ue(rbsp);

      } else if (pps->slice_group_map_type == 6) {

         unsigned pic_size_in_map_units_minus1;

         pic_size_in_map_units_minus1 = vl_rbsp_ue(rbsp);

         for (i = 0; i <= pic_size_in_map_units_minus1; ++i)
            /* slice_group_id[i] */
            vl_rbsp_u(rbsp, log2(pps->num_slice_groups_minus1 + 1));
      }
   }

   pps->num_ref_idx_l0_default_active_minus1 = vl_rbsp_ue(rbsp);

   pps->num_ref_idx_l1_default_active_minus1 = vl_rbsp_ue(rbsp);

   pps->weighted_pred_flag = vl_rbsp_u(rbsp, 1);

   pps->weighted_bipred_idc = vl_rbsp_u(rbsp, 2);

   pps->pic_init_qp_minus26 = vl_rbsp_se(rbsp);

   /* pic_init_qs_minus26 */
   vl_rbsp_se(rbsp);

   pps->chroma_qp_index_offset = vl_rbsp_se(rbsp);

   pps->deblocking_filter_control_present_flag = vl_rbsp_u(rbsp, 1);

   pps->constrained_intra_pred_flag = vl_rbsp_u(rbsp, 1);

   pps->redundant_pic_cnt_present_flag = vl_rbsp_u(rbsp, 1);

   if (vl_rbsp_more_data(rbsp)) {
      pps->transform_8x8_mode_flag = vl_rbsp_u(rbsp, 1);

      /* pic_scaling_matrix_present_flag */
      if (vl_rbsp_u(rbsp, 1)) {

         scaling_list(rbsp, pps->ScalingList4x4[0], 16, Default_4x4_Intra,
                      sps->seq_scaling_matrix_present_flag ? NULL : Default_4x4_Intra);
         scaling_list(rbsp, pps->ScalingList4x4[1], 16, Default_4x4_Intra, pps->ScalingList4x4[0]);
         scaling_list(rbsp, pps->ScalingList4x4[2], 16, Default_4x4_Intra, pps->ScalingList4x4[1]);
         scaling_list(rbsp, pps->ScalingList4x4[3], 16, Default_4x4_Inter,
                      sps->seq_scaling_matrix_present_flag ? NULL : Default_4x4_Inter);
         scaling_list(rbsp, pps->ScalingList4x4[4], 16, Default_4x4_Inter, pps->ScalingList4x4[3]);
         scaling_list(rbsp, pps->ScalingList4x4[5], 16, Default_4x4_Inter, pps->ScalingList4x4[4]);

         if (pps->transform_8x8_mode_flag) {
            scaling_list(rbsp, pps->ScalingList8x8[0], 64, Default_8x8_Intra,
                         sps->seq_scaling_matrix_present_flag ? NULL : Default_8x8_Intra);
            scaling_list(rbsp, pps->ScalingList8x8[1], 64, Default_8x8_Inter,
                         sps->seq_scaling_matrix_present_flag ? NULL :  Default_8x8_Inter);
            if (sps->chroma_format_idc == 3) {
               scaling_list(rbsp, pps->ScalingList8x8[2], 64, Default_8x8_Intra, pps->ScalingList8x8[0]);
               scaling_list(rbsp, pps->ScalingList8x8[3], 64, Default_8x8_Inter, pps->ScalingList8x8[1]);
               scaling_list(rbsp, pps->ScalingList8x8[4], 64, Default_8x8_Intra, pps->ScalingList8x8[2]);
               scaling_list(rbsp, pps->ScalingList8x8[5], 64, Default_8x8_Inter, pps->ScalingList8x8[3]);
            }
         }
      }

      pps->second_chroma_qp_index_offset = vl_rbsp_se(rbsp);
   }
}

static void ref_pic_list_mvc_modification(h264d_prc_t *p_prc, struct vl_rbsp *rbsp)
{
   // TODO
   assert (0);
}

static void ref_pic_list_modification(h264d_prc_t *p_prc, struct vl_rbsp *rbsp,
                                      enum pipe_h264_slice_type slice_type)
{
   unsigned modification_of_pic_nums_idc;

   if (slice_type != 2 && slice_type != 4) {
      /* ref_pic_list_modification_flag_l0 */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            modification_of_pic_nums_idc = vl_rbsp_ue(rbsp);
            if (modification_of_pic_nums_idc == 0 ||
                modification_of_pic_nums_idc == 1)
               /* abs_diff_pic_num_minus1 */
               vl_rbsp_ue(rbsp);
            else if (modification_of_pic_nums_idc == 2)
               /* long_term_pic_num */
               vl_rbsp_ue(rbsp);
         } while (modification_of_pic_nums_idc != 3);
      }
   }

   if (slice_type == 1) {
      /* ref_pic_list_modification_flag_l1 */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            modification_of_pic_nums_idc = vl_rbsp_ue(rbsp);
            if (modification_of_pic_nums_idc == 0 ||
                modification_of_pic_nums_idc == 1)
               /* abs_diff_pic_num_minus1 */
               vl_rbsp_ue(rbsp);
            else if (modification_of_pic_nums_idc == 2)
               /* long_term_pic_num */
               vl_rbsp_ue(rbsp);
         } while (modification_of_pic_nums_idc != 3);
      }
   }
}

static void pred_weight_table (h264d_prc_t *p_prc, struct vl_rbsp *rbsp,
                              struct pipe_h264_sps *sps, enum pipe_h264_slice_type slice_type)
{
   unsigned ChromaArrayType = sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
   unsigned i, j;

   /* luma_log2_weight_denom */
   vl_rbsp_ue(rbsp);

   if (ChromaArrayType != 0)
      /* chroma_log2_weight_denom */
      vl_rbsp_ue(rbsp);

   for (i = 0; i <= p_prc->picture.h264.num_ref_idx_l0_active_minus1; ++i) {
      /* luma_weight_l0_flag */
      if (vl_rbsp_u(rbsp, 1)) {
         /* luma_weight_l0[i] */
         vl_rbsp_se(rbsp);
         /* luma_offset_l0[i] */
         vl_rbsp_se(rbsp);
      }
      if (ChromaArrayType != 0) {
         /* chroma_weight_l0_flag */
         if (vl_rbsp_u(rbsp, 1)) {
            for (j = 0; j < 2; ++j) {
               /* chroma_weight_l0[i][j] */
               vl_rbsp_se(rbsp);
               /* chroma_offset_l0[i][j] */
               vl_rbsp_se(rbsp);
            }
         }
      }
   }

   if (slice_type == 1) {
      for (i = 0; i <= p_prc->picture.h264.num_ref_idx_l1_active_minus1; ++i) {
         /* luma_weight_l1_flag */
         if (vl_rbsp_u(rbsp, 1)) {
            /* luma_weight_l1[i] */
            vl_rbsp_se(rbsp);
            /* luma_offset_l1[i] */
            vl_rbsp_se(rbsp);
         }
         if (ChromaArrayType != 0) {
            /* chroma_weight_l1_flag */
            if (vl_rbsp_u(rbsp, 1)) {
               for (j = 0; j < 2; ++j) {
                  /* chroma_weight_l1[i][j] */
                  vl_rbsp_se(rbsp);
                  /* chroma_offset_l1[i][j] */
                  vl_rbsp_se(rbsp);
               }
            }
         }
      }
   }
}

static void dec_ref_pic_marking(h264d_prc_t *p_prc, struct vl_rbsp *rbsp,
                                bool IdrPicFlag)
{
   unsigned memory_management_control_operation;

   if (IdrPicFlag) {
      /* no_output_of_prior_pics_flag */
      vl_rbsp_u(rbsp, 1);
      /* long_term_reference_flag */
      vl_rbsp_u(rbsp, 1);
   } else {
      /* adaptive_ref_pic_marking_mode_flag */
      if (vl_rbsp_u(rbsp, 1)) {
         do {
            memory_management_control_operation = vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 1 ||
                memory_management_control_operation == 3)
               /* difference_of_pic_nums_minus1 */
               vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 2)
               /* long_term_pic_num */
               vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 3 ||
                memory_management_control_operation == 6)
               /* long_term_frame_idx */
               vl_rbsp_ue(rbsp);

            if (memory_management_control_operation == 4)
               /* max_long_term_frame_idx_plus1 */
               vl_rbsp_ue(rbsp);
         } while (memory_management_control_operation != 0);
      }
   }
}

static void slice_header(h264d_prc_t *p_prc, struct vl_rbsp *rbsp,
                         unsigned nal_ref_idc, unsigned nal_unit_type)
{
   enum pipe_h264_slice_type slice_type;
   struct pipe_h264_pps *pps;
   struct pipe_h264_sps *sps;
   unsigned frame_num, prevFrameNum;
   bool IdrPicFlag = nal_unit_type == 5;

   if (IdrPicFlag != p_prc->codec_data.IdrPicFlag)
      h264d_end_frame(p_prc);

   p_prc->codec_data.IdrPicFlag = IdrPicFlag;

   /* first_mb_in_slice */
   vl_rbsp_ue(rbsp);

   slice_type = vl_rbsp_ue(rbsp) % 5;

   /* get picture parameter set */
   pps = pic_parameter_set_id(p_prc, rbsp);
   if (!pps)
      return;

   /* get sequence parameter set */
   sps = pps->sps;
   if (!sps)
      return;

   if (pps != p_prc->picture.h264.pps)
      h264d_end_frame(p_prc);

   p_prc->picture.h264.pps = pps;

   if (sps->separate_colour_plane_flag == 1 )
      /* colour_plane_id */
      vl_rbsp_u(rbsp, 2);

   /* frame number handling */
   frame_num = vl_rbsp_u(rbsp, sps->log2_max_frame_num_minus4 + 4);

   if (frame_num != p_prc->picture.h264.frame_num)
      h264d_end_frame(p_prc);

   prevFrameNum = p_prc->picture.h264.frame_num;
   p_prc->picture.h264.frame_num = frame_num;

   p_prc->picture.h264.field_pic_flag = 0;
   p_prc->picture.h264.bottom_field_flag = 0;

   if (!sps->frame_mbs_only_flag) {
      unsigned field_pic_flag = vl_rbsp_u(rbsp, 1);

      if (!field_pic_flag && field_pic_flag != p_prc->picture.h264.field_pic_flag)
         h264d_end_frame(p_prc);

      p_prc->picture.h264.field_pic_flag = field_pic_flag;

      if (p_prc->picture.h264.field_pic_flag) {
         unsigned bottom_field_flag = vl_rbsp_u(rbsp, 1);

         if (bottom_field_flag != p_prc->picture.h264.bottom_field_flag)
            h264d_end_frame(p_prc);

         p_prc->picture.h264.bottom_field_flag = bottom_field_flag;
      }
   }

   if (IdrPicFlag) {
      /* set idr_pic_id */
      unsigned idr_pic_id = vl_rbsp_ue(rbsp);

      if (idr_pic_id != p_prc->codec_data.idr_pic_id)
         h264d_end_frame(p_prc);

      p_prc->codec_data.idr_pic_id = idr_pic_id;
   }

   if (sps->pic_order_cnt_type == 0) {
      /* pic_order_cnt_lsb */
      unsigned log2_max_pic_order_cnt_lsb = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
      unsigned max_pic_order_cnt_lsb = 1 << log2_max_pic_order_cnt_lsb;
      int pic_order_cnt_lsb = vl_rbsp_u(rbsp, log2_max_pic_order_cnt_lsb);
      int pic_order_cnt_msb;

      if (pic_order_cnt_lsb != p_prc->codec_data.pic_order_cnt_lsb)
         h264d_end_frame(p_prc);

      if (IdrPicFlag) {
         p_prc->codec_data.pic_order_cnt_msb = 0;
         p_prc->codec_data.pic_order_cnt_lsb = 0;
      }

      if ((pic_order_cnt_lsb < p_prc->codec_data.pic_order_cnt_lsb) &&
          (p_prc->codec_data.pic_order_cnt_lsb - pic_order_cnt_lsb) >= (max_pic_order_cnt_lsb / 2))
         pic_order_cnt_msb = p_prc->codec_data.pic_order_cnt_msb + max_pic_order_cnt_lsb;

      else if ((pic_order_cnt_lsb > p_prc->codec_data.pic_order_cnt_lsb) &&
          (pic_order_cnt_lsb - p_prc->codec_data.pic_order_cnt_lsb) > (max_pic_order_cnt_lsb / 2))
         pic_order_cnt_msb = p_prc->codec_data.pic_order_cnt_msb - max_pic_order_cnt_lsb;

      else
         pic_order_cnt_msb = p_prc->codec_data.pic_order_cnt_msb;

      p_prc->codec_data.pic_order_cnt_msb = pic_order_cnt_msb;
      p_prc->codec_data.pic_order_cnt_lsb = pic_order_cnt_lsb;

      if (pps->bottom_field_pic_order_in_frame_present_flag && !p_prc->picture.h264.field_pic_flag) {
         /* delta_pic_oreder_cnt_bottom */
         unsigned delta_pic_order_cnt_bottom = vl_rbsp_se(rbsp);

         if (delta_pic_order_cnt_bottom != p_prc->codec_data.delta_pic_order_cnt_bottom)
            h264d_end_frame(p_prc);

         p_prc->codec_data.delta_pic_order_cnt_bottom = delta_pic_order_cnt_bottom;
      }

      if (!p_prc->picture.h264.field_pic_flag) {
         p_prc->picture.h264.field_order_cnt[0] = pic_order_cnt_msb + pic_order_cnt_lsb; /* (8-4) */
         p_prc->picture.h264.field_order_cnt[1] = p_prc->picture.h264.field_order_cnt [0] +
                                        p_prc->codec_data.delta_pic_order_cnt_bottom;
      } else if (!p_prc->picture.h264.bottom_field_flag)
         p_prc->picture.h264.field_order_cnt[0] = pic_order_cnt_msb + pic_order_cnt_lsb;
      else
         p_prc->picture.h264.field_order_cnt[1] = pic_order_cnt_msb + pic_order_cnt_lsb;

   } else if (sps->pic_order_cnt_type == 1) {
      /* delta_pic_order_cnt[0] */
      unsigned MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);
      unsigned FrameNumOffset, absFrameNum, expectedPicOrderCnt;

      if (!sps->delta_pic_order_always_zero_flag) {
         unsigned delta_pic_order_cnt[2];

         delta_pic_order_cnt[0] = vl_rbsp_se(rbsp);

         if (delta_pic_order_cnt[0] != p_prc->codec_data.delta_pic_order_cnt[0])
            h264d_end_frame(p_prc);

         p_prc->codec_data.delta_pic_order_cnt[0] = delta_pic_order_cnt[0];

         if (pps->bottom_field_pic_order_in_frame_present_flag && !p_prc->picture.h264.field_pic_flag) {
            /* delta_pic_order_cnt[1] */
            delta_pic_order_cnt[1] = vl_rbsp_se(rbsp);

            if (delta_pic_order_cnt[1] != p_prc->codec_data.delta_pic_order_cnt[1])
               h264d_end_frame(p_prc);

            p_prc->codec_data.delta_pic_order_cnt[1] = delta_pic_order_cnt[1];
         }
      }

      if (IdrPicFlag)
         FrameNumOffset = 0;
      else if (prevFrameNum > frame_num)
         FrameNumOffset = p_prc->codec_data.prevFrameNumOffset + MaxFrameNum;
      else
         FrameNumOffset = p_prc->codec_data.prevFrameNumOffset;

      p_prc->codec_data.prevFrameNumOffset = FrameNumOffset;

      if (sps->num_ref_frames_in_pic_order_cnt_cycle != 0)
         absFrameNum = FrameNumOffset + frame_num;
      else
         absFrameNum = 0;

      if (nal_ref_idc == 0 && absFrameNum > 0)
         absFrameNum = absFrameNum - 1;

      if (absFrameNum > 0) {
         unsigned picOrderCntCycleCnt = (absFrameNum - 1) / sps->num_ref_frames_in_pic_order_cnt_cycle;
         unsigned frameNumInPicOrderCntCycle = (absFrameNum - 1) % sps->num_ref_frames_in_pic_order_cnt_cycle;
         signed ExpectedDeltaPerPicOrderCntCycle = 0;
         unsigned i;

         for(i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; ++i)
            ExpectedDeltaPerPicOrderCntCycle += sps->offset_for_ref_frame[i];

         expectedPicOrderCnt = picOrderCntCycleCnt * ExpectedDeltaPerPicOrderCntCycle;
         for(i = 0; i <= frameNumInPicOrderCntCycle; ++i)
            expectedPicOrderCnt += sps->offset_for_ref_frame[i];

      } else
         expectedPicOrderCnt = 0;

      if (nal_ref_idc == 0)
         expectedPicOrderCnt += sps->offset_for_non_ref_pic;

      if (!p_prc->picture.h264.field_pic_flag) {
         p_prc->picture.h264.field_order_cnt[0] = expectedPicOrderCnt + p_prc->codec_data.delta_pic_order_cnt[0];
         p_prc->picture.h264.field_order_cnt[1] = p_prc->picture.h264.field_order_cnt[0] +
            sps->offset_for_top_to_bottom_field + p_prc->codec_data.delta_pic_order_cnt[1];

      } else if (!p_prc->picture.h264.bottom_field_flag)
         p_prc->picture.h264.field_order_cnt[0] = expectedPicOrderCnt + p_prc->codec_data.delta_pic_order_cnt[0];
      else
         p_prc->picture.h264.field_order_cnt[1] = expectedPicOrderCnt + sps->offset_for_top_to_bottom_field +
            p_prc->codec_data.delta_pic_order_cnt[0];

   } else if (sps->pic_order_cnt_type == 2) {   /* 8.2.1.3 */
      unsigned MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);
      unsigned FrameNumOffset, tempPicOrderCnt;

      if (IdrPicFlag)
         FrameNumOffset = 0;
      else if (prevFrameNum > frame_num)
         FrameNumOffset = p_prc->codec_data.prevFrameNumOffset + MaxFrameNum;
      else
         FrameNumOffset = p_prc->codec_data.prevFrameNumOffset;

      p_prc->codec_data.prevFrameNumOffset = FrameNumOffset;

      if (IdrPicFlag)
         tempPicOrderCnt = 0;
      else if (nal_ref_idc == 0)
         tempPicOrderCnt = 2 * (FrameNumOffset + frame_num) - 1;
      else
         tempPicOrderCnt = 2 * (FrameNumOffset + frame_num);

      if (!p_prc->picture.h264.field_pic_flag) {
         p_prc->picture.h264.field_order_cnt[0] = tempPicOrderCnt;
         p_prc->picture.h264.field_order_cnt[1] = tempPicOrderCnt;

      } else if (!p_prc->picture.h264.bottom_field_flag) /* negation not in specs */
         p_prc->picture.h264.field_order_cnt[0] = tempPicOrderCnt;
      else
         p_prc->picture.h264.field_order_cnt[1] = tempPicOrderCnt;
   }

   if (pps->redundant_pic_cnt_present_flag)
      /* redundant_pic_cnt */
      vl_rbsp_ue(rbsp);

   if (slice_type == PIPE_H264_SLICE_TYPE_B)
      /* direct_spatial_mv_pred_flag */
      vl_rbsp_u(rbsp, 1);

   p_prc->picture.h264.num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   p_prc->picture.h264.num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;

   if (slice_type == PIPE_H264_SLICE_TYPE_P ||
       slice_type == PIPE_H264_SLICE_TYPE_SP ||
       slice_type == PIPE_H264_SLICE_TYPE_B) {

      /* num_ref_idx_active_override_flag */
      if (vl_rbsp_u (rbsp, 1)) {
         p_prc->picture.h264.num_ref_idx_l0_active_minus1 = vl_rbsp_ue(rbsp);

         if (slice_type == PIPE_H264_SLICE_TYPE_B)
            p_prc->picture.h264.num_ref_idx_l1_active_minus1 = vl_rbsp_ue(rbsp);
      }
   }

   if (nal_unit_type == 20 || nal_unit_type == 21)
      ref_pic_list_mvc_modification(p_prc, rbsp);
   else
      ref_pic_list_modification(p_prc, rbsp, slice_type);

   if ((pps->weighted_pred_flag && (slice_type == PIPE_H264_SLICE_TYPE_P || slice_type == PIPE_H264_SLICE_TYPE_SP)) ||
       (pps->weighted_bipred_idc == 1 && slice_type == PIPE_H264_SLICE_TYPE_B))
      pred_weight_table(p_prc, rbsp, sps, slice_type);

   if (nal_ref_idc != 0)
      dec_ref_pic_marking(p_prc, rbsp, IdrPicFlag);

   if (pps->entropy_coding_mode_flag && slice_type != PIPE_H264_SLICE_TYPE_I && slice_type != PIPE_H264_SLICE_TYPE_SI)
      /* cabac_init_idc */
      vl_rbsp_ue(rbsp);

   /* slice_qp_delta */
   vl_rbsp_se(rbsp);

   if (slice_type == PIPE_H264_SLICE_TYPE_SP || slice_type == PIPE_H264_SLICE_TYPE_SI) {
      if (slice_type == PIPE_H264_SLICE_TYPE_SP)
         /* sp_for_switch_flag */
         vl_rbsp_u(rbsp, 1);

      /*slice_qs_delta */
      vl_rbsp_se(rbsp);
   }

   if (pps->deblocking_filter_control_present_flag) {
      unsigned disable_deblocking_filter_idc = vl_rbsp_ue(rbsp);

      if (disable_deblocking_filter_idc != 1) {
         /* slice_alpha_c0_offset_div2 */
         vl_rbsp_se(rbsp);

         /* slice_beta_offset_div2 */
         vl_rbsp_se(rbsp);
      }
   }

   if (pps->num_slice_groups_minus1 > 0 && pps->slice_group_map_type >= 3 && pps->slice_group_map_type <= 5)
      /* slice_group_change_cycle */
      vl_rbsp_u(rbsp, 2);
}

static void h264d_need_target(h264d_prc_t *p_prc)
{
   struct pipe_video_buffer templat = {};
   struct vl_screen *omx_screen;
   struct pipe_screen *pscreen;

   omx_screen = p_prc->screen;
   assert(omx_screen);

   pscreen = omx_screen->pscreen;
   assert(pscreen);

   if (!p_prc->target) {
      memset(&templat, 0, sizeof(templat));

      templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
      templat.width = p_prc->codec->width;
      templat.height = p_prc->codec->height;
      templat.buffer_format = pscreen->get_video_param(
            pscreen,
            PIPE_VIDEO_PROFILE_UNKNOWN,
            PIPE_VIDEO_ENTRYPOINT_BITSTREAM,
            PIPE_VIDEO_CAP_PREFERED_FORMAT
      );
      templat.interlaced = pscreen->get_video_param(
          pscreen,
          PIPE_VIDEO_PROFILE_UNKNOWN,
          PIPE_VIDEO_ENTRYPOINT_BITSTREAM,
          PIPE_VIDEO_CAP_PREFERS_INTERLACED
      );

      p_prc->target = p_prc->pipe->create_video_buffer(p_prc->pipe, &templat);
   }
}

static void h264d_begin_frame(h264d_prc_t *p_prc)
{
   //TODO: sane buffer handling

   if (p_prc->frame_started)
      return;

   /* Set codec if not already set */
   if (!p_prc->codec) {
      struct pipe_video_codec templat = {};

      templat.profile = p_prc->profile;
      templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
      templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
      templat.max_references = p_prc->picture.h264.num_ref_frames;
      templat.expect_chunked_decode = true;
      templat.width = p_prc->out_port_def_.format.video.nFrameWidth;
      templat.height = p_prc->out_port_def_.format.video.nFrameHeight;
      templat.level = p_prc->picture.h264.pps->sps->level_idc;

      p_prc->codec = p_prc->pipe->create_video_codec(p_prc->pipe, &templat);
   }

   /* Set target screen */
   h264d_need_target(p_prc);

   if (p_prc->first_buf_in_frame)
      p_prc->timestamp = p_prc->timestamps[0];
   p_prc->first_buf_in_frame = false;

   p_prc->picture.h264.num_ref_frames = p_prc->picture.h264.pps->sps->max_num_ref_frames;

   p_prc->picture.h264.slice_count = 0;
   p_prc->codec->begin_frame(p_prc->codec, p_prc->target, &p_prc->picture.base);
   p_prc->frame_started = true;
}

static void decode_buffer(h264d_prc_t *p_prc, struct vl_vlc *vlc, unsigned min_bits_left)
{
   unsigned nal_ref_idc, nal_unit_type;

   if (!vl_vlc_search_byte(vlc, vl_vlc_bits_left(vlc) - min_bits_left, 0x00))
      return;

   if (vl_vlc_peekbits(vlc, 24) != 0x000001) {
      vl_vlc_eatbits(vlc, 8);
      return;
   }

   if (p_prc->slice) {
      unsigned bytes = p_prc->bytes_left - (vl_vlc_bits_left (vlc) / 8);
      ++p_prc->picture.h264.slice_count;
      p_prc->codec->decode_bitstream(p_prc->codec, p_prc->target, &p_prc->picture.base,
                                    1, &p_prc->slice, &bytes);
      p_prc->slice = NULL;
   }

   vl_vlc_eatbits(vlc, 24);

   /* forbidden_zero_bit */
   vl_vlc_eatbits(vlc, 1);

   nal_ref_idc = vl_vlc_get_uimsbf(vlc, 2);

   if (nal_ref_idc != p_prc->codec_data.nal_ref_idc &&
       (nal_ref_idc * p_prc->codec_data.nal_ref_idc) == 0)
      h264d_end_frame(p_prc);

   p_prc->codec_data.nal_ref_idc = nal_ref_idc;

   nal_unit_type = vl_vlc_get_uimsbf(vlc, 5);

   if (nal_unit_type != 1 && nal_unit_type != 5)
      h264d_end_frame(p_prc);

   if (nal_unit_type == 7) {
      struct vl_rbsp rbsp;
      vl_rbsp_init(&rbsp, vlc, ~0);
      seq_parameter_set(p_prc, &rbsp);
      update_port_parameters(p_prc);

   } else if (nal_unit_type == 8) {
      struct vl_rbsp rbsp;
      vl_rbsp_init(&rbsp, vlc, ~0);
      picture_parameter_set(p_prc, &rbsp);

   } else if (nal_unit_type == 1 || nal_unit_type == 5) {
      /* Coded slice of a non-IDR or IDR picture */
      unsigned bits = vl_vlc_valid_bits(vlc);
      unsigned bytes = bits / 8 + 4;
      struct vl_rbsp rbsp;
      uint8_t buf[8];
      const void *ptr = buf;
      unsigned i;

      buf[0] = 0x0;
      buf[1] = 0x0;
      buf[2] = 0x1;
      buf[3] = (nal_ref_idc << 5) | nal_unit_type;
      for (i = 4; i < bytes; ++i)
         buf[i] = vl_vlc_peekbits(vlc, bits) >> ((bytes - i - 1) * 8);

      p_prc->bytes_left = (vl_vlc_bits_left(vlc) - bits) / 8;
      p_prc->slice = vlc->data;

      vl_rbsp_init(&rbsp, vlc, 128);
      slice_header(p_prc, &rbsp, nal_ref_idc, nal_unit_type);

      h264d_begin_frame(p_prc);

      ++p_prc->picture.h264.slice_count;
      p_prc->codec->decode_bitstream(p_prc->codec, p_prc->target, &p_prc->picture.base,
                                    1, &ptr, &bytes);
   }

   /* resync to byte boundary */
   vl_vlc_eatbits(vlc, vl_vlc_valid_bits(vlc) % 8);
}

static void reset_stream_parameters(h264d_prc_t * ap_prc)
{
   assert(ap_prc);
   TIZ_INIT_OMX_PORT_STRUCT(ap_prc->out_port_def_,
                            OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX);

   tiz_api_GetParameter (tiz_get_krn (handleOf (ap_prc)), handleOf (ap_prc),
                          OMX_IndexParamPortDefinition, &(ap_prc->out_port_def_));

   ap_prc->p_inhdr_ = 0;
   ap_prc->num_in_buffers = 0;
   ap_prc->first_buf_in_frame = true;
   ap_prc->eos_ = false;
   ap_prc->frame_finished = false;
   ap_prc->frame_started = false;
   ap_prc->picture.h264.field_order_cnt[0] = ap_prc->picture.h264.field_order_cnt[1] = INT_MAX;
   ap_prc->slice = NULL;
}

static void h264d_deint(h264d_prc_t *p_prc, struct pipe_video_buffer *src_buf,
                        struct pipe_video_buffer *dst_buf)
{
   struct vl_compositor *compositor = &p_prc->compositor;
   struct vl_compositor_state *s = &p_prc->cstate;
   struct pipe_surface **dst_surface;
   struct u_rect dst_rect;

   dst_surface = dst_buf->get_surfaces(dst_buf);
   vl_compositor_clear_layers(s);

   dst_rect.x0 = 0;
   dst_rect.x1 = src_buf->width;
   dst_rect.y0 = 0;
   dst_rect.y1 = src_buf->height;

   vl_compositor_set_yuv_layer(s, compositor, 0, src_buf, NULL, NULL, true);
   vl_compositor_set_layer_dst_area(s, 0, &dst_rect);
   vl_compositor_render(s, compositor, dst_surface[0], NULL, false);

   dst_rect.x1 /= 2;
   dst_rect.y1 /= 2;

   vl_compositor_set_yuv_layer(s, compositor, 0, src_buf, NULL, NULL, false);
   vl_compositor_set_layer_dst_area(s, 0, &dst_rect);
   vl_compositor_render(s, compositor, dst_surface[1], NULL, false);
}

static void h264d_fill_output(h264d_prc_t *p_prc, struct pipe_video_buffer *buf,
                              OMX_BUFFERHEADERTYPE* output)
{
   tiz_port_t *out_port = tiz_krn_get_port(tiz_get_krn(handleOf(p_prc)), OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX);
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &out_port->portdef_.format.video;

   struct pipe_sampler_view **views;
   unsigned i, j;
   unsigned width, height;

   views = buf->get_sampler_view_planes(buf);

   if (!output->pBuffer) {
      struct pipe_video_buffer *dst_buf = NULL;
      struct pipe_surface **dst_surface = NULL;
      struct u_rect src_rect;
      struct u_rect dst_rect;
      struct vl_compositor *compositor = &p_prc->compositor;
      struct vl_compositor_state *s = &p_prc->cstate;
      enum vl_compositor_deinterlace deinterlace = VL_COMPOSITOR_WEAVE;

      dst_buf = util_hash_table_get(p_prc->video_buffer_map, output);
      assert(dst_buf);

      dst_surface = dst_buf->get_surfaces(dst_buf);
      assert(views);

      src_rect.x0 = 0;
      src_rect.y0 = 0;
      src_rect.x1 = def->nFrameWidth;
      src_rect.y1 = def->nFrameHeight;

      dst_rect.x0 = 0;
      dst_rect.y0 = 0;
      dst_rect.x1 = def->nFrameWidth;
      dst_rect.y1 = def->nFrameHeight;

      vl_compositor_clear_layers(s);
      vl_compositor_set_buffer_layer(s, compositor, 0, buf,
              &src_rect, NULL, deinterlace);
      vl_compositor_set_layer_dst_area(s, 0, &dst_rect);
      vl_compositor_render(s, compositor, dst_surface[0], NULL, false);

      p_prc->pipe->flush(p_prc->pipe, NULL, 0);

      return;
   }

   for (i = 0; i < 2 /* NV12 */; i++) {
      if (!views[i]) continue;
      width = def->nFrameWidth;
      height = def->nFrameHeight;
      vl_video_buffer_adjust_size(&width, &height, i, buf->chroma_format, buf->interlaced);
      for (j = 0; j < views[i]->texture->array_size; ++j) {
         struct pipe_box box = {0, 0, j, width, height, 1};
         struct pipe_transfer *transfer;
         uint8_t *map, *dst;
         map = p_prc->pipe->transfer_map(p_prc->pipe, views[i]->texture, 0,
                  PIPE_TRANSFER_READ, &box, &transfer);
         if (!map)
            return;

         dst = ((uint8_t*)output->pBuffer + output->nOffset) + j * def->nStride +
               i * def->nFrameWidth * def->nFrameHeight;
         util_copy_rect(dst,
            views[i]->texture->format,
            def->nStride * views[i]->texture->array_size, 0, 0,
            box.width, box.height, map, transfer->stride, 0, 0);

         pipe_transfer_unmap(p_prc->pipe, transfer);
      }
   }
}

static void h264d_frame_decoded(h264d_prc_t *p_prc, OMX_BUFFERHEADERTYPE* input,
                                OMX_BUFFERHEADERTYPE* output)
{
   OMX_TICKS timestamp;

   if (!input->pInputPortPrivate) {
      input->pInputPortPrivate = h264d_flush(p_prc, &timestamp);
      if (timestamp != OMX_VID_DEC_AVC_TIMESTAMP_INVALID)
         input->nTimeStamp = timestamp;
   }

   if (input->pInputPortPrivate) {
      if (output->pInputPortPrivate && !p_prc->disable_tunnel) {
         struct pipe_video_buffer *tmp, *vbuf, *new_vbuf;

         tmp = output->pOutputPortPrivate;
         vbuf = input->pInputPortPrivate;
         if (vbuf->interlaced) {
            /* re-allocate the progressive buffer */
            tiz_port_t *port;
            struct pipe_video_buffer templat = {};

            port = tiz_krn_get_port(tiz_get_krn(handleOf (p_prc)), OMX_VID_DEC_AVC_INPUT_PORT_INDEX);
            memset(&templat, 0, sizeof(templat));
            templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
            templat.width = port->portdef_.format.video.nFrameWidth;
            templat.height = port->portdef_.format.video.nFrameHeight;
            templat.buffer_format = PIPE_FORMAT_NV12;
            templat.interlaced = false;
            new_vbuf = p_prc->pipe->create_video_buffer(p_prc->pipe, &templat);

            /* convert the interlaced to the progressive */
            h264d_deint(p_prc, input->pInputPortPrivate, new_vbuf);
            p_prc->pipe->flush(p_prc->pipe, NULL, 0);

            /* set the progrssive buffer for next round */
            vbuf->destroy(vbuf);
            input->pInputPortPrivate = new_vbuf;
         }
         output->pOutputPortPrivate = input->pInputPortPrivate;
         input->pInputPortPrivate = tmp;
      } else {
         h264d_fill_output(p_prc, input->pInputPortPrivate, output);
      }
      output->nFilledLen = output->nAllocLen;
      output->nTimeStamp = input->nTimeStamp;
   }

   if (p_prc->eos_ && input->pInputPortPrivate)
      h264d_free_input_port_private(input);
   else
      input->nFilledLen = 0;
}

/* Replacement for bellagio's omx_base_filter_BufferMgmtFunction */
static void h264d_manage_buffers(h264d_prc_t* p_prc) {
   bool next_is_eos = p_prc->num_in_buffers == 2 ? !!(p_prc->in_buffers[1]->nFlags & OMX_BUFFERFLAG_EOS) : false;
   h264d_frame_decoded(p_prc, p_prc->in_buffers[0], p_prc->p_outhdr_);

   p_prc->p_outhdr_->nTimeStamp = p_prc->in_buffers[0]->nTimeStamp;

   /* Realase output buffer if filled or eos
      Keep if two input buffers are being decoded */
   if ((!next_is_eos) && ((p_prc->p_outhdr_->nFilledLen > 0) || p_prc->use_eglimage  || p_prc->eos_)) {
      h264d_buffer_filled(p_prc, p_prc->p_outhdr_);
   }

   /* Release input buffer if possible */
   if (p_prc->in_buffers[0]->nFilledLen == 0) {
      h264d_buffer_emptied(p_prc, p_prc->in_buffers[0]);
   }
}

static OMX_ERRORTYPE decode_frame(h264d_prc_t *p_prc,
                                  OMX_BUFFERHEADERTYPE *in_buf)
{
   unsigned i = p_prc->num_in_buffers++;
   p_prc->in_buffers[i] = in_buf;
   p_prc->sizes[i] = in_buf->nFilledLen;
   p_prc->inputs[i] = in_buf->pBuffer;
   p_prc->timestamps[i] = in_buf->nTimeStamp;

   while (p_prc->num_in_buffers > (!!(in_buf->nFlags & OMX_BUFFERFLAG_EOS) ? 0 : 1)) {
      p_prc->eos_ = !!(p_prc->in_buffers[0]->nFlags & OMX_BUFFERFLAG_EOS);
      unsigned min_bits_left = p_prc->eos_ ? 32 : MAX2(in_buf->nFilledLen * 8, 32);
      struct vl_vlc vlc;

      vl_vlc_init(&vlc, p_prc->num_in_buffers, p_prc->inputs, p_prc->sizes);

      if (p_prc->slice)
         p_prc->bytes_left = vl_vlc_bits_left(&vlc) / 8;

      while (vl_vlc_bits_left (&vlc) > min_bits_left) {
         decode_buffer(p_prc, &vlc, min_bits_left);
         vl_vlc_fillbits(&vlc);
      }

      if (p_prc->slice) {
         unsigned bytes = p_prc->bytes_left - vl_vlc_bits_left(&vlc) / 8;

         p_prc->codec->decode_bitstream(p_prc->codec, p_prc->target, &p_prc->picture.base,
                                 1, &p_prc->slice, &bytes);

         if (p_prc->num_in_buffers)
            p_prc->slice = p_prc->inputs[1];
         else
            p_prc->slice = NULL;
      }

      if (p_prc->eos_ && p_prc->frame_started)
         h264d_end_frame(p_prc);

      if (p_prc->frame_finished) {
         p_prc->frame_finished = false;
         h264d_manage_buffers(p_prc);
      } else if (p_prc->eos_) {
         h264d_free_input_port_private(p_prc->in_buffers[0]);
         h264d_manage_buffers(p_prc);
      } else {
         p_prc->in_buffers[0]->nFilledLen = 0;
         h264d_buffer_emptied(p_prc, p_prc->in_buffers[0]);
      }

      if (p_prc->out_port_disabled_) {
         /* In case out port is disabled, h264d_buffer_emptied will fail to release input port.
          * We need to wait before shifting the buffers in that case and check in
          * get_input_buffer when out port is enabled to release and shift the buffers.
          * Infinite looping occurs if buffer is not released */
         if (p_prc->num_in_buffers == 2) {
            /* Set the delta value for use in get_input_buffer before exiting */
            dec_frame_delta = MIN2((min_bits_left - vl_vlc_bits_left(&vlc)) / 8, p_prc->sizes[1]);
         }
         break;
      }

      h264d_shift_buffers_left(p_prc);
   }

   return OMX_ErrorNone;
}

/*
 * h264dprc
 */

static void * h264d_prc_ctor(void *ap_obj, va_list * app)
{
   h264d_prc_t *p_prc = super_ctor(typeOf (ap_obj, "h264dprc"), ap_obj, app);
   assert(p_prc);
   p_prc->p_inhdr_ = 0;
   p_prc->p_outhdr_ = 0;
   p_prc->first_buf_in_frame = true;
   p_prc->eos_ = false;
   p_prc->in_port_disabled_   = false;
   p_prc->out_port_disabled_   = false;
   p_prc->picture.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
   p_prc->profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
   reset_stream_parameters(p_prc);

   return p_prc;
}

static void * h264d_prc_dtor(void *ap_obj)
{
   return super_dtor(typeOf(ap_obj, "h264dprc"), ap_obj);
}

static OMX_ERRORTYPE h264d_prc_allocate_resources(void *ap_obj, OMX_U32 a_pid)
{
   h264d_prc_t *p_prc = ap_obj;
   struct pipe_screen *screen;
   vl_csc_matrix csc;

   assert (p_prc);

   p_prc->screen = omx_get_screen();
   if (!p_prc->screen)
      return OMX_ErrorInsufficientResources;

   screen = p_prc->screen->pscreen;
   p_prc->pipe = screen->context_create(screen, p_prc->screen, 0);
   if (!p_prc->pipe)
      return OMX_ErrorInsufficientResources;

   if (!vl_compositor_init(&p_prc->compositor, p_prc->pipe)) {
      p_prc->pipe->destroy(p_prc->pipe);
      p_prc->pipe = NULL;
      return OMX_ErrorInsufficientResources;
   }

   if (!vl_compositor_init_state(&p_prc->cstate, p_prc->pipe)) {
      vl_compositor_cleanup(&p_prc->compositor);
      p_prc->pipe->destroy(p_prc->pipe);
      p_prc->pipe = NULL;
      return OMX_ErrorInsufficientResources;
   }

   vl_csc_get_matrix(VL_CSC_COLOR_STANDARD_BT_601, NULL, true, &csc);
   if (!vl_compositor_set_csc_matrix(&p_prc->cstate, (const vl_csc_matrix *)&csc, 1.0f, 0.0f)) {
      vl_compositor_cleanup(&p_prc->compositor);
      p_prc->pipe->destroy(p_prc->pipe);
      p_prc->pipe = NULL;
      return OMX_ErrorInsufficientResources;
   }

   LIST_INITHEAD(&p_prc->codec_data.dpb_list);

   p_prc->video_buffer_map = util_hash_table_create(handle_hash, handle_compare);

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_deallocate_resources(void *ap_obj)
{
   h264d_prc_t *p_prc = ap_obj;
   assert(p_prc);

   /* Clear hash table */
   util_hash_table_foreach(p_prc->video_buffer_map,
                            &hash_table_clear_item_callback,
                            NULL);
   util_hash_table_destroy(p_prc->video_buffer_map);

   if (p_prc->pipe) {
      vl_compositor_cleanup_state(&p_prc->cstate);
      vl_compositor_cleanup(&p_prc->compositor);
      p_prc->pipe->destroy(p_prc->pipe);
   }

   if (p_prc->screen)
      omx_put_screen();

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_prepare_to_transfer(void *ap_obj, OMX_U32 a_pid)
{
   h264d_prc_t *p_prc = ap_obj;
   assert(p_prc);

   TIZ_INIT_OMX_PORT_STRUCT(p_prc->out_port_def_,
                            OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX);
   tiz_check_omx(
      tiz_api_GetParameter(tiz_get_krn(handleOf(p_prc)), handleOf(p_prc),
                           OMX_IndexParamPortDefinition, &(p_prc->out_port_def_)));

   p_prc->first_buf_in_frame = true;
   p_prc->eos_ = false;
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_transfer_and_process(void *ap_obj, OMX_U32 a_pid)
{
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_stop_and_return(void *ap_obj)
{
   h264d_prc_t *p_prc = (h264d_prc_t *) ap_obj;
   return h264d_release_all_headers (p_prc);
}

static OMX_ERRORTYPE h264d_prc_buffers_ready(const void *ap_obj)
{
   h264d_prc_t *p_prc = (h264d_prc_t *) ap_obj;
   OMX_BUFFERHEADERTYPE *in_buf = NULL;
   OMX_BUFFERHEADERTYPE *out_buf = NULL;

   assert(p_prc);

   /* Set parameters if start of stream */
   if (!p_prc->eos_ && p_prc->first_buf_in_frame && (in_buf = get_input_buffer(p_prc))) {
      decode_frame(p_prc, in_buf);
   }

   /* Don't get input buffer if output buffer not found */
   while (!p_prc->eos_ && (out_buf = get_output_buffer(p_prc)) && (in_buf = get_input_buffer(p_prc))) {
      if (!p_prc->out_port_disabled_) {
         decode_frame(p_prc, in_buf);
      }
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_port_flush(const void *ap_obj, OMX_U32 a_pid)
{
   h264d_prc_t *p_prc = (h264d_prc_t *) ap_obj;
   if (OMX_ALL == a_pid || OMX_VID_DEC_AVC_INPUT_PORT_INDEX == a_pid) {
      release_input_headers(p_prc);
      reset_stream_parameters(p_prc);
   }
   if (OMX_ALL == a_pid || OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX == a_pid) {
      release_output_header(p_prc);
   }
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_port_disable(const void *ap_obj, OMX_U32 a_pid)
{
   h264d_prc_t *p_prc = (h264d_prc_t *) ap_obj;
   assert(p_prc);
   if (OMX_ALL == a_pid || OMX_VID_DEC_AVC_INPUT_PORT_INDEX == a_pid) {
      /* Release all buffers */
      h264d_release_all_headers(p_prc);
      reset_stream_parameters(p_prc);
      p_prc->in_port_disabled_ = true;
   }
   if (OMX_ALL == a_pid || OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX == a_pid) {
      release_output_header(p_prc);
      p_prc->out_port_disabled_ = true;
   }
   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264d_prc_port_enable(const void *ap_obj, OMX_U32 a_pid)
{
   h264d_prc_t * p_prc = (h264d_prc_t *) ap_obj;
   assert(p_prc);
   if (OMX_ALL == a_pid || OMX_VID_DEC_AVC_INPUT_PORT_INDEX == a_pid) {
      if (p_prc->in_port_disabled_) {
         reset_stream_parameters(p_prc);
         p_prc->in_port_disabled_ = false;
      }
   }
   if (OMX_ALL == a_pid || OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX == a_pid) {
      p_prc->out_port_disabled_ = false;
   }
   return OMX_ErrorNone;
}

/*
 * h264d_prc_class
 */

static void * h264d_prc_class_ctor(void *ap_obj, va_list * app)
{
   /* NOTE: Class methods might be added in the future. None for now. */
   return super_ctor(typeOf(ap_obj, "h264dprc_class"), ap_obj, app);
}

/*
 * initialization
 */

void * h264d_prc_class_init(void * ap_tos, void * ap_hdl)
{
   void * tizprc = tiz_get_type(ap_hdl, "tizprc");
   void * h264dprc_class = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (classOf(tizprc), "h264dprc_class", classOf(tizprc),
       sizeof(h264d_prc_class_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, h264d_prc_class_ctor,
       /* TIZ_CLASS_COMMENT: stop value*/
       0);
   return h264dprc_class;
}

void * h264d_prc_init(void * ap_tos, void * ap_hdl)
{
   void * tizprc = tiz_get_type(ap_hdl, "tizprc");
   void * h264dprc_class = tiz_get_type(ap_hdl, "h264dprc_class");
   TIZ_LOG_CLASS (h264dprc_class);
   void * h264dprc = factory_new
     /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
     (h264dprc_class, "h264dprc", tizprc, sizeof(h264d_prc_t),
      /* TIZ_CLASS_COMMENT: */
      ap_tos, ap_hdl,
      /* TIZ_CLASS_COMMENT: class constructor */
      ctor, h264d_prc_ctor,
      /* TIZ_CLASS_COMMENT: class destructor */
      dtor, h264d_prc_dtor,
      /* TIZ_CLASS_COMMENT: */
      tiz_srv_allocate_resources, h264d_prc_allocate_resources,
      /* TIZ_CLASS_COMMENT: */
      tiz_srv_deallocate_resources, h264d_prc_deallocate_resources,
      /* TIZ_CLASS_COMMENT: */
      tiz_srv_prepare_to_transfer, h264d_prc_prepare_to_transfer,
      /* TIZ_CLASS_COMMENT: */
      tiz_srv_transfer_and_process, h264d_prc_transfer_and_process,
      /* TIZ_CLASS_COMMENT: */
      tiz_srv_stop_and_return, h264d_prc_stop_and_return,
      /* TIZ_CLASS_COMMENT: */
      tiz_prc_buffers_ready, h264d_prc_buffers_ready,
      /* TIZ_CLASS_COMMENT: */
      tiz_prc_port_flush, h264d_prc_port_flush,
      /* TIZ_CLASS_COMMENT: */
      tiz_prc_port_disable, h264d_prc_port_disable,
      /* TIZ_CLASS_COMMENT: */
      tiz_prc_port_enable, h264d_prc_port_enable,
      /* TIZ_CLASS_COMMENT: stop value*/
      0);

   return h264dprc;
}
