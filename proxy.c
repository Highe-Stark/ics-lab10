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
void *thread(void *vargs);
void doit(int fd, struct sockaddr_in *csock);
int Rio_readn_w(int fd, void *buf, size_t maxsize, size_t* size);
int Rio_readnb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size);
int Rio_readlineb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size);
int Rio_writen_w(int fd, void *buf, size_t n, size_t *size);

sem_t mutex;

typedef struct {
    int thid;
    int connfd;
    struct sockaddr_in sock_in;
} thr_sock_t;

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

    signal(SIGPIPE, SIG_IGN);
    Sem_init(&mutex, 0, 1);

    int connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t client_len;
    struct sockaddr_storage client_addr;
    pthread_t tid;

    int listenfd = open_listenfd(argv[1]);
    while (1) {
        client_len = sizeof(client_addr);
        connfd = Accept(listenfd, (SA *)&client_addr, &client_len);
        int flags = NI_NUMERICHOST | NI_NUMERICSERV;
        Getnameinfo((SA *) &client_addr, client_len, hostname, MAXLINE, port, MAXLINE, flags);

        struct sockaddr_in *csock_in;
        thr_sock_t *thr_sock = Malloc(sizeof(thr_sock_t));
        thr_sock->connfd = connfd;
        csock_in = &thr_sock->sock_in;

        memset(csock_in, 0, sizeof(struct sockaddr_in));
        csock_in->sin_family = AF_INET;
        inet_pton(AF_INET, hostname, &(csock_in->sin_addr.s_addr));
        csock_in->sin_port = htons((short) atoi(port));
        Pthread_create(&tid, NULL, thread, thr_sock);
        // doit (connfd, &csock_in);
        // Close(connfd);
    }

    exit(0);
}

/* 
 * thread - thread routine
 */
void *thread(void *vargs)
{
    thr_sock_t thr_sock = *((thr_sock_t *)vargs);
    int connfd = thr_sock.connfd;
    struct sockaddr_in *csock_in = &thr_sock.sock_in;
    Pthread_detach(pthread_self());
    Free(vargs);
    doit(connfd, csock_in);
    printf(">> [-]\n");
    Close(connfd);
    return NULL;
}

/*
 * doit - get server url and forward request to server
 */
void doit(int fd, struct sockaddr_in *csock)
{
    int clientfd = fd;
    FILE *log = fopen("myproxy.log", "a+");
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];

    /* Read request line and headers */
    rio_t crio;             // client rio
    Rio_readinitb(&crio, clientfd);
    size_t actsize = 0;     // actual size
    int res = 0;
    if ((res = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize)) == -1){
        fprintf(stderr, "Rio_readlineb_w error");
        fclose(log);
        return;
    }
    if (actsize == 0) { fclose(log); return;}
    
    sscanf(buf, "%s %s %s", method, uri, version);
    fprintf(log, ">> Jarvis: Request Headers\n>> %s %s %s\n", method, uri, version);
    fflush(log);
    
    char server_hostname[MAXLINE], server_pathname[MAXLINE], server_port[MAXLINE];
    if (parse_uri(uri, server_hostname, server_pathname, server_port)) {
        fprintf(log, ">> Jarvis: Parse uri error\n");
        fclose(log);
        return;
    }

    /* connect to server */
    rio_t srio;            // server rio
    int serverfd = open_clientfd(server_hostname, server_port);
    if (serverfd < 0) {
        fprintf(log, "! Open clientfd Error ! Server hostname: %s, Server port : %s\n", server_hostname, server_port);
        fflush(log);
        fclose(log);
        return;
    }
    Rio_readinitb(&srio, serverfd);

    /* Forward request headers to server */
    int bodysize = 0;
    sprintf(buf, "%s /%s %s\r\n", method, server_pathname, version);
    fprintf(log, "\n>>>>> Forward request headers to server\n");
    while (strcmp(buf, "\r\n") && res == 1 ) {
        // log request to server
        fprintf(log, "%s", buf);
        fflush(log);

        if (strncasecmp(buf, "Content-Length", 14) == 0) {
            sscanf(buf, "Content-Length: %d", &bodysize);
            fprintf(log, "> Parse %s > length : %d\n", buf, bodysize);
            fflush(log);
        }

        res = Rio_writen_w(serverfd, buf, strlen(buf), &actsize);
        if (res != 1) {
            fprintf(log, "! Rio_writen_w Error ! Status : %d, write size : %ld\n", res, actsize);
            fflush(log); fclose(log);
            Close(serverfd); return;
        }
        res = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize);
        if (res != 1 || actsize == 0) {
            fprintf(log, "! Rio_readlineb_w Error ! Status : %d, read size : %ld\n", res, actsize); fflush(log); fclose(log);
            Close(serverfd); return;
        }
    }
    res = Rio_writen_w(serverfd, "\r\n", strlen("\r\n"), &actsize);
    if (res != 1)
    {
        fprintf(log, "! Rio_writen_w Error ! Status : %d, write size : %ld\n", res, actsize);
        fflush(log);
        fclose(log);
        Close(serverfd);
        return;
    }

    /* Forward request body to server if any */
    int hasbody = 0;
    if (strncasecmp(method, "GET", 3) != 0 && bodysize == 0) hasbody = 1;
    if (bodysize > 0) {
        char body[MAXBUF] = "\0";
        int readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
        res = 1;
        // request body
        fprintf(log, "<Request Body>\n");
        fflush(log);
        while (bodysize > 0 && res == 1) {
            if ((res = Rio_readnb_w(&crio, body, readsize, &actsize))!= 1) {
                fprintf(log, "! Rio_readnb_w error ! Status : %d, read size : %ld\n", res, actsize);
                fflush(log);
                fclose(log);
                Close(serverfd);
                return;
            }
            if (actsize == 0) break;
            bodysize -= actsize;
            readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;

            if ((res = Rio_writen_w(serverfd, body, actsize, &actsize)) != 1) {
                fprintf(log, "! Rio_writen_w Error ! Status : %d, write size : %ld\n", res, actsize);
                fflush(log);
                fclose(log);
                Close(serverfd);
                return;
            }
        }
        fprintf(log, ">> exit while loop status : %d\n", res); fflush(log);
        if ((res = Rio_writen_w(serverfd, "\r\n", strlen("\r\n"), &actsize)) != 1)
        {
            fprintf(log, "! Rio_writen_w Error ! Status : %d, write size : %ld\n", res, actsize);
            fflush(log);
            fclose(log);
            Close(serverfd);
            return;
        }
    } else if (hasbody) {
        actsize = 1;
        res = 1;
        char pre = '\0';
        char body[2];
        while (res == 1 && actsize) {
            if ((res = Rio_readnb_w(&crio, body, 1, &actsize)) != 1) {
                fprintf(log, "! Rio_readnb_w Error ! status : %d, readsize : %ld\n", res, actsize);
                fflush(log);
                fclose(log);
                Close(serverfd);
                return;
            }
            if (actsize == 0) break;
            fprintf(log, "%c", body[0]); fflush(log);
            if ((res = Rio_writen_w(serverfd, body, 1, &actsize)) != 1) {
                fprintf(log, "! Rio_writen_w Error ! status : %d, write size : %ld\n", res, actsize);
                fflush(log);
                fclose(log);
                Close(serverfd);
                return;
            }
            if (body[0] == '\n' && pre == '\r') break;
            pre = body[0];
        }
        fprintf(log, ">> Exit Loop Status : %d\n", res); fflush(log);
    }

    int flow = 0;
    bodysize = 0;
    fprintf(log, "\n<<<<< Receive response from server.\n"); fflush(log);
    /* Forward response headers to client */
    res = 1;
    while (res == 1) {
        fprintf(log, ">>");fflush(log);
        printf(">> [1]\n");
        if ((res = Rio_readlineb_w(&srio, buf, MAXLINE, &actsize)) != 1) {
            fprintf(log, "\n! Rio_readlineb_w Error ! Status : %d, Read size: %ld\n", res, actsize);
            fflush(log);
            fclose(log);
            Close(serverfd);
            return;
        }
        printf(">> [2]\n");
        if (actsize == 0) {
            printf("Read 0 byte\n");
            break;
        }
        //
        fprintf(log, "%s", buf);
        fflush(log);
        // 
        if (strncasecmp(buf, "Content-Length", 14) == 0) {
            sscanf(buf, "Content-Length: %d", &bodysize);
            fprintf(log, "> Paring -> Content-Length : %d\n", bodysize);
            fflush(log);
        }
        if ((res = Rio_writen_w(clientfd, buf, strlen(buf), &actsize)) != 1) {
            fprintf(log, "! Rio_writen_w ERROR ! Status : %d, Write size : %ld\n", res, actsize);
            fflush(log);
            fclose(log);
            Close(serverfd);
            return;
        }
        flow += actsize;
        if (strcmp(buf, "\r\n") == 0) break;
    }
    printf(">> [3]\n");

    // Forward Response Body to client if any
    if (bodysize > 0) {
        char body[MAXBUF] = "\0";
        int readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
        // log response body
        fprintf(log, "<Response body>\n");
        fflush(log);
        res = 1;
        while (bodysize > 0)
        {
            res = Rio_readnb_w(&srio, body, readsize, &actsize);
            if (actsize == 0) break;
            if (res != 1) {
                fprintf(log, "! Rio_readnb_w Error ! Status : %d, Read size : %ld\n", res, actsize);
                fflush(log);
                fclose(log);
                Close(serverfd);
                return;
            }
            bodysize -= actsize;
            readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;

            res = Rio_writen_w(clientfd, body, actsize, &actsize);
            if (res != 1) {
                fprintf(log, "! Rio_writen_w Error ! Status : %d, Write size : %ld\n", res, actsize);
                fflush(log);
                fclose(log);
                Close(serverfd);
                break;
            }
            flow += actsize;
        }
        res = Rio_writen_w(clientfd, "\r\n", strlen("\r\n"), &actsize);
        // flow += 2;
    }

    Close(serverfd);

    fprintf(log, "\n>> Jarvis: Connection Closed.\n"); fflush(log);
    char logContent[MAXLINE];
    format_log_entry(logContent, csock, uri, flow);
    fprintf(log, "%s\n", logContent); fflush(log);
    P(&mutex);
    fprintf(stdout, "%s\n", logContent);
    fflush(stdout);
    V(&mutex);
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
    ssize_t stat = rio_writen(fd, buf, n);
    if (stat != n) {
        *size = 0;
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


