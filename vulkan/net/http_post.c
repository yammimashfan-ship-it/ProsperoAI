/*
 * ProsperoAI - one HTTP POST with its answer read as it arrives.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "http_post.h"

#include "console_curl.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

struct post_state
{
    prospero_http_body_fn on_body;
    void *user;
};

static pthread_once_t global_once = PTHREAD_ONCE_INIT;
static CURLcode global_result;

static void global_start(void)
{
    global_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

static size_t on_data(char *bytes, size_t size, size_t count, void *user)
{
    struct post_state *state = user;
    const size_t length = size * count;
    if (length == 0)
        return 0;
    return state->on_body(state->user, bytes, length);
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size)
        snprintf(error, error_size, "%s", message);
}

int prospero_http_post(const char *url, const char *bearer, const char *body, size_t body_size,
                       prospero_http_body_fn on_body, void *user, long *status, char *error,
                       size_t error_size)
{
    if (status)
        *status = 0;
    set_error(error, error_size, "");
    if (!url || !body || !on_body)
    {
        set_error(error, error_size, "bad request");
        return 1;
    }
    pthread_once(&global_once, global_start);
    if (global_result != CURLE_OK)
    {
        set_error(error, error_size, "libcurl could not start");
        return 1;
    }

    char curl_error[CURL_ERROR_SIZE] = {0};
    char authorization[512];
    struct post_state state = {on_body, user};
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: text/event-stream, application/json");
    if (bearer && bearer[0])
    {
        snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, authorization);
    }

    CURLM *multi = curl_multi_init();
    CURL *easy = curl_easy_init();
    if (!multi || !easy || !headers)
    {
        set_error(error, error_size, "libcurl could not make a request");
        if (easy)
            curl_easy_cleanup(easy);
        if (multi)
            curl_multi_cleanup(multi);
        curl_slist_free_all(headers);
        return 1;
    }
    console_curl_setup(easy);
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, (long)CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_URL, url);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, "ProsperoAI/1.0");
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT, 10L);
    /* The server sends a comment line every few seconds while the model thinks, so a
     * connection that brings nothing for two minutes has failed. */
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(easy, CURLOPT_POST, 1L);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_size);
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, on_data);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, curl_error);

    int failed = 0;
    if (curl_multi_add_handle(multi, easy) != CURLM_OK)
    {
        set_error(error, error_size, "libcurl could not start the request");
        failed = 1;
    }
    else
    {
        CURLcode result = CURLE_OK;
        int done = 0;
        while (!done)
        {
            int running = 0;
            CURLMcode code = curl_multi_perform(multi, &running);
            if (code == CURLM_OK && running)
                code = curl_multi_poll(multi, NULL, 0, 250, NULL);
            if (code != CURLM_OK)
            {
                set_error(error, error_size, curl_multi_strerror(code));
                failed = 1;
                break;
            }
            int left = 0;
            for (CURLMsg *message; (message = curl_multi_info_read(multi, &left)) != NULL;)
                if (message->msg == CURLMSG_DONE && message->easy_handle == easy)
                {
                    result = message->data.result;
                    done = 1;
                }
            if (!running && !done)
            {
                set_error(error, error_size, "the request ended without an answer");
                failed = 1;
                break;
            }
        }
        if (!failed && result != CURLE_OK)
        {
            set_error(error, error_size, curl_error[0] ? curl_error : curl_easy_strerror(result));
            failed = 1;
        }
        long code = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);
        if (status)
            *status = code;
        /* An answer that was cut short still counts as an answer: the caller has what
         * arrived and the status says what the server meant. */
        if (failed && code != 0 && result == CURLE_WRITE_ERROR)
            failed = 0;
        curl_multi_remove_handle(multi, easy);
    }
    curl_easy_cleanup(easy);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headers);
    return failed;
}
