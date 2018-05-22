/*
 * proxy.c - ICS Web proxy
 *
 *
 */

#include "csapp.h"
#include <stdarg.h>
#include <sys/select.h>

/*
 * Function prototypes
 */
int parse_uri(char *uri, char *target_addr, char *path, char *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, size_t size);
int proxy (char *port);
void doit(int fd, struct sockaddr_in *csock);
// void read_requesthdrs(rio_t *rp);

/*
 * main - Main routine for the proxy program
 */
int main(int argc, char **argv)
{
    /* Check arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
        exit(0);
    }
    int port = atoi(argv[1]);
    if (port < 1024 || port > 65536) {
        fprintf(stderr, "Invalid port number");
        exit(1);
    }
    proxy(argv[1]);

    exit(0);
}

/*
 * proxy - Codes written by higher stark
 */
int proxy(char *portstr)
{
    int connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    int listenfd = open_listenfd(portstr);
    while (1) {
        clientlen = sizeof (clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        fprintf(stdout, "Hey, I am alive! Accepted connection from (%s %s)\n", hostname, port);
        // construct client socket address structure
        struct sockaddr_in client_sock_in;
        memset(&client_sock_in, 0, sizeof(client_sock_in));
        client_sock_in.sin_family = AF_INET;
        // struct in_addr addr;
        inet_pton(AF_INET, hostname, (void *) &(client_sock_in.sin_addr));
        client_sock_in.sin_port = htons((short) atoi(port));

        doit(connfd, &client_sock_in);
        Close(connfd);
    }
}

/*
 * doit - get server url and forward request to server
 */
void doit(int fd, struct sockaddr_in *csock)
{
    FILE *log = fopen("proxy.log", "a+");
    fprintf(log, "Hey, I am alive! Accepted connection\n");
    
    //struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    // char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t crio;  // client rio

    /* Read request line and headers */
    Rio_readinitb(&crio, fd);
    Rio_readlineb(&crio, buf, MAXLINE);
    fprintf(log, ">> Jarvis: Request Headers\n");
    fprintf(log, ">> Jarvis: %s", buf);
    fflush(log);
    sscanf(buf, "%s %s %s", method, uri, version);
    fprintf(log, "%s %s %s\n", method, uri, version);
    fprintf(log, ">> Jarvis: uri = %s\n", uri);
    fflush(log);
    
    char server_hostname[MAXLINE], server_pathname[MAXLINE], server_port[MAXLINE];
    if (parse_uri(uri, server_hostname, server_pathname, server_port)) {
        char serr[MAXLINE];
        sprintf(serr, "%s 404 NOT FOUND\r\n\r\n", version);
        Rio_writen(fd, serr, strlen(serr));
        fprintf(log, ">> Jarvis: Parse uri error\n");
        fclose(log);
        return;
    }
    if (server_hostname == NULL || server_port == NULL) {
        
        fprintf(log, ">> Jarvis: Invalid server socket.\n");
        char serr[MAXLINE];
        sprintf(serr, "%s 404 NOT FOUND\r\n\r\n", version);
        Rio_writen(fd, serr, strlen(serr));
        fclose(log);
        return;
    }
    /* connect to server */
    int clientfd = Open_clientfd(server_hostname, server_port);
    rio_t srio;   // server rio
    Rio_readinitb(&srio, clientfd);

    /* Forward request headers to server */
    int body = 1;
    sprintf(buf, "%s /%s %s\r\n", method, server_pathname, version);
    if (strncasecmp(method, "GET", 3) == 0) {
        body = 0;
    }
    fprintf(log, ">> Jarvis: forward request line to server\n%s\n", buf);
    while (strcmp(buf, "\r\n")) {
        Rio_writen(clientfd, buf, strlen(buf));
        Rio_readlineb(&crio, buf, MAXLINE);
    }
    Rio_writen(clientfd, "\r\n", strlen("\r\n"));

    /* Forward request body to server if any */
    if (body) {
        Rio_readlineb(&crio, buf, MAXLINE);
        while (strcmp(buf, "\r\n")) {
            Rio_writen(clientfd, buf, strlen(buf));
            Rio_readlineb(&crio, buf, MAXLINE);
        }
        Rio_writen(clientfd, "\r\n", strlen("\r\n"));
    }

    int flow = 0;
    body = 0;
    /* Forward response headers to client */
    Rio_readlineb(&srio, buf, MAXLINE);
    while(strcmp(buf, "\r\n")) {
        Rio_writen(fd, buf, strlen(buf));
        if (strncasecmp(buf, "Content-length", 14) == 0) {
            body = 1;
        }
        flow += strlen(buf);
        Rio_readlineb(&srio, buf, MAXLINE);
    }
    Rio_writen(fd, "\r\n", strlen("\r\n"));
    // Forward Response Body to client if any
    if (body) {
        Rio_readlineb(&srio, buf, MAXLINE);
        while (strcmp(buf, "\r\n"))
        {
            Rio_writen(fd, buf, strlen(buf));
            flow += strlen(buf);
            Rio_readlineb(&srio, buf, MAXLINE);
        }
        Rio_writen(fd, "\r\n", strlen("\r\n"));
    }

    Close(clientfd);

    // log
    char logContent[MAXLINE];
    format_log_entry(logContent, csock, uri, flow);
    fprintf(log, "%s\n", logContent);
    fclose(log);
}

/* 
 * get_cnt_len - get response content length
 */
int get_cnt_len(const char *res, long *len) 
{
    char *cnt_len = "Content-length";
    if (strncasecmp(res, cnt_len, strlen(cnt_len)) != 0) {
        return -1;
    }
    return 0;
}

/* 
 * read_requesthdrs - get request Headers and display
 *
void read_requesthdrs(rio_t *rp) {
    char buf[MAXLINE];

    Rio_readlineb(rp, buf, MAXLINE);
    while (strcmp(buf, "/r/n")) {
        Rio_readlineb(rp, buf, MAXLINE);
        fprintf(stdout, ">> Jarvis: %s", buf);
    }
    return;
}
 */

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, char *port)
{
    char *hostbegin;
    char *hostend;
    char *pathbegin;
    int len;

    if (strncasecmp(uri, "http://", 7) != 0) {
        hostname[0] = '\0';
        return -1;
    }

    /* Extract the host name */
    hostbegin = uri + 7;
    hostend = strpbrk(hostbegin, " :/\r\n\0");
    if (hostend == NULL)
        return -1;
    len = hostend - hostbegin;
    strncpy(hostname, hostbegin, len);
    hostname[len] = '\0';

    /* Extract the port number */
    if (*hostend == ':') {
        char *p = hostend + 1;
        while (isdigit(*p))
            *port++ = *p++;
        *port = '\0';
    } else {
        strcpy(port, "80");
    }

    /* Extract the path */
    pathbegin = strchr(hostbegin, '/');
    if (pathbegin == NULL) {
        pathname[0] = '\0';
    }
    else {
        pathbegin++;
        strcpy(pathname, pathbegin);
    }

    return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), the number of bytes
 * from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr,
                      char *uri, size_t size)
{
    time_t now;
    char time_str[MAXLINE];
    unsigned long host;
    unsigned char a, b, c, d;

    /* Get a formatted time string */
    now = time(NULL);
    strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

    /*
     * Convert the IP address in network byte order to dotted decimal
     * form. Note that we could have used inet_ntoa, but chose not to
     * because inet_ntoa is a Class 3 thread unsafe function that
     * returns a pointer to a static variable (Ch 12, CS:APP).
     */
    host = ntohl(sockaddr->sin_addr.s_addr);
    a = host >> 24;
    b = (host >> 16) & 0xff;
    c = (host >> 8) & 0xff;
    d = host & 0xff;

    /* Return the formatted log entry string */
    sprintf(logstring, "%s: %d.%d.%d.%d %s %zu", time_str, a, b, c, d, uri, size);
}


