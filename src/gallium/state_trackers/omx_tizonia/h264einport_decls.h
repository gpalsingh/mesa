#ifndef H264EINPORT_DECLS_H
#define H264EINPORT_DECLS_H

#include <OMX_TizoniaExt.h>
#include <tizonia/OMX_Types.h>

#include <tizvideoport_decls.h>

typedef struct h264e_inport h264e_inport_t;
struct h264e_inport
{
   /* Object */
   const tiz_videoport_t _;
};

typedef struct h264e_inport_class h264e_inport_class_t;
struct h264e_inport_class
{
   /* Class */
   const tiz_videoport_class_t _;
   /* NOTE: Class methods might be added in the future */
};

#endif /* H264EINPORT_DECLS_H */
