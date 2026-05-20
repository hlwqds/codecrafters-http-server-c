#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

typedef struct {
	char *http_method;
	char *request_target;
	char *version;	
} http_request_line;

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

int main() {
	// Disable output buffering
	setbuf(stdout, NULL);
 	setbuf(stderr, NULL);

	// You can use print statements as follows for debugging, they'll be visible when running tests.
	printf("Logs from your program will appear here!\n");

	// TODO: Uncomment the code below to pass the first stage
	
	 int server_fd, client_addr_len;
	 struct sockaddr_in client_addr;
	
	 server_fd = socket(AF_INET, SOCK_STREAM, 0);
	 if (server_fd == -1) {
	 	printf("Socket creation failed: %s...\n", strerror(errno));
	 	return 1;
	 }
	
	 // Since the tester restarts your program quite often, setting SO_REUSEADDR
	 // ensures that we don't run into 'Address already in use' errors
	 int reuse = 1;
	 if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
	 	printf("SO_REUSEADDR failed: %s \n", strerror(errno));
	 	return 1;
	 }
	
	 struct sockaddr_in serv_addr = { .sin_family = AF_INET ,
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
	
	 int conn = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
	 char req_buf[1024];
	 read(conn, req_buf, sizeof(req_buf));
	 http_request_line *req = NULL;
	 parse_http_request_line(req_buf, &req);
	 char *succ = "HTTP/1.1 200 OK\r\n\r\n";
	 char *not_found = "HTTP/1.1 404 Not Found\r\n\r\n";
	 if (strcmp(req->request_target, "/") == 0) {
		 send(conn, succ, strlen(succ), 0);
	 } else {
		 send(conn, not_found, strlen(not_found), 0);
	 }
	 printf("Client connected\n");
	
	 close(server_fd);

	return 0;
}
