#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include "uthash.h"

typedef struct {
	char *http_method;
	char *request_target;
	char *version;
} http_request_line;

typedef struct {
	char key[256];
	char value[256];
	UT_hash_handle hh;
} header_entry;

typedef struct {
	header_entry *header_table;
} http_headers;

static const char *parse_http_request_line(const char *request, http_request_line **req_line) {
	char *end = strstr(request, "\r\n");
	if (end == NULL) {
		*req_line = NULL;
		return request;
	}
	*req_line = calloc(1, sizeof(http_request_line));
	char buf[256] = {0};
	int step = 0;
	const char *p = request;
	while (p < end) {
		const char *start = p;
		while (p < end && *p != ' ') {
			p++;
		}
		strncpy(buf, start, p - start);
		if (p < end) p++;
		if (step == 0) {
			(*req_line)->http_method = strdup(buf);
		} else if (step == 1) {
			(*req_line)->request_target = strdup(buf);
		} else if (step == 2) {
			(*req_line)->version = strdup(buf);
		}
		step++;
		memset(buf, 0, sizeof(buf));
	}
	return end + 2;
}

static const char *parse_http_headers(const char *request, http_headers **headers) {
	*headers = calloc(1, sizeof(http_headers));
	while (true) {
		const char *end = strstr(request, "\r\n");
		if (end == request) {
			return end + 2;
		}
		if (end == NULL) break;
		const char *key_end = strstr(request, ": ");
		if (key_end == NULL || key_end > end) break;
		header_entry *e = calloc(1, sizeof(*e));
		size_t klen = key_end - request;
		size_t vlen = end - key_end - 2;
		snprintf(e->key, sizeof(e->key), "%.*s", (int)klen, request);
		snprintf(e->value, sizeof(e->value), "%.*s", (int)vlen, key_end + 2);
		HASH_ADD_STR((*headers)->header_table, key, e);
		request = end + 2;
	}
	return request;
}

static void free_headers(http_headers *headers) {
	header_entry *e, *tmp;
	HASH_ITER(hh, headers->header_table, e, tmp) {
		HASH_DEL(headers->header_table, e);
		free(e);
	}
	free(headers);
}

static char *handle_user_agent(const http_headers *headers) {
	char buf[512] = {0};
	header_entry *e = NULL;
	HASH_FIND_STR(headers->header_table, "User-Agent", e);
	int len = 0;
	len += snprintf(buf + len, sizeof(buf) - len, "HTTP/1.1 200 OK\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "Content-Type: text/plain\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "Content-Length: %lu\r\n", strlen(e->value));
	len += snprintf(buf + len, sizeof(buf) - len, "\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "%s", e->value);
	return strdup(buf);
}

static char *handle_echo(const char *request_target) {
	char buf[512] = {0};
	const char *echo = request_target + strlen("/echo/");
	int len = 0;
	len += snprintf(buf + len, sizeof(buf) - len, "HTTP/1.1 200 OK\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "Content-Type: text/plain\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "Content-Length: %lu\r\n", strlen(echo));
	len += snprintf(buf + len, sizeof(buf) - len, "\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "%s", echo);
	return strdup(buf);
}

int main() {
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("Logs from your program will appear here!\n");

	int server_fd;
	socklen_t client_addr_len;
	struct sockaddr_in client_addr;

	server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd == -1) {
		printf("Socket creation failed: %s...\n", strerror(errno));
		return 1;
	}

	int reuse = 1;
	if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		printf("SO_REUSEADDR failed: %s \n", strerror(errno));
		return 1;
	}

	struct sockaddr_in serv_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(4221),
		.sin_addr = { htonl(INADDR_ANY) },
	};

	if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) != 0) {
		printf("Bind failed: %s \n", strerror(errno));
		return 1;
	}

	int connection_backlog = 5;
	if (listen(server_fd, connection_backlog) != 0) {
		printf("Listen failed: %s \n", strerror(errno));
		return 1;
	}

	printf("Waiting for a client to connect...\n");
	client_addr_len = sizeof(client_addr);

	while (true) {
		int conn = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
		printf("Client connected\n");
		if (fork() == 0) {
			char req_buf[1024] = {0};
			read(conn, req_buf, sizeof(req_buf) - 1);
			http_request_line *req = NULL;
			const char *parsed = parse_http_request_line(req_buf, &req);
			http_headers *headers = NULL;
			parsed = parse_http_headers(parsed, &headers);
			char *succ = "HTTP/1.1 200 OK\r\n\r\n";
			char *not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
			if (strcmp(req->request_target, "/") == 0) {
				send(conn, succ, strlen(succ), 0);
			} else if (strncmp(req->request_target, "/echo/", strlen("/echo/")) == 0) {
				char *response = handle_echo(req->request_target);
				send(conn, response, strlen(response), 0);
				free(response);
			} else if (strcmp(req->request_target, "/user-agent") == 0) {
				char *response = handle_user_agent(headers);
				send(conn, response, strlen(response), 0);
				free(response);
			} else {
				send(conn, not_found, strlen(not_found), 0);
			}
			free_headers(headers);
			close(conn);
			_exit(0);
		}
		close(conn);
	}
	close(server_fd);

	return 0;
}
