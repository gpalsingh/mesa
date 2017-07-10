#include "h264e_common.h"

void enc_ReleaseTasks (struct list_head *head)
{
    struct encode_task *i, *next;

    if (!head || !head->next)
        return;

    LIST_FOR_EACH_ENTRY_SAFE(i, next, head, list) {
        pipe_resource_reference(&i->bitstream, NULL);
        i->buf->destroy(i->buf);
        FREE(i);
    }
}
