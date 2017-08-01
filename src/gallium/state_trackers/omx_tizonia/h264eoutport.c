#include <assert.h>
#include <string.h>
#include <limits.h>

#include <tizplatform.h>

#include "vl/vl_winsys.h"

#include "h264eoutport.h"
#include "h264eoutport_decls.h"
#include "h264e_common.h"

/*
 * h264eoutport class
 */

static void * h264e_outport_ctor(void * ap_obj, va_list * app)
{
   return super_ctor(typeOf(ap_obj, "h264eoutport"), ap_obj, app);
}

static void * h264e_outport_dtor(void * ap_obj)
{
   return super_dtor(typeOf(ap_obj, "h264eoutport"), ap_obj);
}

/*
 * from tiz_api
 */

static OMX_ERRORTYPE h264e_outport_AllocateBuffer(const void * ap_obj, OMX_HANDLETYPE ap_hdl,
                                                  OMX_BUFFERHEADERTYPE ** buf, OMX_U32 idx,
                                                  OMX_PTR private, OMX_U32 size)
{
   OMX_ERRORTYPE r;

   r = super_UseBuffer(typeOf(ap_obj, "h264eoutport"), ap_obj, ap_hdl,
                            buf, idx, private, size, NULL);
   if (r)
      return r;

   (*buf)->pBuffer = NULL;
   (*buf)->pOutputPortPrivate = CALLOC(1, sizeof(struct output_buf_private));
   if (!(*buf)->pOutputPortPrivate) {
      super_FreeBuffer(typeOf(ap_obj, "h264eoutport"), ap_obj, ap_hdl, idx, *buf);
      return OMX_ErrorInsufficientResources;
   }

   return OMX_ErrorNone;
}

static OMX_ERRORTYPE h264e_outport_FreeBuffer(const void * ap_obj, OMX_HANDLETYPE ap_hdl,
                                              OMX_U32 idx, OMX_BUFFERHEADERTYPE *buf)
{
   h264e_prc_t *p_prc = tiz_get_prc(ap_hdl);
   struct output_buf_private *outp = buf->pOutputPortPrivate;

   if (outp) {
      if (outp->transfer)
         pipe_transfer_unmap(p_prc->t_pipe, outp->transfer);
      pipe_resource_reference(&outp->bitstream, NULL);
      FREE(outp);
      buf->pOutputPortPrivate = NULL;
   }
   buf->pBuffer = NULL;

   return super_FreeBuffer(typeOf(ap_obj, "h264eoutport"), ap_obj, ap_hdl, idx, buf);
}

/*
 * h264e_outport_class
 */

static void * h264e_outport_class_ctor(void * ap_obj, va_list * app)
{
    /* NOTE: Class methods might be added in the future. None for now. */
    return super_ctor(typeOf(ap_obj, "h264eoutport_class"), ap_obj, app);
}

/*
 * initialization
 */

void * h264e_outport_class_init(void * ap_tos, void * ap_hdl)
{
   void * tizavcport = tiz_get_type(ap_hdl, "tizavcport");
   void * h264eoutport_class
     = factory_new (classOf(tizavcport), "h264eoutport_class",
                    classOf(tizavcport), sizeof(h264e_outport_class_t),
                    ap_tos, ap_hdl, ctor, h264e_outport_class_ctor, 0);
   return h264eoutport_class;
}

void * h264e_outport_init(void * ap_tos, void * ap_hdl)
{
   void * tizavcport = tiz_get_type(ap_hdl, "tizavcport");
   void * h264eoutport_class = tiz_get_type(ap_hdl, "h264eoutport_class");
   void * h264eoutport = factory_new
     /* TIZ_CLASS_COMMENT: class type, class name, parent, size */
     (h264eoutport_class, "h264eoutport", tizavcport,
      sizeof(h264e_outport_t),
      /* TIZ_CLASS_COMMENT: class constructor */
      ap_tos, ap_hdl,
      /* TIZ_CLASS_COMMENT: class constructor */
      ctor, h264e_outport_ctor,
      /* TIZ_CLASS_COMMENT: class destructor */
      dtor, h264e_outport_dtor,
      /* TIZ_CLASS_COMMENT: */
      tiz_api_AllocateBuffer, h264e_outport_AllocateBuffer,
      /* TIZ_CLASS_COMMENT: */
      tiz_api_FreeBuffer, h264e_outport_FreeBuffer,
      /* TIZ_CLASS_COMMENT: stop value*/
      0);

   return h264eoutport;
}
