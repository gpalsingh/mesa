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
