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

#ifndef VL_ST_COMMON_H
#define VL_ST_COMMON_H

#include "vl_winsys.h"

typedef vid_dec_PrivateType vid_dec_PrivateType_;
typedef struct h264d_prc_t h264d_prc_t_;

enum lib_type {
   OMX_MESA_USE_BELLAGIO = 0,
   OMX_MESA_USE_TIZONIA
};

struct vl_screen *get_screen(void);
void put_screen(void);
void slice_header_h264(void *prc, struct vl_rbsp *rbsp,
                       unsigned nal_ref_idc, unsigned nal_unit_type,
                       enum lib_type prc_type);

#endif
