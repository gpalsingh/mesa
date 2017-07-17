#include <tizplatform.h>
#include <tizkernel.h>
#include <tizutils.h>

#include "pipe/p_screen.h"
#include "pipe/p_video_codec.h"
#include "util/u_memory.h"
#include "vl/vl_video_buffer.h"

#include "entrypoint.h"
#include "h264e.h"
#include "h264eprc.h"
#include "h264eprc_decls.h"
#include "h264e_common.h"

/* Utility functions */

static void enc_MoveTasks(struct list_head *from, struct list_head *to)
{
    to->prev->next = from->next;
    from->next->prev = to->prev;
    from->prev->next = to;
    to->prev = from->prev;
    LIST_INITHEAD(from);
}

static void enc_GetPictureParamPreset(struct pipe_h264_enc_picture_desc *picture)
{
   picture->motion_est.enc_disable_sub_mode = 0x000000fe;
   picture->motion_est.enc_ime2_search_range_x = 0x00000001;
   picture->motion_est.enc_ime2_search_range_y = 0x00000001;
   picture->pic_ctrl.enc_constraint_set_flags = 0x00000040;
}

static enum pipe_video_profile enc_TranslateOMXProfileToPipe(unsigned omx_profile)
{
   switch (omx_profile) {
   case OMX_VIDEO_AVCProfileBaseline:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE;
   case OMX_VIDEO_AVCProfileMain:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN;
   case OMX_VIDEO_AVCProfileExtended:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_EXTENDED;
   case OMX_VIDEO_AVCProfileHigh:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
   case OMX_VIDEO_AVCProfileHigh10:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10;
   case OMX_VIDEO_AVCProfileHigh422:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH422;
   case OMX_VIDEO_AVCProfileHigh444:
      return PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH444;
   default:
      return PIPE_VIDEO_PROFILE_UNKNOWN;
   }
}

static unsigned enc_TranslateOMXLevelToPipe(unsigned omx_level)
{
   switch (omx_level) {
   case OMX_VIDEO_AVCLevel1:
   case OMX_VIDEO_AVCLevel1b:
      return 10;
   case OMX_VIDEO_AVCLevel11:
      return 11;
   case OMX_VIDEO_AVCLevel12:
      return 12;
   case OMX_VIDEO_AVCLevel13:
      return 13;
   case OMX_VIDEO_AVCLevel2:
      return 20;
   case OMX_VIDEO_AVCLevel21:
      return 21;
   case OMX_VIDEO_AVCLevel22:
      return 22;
   case OMX_VIDEO_AVCLevel3:
      return 30;
   case OMX_VIDEO_AVCLevel31:
      return 31;
   case OMX_VIDEO_AVCLevel32:
      return 32;
   case OMX_VIDEO_AVCLevel4:
      return 40;
   case OMX_VIDEO_AVCLevel41:
      return 41;
   default:
   case OMX_VIDEO_AVCLevel42:
      return 42;
   case OMX_VIDEO_AVCLevel5:
      return 50;
   case OMX_VIDEO_AVCLevel51:
      return 51;
   }
}

/* H264e spefific */

static OMX_BUFFERHEADERTYPE * get_input_buffer (h264e_prc_t * p_prc) {
    assert (p_prc);

    if (p_prc->in_port_disabled_) {
        return NULL;
    }

    assert (!p_prc->p_inhdr_); /* encode_frame expects new buffers every time */

    tiz_krn_claim_buffer (tiz_get_krn (handleOf (p_prc)),
                          OMX_VID_ENC_AVC_INPUT_PORT_INDEX, 0,
                          &p_prc->p_inhdr_);
    return p_prc->p_inhdr_;
}

static OMX_BUFFERHEADERTYPE * get_output_buffer (h264e_prc_t * p_prc) {
    assert (p_prc);

    if (p_prc->out_port_disabled_) {
        return NULL;
    }

    if (!p_prc->p_outhdr_) {
        tiz_krn_claim_buffer (tiz_get_krn (handleOf (p_prc)),
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX, 0,
                              &p_prc->p_outhdr_);
    }
    return p_prc->p_outhdr_;
}

static OMX_ERRORTYPE h264e_buffer_emptied (h264e_prc_t * p_prc, OMX_BUFFERHEADERTYPE * p_hdr)
{
    OMX_ERRORTYPE r = OMX_ErrorNone;

    assert (p_prc);
    assert (p_prc->p_inhdr_ == p_hdr);

    if (!p_prc->out_port_disabled_) {
        assert (p_hdr->nFilledLen == 0);
        p_hdr->nOffset = 0;

        if ((p_hdr->nFlags & OMX_BUFFERFLAG_EOS) != 0) {
            p_prc->eos_ = true;
        }

        r = tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)), 0, p_hdr);
        p_prc->p_inhdr_ = NULL;
    }

    return r;
}

static OMX_ERRORTYPE h264e_buffer_filled (h264e_prc_t * p_prc, OMX_BUFFERHEADERTYPE * p_hdr)
{
    OMX_ERRORTYPE r = OMX_ErrorNone;

    assert (p_prc);
    assert (p_prc->p_outhdr_ == p_hdr);
    assert (p_hdr);

    if (!p_prc->in_port_disabled_) {
        p_hdr->nOffset = 0;

        if (p_prc->eos_) {
            /* EOS has been received and all the input data has been consumed
             * already, so its time to propagate the EOS flag */
            p_prc->p_outhdr_->nFlags |= OMX_BUFFERFLAG_EOS;
        }

        r = tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)),
                                    OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX,
                                    p_hdr);
        p_prc->p_outhdr_ = NULL;
    }

    return r;
}


static void release_input_header (h264e_prc_t * p_prc) {
    assert(!p_prc->in_port_disabled_);
    if (p_prc->p_inhdr_) {
        (void) tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)),
                                       OMX_VID_ENC_AVC_INPUT_PORT_INDEX,
                                       p_prc->p_inhdr_);
    }
    p_prc->p_inhdr_ = NULL;
}

static void release_output_header (h264e_prc_t * p_prc) {
    if (p_prc->p_outhdr_) {
        assert(!p_prc->out_port_disabled_);
        (void) tiz_krn_release_buffer (tiz_get_krn (handleOf (p_prc)),
                                       OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX,
                                       p_prc->p_outhdr_);
        p_prc->p_outhdr_ = NULL;
    }
}

static OMX_ERRORTYPE h264e_release_all_headers (h264e_prc_t * p_prc)
{
    assert (p_prc);

    release_input_header (p_prc);
    release_output_header (p_prc);

    return OMX_ErrorNone;
}

static void reset_stream_parameters (h264e_prc_t * ap_prc)
{
    assert (ap_prc);
    TIZ_INIT_OMX_PORT_STRUCT (ap_prc->in_port_def_,
                              OMX_VID_ENC_AVC_INPUT_PORT_INDEX);
    TIZ_INIT_OMX_PORT_STRUCT (ap_prc->out_port_def_,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    TIZ_INIT_OMX_PORT_STRUCT (ap_prc->bitrate,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    TIZ_INIT_OMX_PORT_STRUCT (ap_prc->quant,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    TIZ_INIT_OMX_PORT_STRUCT (ap_prc->profile_level,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    ap_prc->p_inhdr_ = 0;
    ap_prc->p_outhdr_ = 0;
    /* Check if need to clear resources to reset current_scale_buffer and others */
    ap_prc->eos_ = false;
}

static void h264e_buffer_encoded (h264e_prc_t * p_prc, OMX_BUFFERHEADERTYPE* input, OMX_BUFFERHEADERTYPE* output)
{
   struct output_buf_private *outp = output->pOutputPortPrivate;
   struct input_buf_private *inp = input->pInputPortPrivate;
   struct encode_task *task;
   struct pipe_box box = {};
   unsigned size;

   if (!inp || LIST_IS_EMPTY(&inp->tasks)) {
      input->nFilledLen = 0; /* mark buffer as empty */
      enc_MoveTasks(&p_prc->used_tasks, &inp->tasks);
      return;
   }

   task = LIST_ENTRY(struct encode_task, inp->tasks.next, list);
   LIST_DEL(&task->list);
   LIST_ADDTAIL(&task->list, &p_prc->used_tasks);

   if (!task->bitstream)
      return;

   /* ------------- map result buffer ----------------- */

   if (outp->transfer)
      pipe_transfer_unmap(p_prc->t_pipe, outp->transfer);

   pipe_resource_reference(&outp->bitstream, task->bitstream);
   pipe_resource_reference(&task->bitstream, NULL);

   box.width = outp->bitstream->width0;
   box.height = outp->bitstream->height0;
   box.depth = outp->bitstream->depth0;

   output->pBuffer = p_prc->t_pipe->transfer_map(p_prc->t_pipe, outp->bitstream, 0,
                                                PIPE_TRANSFER_READ_WRITE,
                                                &box, &outp->transfer);

   /* ------------- get size of result ----------------- */

   p_prc->codec->get_feedback(p_prc->codec, task->feedback, &size);

   output->nOffset = 0;
   output->nFilledLen = size; /* mark buffer as full */

   input->nFilledLen = 0; /* set input buffer for clearing */

   /* all output buffers contain exactly one frame */
   output->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
}

/* Replacement for bellagio's omx_base_filter_BufferMgmtFunction */
static OMX_ERRORTYPE h264e_manage_buffers(h264e_prc_t* p_prc) {
    OMX_BUFFERHEADERTYPE * in_buf = p_prc->p_inhdr_;
    OMX_BUFFERHEADERTYPE * out_buf = p_prc->p_outhdr_;
    OMX_ERRORTYPE r = OMX_ErrorNone;

    if (in_buf->nFilledLen > 0) {
        h264e_buffer_encoded (p_prc, in_buf, out_buf);
    } else {
        in_buf->nFilledLen = 0;
    }

    out_buf->nTimeStamp = in_buf->nTimeStamp;

    /* Release input buffer if possible */
    if (in_buf->nFilledLen == 0) {
        r = h264e_buffer_emptied(p_prc, in_buf);
    }

    /* Realase output buffer if filled or eos */
    if ((out_buf->nFilledLen != 0) || p_prc->eos_) {
        r = h264e_buffer_filled(p_prc, out_buf);
    }

    return r;
}

/* End of Utlility functions */


/* Encoder internal functions */

static struct encode_task *enc_NeedTask(h264e_prc_t * p_prc)
{
    OMX_VIDEO_PORTDEFINITIONTYPE *def = &p_prc->in_port_def_.format.video;

    struct pipe_video_buffer templat = {};
    struct encode_task *task;

    if (!LIST_IS_EMPTY(&p_prc->free_tasks)) {
        task = LIST_ENTRY(struct encode_task, p_prc->free_tasks.next, list);
        LIST_DEL(&task->list);
        return task;
    }

    /* allocate a new one */
    task = CALLOC_STRUCT(encode_task);
    if (!task)
        return NULL;

    templat.buffer_format = PIPE_FORMAT_NV12;
    templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
    templat.width = def->nFrameWidth;
    templat.height = def->nFrameHeight;
    templat.interlaced = false;

    task->buf = p_prc->s_pipe->create_video_buffer(p_prc->s_pipe, &templat);
    if (!task->buf) {
        FREE(task);
        return NULL;
    }

    return task;
}

static void enc_ScaleInput(h264e_prc_t * p_prc, struct pipe_video_buffer **vbuf, unsigned *size)
{
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &p_prc->in_port_def_.format.video;
   struct pipe_video_buffer *src_buf = *vbuf;
   struct vl_compositor *compositor = &p_prc->compositor;
   struct vl_compositor_state *s = &p_prc->cstate;
   struct pipe_sampler_view **views;
   struct pipe_surface **dst_surface;
   unsigned i;

   if (!p_prc->scale_buffer[p_prc->current_scale_buffer])
      return;

   views = src_buf->get_sampler_view_planes(src_buf);
   dst_surface = p_prc->scale_buffer[p_prc->current_scale_buffer]->get_surfaces
                 (p_prc->scale_buffer[p_prc->current_scale_buffer]);
   vl_compositor_clear_layers(s);

   for (i = 0; i < VL_MAX_SURFACES; ++i) {
      struct u_rect src_rect;
      if (!views[i] || !dst_surface[i])
         continue;
      src_rect.x0 = 0;
      src_rect.y0 = 0;
      src_rect.x1 = def->nFrameWidth;
      src_rect.y1 = def->nFrameHeight;
      if (i > 0) {
         src_rect.x1 /= 2;
         src_rect.y1 /= 2;
      }
      vl_compositor_set_rgba_layer(s, compositor, 0, views[i], &src_rect, NULL, NULL);
      vl_compositor_render(s, compositor, dst_surface[i], NULL, false);
   }
   *size  = p_prc->scale.xWidth * p_prc->scale.xHeight * 2;
   *vbuf = p_prc->scale_buffer[p_prc->current_scale_buffer++];
   p_prc->current_scale_buffer %= OMX_VID_ENC_NUM_SCALING_BUFFERS;
}

static void enc_ControlPicture(h264e_prc_t * p_prc, struct pipe_h264_enc_picture_desc *picture)
{
   struct pipe_h264_enc_rate_control *rate_ctrl = &picture->rate_ctrl;

   /* Get bitrate from port */
   switch (p_prc->bitrate.eControlRate) {
   case OMX_Video_ControlRateVariable:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_VARIABLE;
      break;
   case OMX_Video_ControlRateConstant:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_CONSTANT;
      break;
   case OMX_Video_ControlRateVariableSkipFrames:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_VARIABLE_SKIP;
      break;
   case OMX_Video_ControlRateConstantSkipFrames:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_CONSTANT_SKIP;
      break;
   default:
      rate_ctrl->rate_ctrl_method = PIPE_H264_ENC_RATE_CONTROL_METHOD_DISABLE;
      break;
   }

   rate_ctrl->frame_rate_den = OMX_VID_ENC_CONTROL_FRAME_RATE_DEN_DEFAULT;
   rate_ctrl->frame_rate_num = ((p_prc->frame_rate) >> 16) * rate_ctrl->frame_rate_den;

   if (rate_ctrl->rate_ctrl_method != PIPE_H264_ENC_RATE_CONTROL_METHOD_DISABLE) {
      if (p_prc->bitrate.nTargetBitrate < OMX_VID_ENC_BITRATE_MIN)
         rate_ctrl->target_bitrate = OMX_VID_ENC_BITRATE_MIN;
      else if (p_prc->bitrate.nTargetBitrate < OMX_VID_ENC_BITRATE_MAX)
         rate_ctrl->target_bitrate = p_prc->bitrate.nTargetBitrate;
      else
         rate_ctrl->target_bitrate = OMX_VID_ENC_BITRATE_MAX;
      rate_ctrl->peak_bitrate = rate_ctrl->target_bitrate;
      if (rate_ctrl->target_bitrate < OMX_VID_ENC_BITRATE_MEDIAN)
         rate_ctrl->vbv_buffer_size = MIN2((rate_ctrl->target_bitrate * 2.75), OMX_VID_ENC_BITRATE_MEDIAN);
      else
         rate_ctrl->vbv_buffer_size = rate_ctrl->target_bitrate;

      if (rate_ctrl->frame_rate_num) {
         unsigned long long t = rate_ctrl->target_bitrate;
         t *= rate_ctrl->frame_rate_den;
         rate_ctrl->target_bits_picture = t / rate_ctrl->frame_rate_num;
      } else {
         rate_ctrl->target_bits_picture = rate_ctrl->target_bitrate;
      }
      rate_ctrl->peak_bits_picture_integer = rate_ctrl->target_bits_picture;
      rate_ctrl->peak_bits_picture_fraction = 0;
   }

   picture->quant_i_frames = p_prc->quant.nQpI;
   picture->quant_p_frames = p_prc->quant.nQpP;
   picture->quant_b_frames = p_prc->quant.nQpB;

   picture->frame_num = p_prc->frame_num;
   picture->ref_idx_l0 = p_prc->ref_idx_l0;
   picture->ref_idx_l1 = p_prc->ref_idx_l1;
   picture->enable_vui = (picture->rate_ctrl.frame_rate_num != 0);
   enc_GetPictureParamPreset(picture);
}

static void enc_HandleTask(h264e_prc_t * p_prc, struct encode_task *task,
                           enum pipe_h264_enc_picture_type picture_type)
{
   unsigned size = p_prc->out_port_def_.nBufferSize;
   struct pipe_video_buffer *vbuf = task->buf;
   struct pipe_h264_enc_picture_desc picture = {};

   /* -------------- scale input image --------- */
   enc_ScaleInput(p_prc, &vbuf, &size);
   p_prc->s_pipe->flush(p_prc->s_pipe, NULL, 0);

   /* -------------- allocate output buffer --------- */
   task->bitstream = pipe_buffer_create(p_prc->s_pipe->screen,
                                        PIPE_BIND_VERTEX_BUFFER,
                                        PIPE_USAGE_STAGING, /* map for read */
                                        size);

   picture.picture_type = picture_type;
   picture.pic_order_cnt = task->pic_order_cnt;
   if (p_prc->restricted_b_frames && picture_type == PIPE_H264_ENC_PICTURE_TYPE_B)
      picture.not_referenced = true;
   enc_ControlPicture(p_prc, &picture);

   /* -------------- encode frame --------- */
   p_prc->codec->begin_frame(p_prc->codec, vbuf, &picture.base);
   p_prc->codec->encode_bitstream(p_prc->codec, vbuf, task->bitstream, &task->feedback);
   p_prc->codec->end_frame(p_prc->codec, vbuf, &picture.base);
}

static void enc_ClearBframes(h264e_prc_t * p_prc, struct input_buf_private *inp)
{
   struct encode_task *task;

   if (LIST_IS_EMPTY(&p_prc->b_frames))
      return;

   task = LIST_ENTRY(struct encode_task, p_prc->b_frames.prev, list);
   LIST_DEL(&task->list);

   /* promote last from to P frame */
   p_prc->ref_idx_l0 = p_prc->ref_idx_l1;
   enc_HandleTask(p_prc, task, PIPE_H264_ENC_PICTURE_TYPE_P);
   LIST_ADDTAIL(&task->list, &inp->tasks);
   p_prc->ref_idx_l1 = p_prc->frame_num++;

   /* handle B frames */
   LIST_FOR_EACH_ENTRY(task, &p_prc->b_frames, list) {
      enc_HandleTask(p_prc, task, PIPE_H264_ENC_PICTURE_TYPE_B);
      if (!p_prc->restricted_b_frames)
         p_prc->ref_idx_l0 = p_prc->frame_num;
      p_prc->frame_num++;
   }

   enc_MoveTasks(&p_prc->b_frames, &inp->tasks);
}

static OMX_ERRORTYPE enc_LoadImage(h264e_prc_t * p_prc, OMX_BUFFERHEADERTYPE *buf,
                                   struct pipe_video_buffer *vbuf)
{
   OMX_VIDEO_PORTDEFINITIONTYPE *def = &p_prc->in_port_def_.format.video;
   struct pipe_box box = {};
   struct input_buf_private *inp = buf->pInputPortPrivate;

   if (!inp->resource) {
      struct pipe_sampler_view **views;
      void *ptr;

      views = vbuf->get_sampler_view_planes(vbuf);
      if (!views)
         return OMX_ErrorInsufficientResources;

      ptr = buf->pBuffer;
      box.width = def->nFrameWidth;
      box.height = def->nFrameHeight;
      box.depth = 1;
      p_prc->s_pipe->texture_subdata(p_prc->s_pipe, views[0]->texture, 0,
                                    PIPE_TRANSFER_WRITE, &box,
                                    ptr, def->nStride, 0);
      ptr = ((uint8_t*)buf->pBuffer) + (def->nStride * box.height);
      box.width = def->nFrameWidth / 2;
      box.height = def->nFrameHeight / 2;
      box.depth = 1;
      p_prc->s_pipe->texture_subdata(p_prc->s_pipe, views[1]->texture, 0,
                                    PIPE_TRANSFER_WRITE, &box,
                                    ptr, def->nStride, 0);
   } else {
      struct pipe_blit_info blit;
      struct vl_video_buffer *dst_buf = (struct vl_video_buffer *)vbuf;

      pipe_transfer_unmap(p_prc->s_pipe, inp->transfer);

      box.width = def->nFrameWidth;
      box.height = def->nFrameHeight;
      box.depth = 1;

      p_prc->s_pipe->resource_copy_region(p_prc->s_pipe,
                                         dst_buf->resources[0],
                                         0, 0, 0, 0, inp->resource, 0, &box);

      memset(&blit, 0, sizeof(blit));
      blit.src.resource = inp->resource;
      blit.src.format = inp->resource->format;

      blit.src.box.x = 0;
      blit.src.box.y = def->nFrameHeight;
      blit.src.box.width = def->nFrameWidth;
      blit.src.box.height = def->nFrameHeight / 2 ;
      blit.src.box.depth = 1;

      blit.dst.resource = dst_buf->resources[1];
      blit.dst.format = blit.dst.resource->format;

      blit.dst.box.width = def->nFrameWidth / 2;
      blit.dst.box.height = def->nFrameHeight / 2;
      blit.dst.box.depth = 1;
      blit.filter = PIPE_TEX_FILTER_NEAREST;

      blit.mask = PIPE_MASK_G;
      p_prc->s_pipe->blit(p_prc->s_pipe, &blit);

      blit.src.box.x = 1;
      blit.mask = PIPE_MASK_R;
      p_prc->s_pipe->blit(p_prc->s_pipe, &blit);
      p_prc->s_pipe->flush(p_prc->s_pipe, NULL, 0);

      box.width = inp->resource->width0;
      box.height = inp->resource->height0;
      box.depth = inp->resource->depth0;
      buf->pBuffer = p_prc->s_pipe->transfer_map(p_prc->s_pipe, inp->resource, 0,
                                                PIPE_TRANSFER_WRITE, &box,
                                                &inp->transfer);
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE encode_frame(h264e_prc_t * p_prc, OMX_BUFFERHEADERTYPE * in_buf)
{
    struct input_buf_private *inp = in_buf->pInputPortPrivate;
    enum pipe_h264_enc_picture_type picture_type;
    struct encode_task *task;
    unsigned stacked_num = 0;
    OMX_ERRORTYPE err;

    enc_MoveTasks(&inp->tasks, &p_prc->free_tasks);
    task = enc_NeedTask(p_prc);
    if (!task)
        return OMX_ErrorInsufficientResources;

    /* EOS */
    if (in_buf->nFilledLen == 0) {
        if (in_buf->nFlags & OMX_BUFFERFLAG_EOS) {
            in_buf->nFilledLen = in_buf->nAllocLen;
            enc_ClearBframes(p_prc, inp);
            enc_MoveTasks(&p_prc->stacked_tasks, &inp->tasks);
            p_prc->codec->flush(p_prc->codec);
        }
        return h264e_manage_buffers(p_prc);
    }

    if (in_buf->pOutputPortPrivate) {
        struct pipe_video_buffer *vbuf = in_buf->pOutputPortPrivate;
        in_buf->pOutputPortPrivate = task->buf;
        task->buf = vbuf;
    } else {
        /* ------- load input image into video buffer ---- */
        err = enc_LoadImage(p_prc, in_buf, task->buf);
        if (err != OMX_ErrorNone) {
            FREE(task);
            return err;
        }
    }

    /* -------------- determine picture type --------- */
    if (!(p_prc->pic_order_cnt % OMX_VID_ENC_IDR_PERIOD_DEFAULT) ||
         p_prc->force_pic_type.IntraRefreshVOP) {
        enc_ClearBframes(p_prc, inp);
        picture_type = PIPE_H264_ENC_PICTURE_TYPE_IDR;
        p_prc->force_pic_type.IntraRefreshVOP = OMX_FALSE;
        p_prc->frame_num = 0;
    } else if (p_prc->codec->profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE ||
                  !(p_prc->pic_order_cnt % OMX_VID_ENC_P_PERIOD_DEFAULT) ||
                  (in_buf->nFlags & OMX_BUFFERFLAG_EOS)) {
        picture_type = PIPE_H264_ENC_PICTURE_TYPE_P;
    } else {
        picture_type = PIPE_H264_ENC_PICTURE_TYPE_B;
    }

    task->pic_order_cnt = p_prc->pic_order_cnt++;

    if (picture_type == PIPE_H264_ENC_PICTURE_TYPE_B) {
        /* put frame at the tail of the queue */
        LIST_ADDTAIL(&task->list, &p_prc->b_frames);
    } else {
        /* handle I or P frame */
        p_prc->ref_idx_l0 = p_prc->ref_idx_l1;
        enc_HandleTask(p_prc, task, picture_type);
        LIST_ADDTAIL(&task->list, &p_prc->stacked_tasks);
        LIST_FOR_EACH_ENTRY(task, &p_prc->stacked_tasks, list) {
            ++stacked_num;
        }
        if (stacked_num == p_prc->stacked_frames_num) {
            struct encode_task *t;
            t = LIST_ENTRY(struct encode_task, p_prc->stacked_tasks.next, list);
            LIST_DEL(&t->list);
            LIST_ADDTAIL(&t->list, &inp->tasks);
        }
        p_prc->ref_idx_l1 = p_prc->frame_num++;

        /* handle B frames */
        LIST_FOR_EACH_ENTRY(task, &p_prc->b_frames, list) {
            enc_HandleTask(p_prc, task, PIPE_H264_ENC_PICTURE_TYPE_B);
            if (!p_prc->restricted_b_frames)
                p_prc->ref_idx_l0 = p_prc->frame_num;
            p_prc->frame_num++;
        }

        enc_MoveTasks(&p_prc->b_frames, &inp->tasks);
    }

    if (LIST_IS_EMPTY(&inp->tasks))
        return h264e_buffer_emptied(p_prc, in_buf);
    else
        return h264e_manage_buffers(p_prc);
}

/* End of Encoder Internal functions */

/*
 * h264eprc
 */

static void *
h264e_prc_ctor (void *ap_obj, va_list * app)
{
    h264e_prc_t *p_prc = super_ctor (typeOf (ap_obj, "h264eprc"), ap_obj, app);
    assert (p_prc);
    p_prc->p_inhdr_ = 0;
    p_prc->p_outhdr_ = 0;
    p_prc->profile_level.eProfile = OMX_VIDEO_AVCProfileBaseline;
    p_prc->profile_level.eLevel = OMX_VIDEO_AVCLevel51;
    p_prc->force_pic_type.IntraRefreshVOP = OMX_FALSE;
    p_prc->frame_num = 0;
    p_prc->pic_order_cnt = 0;
    p_prc->restricted_b_frames = debug_get_bool_option("OMX_USE_RESTRICTED_B_FRAMES", FALSE);
    p_prc->scale.xWidth = OMX_VID_ENC_SCALING_WIDTH_DEFAULT;
    p_prc->scale.xHeight = OMX_VID_ENC_SCALING_WIDTH_DEFAULT;
    p_prc->in_port_disabled_    = false;
    p_prc->out_port_disabled_   = false;
    reset_stream_parameters(p_prc);

    return p_prc;
}

static void * h264e_prc_dtor (void *ap_obj)
{
    h264e_prc_t *p_prc = ap_obj;

    return super_dtor (typeOf (ap_obj, "h264eprc"), ap_obj);
}

static OMX_ERRORTYPE h264e_prc_allocate_resources (void *ap_obj, OMX_U32 a_pid)
{
    h264e_prc_t *p_prc = ap_obj;
    struct pipe_screen *screen;

    p_prc->screen = omx_get_screen ();
    if (!p_prc->screen)
        return OMX_ErrorInsufficientResources;

    screen = p_prc->screen->pscreen;
    if (!screen->get_video_param(screen, PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
                                 PIPE_VIDEO_ENTRYPOINT_ENCODE, PIPE_VIDEO_CAP_SUPPORTED))
       return OMX_ErrorBadParameter;

    p_prc->s_pipe = screen->context_create(screen, NULL, 0);
    if (!p_prc->s_pipe)
        return OMX_ErrorInsufficientResources;

    if (!vl_compositor_init(&p_prc->compositor, p_prc->s_pipe)) {
        p_prc->s_pipe->destroy(p_prc->s_pipe);
        p_prc->s_pipe = NULL;
        return OMX_ErrorInsufficientResources;
    }

    if (!vl_compositor_init_state(&p_prc->cstate, p_prc->s_pipe)) {
        vl_compositor_cleanup(&p_prc->compositor);
        p_prc->s_pipe->destroy(p_prc->s_pipe);
        p_prc->s_pipe = NULL;
        return OMX_ErrorInsufficientResources;
    }

    p_prc->t_pipe = screen->context_create(screen, NULL, 0);
    if (!p_prc->t_pipe)
        return OMX_ErrorInsufficientResources;

    LIST_INITHEAD(&p_prc->free_tasks);
    LIST_INITHEAD(&p_prc->used_tasks);
    LIST_INITHEAD(&p_prc->b_frames);
    LIST_INITHEAD(&p_prc->stacked_tasks);

    return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_prc_deallocate_resources (void *ap_obj)
{
    h264e_prc_t *p_prc = ap_obj;
    int i;

    assert (p_prc);

    enc_ReleaseTasks(&p_prc->free_tasks);
    enc_ReleaseTasks(&p_prc->used_tasks);
    enc_ReleaseTasks(&p_prc->b_frames);
    enc_ReleaseTasks(&p_prc->stacked_tasks);

    for (i = 0; i < OMX_VID_ENC_NUM_SCALING_BUFFERS; ++i)
        if (p_prc->scale_buffer[i])
        p_prc->scale_buffer[i]->destroy(p_prc->scale_buffer[i]);

    if (p_prc->s_pipe) {
        vl_compositor_cleanup_state(&p_prc->cstate);
        vl_compositor_cleanup(&p_prc->compositor);
        p_prc->s_pipe->destroy(p_prc->s_pipe);
    }

    if (p_prc->t_pipe)
        p_prc->t_pipe->destroy(p_prc->t_pipe);

    if (p_prc->screen)
        omx_put_screen ();

    return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_prc_prepare_to_transfer (void *ap_obj, OMX_U32 a_pid)
{
    h264e_prc_t *p_prc = ap_obj;
    const void * p_krn = NULL;

    assert (p_prc);

    p_krn = tiz_get_krn (handleOf (p_prc));

    TIZ_INIT_OMX_PORT_STRUCT (p_prc->in_port_def_,
                              OMX_VID_ENC_AVC_INPUT_PORT_INDEX);
    tiz_check_omx (
        tiz_api_GetParameter (p_krn, handleOf (p_prc),
                              OMX_IndexParamPortDefinition, &(p_prc->in_port_def_)));

    TIZ_INIT_OMX_PORT_STRUCT (p_prc->out_port_def_,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    tiz_check_omx (
        tiz_api_GetParameter (p_krn, handleOf (p_prc),
                              OMX_IndexParamPortDefinition, &(p_prc->out_port_def_)));

    TIZ_INIT_OMX_PORT_STRUCT (p_prc->bitrate,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    tiz_check_omx (
        tiz_api_GetParameter (p_krn, handleOf (p_prc),
                              OMX_IndexParamVideoBitrate, &(p_prc->bitrate)));

    TIZ_INIT_OMX_PORT_STRUCT (p_prc->quant,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    tiz_check_omx (
        tiz_api_GetParameter (p_krn, handleOf (p_prc),
                              OMX_IndexParamVideoQuantization, &(p_prc->quant)));

    TIZ_INIT_OMX_PORT_STRUCT (p_prc->profile_level,
                              OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX);
    tiz_check_omx (
        tiz_api_GetParameter (p_krn, handleOf (p_prc),
                              OMX_IndexParamVideoProfileLevelCurrent, &(p_prc->profile_level)));

    p_prc->eos_ = false;

    /* from vid_enc_MessageHandler */
    struct pipe_video_codec templat = {};

    templat.profile = enc_TranslateOMXProfileToPipe(p_prc->profile_level.eProfile);
    templat.level = enc_TranslateOMXLevelToPipe(p_prc->profile_level.eLevel);
    templat.entrypoint = PIPE_VIDEO_ENTRYPOINT_ENCODE;
    templat.chroma_format = PIPE_VIDEO_CHROMA_FORMAT_420;
    templat.width = p_prc->scale_buffer[p_prc->current_scale_buffer] ?
                       p_prc->scale.xWidth : p_prc->in_port_def_.format.video.nFrameWidth;
    templat.height = p_prc->scale_buffer[p_prc->current_scale_buffer] ?
                       p_prc->scale.xHeight : p_prc->in_port_def_.format.video.nFrameHeight;

    if (templat.profile == PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE) {
       struct pipe_screen *screen = p_prc->screen->pscreen;
       templat.max_references = 1;
       p_prc->stacked_frames_num =
       screen->get_video_param(screen,
                               PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH,
                               PIPE_VIDEO_ENTRYPOINT_ENCODE,
                               PIPE_VIDEO_CAP_STACKED_FRAMES);
    } else {
        templat.max_references = OMX_VID_ENC_P_PERIOD_DEFAULT;
        p_prc->stacked_frames_num = 1;
    }
    p_prc->codec = p_prc->s_pipe->create_video_codec(p_prc->s_pipe, &templat);

    return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_prc_transfer_and_process (void *ap_obj, OMX_U32 a_pid)
{
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_prc_stop_and_return (void *ap_obj)
{
    h264e_prc_t *p_prc = (h264e_prc_t *) ap_obj;
    return h264e_release_all_headers (p_prc);
}

static OMX_ERRORTYPE h264e_prc_buffers_ready (const void *ap_obj)
{
    assert(p_prc);
    h264e_prc_t *p_prc = (h264e_prc_t *) ap_obj;
    OMX_BUFFERHEADERTYPE *in_buf = NULL;
    OMX_BUFFERHEADERTYPE *out_buf = NULL;
    OMX_ERRORTYPE r = OMX_ErrorNone;

    /* Don't get input buffer if output buffer not found */
    while (!p_prc->eos_ && (out_buf = get_output_buffer(p_prc)) && (in_buf = get_input_buffer(p_prc))) {
        if (!p_prc->out_port_disabled_) {
           r = encode_frame(p_prc, in_buf);
        }
    }

    return r;
}

static OMX_ERRORTYPE h264e_prc_port_flush (const void *ap_obj, OMX_U32 a_pid)
{
    h264e_prc_t *p_prc = (h264e_prc_t *) ap_obj;
    if (OMX_ALL == a_pid || OMX_VID_ENC_AVC_INPUT_PORT_INDEX == a_pid) {
        release_input_header (p_prc);
        reset_stream_parameters (p_prc);
    }
    if (OMX_ALL == a_pid || OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX == a_pid) {
        release_output_header (p_prc);
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_prc_port_disable (const void *ap_obj, OMX_U32 a_pid)
{
    h264e_prc_t *p_prc = (h264e_prc_t *) ap_obj;
    assert (p_prc);
    if (OMX_ALL == a_pid || OMX_VID_ENC_AVC_INPUT_PORT_INDEX == a_pid) {
        /* Release all buffers */
        h264e_release_all_headers (p_prc);
        reset_stream_parameters(p_prc);
        p_prc->in_port_disabled_ = true;
    }
    if (OMX_ALL == a_pid || OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX == a_pid) {
        release_output_header (p_prc);
        p_prc->out_port_disabled_ = true;
    }
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_prc_port_enable (const void *ap_obj, OMX_U32 a_pid)
{
    h264e_prc_t * p_prc = (h264e_prc_t *) ap_obj;
    assert (p_prc);
    if (OMX_ALL == a_pid || OMX_VID_ENC_AVC_INPUT_PORT_INDEX == a_pid) {
        if (p_prc->in_port_disabled_) {
            reset_stream_parameters (p_prc);
            p_prc->in_port_disabled_ = false;
        }
    }
    if (OMX_ALL == a_pid || OMX_VID_ENC_AVC_OUTPUT_PORT_INDEX == a_pid) {
        p_prc->out_port_disabled_ = false;
    }
    return OMX_ErrorNone;
}

/*
 * h264e_prc_class
 */

static void * h264e_prc_class_ctor (void *ap_obj, va_list * app)
{
    /* NOTE: Class methods might be added in the future. None for now. */
    return super_ctor (typeOf (ap_obj, "h264eprc_class"), ap_obj, app);
}

/*
 * initialization
 */

void * h264e_prc_class_init (void * ap_tos, void * ap_hdl)
{
    void * tizprc = tiz_get_type (ap_hdl, "tizprc");
    void * h264eprc_class = factory_new
        /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
        (classOf (tizprc), "h264eprc_class", classOf (tizprc),
         sizeof (h264e_prc_class_t),
         /* TIZ_CLASS_COMMENT: */
         ap_tos, ap_hdl,
         /* TIZ_CLASS_COMMENT: class constructor */
         ctor, h264e_prc_class_ctor,
         /* TIZ_CLASS_COMMENT: stop value*/
         0);
    return h264eprc_class;
}

void * h264e_prc_init (void * ap_tos, void * ap_hdl)
{
    void * tizprc = tiz_get_type (ap_hdl, "tizprc");
    void * h264eprc_class = tiz_get_type (ap_hdl, "h264eprc_class");
    TIZ_LOG_CLASS (h264eprc_class);
    void * h264eprc = factory_new
      /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
      (h264eprc_class, "h264eprc", tizprc, sizeof (h264e_prc_t),
       /* TIZ_CLASS_COMMENT: */
       ap_tos, ap_hdl,
       /* TIZ_CLASS_COMMENT: class constructor */
       ctor, h264e_prc_ctor,
       /* TIZ_CLASS_COMMENT: class destructor */
       dtor, h264e_prc_dtor,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_allocate_resources, h264e_prc_allocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_deallocate_resources, h264e_prc_deallocate_resources,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_prepare_to_transfer, h264e_prc_prepare_to_transfer,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_transfer_and_process, h264e_prc_transfer_and_process,
       /* TIZ_CLASS_COMMENT: */
       tiz_srv_stop_and_return, h264e_prc_stop_and_return,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_buffers_ready, h264e_prc_buffers_ready,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_flush, h264e_prc_port_flush,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_disable, h264e_prc_port_disable,
       /* TIZ_CLASS_COMMENT: */
       tiz_prc_port_enable, h264e_prc_port_enable,
       /* TIZ_CLASS_COMMENT: stop value*/
       0);

    return h264eprc;
}
