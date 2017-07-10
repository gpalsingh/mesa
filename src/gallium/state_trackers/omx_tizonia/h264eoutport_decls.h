#ifndef H264EOUTPORT_DECLS_H
#define H264EOUTPORT_DECLS_H

#include <OMX_TizoniaExt.h>
#include <OMX_Types.h>

#include <tizavcport_decls.h>

typedef struct h264e_outport h264e_outport_t;
struct h264e_outport
{
  /* Object */
  const tiz_avcport_t _;
};

typedef struct h264e_outport_class h264e_outport_class_t;
struct h264e_outport_class
{
  /* Class */
  const tiz_avcport_class_t _;
  /* NOTE: Class methods might be added in the future */
};

#endif /* H264EOUTPORT_DECLS_H */
