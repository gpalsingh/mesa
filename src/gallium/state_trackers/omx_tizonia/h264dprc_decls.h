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
#include "util/u_hash_table.h"

#include "pipe/p_video_state.h"

#include "vl/vl_rbsp.h"
#include "vl/vl_zscan.h"
#include "vl/vl_compositor.h"

typedef struct h264d_prc_class h264d_prc_class_t;
struct h264d_prc_class
{
   /* Class */
   const tiz_prc_class_t _;
   /* NOTE: Class methods might be added in the future */
};

typedef struct h264d_stream_info h264d_stream_info_t;
struct h264d_stream_info
{
   unsigned int width;
   unsigned int height;
};

typedef struct h264d_prc h264d_prc_t;
struct h264d_prc
{
   /* Object */
   const tiz_prc_t _;
   OMX_BUFFERHEADERTYPE *in_buffers[2];
   OMX_BUFFERHEADERTYPE *p_inhdr_;
   OMX_BUFFERHEADERTYPE *p_outhdr_;
   OMX_PARAM_PORTDEFINITIONTYPE in_port_def_;
   OMX_PARAM_PORTDEFINITIONTYPE out_port_def_;
   const void *inputs[2];
   unsigned sizes[2];
   OMX_TICKS timestamps[2];
   OMX_TICKS timestamp;
   bool eos_;
   bool in_port_disabled_;
   bool out_port_disabled_;
   struct vl_screen *screen;
   struct pipe_context *pipe;
   struct pipe_video_codec *codec;
   struct pipe_video_buffer *target;
   enum pipe_video_profile profile;
   struct util_hash_table *video_buffer_map;
   struct {
      unsigned nal_ref_idc;
      bool IdrPicFlag;
      unsigned idr_pic_id;
      unsigned pic_order_cnt_lsb;
      unsigned pic_order_cnt_msb;
      unsigned delta_pic_order_cnt_bottom;
      unsigned delta_pic_order_cnt[2];
      unsigned prevFrameNumOffset;
      struct pipe_h264_sps sps[32];
      struct pipe_h264_pps pps[256];
      struct list_head dpb_list;
      unsigned dpb_num;
   } codec_data;
   union {
      struct pipe_picture_desc base;
      struct pipe_h264_picture_desc h264;
   } picture;
   h264d_stream_info_t stream_info;
   unsigned num_in_buffers;
   bool first_buf_in_frame;
   bool frame_finished;
   bool frame_started;
   unsigned bytes_left;
   const void *slice;
   bool disable_tunnel;
   struct vl_compositor compositor;
   struct vl_compositor_state cstate;
   bool use_eglimage;
};

#endif