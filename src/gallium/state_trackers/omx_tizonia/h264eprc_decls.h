#ifndef H264DPRC_DECLS_H
#define H264DPRC_DECLS_H

#include <tizonia/OMX_Core.h>

#include <tizprc_decls.h>

#include "util/list.h"

#include "vl/vl_defines.h"
#include "vl/vl_compositor.h"

typedef struct h264d_prc_class h264d_prc_class_t;
struct h264d_prc_class
{
    /* Class */
    const tiz_prc_class_t _;
    /* NOTE: Class methods might be added in the future */
};

typedef struct h264d_prc h264d_prc_t;
struct h264d_prc
{
    /* Object */
    const tiz_prc_t _;
    OMX_BUFFERHEADERTYPE *p_inhdr_;
    OMX_BUFFERHEADERTYPE *p_outhdr_;
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
    OMX_VIDEO_PARAM_PROFILELEVELTYPE profile_level;
    OMX_CONFIG_INTRAREFRESHVOPTYPE force_pic_type;
    struct vl_compositor compositor;
    struct vl_compositor_state cstate;
    struct pipe_video_buffer *scale_buffer[OMX_VID_ENC_NUM_SCALING_BUFFERS];
    OMX_CONFIG_SCALEFACTORTYPE scale;
    OMX_U32 current_scale_buffer;
    OMX_U32 stacked_frames_num;
};

#endif
