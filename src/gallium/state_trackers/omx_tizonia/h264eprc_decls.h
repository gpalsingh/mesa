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

#ifndef H264DPRC_DECLS_H
#define H264DPRC_DECLS_H

#include <tizonia/OMX_Core.h>

#include <tizprc_decls.h>
#include <tizport_decls.h>

#include "util/list.h"

#include "vl/vl_defines.h"
#include "vl/vl_compositor.h"

#define OMX_VID_ENC_NUM_SCALING_BUFFERS 4

struct encode_task {
   struct list_head list;

   struct pipe_video_buffer *buf;
   unsigned pic_order_cnt;
   struct pipe_resource *bitstream;
   void *feedback;
};

typedef struct h264e_prc_class h264e_prc_class_t;
struct h264e_prc_class
{
   /* Class */
   const tiz_prc_class_t _;
   /* NOTE: Class methods might be added in the future */
};

typedef struct h264e_prc h264e_prc_t;
struct h264e_prc
{
   /* Object */
   const tiz_prc_t _;
   OMX_BUFFERHEADERTYPE *p_inhdr_;
   OMX_BUFFERHEADERTYPE *p_outhdr_;
   OMX_PARAM_PORTDEFINITIONTYPE in_port_def_;
   OMX_PARAM_PORTDEFINITIONTYPE out_port_def_;
   struct vl_screen *screen;
   struct pipe_context *s_pipe;
   struct pipe_context *t_pipe;
   struct pipe_video_codec *codec;
   struct list_head free_tasks;
   struct list_head used_tasks;
   struct list_head b_frames;
   struct list_head stacked_tasks;
   OMX_U32 frame_rate;
   OMX_U32 frame_num;
   OMX_U32 pic_order_cnt;
   OMX_U32 ref_idx_l0, ref_idx_l1;
   OMX_BOOL restricted_b_frames;
   OMX_VIDEO_PARAM_BITRATETYPE bitrate;
   OMX_VIDEO_PARAM_QUANTIZATIONTYPE quant;
   OMX_VIDEO_PARAM_PROFILELEVELTYPE profile_level;
   OMX_CONFIG_INTRAREFRESHVOPTYPE force_pic_type;
   struct vl_compositor compositor;
   struct vl_compositor_state cstate;
   struct pipe_video_buffer *scale_buffer[OMX_VID_ENC_NUM_SCALING_BUFFERS];
   OMX_CONFIG_SCALEFACTORTYPE scale;
   OMX_U32 current_scale_buffer;
   OMX_U32 stacked_frames_num;
   bool eos_;
   bool in_port_disabled_;
   bool out_port_disabled_;
};

#endif
