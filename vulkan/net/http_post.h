/*
 * ProsperoAI - one HTTP POST with its answer read as it arrives.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * For text models answered by another machine (vulkan/remote_model.cpp): the request
 * goes to a chat server on the local network, usually plain http, and the answer
 * streams back as server-sent events. Built on the same libcurl as https_get.c.
 */
#ifndef PROSPERO_HTTP_POST_H
#define PROSPERO_HTTP_POST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

    /* Receives each piece of the answer's body. Returning anything other than `size`
     * ends the request. */
    typedef size_t (*prospero_http_body_fn)(void *user, const char *data, size_t size);

    /* POSTs `body` (JSON) to `url`, http or https, sending `bearer` as an
     * Authorization bearer key when it is not NULL or empty. Waits up to ten seconds
     * for the connection and gives up after two minutes in which nothing arrives.
     * 0 and the HTTP status on an answer, whatever its status; nonzero and a message
     * in `error` when no answer came. */
    int prospero_http_post(const char *url, const char *bearer, const char *body, size_t body_size,
                           prospero_http_body_fn on_body, void *user, long *status, char *error,
                           size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* PROSPERO_HTTP_POST_H */
