/*
 * A partial implementation of HTTP/1.0
 *
 * This code is mainly intended as a replacement for the book's 'tiny.c' server
 * It provides a *partial* implementation of HTTP/1.0 which can form a basis for
 * the assignment.
 *
 * @author G. Back for CS 3214 Spring 2018
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/limits.h>
#include <jansson.h>

#include "http.h"
#include "hexdump.h"
#include "socket.h"
#include "bufio.h"
#include "main.h"
#include <jwt.h>
// Need macros here because of the sizeof
#define CRLF "\r\n"
#define CR "\r"
#define STARTS_WITH(field_name, header) \
    (!strncasecmp(field_name, header, sizeof(header) - 1))

static bool
handle_api(struct http_transaction *ta, char *req_path);

static const char * KEY = "key";

/* Parse HTTP request line, setting req_method, req_path, and req_version. */
static bool
http_parse_request(struct http_transaction *ta)
{
    size_t req_offset;
    ssize_t len = bufio_readline(ta->client->bufio, &req_offset);
    if (len < 2)       // error, EOF, or less than 2 characters
        return false;

    char *request = bufio_offset2ptr(ta->client->bufio, req_offset);
    request[len-2] = '\0';  // replace LF with 0 to ensure zero-termination
    char *endptr;
    char *method = strtok_r(request, " ", &endptr);
    if (method == NULL)
        return false;

    if (!strcmp(method, "GET"))
        ta->req_method = HTTP_GET;
    else if (!strcmp(method, "POST"))
        ta->req_method = HTTP_POST;
    else
        ta->req_method = HTTP_UNKNOWN;

    char *req_path = strtok_r(NULL, " ", &endptr);
    if (req_path == NULL)
        return false;

    ta->req_path = bufio_ptr2offset(ta->client->bufio, req_path);

    char *http_version = strtok_r(NULL, CR, &endptr);
    if (http_version == NULL)  // would be HTTP 0.9
        return false;

    // record client's HTTP version in request
    if (!strcmp(http_version, "HTTP/1.1"))
        ta->req_version = HTTP_1_1;
    else if (!strcmp(http_version, "HTTP/1.0"))
        ta->req_version = HTTP_1_0;
    else
        return false;

    return true;
}

/* Process HTTP headers. */
static bool
http_process_headers(struct http_transaction *ta)
{
    for (;;) {
        size_t header_offset;
        ssize_t len = bufio_readline(ta->client->bufio, &header_offset);
        if (len <= 0)
            return false;

        char *header = bufio_offset2ptr(ta->client->bufio, header_offset);
        if (len == 2 && STARTS_WITH(header, CRLF))       // empty CRLF
            return true;

        header[len-2] = '\0';
        /* Each header field consists of a name followed by a 
         * colon (":") and the field value. Field names are 
         * case-insensitive. The field value MAY be preceded by 
         * any amount of LWS, though a single SP is preferred.
         */
        char *endptr;
        char *field_name = strtok_r(header, ":", &endptr);
        if (field_name == NULL)
            return false;

        // skip white space
        char *field_value = endptr;
        while (*field_value == ' ' || *field_value == '\t')
            field_value++;

        // you may print the header like so
        // printf("Header: %s: %s\n", field_name, field_value);
        if (!strcasecmp(field_name, "Content-Length")) {
            ta->req_content_len = atoi(field_value);
        }

        /* TODO: Handle other headers here. Both field_value and field_name
         * are zero-terminated strings.
         */
    }
}

const int MAX_HEADER_LEN = 2048;

/* add a formatted header to the response buffer. */
void 
http_add_header(buffer_t * resp, char* key, char* fmt, ...)
{
    va_list ap;

    buffer_appends(resp, key);
    buffer_appends(resp, ": ");

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(resp, MAX_HEADER_LEN);
    int len = vsnprintf(error, MAX_HEADER_LEN, fmt, ap);
    resp->len += len > MAX_HEADER_LEN ? MAX_HEADER_LEN - 1 : len;
    va_end(ap);

    buffer_appends(resp, "\r\n");
}

/* add a content-length header. */
static void
add_content_length(buffer_t *res, size_t len)
{
    http_add_header(res, "Content-Length", "%ld", len);
}

/* start the response by writing the first line of the response 
 * to the response buffer.  Used in send_response_header */
static void
start_response(struct http_transaction * ta, buffer_t *res)
{
    buffer_init(res, 80);

    /* TODO: Hint: you must change this as you implement HTTP/1.1.
     * Respond with the highest version the client supports
     * as indicated in the version field of the request.
     */
    if (ta->req_version == HTTP_1_0)
        buffer_appends(res, "HTTP/1.0 ");
    else 
        buffer_appends(res, "HTTP/1.1 ");

    switch (ta->resp_status) {
    case HTTP_OK:
        buffer_appends(res, "200 OK");
        break;
    case HTTP_PARTIAL_CONTENT:
        buffer_appends(res, "206 Partial Content");
        break;
    case HTTP_BAD_REQUEST:
        buffer_appends(res, "400 Bad Request");
        break;
    case HTTP_PERMISSION_DENIED:
        buffer_appends(res, "403 Permission Denied");
        break;
    case HTTP_NOT_FOUND:
        buffer_appends(res, "404 Not Found");
        break;
    case HTTP_METHOD_NOT_ALLOWED:
        buffer_appends(res, "405 Method Not Allowed");
        break;
    case HTTP_REQUEST_TIMEOUT:
        buffer_appends(res, "408 Request Timeout");
        break;
    case HTTP_REQUEST_TOO_LONG:
        buffer_appends(res, "414 Request Too Long");
        break;
    case HTTP_NOT_IMPLEMENTED:
        buffer_appends(res, "501 Not Implemented");
        break;
    case HTTP_SERVICE_UNAVAILABLE:
        buffer_appends(res, "503 Service Unavailable");
        break;
    case HTTP_INTERNAL_ERROR:
    default:
        buffer_appends(res, "500 Internal Server Error");
        break;
    }
    buffer_appends(res, CRLF);
}

/* Send response headers to client */
static bool
send_response_header(struct http_transaction *ta)
{
    buffer_t response;
    start_response(ta, &response);
    buffer_appends(&ta->resp_headers, CRLF);

    buffer_t *response_and_headers[2] = {
        &response, &ta->resp_headers
    };

    int rc = bufio_sendbuffers(ta->client->bufio, response_and_headers, 2);
    buffer_delete(&response);
    return rc != -1;
}

/* Send a full response to client with the content in resp_body. */
static bool
send_response(struct http_transaction *ta)
{
    // add content-length.  All other headers must have already been set.
    add_content_length(&ta->resp_headers, ta->resp_body.len);
    buffer_appends(&ta->resp_headers, CRLF);

    buffer_t response;
    start_response(ta, &response);

    buffer_t *response_and_headers[3] = {
        &response, &ta->resp_headers, &ta->resp_body
    };

    int rc = bufio_sendbuffers(ta->client->bufio, response_and_headers, 3);
    buffer_delete(&response);
    return rc != -1;
}

const int MAX_ERROR_LEN = 2048;

/* Send an error response. */
static bool
send_error(struct http_transaction * ta, enum http_response_status status, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    char *error = buffer_ensure_capacity(&ta->resp_body, MAX_ERROR_LEN);
    int len = vsnprintf(error, MAX_ERROR_LEN, fmt, ap);
    ta->resp_body.len += len > MAX_ERROR_LEN ? MAX_ERROR_LEN - 1 : len;
    va_end(ap);
    ta->resp_status = status;
    http_add_header(&ta->resp_headers, "Content-Type", "text/plain");
    return send_response(ta);
}

/* Send Not Found response. */
static bool
send_not_found(struct http_transaction *ta)
{
    return send_error(ta, HTTP_NOT_FOUND, "File %s not found", 
        bufio_offset2ptr(ta->client->bufio, ta->req_path));
}

/* A start at assigning an appropriate mime type.  Real-world 
 * servers use more extensive lists such as /etc/mime.types
 */
static const char *
guess_mime_type(char *filename)
{
    char *suffix = strrchr(filename, '.');
    if (suffix == NULL)
        return "text/plain";

    if (!strcasecmp(suffix, ".html"))
        return "text/html";

    if (!strcasecmp(suffix, ".gif"))
        return "image/gif";

    if (!strcasecmp(suffix, ".png"))
        return "image/png";

    if (!strcasecmp(suffix, ".jpg"))
        return "image/jpeg";

    if (!strcasecmp(suffix, ".js"))
        return "text/javascript";
    //support for svg
    if (!strcasecmp(suffix, ".svg"))
        return "image/svg+xml";
    //support for mp4
    if (!strcasecmp(suffix, ".mp4"))
        return "video/mp4";
    //support for css
    if (!strcasecmp(suffix, ".css"))
        return "text/css";
    /* hint: you need to add support for (at least) .css, .svg, and .mp4 */
    return "text/plain";
}

/* Handle HTTP transaction for static files. */
static bool
handle_static_asset(struct http_transaction *ta, char *basedir)
{
    char fname[PATH_MAX];

    assert (basedir != NULL || !!!"No base directory. Did you specify -R?");
    char *req_path = bufio_offset2ptr(ta->client->bufio, ta->req_path);
    // The code below is vulnerable to an attack.  Can you see
    // which?  FIXME: to avoid indirect object reference (IDOR) attacks.
    snprintf(fname, sizeof fname, "%s%s", basedir, req_path);
    //if "../" is in path say no
    char *contain = "../";
    if (strstr(req_path,contain) != NULL) {
        return send_not_found(ta);
    }

    if (access(fname, R_OK)) {
        if (errno == EACCES)
            return send_error(ta, HTTP_PERMISSION_DENIED, "Permission denied.");
        else
            return send_not_found(ta);
    }
    http_add_header(&ta->resp_headers, "Accept-Ranges", "bytes");
    // Determine file size
    struct stat st;
    int rc = stat(fname, &st);
    if (rc == -1)
        return send_error(ta, HTTP_INTERNAL_ERROR, "Could not stat file.");

    int filefd = open(fname, O_RDONLY);
    if (filefd == -1) {
        return send_not_found(ta);
    }
    ta->resp_status = HTTP_OK;
    http_add_header(&ta->resp_headers, "Content-Type", "%s", guess_mime_type(fname));
    off_t from = 0, to = st.st_size - 1;

    off_t content_length = to + 1 - from;
    add_content_length(&ta->resp_headers, content_length);
    bool success = send_response_header(ta);
    if (!success)
        goto out;
    
    // sendfile may send fewer bytes than requested, hence the loop
    while (success && from <= to)
        success = bufio_sendfile(ta->client->bufio, filefd, &from, to + 1 - from) > 0;

out:
    close(filefd);
    return success;
}

static bool
handle_api(struct http_transaction *ta, char *req_path)
{
    if (strstr(req_path, "login") != NULL) {
        if (ta->req_method == HTTP_POST) {
            json_error_t json_error;
            json_t *json = json_loadb(bufio_offset2ptr(ta->client->bufio, ta->req_body), ta->req_content_len, 0, &json_error);
            const char * username;
            const char * password;

            // Unpack json and authenticate user
            if (json_unpack(json, "{s:s, s:s}", "username", &username, "password", &password) == -1)
                return send_error(ta, HTTP_BAD_REQUEST, "Bad Request");
            if (strcmp(username, "user2023") != 0 || strcmp(password, "passwordf23") != 0)
                return send_error(ta, HTTP_PERMISSION_DENIED, "Forbidden");

            // Setup JSON Web Token signature
            jwt_t *json_web_token;
            if (jwt_new(&json_web_token))
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Server Error");
            // Claims stored in JSON
            time_t current_time = time(NULL);
            if (jwt_add_grant_int(json_web_token, "exp", current_time + token_expiration_time))
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Server Error");
            if (jwt_add_grant_int(json_web_token, "iat", current_time))
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Server Error");
            if (jwt_add_grant(json_web_token, "sub", "user2023"))
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Server Error");

            if (jwt_set_alg(json_web_token, JWT_ALG_HS256, (unsigned char *)KEY, strlen(KEY)))
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Server Error");

            char * json_string = jwt_encode_str(json_web_token);
            if (json_string == NULL)
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Server Error");
            // free(json_web_token); // FIXME: more efficient, correct though?

            // Validate JSON Web Token
            jwt_t *new_json_web_token;
            if (jwt_decode(&new_json_web_token, json_string, (unsigned char *)KEY, strlen(KEY)))
                return send_error(ta, HTTP_PERMISSION_DENIED, "Forbidden");

            char *claims = jwt_get_grants_json(new_json_web_token, NULL); // FIXME: could get grants before set_alg?
            if (claims == NULL)
                return send_error(ta, HTTP_INTERNAL_ERROR, "Internal Error");
            jwt_free(new_json_web_token);

            ta->resp_status = HTTP_OK;
            http_add_header(&ta->resp_headers, "Set-Cookie", "auth_token=%s; Path=/; Max-Age=%d; HttpOnly", json_string, token_expiration_time);
            // http_add_header(&ta->resp_headers, "Set-Cookie", "auth_token=%s; Path=/; Max-Age=%d; HttpOnly; SameSite=Lax", json_string, token_expiration_time); // FIXME: spec says so?
            http_add_header(&ta->resp_headers, "Content-Type", "application/json");
            buffer_appends(&ta->resp_body, claims);
            free(json_string);
            free(json_web_token);

            return send_response(ta);
        } else if (ta->req_method == HTTP_GET) {
            ta->resp_status = HTTP_OK;
            buffer_appends(&ta->resp_body, "{}");
            return send_response(ta);
        } else {
            return send_error(ta, HTTP_NOT_IMPLEMENTED, "API not implemented");
        }
    } else if (strstr(req_path, "logout") != NULL) {

    }

    return send_error(ta, HTTP_NOT_FOUND, "API not found");
}

/* Set up an http client, associating it with a bufio buffer. */
void 
http_setup_client(struct http_client *self, struct bufio *bufio)
{
    self->bufio = bufio;
}

/* Handle a single HTTP transaction.  Returns true on success. */
bool
http_handle_transaction(struct http_client *self)
{
    struct http_transaction ta;
    memset(&ta, 0, sizeof ta);
    ta.client = self;

    if (!http_parse_request(&ta))
        return false;

    if (!http_process_headers(&ta))
        return false;

    if (ta.req_content_len > 0) {
        int rc = bufio_read(self->bufio, ta.req_content_len, &ta.req_body);
        if (rc != ta.req_content_len)
            return false;

        // To see the body, use this:
        char *body = bufio_offset2ptr(ta.client->bufio, ta.req_body);
        hexdump(body, ta.req_content_len);
    }

    buffer_init(&ta.resp_headers, 1024);
    http_add_header(&ta.resp_headers, "Server", "CS3214-Personal-Server");
    buffer_init(&ta.resp_body, 0);

    bool rc = false;
    char *req_path = bufio_offset2ptr(ta.client->bufio, ta.req_path);
    if (STARTS_WITH(req_path, "/api")) {
        rc = handle_api(&ta, req_path);
    } else
    if (STARTS_WITH(req_path, "/private")) {
        /* TODO: not implemented */
    } else {
        rc = handle_static_asset(&ta, server_root);
    }

    buffer_delete(&ta.resp_headers);
    buffer_delete(&ta.resp_body);
    //makes sure its HTTP_1_1
    if(ta.req_version != HTTP_1_1){
        return false;
    } else {
        return rc;
    }
    
}
