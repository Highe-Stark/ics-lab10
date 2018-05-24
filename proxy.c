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
int Rio_readn_w(int fd, void *buf, size_t maxsize, size_t* size);
int Rio_readnb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size);
int Rio_readlineb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size);
int Rio_writen_w(int fd, void *buf, size_t n, size_t *size);

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
    FILE *fp = fopen("std.txt", "w");

    int connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    int listenfd = open_listenfd(portstr);
    while (1) {
        clientlen = sizeof (clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);

        fprintf(fp, "Client Addr: %s\n", ((SA *)&clientaddr)->sa_data);
        fprintf(fp, "hostname : %s, port : %s\n", hostname, port);
        // construct client socket address structure
        struct sockaddr_in client_sock_in;
        memset(&client_sock_in, 0, sizeof(client_sock_in));
        client_sock_in.sin_family = AF_INET;
        struct in_addr addr;
        int ret = inet_pton(AF_INET, hostname, &addr.s_addr);
        // addr.s_addr = htonl(hostname);
        client_sock_in.sin_addr = addr;
        // inet_pton(AF_INET, hostname, (void *) &(client_sock_in.sin_addr));
        client_sock_in.sin_port = htons((short) atoi(port));

        doit(connfd, &client_sock_in);
        Close(connfd);
    }

    return 0;
}

/*
 * doit - get server url and forward request to server
 */
void doit(int fd, struct sockaddr_in *csock)
{
    int clientfd = fd;
    FILE *log = fopen("proxy.log", "a+");
    // fprintf(log, "Hey, I am alive! Accepted connection\n");
    
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    rio_t crio;             // client rio

    size_t actsize = 0;     // actual size
    int res = 0;
    /* Read request line and headers */
    Rio_readinitb(&crio, clientfd);
    if ((res = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize)) == -1){
        fprintf(stderr, "Rio_readlineb_w error");
        return;
    }
    
    sscanf(buf, "%s %s %s", method, uri, version);
    // fprintf(log, ">> Jarvis: Request Headers\n>> %s %s %s\n", method, uri, version);
    // fflush(log);
    
    char server_hostname[MAXLINE], server_pathname[MAXLINE], server_port[MAXLINE];
    if (parse_uri(uri, server_hostname, server_pathname, server_port)) {
        char serr[MAXLINE];
        sprintf(serr, "%s 404 NOT FOUND\r\n\r\n", version);
        res = Rio_writen_w(clientfd, serr, strlen(serr), &actsize);
        // fprintf(log, ">> Jarvis: Parse uri error\n");
        fclose(log);
        return;
    }

    /* connect to server */
    int serverfd = Open_clientfd(server_hostname, server_port);
    rio_t srio;            // server rio
    Rio_readinitb(&srio, serverfd);

    /* Forward request headers to server */
    int bodysize = 0;
    sprintf(buf, "%s /%s %s\r\n", method, server_pathname, version);
    if (strncasecmp(method, "GET", 3) == 0) {
        bodysize = 0;
    }
    // fprintf(log, ">>>>> Forward request line to server\n");
    while (strcmp(buf, "\r\n") && res == 1 ) {
        // log request to server
        // fprintf(log, "%s", buf);
        // fflush(log);

        if (strncasecmp(buf, "Content-Length", 14) == 0) {
            sscanf(buf, "Content-Length: %d", &bodysize);
        }

        res = Rio_writen_w(serverfd, buf, strlen(buf), &actsize);
        if (res == -1) break;
        res = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize);
    }
    res = Rio_writen_w(serverfd, "\r\n", strlen("\r\n"), &actsize);

    /* Forward request body to server if any */
    if (bodysize > 0) {
        char body[MAXBUF] = "\0";
        int readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
        res = 1;
        // request body
        // fprintf(log, "<Request Body>\n");
        while (bodysize > 0 && res == 1) {
            res = Rio_readnb_w(&crio, body, readsize, &actsize);
            if (res == -1) {
                fprintf(log, "Rio_readnb_w error\n");
            }
            bodysize -= actsize;
            readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
            // request body
            // fprintf(log, ">> %s", body);
            // fflush(log);

            res = Rio_writen_w(serverfd, body, actsize, &actsize);
        }
        res = Rio_writen_w(serverfd, "\r\n", strlen("\r\n"), &actsize);
    }

    int flow = 0;
    bodysize = 0;
    // fprintf(log, "\n<<<<< Receive response from server.\n");
    /* Forward response headers to client */
    res = 1;
    while (res == 1) {
        if ((res = Rio_readlineb_w(&srio, buf, MAXLINE, &actsize)) != 1) break;
        if (strncasecmp(buf, "Content-Length", 14) == 0) {
            sscanf(buf, "Content-Length: %d", &bodysize);
            // fprintf(log, "> Paring -> Content-Length : %d\n", bodysize);
            // fflush(log);
        }
        if ((res = Rio_writen_w(clientfd, buf, strlen(buf), &actsize)) != 1) {
            fprintf(log, "! Forward to client ERROR !");
            fflush(log);
            break;
        }
        //
        // fprintf(log, "%s", buf);
        // fflush(log);
        // 
        if (strcmp(buf, "\r\n") == 0) break;
        flow += actsize;
    }

    // Forward Response Body to client if any
    if (bodysize > 0) {
        char body[MAXBUF] = "\0";
        int readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
        // log response body
        // fprintf(log, "<Response body>\n");
        // fflush(log);
        res = 1;
        while (bodysize > 0)
        {
            res = Rio_readnb_w(&srio, body, readsize, &actsize);
            // fprintf(log, ">> Rio_readnb_w stat: %d\n", res);
            if (res == -1) {
                // fprintf(log, "! Rio_readnb_w Error !\n%s\n", body);
                // fflush(log);
                break;
            }
            bodysize -= actsize;
            readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
            // fprintf(log, ">> %s", body);
            // fflush(log);

            res = Rio_writen_w(clientfd, body, actsize, &actsize);
            if (res != 1) {
                fprintf(log, "! Rio_writen_w Error !\n");
                fflush(log);
                break;
            }
            flow += actsize;
        }
        res = Rio_writen_w(clientfd, "\r\n", strlen("\r\n"), &actsize);
        flow += 2;
    }

    Close(serverfd);

    // fprintf(log, "\n>> Jarvis: Connection Closed.\n");
    char logContent[MAXLINE];
    format_log_entry(logContent, csock, uri, flow);
    fprintf(log, "%s", logContent);
    fclose(log);
}

/*
 * Rio_readn_w - wrapper for rio_readn
 * If error detected, return 0;
 * return 0 encountered EOF
 */
int Rio_readn_w(int fd, void *buf, size_t maxsize, size_t *size) 
{
    ssize_t stat = rio_readn(fd, buf, maxsize);
    if (stat == -1) {
        *size = 0;
        fprintf(stderr, "Read %d bytes failed", (int) maxsize);
        fflush(stderr);
        return -1;
    } else if (stat == 0) {
        *size = stat;
        return 0;
    } else {
        *size = stat;
        return 1;
    }
}

/* 
 * Rio_readnb_w - wrapper for rio_readnb
 * If error detected, return 0;
 * return 0 if encounter EOF
 */
int Rio_readnb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size) 
{
    ssize_t stat = rio_readnb(rp, buf, maxsize);
    if (stat == -1) {
        *size = 0;
        return -1;
    } else if (stat == 0) {
        *size = stat;
        return 0;
    } else {
        *size = stat;
        return 1;
    }
}

/* 
 * Rio_readlineb_w - wrapper for rio_readlineb
 * If error detected, or encouter EOF, return 0
 */
int Rio_readlineb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size)
{
    ssize_t stat = rio_readlineb(rp, buf, maxsize);
    if (stat == -1) {
        *size = 0;
        fprintf(stderr, "Readline failed.");
        fflush(stderr);
        return -1;
    }
    else if (stat == 0) {
        *size = 0;
        return 0;
    } else {
        *size = stat;
        return 1;
    }
    // return 0;
}

/* 
 * Rio_writen_w - wrapper for rio_writen
 * If error detected, return 0;
 * if normal, return 1;
 */
int Rio_writen_w(int fd, void *buf, size_t n, size_t *size)
{
    // ssize_t stat = Rio_writen(fd, buf, n);
    ssize_t stat = rio_writen(fd, buf, n);
    if (stat != n) {
        *size = 0;
        fprintf(stderr, "Write failed.");
        fflush(stderr);
        return -1;
    } else {
        *size = stat;
        return 1;
    }
}

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


