#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER cmb_copy

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "./cmb_copy-tp.h"

#if !defined(_CMB_COPY_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _CMB_COPY_TP_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
    /* Tracepoint provider name */
    cmb_copy,
    /* Tracepoint name */
    my_first_tracepoint,
    /* Input arguments */
    TP_ARGS(
        int, my_integer_arg,
        char*, my_string_arg
    ),
    /* Output event fields */
    TP_FIELDS(
        ctf_string(my_string_field, my_string_arg)
        ctf_integer(int, my_integer_field, my_integer_arg)
    )
)

#endif /* _CMB_COPY_TP_H */

#include <lttng/tracepoint-event.h>