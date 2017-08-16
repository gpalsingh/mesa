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

/*
 * Authors:
 *      Christian König <christian.koenig@amd.com>
 *
 */

#include "pipe/p_video_codec.h"
#include "util/u_memory.h"
#include "util/u_video.h"
#include "vl/vl_rbsp.h"
#include "vl/vl_zscan.h"

#include "entrypoint.h"
#include "vid_dec.h"

#define DPB_MAX_SIZE 5

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

static void vid_dec_h264_Decode(vid_dec_PrivateType *priv, struct vl_vlc *vlc, unsigned min_bits_left);
static void vid_dec_h264_EndFrame(vid_dec_PrivateType *priv);
static struct pipe_video_buffer *vid_dec_h264_Flush(vid_dec_PrivateType *priv, OMX_TICKS *timestamp);

void vid_dec_h264_Init(vid_dec_PrivateType *priv)
{
   priv->picture.base.profile = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;

   priv->Decode = vid_dec_h264_Decode;
   priv->EndFrame = vid_dec_h264_EndFrame;
   priv->Flush = vid_dec_h264_Flush;

   LIST_INITHEAD(&priv->codec_data.h264.dpb_list);
   priv->picture.h264.field_order_cnt[0] = priv->picture.h264.field_order_cnt[1] = INT_MAX;
   priv->first_buf_in_frame = true;
}

static void vid_dec_h264_BeginFrame(vid_dec_PrivateType *priv)
{
   //TODO: sane buffer handling

   if (priv->frame_started)
      return;

   if (!priv->codec) {
      struct pipe_video_codec templat = {};
      omx_base_video_PortType *port;

      port = (omx_base_video_PortType *)priv->ports[OMX_BASE_FILTER_INPUTPORT_INDEX];
      templat.profile = priv->profile;
      templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
      templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
      templat.max_references = priv->picture.h264.num_ref_frames;
      templat.expect_chunked_decode = true;
      templat.width = port->sPortParam.format.video.nFrameWidth;
      templat.height = port->sPortParam.format.video.nFrameHeight;
      templat.level = priv->picture.h264.pps->sps->level_idc;

      priv->codec = priv->pipe->create_video_codec(priv->pipe, &templat);
   }

   vid_dec_NeedTarget(priv);

   if (priv->first_buf_in_frame)
      priv->timestamp = priv->timestamps[0];
   priv->first_buf_in_frame = false;

   priv->picture.h264.num_ref_frames = priv->picture.h264.pps->sps->max_num_ref_frames;

   priv->picture.h264.slice_count = 0;
   priv->codec->begin_frame(priv->codec, priv->target, &priv->picture.base);
   priv->frame_started = true;
}

static struct pipe_video_buffer *vid_dec_h264_Flush(vid_dec_PrivateType *priv,
                                                    OMX_TICKS *timestamp)
{
   struct dpb_list *entry, *result = NULL;
   struct pipe_video_buffer *buf;

   /* search for the lowest poc and break on zeros */
   LIST_FOR_EACH_ENTRY(entry, &priv->codec_data.h264.dpb_list, list) {

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

   --priv->codec_data.h264.dpb_num;
   LIST_DEL(&result->list);
   FREE(result);

   return buf;
}

static void vid_dec_h264_EndFrame(vid_dec_PrivateType *priv)
{
   struct dpb_list *entry;
   struct pipe_video_buffer *tmp;
   bool top_field_first;
   OMX_TICKS timestamp;

   if (!priv->frame_started)
      return;

   priv->codec->end_frame(priv->codec, priv->target, &priv->picture.base);
   priv->frame_started = false;

   // TODO: implement frame number handling
   priv->picture.h264.frame_num_list[0] = priv->picture.h264.frame_num;
   priv->picture.h264.field_order_cnt_list[0][0] = priv->picture.h264.frame_num;
   priv->picture.h264.field_order_cnt_list[0][1] = priv->picture.h264.frame_num;

   top_field_first = priv->picture.h264.field_order_cnt[0] <  priv->picture.h264.field_order_cnt[1];

   if (priv->picture.h264.field_pic_flag && priv->picture.h264.bottom_field_flag != top_field_first)
      return;

   /* add the decoded picture to the dpb list */
   entry = CALLOC_STRUCT(dpb_list);
   if (!entry)
      return;

   priv->first_buf_in_frame = true;
   entry->buffer = priv->target;
   entry->timestamp = priv->timestamp;
   entry->poc = MIN2(priv->picture.h264.field_order_cnt[0], priv->picture.h264.field_order_cnt[1]);
   LIST_ADDTAIL(&entry->list, &priv->codec_data.h264.dpb_list);
   ++priv->codec_data.h264.dpb_num;
   priv->target = NULL;
   priv->picture.h264.field_order_cnt[0] = priv->picture.h264.field_order_cnt[1] = INT_MAX;

   if (priv->codec_data.h264.dpb_num <= DPB_MAX_SIZE)
      return;

   tmp = priv->in_buffers[0]->pInputPortPrivate;
   priv->in_buffers[0]->pInputPortPrivate = vid_dec_h264_Flush(priv, &timestamp);
   priv->in_buffers[0]->nTimeStamp = timestamp;
   priv->target = tmp;
   priv->frame_finished = priv->in_buffers[0]->pInputPortPrivate != NULL;
}

static void vui_parameters(struct vl_rbsp *rbsp)
{
   // TODO
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

static struct pipe_h264_sps *seq_parameter_set_id(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp)
{
   unsigned id = vl_rbsp_ue(rbsp);
   if (id >= ARRAY_SIZE(priv->codec_data.h264.sps))
      return NULL; /* invalid seq_parameter_set_id */

   return &priv->codec_data.h264.sps[id];
}

static void seq_parameter_set(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp)
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

   sps = seq_parameter_set_id(priv, rbsp);
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

         scaling_list(rbsp, sps->ScalingList4x4[0], 16, Default_4x4_Intra, Default_4x4_Intra);
         scaling_list(rbsp, sps->ScalingList4x4[1], 16, Default_4x4_Intra, sps->ScalingList4x4[0]);
         scaling_list(rbsp, sps->ScalingList4x4[2], 16, Default_4x4_Intra, sps->ScalingList4x4[1]);
         scaling_list(rbsp, sps->ScalingList4x4[3], 16, Default_4x4_Inter, Default_4x4_Inter);
         scaling_list(rbsp, sps->ScalingList4x4[4], 16, Default_4x4_Inter, sps->ScalingList4x4[3]);
         scaling_list(rbsp, sps->ScalingList4x4[5], 16, Default_4x4_Inter, sps->ScalingList4x4[4]);

         scaling_list(rbsp, sps->ScalingList8x8[0], 64, Default_8x8_Intra, Default_8x8_Intra);
         scaling_list(rbsp, sps->ScalingList8x8[1], 64, Default_8x8_Inter, Default_8x8_Inter);
         if (sps->chroma_format_idc == 3) {
            scaling_list(rbsp, sps->ScalingList8x8[2], 64, Default_8x8_Intra, sps->ScalingList8x8[0]);
            scaling_list(rbsp, sps->ScalingList8x8[3], 64, Default_8x8_Inter, sps->ScalingList8x8[1]);
            scaling_list(rbsp, sps->ScalingList8x8[4], 64, Default_8x8_Intra, sps->ScalingList8x8[2]);
            scaling_list(rbsp, sps->ScalingList8x8[5], 64, Default_8x8_Inter, sps->ScalingList8x8[3]);
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
   vl_rbsp_ue(rbsp);

   /* pic_height_in_map_units_minus1 */
   vl_rbsp_ue(rbsp);

   sps->frame_mbs_only_flag = vl_rbsp_u(rbsp, 1);
   if (!sps->frame_mbs_only_flag)
      sps->mb_adaptive_frame_field_flag = vl_rbsp_u(rbsp, 1);

   sps->direct_8x8_inference_flag = vl_rbsp_u(rbsp, 1);

   /* frame_cropping_flag */
   if (vl_rbsp_u(rbsp, 1)) {
      /* frame_crop_left_offset */
      vl_rbsp_ue(rbsp);

      /* frame_crop_right_offset */
      vl_rbsp_ue(rbsp);

      /* frame_crop_top_offset */
      vl_rbsp_ue(rbsp);

      /* frame_crop_bottom_offset */
      vl_rbsp_ue(rbsp);
   }

   /* vui_parameters_present_flag */
   if (vl_rbsp_u(rbsp, 1))
      vui_parameters(rbsp);
}

static struct pipe_h264_pps *pic_parameter_set_id(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp)
{
   unsigned id = vl_rbsp_ue(rbsp);
   if (id >= ARRAY_SIZE(priv->codec_data.h264.pps))
      return NULL; /* invalid pic_parameter_set_id */

   return &priv->codec_data.h264.pps[id];
}

static void picture_parameter_set(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp)
{
   struct pipe_h264_sps *sps;
   struct pipe_h264_pps *pps;
   unsigned i;

   pps = pic_parameter_set_id(priv, rbsp);
   if (!pps)
      return;

   memset(pps, 0, sizeof(*pps));

   sps = pps->sps = seq_parameter_set_id(priv, rbsp);
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

static void ref_pic_list_mvc_modification(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp)
{
   // TODO
   assert(0);
}

static void ref_pic_list_modification(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp,
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

static void pred_weight_table(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp,
                              struct pipe_h264_sps *sps, enum pipe_h264_slice_type slice_type)
{
   unsigned ChromaArrayType = sps->separate_colour_plane_flag ? 0 : sps->chroma_format_idc;
   unsigned i, j;

   /* luma_log2_weight_denom */
   vl_rbsp_ue(rbsp);

   if (ChromaArrayType != 0)
      /* chroma_log2_weight_denom */
      vl_rbsp_ue(rbsp);

   for (i = 0; i <= priv->picture.h264.num_ref_idx_l0_active_minus1; ++i) {
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
      for (i = 0; i <= priv->picture.h264.num_ref_idx_l1_active_minus1; ++i) {
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

static void dec_ref_pic_marking(vid_dec_PrivateType *priv, struct vl_rbsp *rbsp,
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

static void vid_dec_h264_Decode(vid_dec_PrivateType *priv, struct vl_vlc *vlc, unsigned min_bits_left)
{
   unsigned nal_ref_idc, nal_unit_type;

   if (!vl_vlc_search_byte(vlc, vl_vlc_bits_left(vlc) - min_bits_left, 0x00))
      return;

   if (vl_vlc_peekbits(vlc, 24) != 0x000001) {
      vl_vlc_eatbits(vlc, 8);
      return;
   }

   if (priv->slice) {
      unsigned bytes = priv->bytes_left - (vl_vlc_bits_left(vlc) / 8);
      ++priv->picture.h264.slice_count;
      priv->codec->decode_bitstream(priv->codec, priv->target, &priv->picture.base,
                                    1, &priv->slice, &bytes);
      priv->slice = NULL;
   }

   vl_vlc_eatbits(vlc, 24);

   /* forbidden_zero_bit */
   vl_vlc_eatbits(vlc, 1);

   nal_ref_idc = vl_vlc_get_uimsbf(vlc, 2);

   if (nal_ref_idc != priv->codec_data.h264.nal_ref_idc &&
       (nal_ref_idc * priv->codec_data.h264.nal_ref_idc) == 0)
      vid_dec_h264_EndFrame(priv);

   priv->codec_data.h264.nal_ref_idc = nal_ref_idc;

   nal_unit_type = vl_vlc_get_uimsbf(vlc, 5);

   if (nal_unit_type != 1 && nal_unit_type != 5)
      vid_dec_h264_EndFrame(priv);

   if (nal_unit_type == 7) {
      struct vl_rbsp rbsp;
      vl_rbsp_init(&rbsp, vlc, ~0);
      seq_parameter_set(priv, &rbsp);

   } else if (nal_unit_type == 8) {
      struct vl_rbsp rbsp;
      vl_rbsp_init(&rbsp, vlc, ~0);
      picture_parameter_set(priv, &rbsp);

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

      priv->bytes_left = (vl_vlc_bits_left(vlc) - bits) / 8;
      priv->slice = vlc->data;

      vl_rbsp_init(&rbsp, vlc, 128);
      slice_header(priv, &rbsp, nal_ref_idc, nal_unit_type);

      vid_dec_h264_BeginFrame(priv);

      ++priv->picture.h264.slice_count;
      priv->codec->decode_bitstream(priv->codec, priv->target, &priv->picture.base,
                                    1, &ptr, &bytes);
   }

   /* resync to byte boundary */
   vl_vlc_eatbits(vlc, vl_vlc_valid_bits(vlc) % 8);
}
