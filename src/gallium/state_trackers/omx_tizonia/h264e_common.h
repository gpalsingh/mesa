#ifndef H264E_COMMON_H
#define H264E_COMMON_H

#include "util/list.h"
#include "util/u_memory.h"

#include "h264eprc_decls.h"

struct input_buf_private {
   struct list_head tasks;

   struct pipe_resource *resource;
   struct pipe_transfer *transfer;
};

struct output_buf_private {
   struct pipe_resource *bitstream;
   struct pipe_transfer *transfer;
};

h264e_prc_t * get_prc_from_handle(OMX_HANDLETYPE ap_hdl);
void enc_ReleaseTasks(struct list_head *head);

#endif
