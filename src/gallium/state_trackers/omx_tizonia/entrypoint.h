#ifndef OMX_TIZ_ENTRYPOINT_H
#define OMX_TIZ_ENTRYPOINT_H

#include "vl/vl_winsys.h"

PUBLIC OMX_ERRORTYPE OMX_ComponentInit (OMX_HANDLETYPE ap_hdl);

struct vl_screen *omx_get_screen(void);
void omx_put_screen(void);

#endif
