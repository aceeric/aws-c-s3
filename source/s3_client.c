/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_request.h"

#include <aws/common/clock.h>
#include <aws/common/string.h>

#include <aws/auth/credentials.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_config.h>
#include <aws/auth/signing_result.h>

#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>

#include <aws/common/atomics.h>
#include <aws/io/socket.h>

#include <inttypes.h>

static void s_s3_client_dec_shutdown_wait_count(struct aws_s3_client *client);
static void s_s3_client_connection_manager_shutdown_callback(void *user_data);

static void s_s3_client_signing_complete(struct aws_signing_result *result, int error_code, void *user_data);
static void s_s3_client_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data);

/* For making requests, the client uses these callbacks for HTTP requests, which will call into the actual
 * aws_s3_request functions.  We could potentially call the s3-request functions directly, but this will allow us to do
 * any client level processing per request if needed. */
static void s_s3_client_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data);
static int s_s3_client_incoming_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *headers,
    size_t num_headers,
    void *user_data);
static int s_s3_client_incoming_header_block_done(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data);
static int s_s3_client_incoming_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data);
static void s_s3_client_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data);

struct aws_s3_client *aws_s3_client_new(
    struct aws_allocator *allocator,
    const struct aws_s3_client_config *client_config) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client_config);

    if (client_config->client_bootstrap == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; client_bootstrap provided in options is invalid.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (client_config->credentials_provider == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "Cannot create client from client_config; credentials_provider provided in options is invalid.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_s3_client *client = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_client));

    if (client == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "Could not allocate aws_s3_client");
        return NULL;
    }

    client->allocator = allocator;
    aws_atomic_init_int(&client->ref_count, 1);

    client->client_bootstrap = client_config->client_bootstrap;

    client->credentials_provider = client_config->credentials_provider;
    aws_credentials_provider_acquire(client->credentials_provider);

    client->shutdown_callback = client_config->shutdown_callback;
    client->shutdown_callback_user_data = client_config->shutdown_callback_user_data;
    aws_atomic_init_int(&client->shutdown_wait_count, 0);

    /* Make a copy of the region string. */
    client->region = aws_string_new_from_array(allocator, client_config->region.ptr, client_config->region.len);

    if (client->region == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not allocate aws_s3_client region string", (void *)client);
        goto error_clean_up;
    }

    /* Make a copy of the endpoint string. */
    client->endpoint = aws_string_new_from_array(allocator, client_config->endpoint.ptr, client_config->endpoint.len);

    if (client->endpoint == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not allocate aws_s3_client endpoint string", (void *)client);
        goto error_clean_up;
    }

    /* Setup the connection manager */
    {
        const uint16_t port = 80;       // TODO
        const int max_connections = 10; // TODO
        const int timeout_seconds = 3;  // TODO

        struct aws_socket_options socket_options;
        AWS_ZERO_STRUCT(socket_options);
        socket_options.type = AWS_SOCKET_STREAM;
        socket_options.domain = AWS_SOCKET_IPV4;
        socket_options.connect_timeout_ms =
            (uint32_t)aws_timestamp_convert(timeout_seconds, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

        struct aws_http_connection_manager_options manager_options;
        AWS_ZERO_STRUCT(manager_options);
        manager_options.bootstrap = client->client_bootstrap;
        manager_options.initial_window_size = SIZE_MAX;
        manager_options.socket_options = &socket_options;
        manager_options.tls_connection_options = NULL;
        manager_options.proxy_options = NULL;
        manager_options.host = aws_byte_cursor_from_string(client->endpoint);
        manager_options.port = port;
        manager_options.max_connections = max_connections;
        manager_options.shutdown_complete_callback = s_s3_client_connection_manager_shutdown_callback;
        manager_options.shutdown_complete_user_data = client;

        client->connection_manager = aws_http_connection_manager_new(allocator, &manager_options);

        if (client->connection_manager == NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_S3_CLIENT, "id=%p: Could not allocate aws_s3_client connection manager", (void *)client);
            goto error_clean_up;
        }

        aws_atomic_fetch_add(&client->shutdown_wait_count, 1);
    }

    return client;

error_clean_up:

    if (client != NULL) {
        aws_s3_client_release(client);
        client = NULL;
    }

    return NULL;
}

void aws_s3_client_acquire(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    aws_atomic_fetch_add(&client->ref_count, 1);
}

void aws_s3_client_release(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    size_t new_ref_count = aws_atomic_fetch_sub(&client->ref_count, 1) - 1;

    if (new_ref_count > 0) {
        return;
    }

    if (client->credentials_provider != NULL) {
        aws_credentials_provider_release(client->credentials_provider);
        client->credentials_provider = NULL;
    }

    if (client->connection_manager != NULL) {
        aws_http_connection_manager_release(client->connection_manager);
        client->connection_manager = NULL;
    }

    if (client->region != NULL) {
        aws_string_destroy(client->region);
        client->region = NULL;
    }

    if (client->endpoint != NULL) {
        aws_string_destroy(client->endpoint);
        client->endpoint = NULL;
    }
}

static void s_s3_client_dec_shutdown_wait_count(struct aws_s3_client *client) {
    AWS_PRECONDITION(client);

    size_t new_count = aws_atomic_fetch_sub(&client->shutdown_wait_count, 1) - 1;

    /* If there are still systems shutting down, don't trigger the shutdown callback yet. */
    if (new_count > 0) {
        return;
    }

    aws_s3_client_shutdown_complete_callback_fn *shutdown_callback = client->shutdown_callback;
    void *shutdown_user_data = client->shutdown_callback_user_data;

    aws_mem_release(client->allocator, client);
    client = NULL;

    shutdown_callback(shutdown_user_data);
}

static void s_s3_client_connection_manager_shutdown_callback(void *user_data) {
    s_s3_client_dec_shutdown_wait_count(user_data);
}

int s3_client_make_request(struct aws_s3_client *client, struct aws_s3_request *request) {

    AWS_PRECONDITION(client);
    AWS_PRECONDITION(request);

    /* Setup signing of request */
    request->signable = aws_signable_new_http_request(client->allocator, request->message);

    if (request->signable == NULL) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not allocate signable for http request", (void *)client);
        return AWS_OP_ERR;
    }

    struct aws_date_time now;
    aws_date_time_init_now(&now);

    struct aws_byte_cursor service_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("s3");

    struct aws_signing_config_aws signing_config;
    AWS_ZERO_STRUCT(signing_config);
    signing_config.config_type = AWS_SIGNING_CONFIG_AWS;
    signing_config.algorithm = AWS_SIGNING_ALGORITHM_V4;
    signing_config.credentials_provider = client->credentials_provider;
    signing_config.region = aws_byte_cursor_from_array(client->region->bytes, client->region->len);
    signing_config.service = service_name;
    signing_config.date = now;
    signing_config.signed_body_value = g_aws_signed_body_value_unsigned_payload;
    signing_config.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;

    if (aws_sign_request_aws(
            client->allocator,
            request->signable,
            (struct aws_signing_config_base *)&signing_config,
            s_s3_client_signing_complete,
            request)) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not sign request", (void *)client);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_s3_client_signing_complete(struct aws_signing_result *result, int error_code, void *user_data) {
    AWS_PRECONDITION(result);
    AWS_PRECONDITION(user_data);

    struct aws_s3_request *request = user_data;
    struct aws_s3_client *client = request->client;

    if (error_code != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT, "id=%p: Could not sign request due to error_code %d", (void *)client, error_code);
        goto error_clean_up;
    }

    if (aws_apply_signing_result_to_http_request(request->message, client->allocator, result)) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p: Could not apply signing result to http request due to error %d",
            (void *)client,
            error_code);
        goto error_clean_up;
    }

    aws_http_connection_manager_acquire_connection(
        client->connection_manager, s_s3_client_on_acquire_connection, request);

    return;

error_clean_up:
    aws_s3_request_finish(request, error_code);
}

static void s_s3_client_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data) {
    AWS_PRECONDITION(connection);
    AWS_PRECONDITION(user_data);

    struct aws_s3_request *request = user_data;
    struct aws_s3_client *client = request->client;

    if (error_code != AWS_ERROR_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_CLIENT,
            "id=%p: Could not acquire connection due to error code %d (%s)",
            (void *)client,
            error_code,
            aws_error_str(error_code));
        goto error_clean_up;
    }

    struct aws_http_make_request_options options;
    AWS_ZERO_STRUCT(options);
    options.self_size = sizeof(struct aws_http_make_request_options);
    options.request = request->message;
    options.user_data = request;
    options.on_response_headers = s_s3_client_incoming_headers;
    options.on_response_header_block_done = s_s3_client_incoming_header_block_done;
    options.on_response_body = s_s3_client_incoming_body;
    options.on_complete = s_s3_client_stream_complete;

    request->stream = aws_http_connection_make_request(connection, &options);

    if (request->stream == NULL) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not make HTTP request", (void *)client);
        goto error_clean_up;
    }

    if (aws_http_stream_activate(request->stream) != AWS_OP_SUCCESS) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "id=%p: Could not activate HTTP stream", (void *)client);
        goto error_clean_up;
    }

    return;

error_clean_up:
    aws_s3_request_finish(request, error_code);
}

static int s_s3_client_incoming_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *headers,
    size_t num_headers,
    void *user_data) {

    (void)stream;

    AWS_PRECONDITION(user_data);
    struct aws_s3_request *request = user_data;

    return aws_s3_request_incoming_headers(request, header_block, headers, num_headers);
}

static int s_s3_client_incoming_header_block_done(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data) {

    (void)stream;

    AWS_PRECONDITION(user_data);
    struct aws_s3_request *request = user_data;

    return aws_s3_request_incoming_header_block_done(request, header_block);
}

static int s_s3_client_incoming_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {

    (void)stream;

    AWS_PRECONDITION(user_data);
    struct aws_s3_request *request = user_data;

    return aws_s3_request_incoming_body(request, data);
}

static void s_s3_client_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data) {
    (void)stream;

    AWS_PRECONDITION(user_data);
    struct aws_s3_request *request = user_data;

    aws_s3_request_stream_complete(request, error_code);

    aws_s3_request_finish(request, error_code);
}
