#ifndef H264D_H
#define H264D_H

#include <tizonia/OMX_Core.h>
#include <tizonia/OMX_Types.h>
#include <tizonia/OMX_Video.h>

#define OMX_VID_DEC_AVC_DEFAULT_FRAME_WIDTH      176
#define OMX_VID_DEC_AVC_DEFAULT_FRAME_HEIGHT     144
#define OMX_VID_DEC_AVC_DEFAULT_FRAME_RATE 15<<16
#define OMX_VID_DEC_AVC_ROLE "video_decoder.avc"
/* With libtizonia, port indexes must start at index 0 */
#define OMX_VID_DEC_AVC_INPUT_PORT_INDEX               0
#define OMX_VID_DEC_AVC_OUTPUT_PORT_INDEX              1
#define OMX_VID_DEC_AVC_INPUT_PORT_MIN_BUF_COUNT       8
#define OMX_VID_DEC_AVC_OUTPUT_PORT_MIN_BUF_COUNT      4
/* 38016 = (width * height) + ((width * height)/2) */
#define OMX_VID_DEC_AVC_PORT_MIN_INPUT_BUF_SIZE  38016
#define OMX_VID_DEC_AVC_PORT_MIN_OUTPUT_BUF_SIZE 345600
#define OMX_VID_DEC_AVC_PORT_NONCONTIGUOUS       OMX_FALSE
#define OMX_VID_DEC_AVC_PORT_ALIGNMENT           0
#define OMX_VID_DEC_AVC_PORT_SUPPLIERPREF        OMX_BufferSupplyInput
#define OMX_VID_DEC_AVC_TIMESTAMP_INVALID ((OMX_TICKS) -1)

OMX_PTR instantiate_h264d_config_port(OMX_HANDLETYPE ap_hdl);
OMX_PTR instantiate_h264d_input_port(OMX_HANDLETYPE ap_hdl);
OMX_PTR instantiate_h264d_output_port(OMX_HANDLETYPE ap_hdl);
OMX_PTR instantiate_h264d_processor(OMX_HANDLETYPE ap_hdl);

#endif                          /* H264D_H */
