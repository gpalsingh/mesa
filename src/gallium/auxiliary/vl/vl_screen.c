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
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <assert.h>
#include <string.h>
#include <stdbool.h>

#include "os/os_thread.h"
#include "util/u_memory.h"
#include "loader/loader.h"
#include "vl_screen.h"

#if defined(HAVE_X11_PLATFORM)
#include <X11/Xlib.h>
#else
#define XOpenDisplay(x) NULL
#define XCloseDisplay(x)
#define Display void
#endif

static mtx_t st_lock = _MTX_INITIALIZER_NP;
static Display *st_display = NULL;
static struct vl_screen *st_screen = NULL;
static unsigned st_usecount = 0;
static const char *st_render_node = NULL;
static int drm_fd;

struct vl_screen *vl_get_screen(const char* render_node)
{
   static bool first_time = true;
   mtx_lock(&st_lock);

   if (!st_screen) {
      if (first_time) {
         st_render_node = debug_get_option(render_node, NULL);
         first_time = false;
      }
      if (st_render_node) {
         drm_fd = loader_open_device(st_render_node);
         if (drm_fd < 0)
            goto error;

         st_screen = vl_drm_screen_create(drm_fd);
         if (!st_screen) {
            close(drm_fd);
            goto error;
         }
      } else {
         st_display = XOpenDisplay(NULL);
         if (!st_display)
            goto error;

         st_screen = vl_dri3_screen_create(st_display, 0);
         if (!st_screen)
            st_screen = vl_dri2_screen_create(st_display, 0);
         if (!st_screen) {
            XCloseDisplay(st_display);
            goto error;
         }
      }
   }

   ++st_usecount;

   mtx_unlock(&st_lock);
   return st_screen;

error:
   mtx_unlock(&st_lock);
   return NULL;
}

void vl_put_screen(void)
{
   mtx_lock(&st_lock);
   if ((--st_usecount) == 0) {
      st_screen->destroy(st_screen);
      st_screen = NULL;

      if (st_render_node)
         close(drm_fd);
      else
         XCloseDisplay(st_display);
   }
   mtx_unlock(&st_lock);
}
