#include "sandia.h"

#include <stdio.h>
#include <errno.h>

#define MAX_REQUEST_LEN (1024*16)
#define MAX_HEADER_COUNT (256)

sandia sandia_create( char* host, uint32_t port ) {
    sandia _sandia;
    _sandia.last_error = success;
    _sandia._is_valid = false;

    _sandia.last_error = sandia_setup_socket(&_sandia, host, port);
    _sandia._is_valid = (_sandia.last_error == success);
    _sandia._sandia_socket.receive_buffer_size = 1024;

    _sandia._headers = (sandia_header*) calloc(MAX_HEADER_COUNT, sizeof (sandia_header) * MAX_HEADER_COUNT);
    _sandia._header_count = 0;

    _sandia.version = HTTP_11;

    return _sandia;
}

void sandia_close( sandia* s ) {
    //freeaddrinfo(s->_sandia_socket._host);
    //freeaddrinfo(s->_sandia_socket._info);
    close(s->_sandia_socket._fd);

    free(s->_headers);
    s->_header_count = 0;

    free(s->_request);
    s->_request_length = 0;

    free(s->_sandia_socket.host_address);
    s->_sandia_socket.port = 0;

#ifdef _WIN32
    WSACleanup();
#endif
}

sandia_error sandia_setup_socket( sandia* s, char* host, uint32_t port ) {
    sandia_error r = success;

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &s->_sandia_socket.wsa) != 0) {
        return error_create_socket;
    }
#endif

    sandia_socket _socket;
    size_t host_length = strlen(host);
    _socket.host_address = (char*) calloc(host_length + 1, sizeof (char));
    strcpy(_socket.host_address, host);

    _socket.port = port;

    _socket._fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_socket._fd == -1) {
        return error_create_socket;
    }

    _socket._host = gethostbyname(_socket.host_address);
    if (_socket._host == NULL) {
        return error_host_name;
    }

    _socket._addresses = (struct in_addr **) _socket._host->h_addr_list;
    _socket.ip = (char*) calloc(128, sizeof (char));
    for (int i = 0; _socket._addresses[i] != NULL; i++) {
        strcpy(_socket.ip, inet_ntoa(*_socket._addresses[i]));
    }

    _socket._address.sin_addr.s_addr = inet_addr(_socket.ip);
    _socket._address.sin_family = AF_INET;
    _socket._address.sin_port = htons(_socket.port);

    s->_sandia_socket = _socket;

    return r;
}

sandia_response sandia_forge_request( sandia* s, request_mode mode, char* uri, char* content, size_t content_length ) {
    sandia_response response;
    response.error = success;

    if (!s->_is_valid) {
        response.error = error_socket_not_ready;
        return response;
    }

    int ret_sock = connect(s->_sandia_socket._fd, (struct sockaddr *) &s->_sandia_socket._address, sizeof (s->_sandia_socket._address));
    if (ret_sock < 0) {
        response.error = error_connection;
        return response;
    }

    char* url_uri = (strlen(uri) > 0 ? uri : "/");

    sandia_build_request(s, mode);
    sandia_append_request(s, url_uri);
    sandia_append_request(s, " ");
    sandia_append_request(s, sandia_version_to_string(s->version));
    sandia_append_request(s, "\r\n");

    sandia_add_header(s, "Host", s->_sandia_socket.host_address);
    sandia_add_header(s, "Connection", "close"); // avoids 'read' socket hang

    if (mode == POST) {
        char header_content_length[16];
        int l = sprintf(header_content_length, "%d", content_length);
        header_content_length[l] = 0;

        sandia_add_header(s, "Content-Length", header_content_length);
    }

    for (int i = 0; i < s->_header_count; i++) {
        sandia_header h = s->_headers[i];
        sandia_append_request(s, h.key);
        sandia_append_request(s, ": ");
        sandia_append_request(s, h.value);
        sandia_append_request(s, "\r\n");
    }

    sandia_append_request(s, "\r\n");

    if (content_length > 0) {
        sandia_append_request_size(s, content, content_length);
    }

    //printf("[-- START REQUIEST--]\n%s\n[-- END REQUEST --]\n", s->_request);

    if (!sandia_send_data(s, s->_request, s->_request_length)) {
        response.error = error_send;
        return response;
    }

    if (!sandia_receive_data(s, &response)) {
        response.error = error_receive;
        return response;
    }

    /*char* responseCopy = (char*) calloc(response.body_length + 1, sizeof (char));
    strncpy(responseCopy, response.body, response.body_length);
    responseCopy[response.body_length] = 0;
    if (responseCopy > 0) {
        char* headers = strtok(responseCopy, "\r\n\r\n");
        if (headers != NULL) {
            printf("headers = %s\n", headers);
        }
    }*/

    return response;
}

sandia_response sandia_get_request( sandia* s, char* uri ) {
    return sandia_forge_request(s, GET, uri, "", 0);
}

sandia_response sandia_post_request( sandia* s, char* uri, char* content, size_t content_size ) {
    return sandia_forge_request(s, POST, uri, content, content_size);
}

bool sandia_send_data( sandia* s, char* data, size_t data_length ) {
    ssize_t bytes_total = 0, bytes_sent;
    while (bytes_total < data_length) {
        bytes_sent = send(s->_sandia_socket._fd, data + bytes_total, data_length - bytes_total, 0);
        if (bytes_sent == -1) {
            break;
        }
        bytes_total += bytes_sent;
    }

    return (bytes_total == data_length);
}

bool sandia_receive_data( sandia* s, sandia_response* response ) {
    ssize_t bytes_total = 0, bytes_received, buffer_size = s->_sandia_socket.receive_buffer_size;
    uint32_t num_reads = 1;

    char* r = (char*) calloc(buffer_size + 1, sizeof (char));
    if (!r) {
        return false;
    }

    //char r[1024];
    while ((bytes_received = recv(s->_sandia_socket._fd, r + bytes_total, s->_sandia_socket.receive_buffer_size, 0)) > 0) {
        bytes_total += bytes_received;
        num_reads++;

        r = (char*) realloc(r, buffer_size * num_reads + 1);
        if (!r) {
            return false;
        }
    }

    if (bytes_total > 0) {
        response->body_length = bytes_total;
        response->body = (char*) calloc(response->body_length + 1, sizeof (char));
        strncpy(response->body, r, response->body_length);
        response->body[response->body_length] = 0;
    }

    return true;
}

bool sandia_is_connected( sandia* s ) {
    int error_code;
    int error_code_size = sizeof (error_code);
    getsockopt(s->_sandia_socket._fd, SOL_SOCKET, SO_ERROR, (void*)&error_code, &error_code_size);

    return (error_code == 0);
}

const size_t _initial_request_length = 1024;

bool sandia_build_request( sandia* s, request_mode mode ) {
    s->_request_length = 0;
    s->_request = (char*) calloc(_initial_request_length, sizeof (char));

    char* m = (char*) calloc(16, sizeof (char));
    if (!m) {
        s->last_error = error_string;
        return false;
    }

    switch (mode) {
        case GET: m = "GET ";
            break;
        case POST: m = "POST ";
            break;
    }

    strcat(s->_request, m);
    s->_request_length += strlen(m);
    s->_request[s->_request_length] = 0;

    return true;
}

bool sandia_append_request_size( sandia* s, char* str, size_t str_len ) {
    if (str_len <= 0) {
        s->last_error = error_string;
        return false;
    }

    if (_initial_request_length < (s->_request_length + str_len)) {
        s->_request = (char*) realloc(s->_request, s->_request_length + str_len + 1);

        if (!s->_request) {
            s->last_error = error_string;
            return false;
        }
    }

    strcat(s->_request, str);
    s->_request_length += str_len;
    s->_request[s->_request_length] = 0;

    return true;
}

bool sandia_append_request( sandia* s, char* str ) {
    return sandia_append_request_size(s, str, strlen(str));
}

sandia_error sandia_add_header( sandia* s, char* key, char* value ) {
    if (s->_header_count >= MAX_HEADER_COUNT) {
        return error_header_limit;
    }

    s->_headers[s->_header_count].key = (char*) calloc(strlen(key) + 1, sizeof (char));
    s->_headers[s->_header_count].value = (char*) calloc(strlen(value) + 1, sizeof (char));

    strcpy(s->_headers[s->_header_count].key, key);
    strcpy(s->_headers[s->_header_count].value, value);

    s->_header_count++;

    return success;
}

sandia_error sandia_add_headers( sandia* s, sandia_header* headers, uint32_t count ) {
    if (s->_header_count >= MAX_HEADER_COUNT) {
        return error_header_limit;
    }

    for (int i = 0; i < count; i++) {
        sandia_header h = headers[i];
        s->_headers[s->_header_count].key = (char*) calloc(strlen(h.key) + 1, sizeof (char));
        s->_headers[s->_header_count].value = (char*) calloc(strlen(h.value) + 1, sizeof (char));

        strcpy(s->_headers[s->_header_count].key, h.key);
        strcpy(s->_headers[s->_header_count].value, h.value);

        s->_header_count++;
    }

    return success;
}

char* sandia_version_to_string( http_version version ) {
    char* str_version = (char*) calloc(16, sizeof (char));

    switch (version) {
        case HTTP_09:
            strcpy(str_version, "HTTP/0.9");
            break;
        case HTTP_10:
            strcpy(str_version, "HTTP/1.0");
            break;
        case HTTP_20:
            strcpy(str_version, "HTTP/2.0");
            break;
        default:
        case UNKNOWN:
        case HTTP_11:
            strcpy(str_version, "HTTP/1.1");
            break;
    }

    return str_version;
}

http_version sandia_string_to_version( char* version ) {
    if (strcmp(version, "HTTP/0.9") == 0) {
        return HTTP_09;
    } else if (strcmp(version, "HTTP/1.0") == 0) {
        return HTTP_10;
    } else if (strcmp(version, "HTTP/1.1") == 0) {
        return HTTP_11;
    } else if (strcmp(version, "HTTP/2.0") == 0) {
        return HTTP_20;
    } else {
        return UNKNOWN;
    }
}