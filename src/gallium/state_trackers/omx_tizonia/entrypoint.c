#include <assert.h>
#include <string.h>
#include <stdbool.h>

#if defined(HAVE_X11_PLATFORM)
#include <X11/Xlib.h>
#else
#define XOpenDisplay(x) NULL
#define XCloseDisplay(x)
#define Display void
#endif

#include "os/os_thread.h"
#include "util/u_memory.h"
#include "loader/loader.h"

#include <tizplatform.h>
#include <tizport.h>
#include <tizscheduler.h>

#include "entrypoint.h"
#include "h264d.h"
#include "h264dprc.h"

static mtx_t omx_lock = _MTX_INITIALIZER_NP;
static Display *omx_display = NULL;
static struct vl_screen *omx_screen = NULL;
static unsigned omx_usecount = 0;
static const char *omx_render_node = NULL;
static int drm_fd;

OMX_ERRORTYPE OMX_ComponentInit (OMX_HANDLETYPE ap_hdl)
{
    tiz_role_factory_t role_factory;
    const tiz_role_factory_t * rf_list[] = {&role_factory};
    tiz_type_factory_t h264dprc_type;
    const tiz_type_factory_t * tf_list[] = {&h264dprc_type};

    strcpy ((OMX_STRING) role_factory.role, OMX_VID_DEC_AVC_ROLE);
    role_factory.pf_cport = instantiate_h264_config_port;
    role_factory.pf_port[0] = instantiate_h264_input_port;
    role_factory.pf_port[1] = instantiate_h264_output_port;
    role_factory.nports = 2;
    role_factory.pf_proc = instantiate_h264_processor;

    strcpy ((OMX_STRING) h264dprc_type.class_name, "h264dprc_class");
    h264dprc_type.pf_class_init = h264d_prc_class_init;
    strcpy ((OMX_STRING) h264dprc_type.object_name, "h264dprc");
    h264dprc_type.pf_object_init = h264d_prc_init;

    /* Initialize the component infrastructure */
    tiz_comp_init (ap_hdl, OMX_VID_DEC_AVC_NAME);

    /* Register the "h264dprc" class */
    tiz_comp_register_types (ap_hdl, tf_list, 1);

    /* Register the component role */
    tiz_comp_register_roles (ap_hdl, rf_list, 1);

    return OMX_ErrorNone;
}
struct vl_screen *omx_get_screen(void)
{
   static bool first_time = true;
   mtx_lock(&omx_lock);

   if (!omx_screen) {
      if (first_time) {
         omx_render_node = debug_get_option("OMX_RENDER_NODE", NULL);
         first_time = false;
      }
      if (omx_render_node) {
         drm_fd = loader_open_device(omx_render_node);
         if (drm_fd < 0)
            goto error;

         omx_screen = vl_drm_screen_create(drm_fd);
         if (!omx_screen) {
            close(drm_fd);
            goto error;
         }
      } else {
         omx_display = XOpenDisplay(NULL);
         if (!omx_display)
            goto error;

         omx_screen = vl_dri3_screen_create(omx_display, 0);
         if (!omx_screen)
            omx_screen = vl_dri2_screen_create(omx_display, 0);
         if (!omx_screen) {
            XCloseDisplay(omx_display);
            goto error;
         }
      }
   }

   ++omx_usecount;

   mtx_unlock(&omx_lock);
   return omx_screen;

error:
   mtx_unlock(&omx_lock);
   return NULL;
}

void omx_put_screen(void)
{
   mtx_lock(&omx_lock);
   if ((--omx_usecount) == 0) {
      omx_screen->destroy(omx_screen);
      omx_screen = NULL;

      if (omx_render_node)
         close(drm_fd);
      else
         XCloseDisplay(omx_display);
   }
   mtx_unlock(&omx_lock);
}
