/**************************************************************************
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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "os/os_thread.h"
#include "util/u_memory.h"
#include "loader/loader.h"
#include "vl_st_common.h"

#if defined(HAVE_X11_PLATFORM)
#include <X11/Xlib.h>
#else
#define XOpenDisplay(x) NULL
#define XCloseDisplay(x)
#define Display void
#endif

static mtx_t st_lock = _MTX_INITIALIZER_NP;
static Display *st_display = NULL;
static struct vl_screen *st_screen = NULL;
static unsigned st_usecount = 0;
static const char *st_render_node = NULL;
static int drm_fd;

struct vl_screen *get_screen(void)
{
   static bool first_time = true;
   mtx_lock(&st_lock);

   if (!st_screen) {
      if (first_time) {
         st_render_node = debug_get_option("OMX_RENDER_NODE", NULL);
         first_time = false;
      }
      if (st_render_node) {
         drm_fd = loader_open_device(st_render_node);
         if (drm_fd < 0)
            goto error;

         st_screen = vl_drm_screen_create(drm_fd);
         if (!st_screen) {
            close(drm_fd);
            goto error;
         }
      } else {
         st_display = XOpenDisplay(NULL);
         if (!st_display)
            goto error;

         st_screen = vl_dri3_screen_create(st_display, 0);
         if (!st_screen)
            st_screen = vl_dri2_screen_create(st_display, 0);
         if (!st_screen) {
            XCloseDisplay(st_display);
            goto error;
         }
      }
   }

   ++st_usecount;

   mtx_unlock(&st_lock);
   return st_screen;

error:
   mtx_unlock(&st_lock);
   return NULL;
}

void put_screen(void)
{
   mtx_lock(&st_lock);
   if ((--st_usecount) == 0) {
      st_screen->destroy(st_screen);
      st_screen = NULL;

      if (st_render_node)
         close(drm_fd);
      else
         XCloseDisplay(st_display);
   }
   mtx_unlock(&st_lock);
}

void slice_header_h264(void *prc, struct vl_rbsp *rbsp,
                       unsigned nal_ref_idc, unsigned nal_unit_type,
                       enum lib_type prc_type)
{
   enum pipe_h264_slice_type slice_type;
   struct pipe_h264_pps *pps;
   struct pipe_h264_sps *sps;
   unsigned frame_num, prevFrameNum;
   bool IdrPicFlag = nal_unit_type == 5;

   if (prc_type == OMX_MESA_USE_BELLAGIO) {
      vid_dec_PrivateType_ priv = (vid_dec_PrivateType_ *) prc;
   } else {
      h264d_prc_t_ priv = (h264d_prc_t_ *) prc;
   }

   if (IdrPicFlag != priv->codec_data.h264.IdrPicFlag)
      vid_dec_h264_EndFrame(priv);

   priv->codec_data.h264.IdrPicFlag = IdrPicFlag;

   /* first_mb_in_slice */
   vl_rbsp_ue(rbsp);

   slice_type = vl_rbsp_ue(rbsp) % 5;

   pps = pic_parameter_set_id(priv, rbsp);
   if (!pps)
      return;

   sps = pps->sps;
   if (!sps)
      return;

   if (pps != priv->picture.h264.pps)
      vid_dec_h264_EndFrame(priv);

   priv->picture.h264.pps = pps;

   if (sps->separate_colour_plane_flag == 1 )
      /* colour_plane_id */
      vl_rbsp_u(rbsp, 2);

   frame_num = vl_rbsp_u(rbsp, sps->log2_max_frame_num_minus4 + 4);

   if (frame_num != priv->picture.h264.frame_num)
      vid_dec_h264_EndFrame(priv);

   prevFrameNum = priv->picture.h264.frame_num;
   priv->picture.h264.frame_num = frame_num;

   priv->picture.h264.field_pic_flag = 0;
   priv->picture.h264.bottom_field_flag = 0;

   if (!sps->frame_mbs_only_flag) {
      unsigned field_pic_flag = vl_rbsp_u(rbsp, 1);

      if (!field_pic_flag && field_pic_flag != priv->picture.h264.field_pic_flag)
         vid_dec_h264_EndFrame(priv);

      priv->picture.h264.field_pic_flag = field_pic_flag;

      if (priv->picture.h264.field_pic_flag) {
         unsigned bottom_field_flag = vl_rbsp_u(rbsp, 1);

         if (bottom_field_flag != priv->picture.h264.bottom_field_flag)
            vid_dec_h264_EndFrame(priv);

         priv->picture.h264.bottom_field_flag = bottom_field_flag;
      }
   }

   if (IdrPicFlag) {
      unsigned idr_pic_id = vl_rbsp_ue(rbsp);

      if (idr_pic_id != priv->codec_data.h264.idr_pic_id)
         vid_dec_h264_EndFrame(priv);

      priv->codec_data.h264.idr_pic_id = idr_pic_id;
   }

   if (sps->pic_order_cnt_type == 0) {
      unsigned log2_max_pic_order_cnt_lsb = sps->log2_max_pic_order_cnt_lsb_minus4 + 4;
      unsigned max_pic_order_cnt_lsb = 1 << log2_max_pic_order_cnt_lsb;
      int pic_order_cnt_lsb = vl_rbsp_u(rbsp, log2_max_pic_order_cnt_lsb);
      int pic_order_cnt_msb;

      if (pic_order_cnt_lsb != priv->codec_data.h264.pic_order_cnt_lsb)
         vid_dec_h264_EndFrame(priv);

      if (IdrPicFlag) {
         priv->codec_data.h264.pic_order_cnt_msb = 0;
         priv->codec_data.h264.pic_order_cnt_lsb = 0;
      }

      if ((pic_order_cnt_lsb < priv->codec_data.h264.pic_order_cnt_lsb) &&
          (priv->codec_data.h264.pic_order_cnt_lsb - pic_order_cnt_lsb) >= (max_pic_order_cnt_lsb / 2))
         pic_order_cnt_msb = priv->codec_data.h264.pic_order_cnt_msb + max_pic_order_cnt_lsb;

      else if ((pic_order_cnt_lsb > priv->codec_data.h264.pic_order_cnt_lsb) &&
          (pic_order_cnt_lsb - priv->codec_data.h264.pic_order_cnt_lsb) > (max_pic_order_cnt_lsb / 2))
         pic_order_cnt_msb = priv->codec_data.h264.pic_order_cnt_msb - max_pic_order_cnt_lsb;

      else
         pic_order_cnt_msb = priv->codec_data.h264.pic_order_cnt_msb;

      priv->codec_data.h264.pic_order_cnt_msb = pic_order_cnt_msb;
      priv->codec_data.h264.pic_order_cnt_lsb = pic_order_cnt_lsb;

      if (pps->bottom_field_pic_order_in_frame_present_flag && !priv->picture.h264.field_pic_flag) {
         unsigned delta_pic_order_cnt_bottom = vl_rbsp_se(rbsp);

         if (delta_pic_order_cnt_bottom != priv->codec_data.h264.delta_pic_order_cnt_bottom)
            vid_dec_h264_EndFrame(priv);

         priv->codec_data.h264.delta_pic_order_cnt_bottom = delta_pic_order_cnt_bottom;
      }

      if (!priv->picture.h264.field_pic_flag) {
         priv->picture.h264.field_order_cnt[0] = pic_order_cnt_msb + pic_order_cnt_lsb;
         priv->picture.h264.field_order_cnt[1] = priv->picture.h264.field_order_cnt [0] +
                                          priv->codec_data.h264.delta_pic_order_cnt_bottom;
      } else if (!priv->picture.h264.bottom_field_flag)
         priv->picture.h264.field_order_cnt[0] = pic_order_cnt_msb + pic_order_cnt_lsb;
      else
         priv->picture.h264.field_order_cnt[1] = pic_order_cnt_msb + pic_order_cnt_lsb;

   } else if (sps->pic_order_cnt_type == 1) {
      unsigned MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);
      unsigned FrameNumOffset, absFrameNum, expectedPicOrderCnt;

      if (!sps->delta_pic_order_always_zero_flag) {
         unsigned delta_pic_order_cnt[2];

         delta_pic_order_cnt[0] = vl_rbsp_se(rbsp);

         if (delta_pic_order_cnt[0] != priv->codec_data.h264.delta_pic_order_cnt[0])
            vid_dec_h264_EndFrame(priv);

         priv->codec_data.h264.delta_pic_order_cnt[0] = delta_pic_order_cnt[0];

         if (pps->bottom_field_pic_order_in_frame_present_flag && !priv->picture.h264.field_pic_flag) {
            delta_pic_order_cnt[1] = vl_rbsp_se(rbsp);

            if (delta_pic_order_cnt[1] != priv->codec_data.h264.delta_pic_order_cnt[1])
               vid_dec_h264_EndFrame(priv);

            priv->codec_data.h264.delta_pic_order_cnt[1] = delta_pic_order_cnt[1];
         }
      }

      if (IdrPicFlag)
         FrameNumOffset = 0;
      else if (prevFrameNum > frame_num)
         FrameNumOffset = priv->codec_data.h264.prevFrameNumOffset + MaxFrameNum;
      else
         FrameNumOffset = priv->codec_data.h264.prevFrameNumOffset;

      priv->codec_data.h264.prevFrameNumOffset = FrameNumOffset;

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

         for (i = 0; i < sps->num_ref_frames_in_pic_order_cnt_cycle; ++i)
            ExpectedDeltaPerPicOrderCntCycle += sps->offset_for_ref_frame[i];

         expectedPicOrderCnt = picOrderCntCycleCnt * ExpectedDeltaPerPicOrderCntCycle;
         for (i = 0; i <= frameNumInPicOrderCntCycle; ++i)
            expectedPicOrderCnt += sps->offset_for_ref_frame[i];

      } else
         expectedPicOrderCnt = 0;

      if (nal_ref_idc == 0)
         expectedPicOrderCnt += sps->offset_for_non_ref_pic;

      if (!priv->picture.h264.field_pic_flag) {
         priv->picture.h264.field_order_cnt[0] = expectedPicOrderCnt + priv->codec_data.h264.delta_pic_order_cnt[0];
         priv->picture.h264.field_order_cnt[1] = priv->picture.h264.field_order_cnt[0] +
            sps->offset_for_top_to_bottom_field + priv->codec_data.h264.delta_pic_order_cnt[1];

      } else if (!priv->picture.h264.bottom_field_flag)
         priv->picture.h264.field_order_cnt[0] = expectedPicOrderCnt + priv->codec_data.h264.delta_pic_order_cnt[0];
      else
         priv->picture.h264.field_order_cnt[1] = expectedPicOrderCnt + sps->offset_for_top_to_bottom_field +
            priv->codec_data.h264.delta_pic_order_cnt[0];

   } else if (sps->pic_order_cnt_type == 2) {
      unsigned MaxFrameNum = 1 << (sps->log2_max_frame_num_minus4 + 4);
      unsigned FrameNumOffset, tempPicOrderCnt;

      if (IdrPicFlag)
         FrameNumOffset = 0;
      else if (prevFrameNum > frame_num)
         FrameNumOffset = priv->codec_data.h264.prevFrameNumOffset + MaxFrameNum;
      else
         FrameNumOffset = priv->codec_data.h264.prevFrameNumOffset;

      priv->codec_data.h264.prevFrameNumOffset = FrameNumOffset;

      if (IdrPicFlag)
         tempPicOrderCnt = 0;
      else if (nal_ref_idc == 0)
         tempPicOrderCnt = 2 * (FrameNumOffset + frame_num) - 1;
      else
         tempPicOrderCnt = 2 * (FrameNumOffset + frame_num);

      if (!priv->picture.h264.field_pic_flag) {
         priv->picture.h264.field_order_cnt[0] = tempPicOrderCnt;
         priv->picture.h264.field_order_cnt[1] = tempPicOrderCnt;

      } else if (!priv->picture.h264.bottom_field_flag)
         priv->picture.h264.field_order_cnt[0] = tempPicOrderCnt;
      else
         priv->picture.h264.field_order_cnt[1] = tempPicOrderCnt;
   }

   if (pps->redundant_pic_cnt_present_flag)
      /* redundant_pic_cnt */
      vl_rbsp_ue(rbsp);

   if (slice_type == PIPE_H264_SLICE_TYPE_B)
      /* direct_spatial_mv_pred_flag */
      vl_rbsp_u(rbsp, 1);

   priv->picture.h264.num_ref_idx_l0_active_minus1 = pps->num_ref_idx_l0_default_active_minus1;
   priv->picture.h264.num_ref_idx_l1_active_minus1 = pps->num_ref_idx_l1_default_active_minus1;

   if (slice_type == PIPE_H264_SLICE_TYPE_P ||
       slice_type == PIPE_H264_SLICE_TYPE_SP ||
       slice_type == PIPE_H264_SLICE_TYPE_B) {

      /* num_ref_idx_active_override_flag */
      if (vl_rbsp_u(rbsp, 1)) {
         priv->picture.h264.num_ref_idx_l0_active_minus1 = vl_rbsp_ue(rbsp);

         if (slice_type == PIPE_H264_SLICE_TYPE_B)
            priv->picture.h264.num_ref_idx_l1_active_minus1 = vl_rbsp_ue(rbsp);
      }
   }

   if (nal_unit_type == 20 || nal_unit_type == 21)
      ref_pic_list_mvc_modification(priv, rbsp);
   else
      ref_pic_list_modification(priv, rbsp, slice_type);

   if ((pps->weighted_pred_flag && (slice_type == PIPE_H264_SLICE_TYPE_P || slice_type == PIPE_H264_SLICE_TYPE_SP)) ||
       (pps->weighted_bipred_idc == 1 && slice_type == PIPE_H264_SLICE_TYPE_B))
      pred_weight_table(priv, rbsp, sps, slice_type);

   if (nal_ref_idc != 0)
      dec_ref_pic_marking(priv, rbsp, IdrPicFlag);

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
