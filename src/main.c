#include <linux/limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "uthash.h"
#include <getopt.h>

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

static char *directory = "";

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

static char *handle_files(const char *request_target, int *resp_len) {
	const char *filename = request_target + strlen("/files/");
	char filename_buf[PATH_MAX];
	sprintf(filename_buf, "%s/%s", directory, filename);
	char file_buf[4096] = {0};
	int fd = open(filename_buf, O_RDONLY);
	if (fd < 0) {
		char *not_found = strdup("HTTP/1.1 404 Not Found\r\n\r\n");
		*resp_len = strlen(not_found);
		return not_found;
	}

	int file_len = 0;
	while (true) {
		int ret = read(fd, file_buf + file_len, sizeof(file_buf) - file_len);
		if (ret <= 0) break;
		file_len += ret;
	}
	close(fd);

	char header[256] = {0};
	int hlen = snprintf(header, sizeof(header),
		"HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %d\r\n\r\n",
		file_len);

	int total = hlen + file_len;
	char *resp = malloc(total);
	memcpy(resp, header, hlen);
	memcpy(resp + hlen, file_buf, file_len);
	*resp_len = total;
	return resp;
}

static char *handle_files_post(http_request_line *req, http_headers *headers, const char *body, int *resp_len) {
	const char *filename = req->request_target + strlen("/files/");
	char filename_buf[PATH_MAX];
	sprintf(filename_buf, "%s/%s", directory, filename);
	char file_buf[4096] = {0};
	int fd = open(filename_buf, O_WRONLY | O_CREAT);
	if (fd < 0) {
		char *not_found = strdup("HTTP/1.1 404 Not Found\r\n\r\n");
		*resp_len = strlen(not_found);
		return not_found;
	}
	header_entry *e = NULL;
	HASH_FIND_STR(headers->header_table, "Content-Length", e);
	if (e != NULL) {
		int len = atoi(e->value);
		int written = 0;
		while (true) {
			int ret = write(fd, body + written, len - written);
			if (ret < 0 || len == written) {
				break;
			}
			written += ret;
		}
	}

	close(fd);
	char *succ = "HTTP/1.1 201 Created\r\n\r\n";
	*resp_len = strlen(succ);
	return strdup(succ);
}

static bool validate_encoding(const char *encoding, const char *support_encoding) {
	const char *p = encoding;
	while (*p != '\0') {
		while (*p == ' ' || *p == ',') p++;
		if (*p == '\0') break;
		const char *start = p;
		while (*p != '\0' && *p != ',' && *p != ' ') p++;
		if ((int)(p - start) == (int)strlen(support_encoding)
			&& strncmp(start, support_encoding, p - start) == 0) {
			return true;
		}
	}
	return false;
}

static char *handle_echo(const char *request_target, http_headers *headers) {
	char buf[512] = {0};
	const char *echo = request_target + strlen("/echo/");
	int len = 0;
	header_entry *e = NULL;
	len += snprintf(buf + len, sizeof(buf) - len, "HTTP/1.1 200 OK\r\n");
	HASH_FIND_STR(headers->header_table, "Accept-Encoding", e);
	if (e != NULL && validate_encoding(e->value, "gzip")) {
		len += snprintf(buf + len, sizeof(buf) - len, "Content-Encoding: gzip\r\n");
	}
	len += snprintf(buf + len, sizeof(buf) - len, "Content-Type: text/plain\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "Content-Length: %lu\r\n", strlen(echo));
	len += snprintf(buf + len, sizeof(buf) - len, "\r\n");
	len += snprintf(buf + len, sizeof(buf) - len, "%s", echo);
	return strdup(buf);
}

static struct option long_options[] = {
	{"directory", required_argument, 0, 'd'},
	{0, 0, 0, 0},	
};

int main(int argc, char *argv[]) {
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	printf("Logs from your program will appear here!\n");

	int server_fd;
	socklen_t client_addr_len;
	struct sockaddr_in client_addr;

	int opt;
	while ((opt = getopt_long(argc, argv, "d:", long_options, NULL)) != -1) {
		switch (opt) {
			case 'd': {
				directory = optarg;
				break;
			} 
		}
	}

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
				char *response = handle_echo(req->request_target, headers);
				send(conn, response, strlen(response), 0);
				free(response);
			} else if (strcmp(req->request_target, "/user-agent") == 0) {
				char *response = handle_user_agent(headers);
				send(conn, response, strlen(response), 0);
				free(response);
			} else if (strncmp(req->request_target, "/files/", strlen("/files/")) == 0) {
				int resp_len = 0;
				char *response = NULL;
				if (strcmp(req->http_method, "POST") == 0) {
					response = handle_files_post(req, headers, parsed, &resp_len);
				} else {
					response = handle_files(req->request_target, &resp_len);
				}
				send(conn, response, resp_len, 0);
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
