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
size_t forward(rio_t *criop, int sfd, char *method);
int parse_cnt_len(const char *hdr, long *len);
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
    Close(connfd);
    return NULL;
}

/*
 * doit - proxy routine
 */
void doit(int cfd, struct sockaddr_in *csock)
{
    rio_t crio, srio;
    rio_readinitb(&crio, cfd);

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE], hostname[MAXLINE], pathname[MAXLINE], port[MAXLINE];
    int stat = 1;
    size_t actsize = 0;     // actual size
    if ((stat = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize)) != 1) {
        printf("<1> Rio_readlineb_w Error, Abort\n");
        return;
    }
    printf("[%ld] %s", actsize, buf);    // DEBUG <
    sscanf(buf, "%s %s %s", method, uri, version);
    if (parse_uri(uri, hostname, pathname, port) == -1) {
        printf("Error parse uri\n");
        return;
    }
    int sfd;
    if ((sfd = open_clientfd(hostname, port)) < 0)
    {
        close(sfd);
        printf("<2> oepn clientfd Error, Abort\n");
        return;
    }
    rio_readinitb(&srio, sfd);
    sprintf(buf, "%s /%s %s\r\n", method, pathname, version);
    if ((stat = Rio_writen_w(sfd, buf, strlen(buf), &actsize)) != 1) {
        close(sfd);
        printf("<3> Rio_writen_w Error, Abort\n");
        return;
    }
    printf("[%ld] %s\n", actsize, buf);     // DEBUG <
    size_t flow = 0;
    if ((flow = forward(&crio, sfd, method)) == -1) {
        close(sfd);
        printf("<4> forward client to server Error, Abort\n");
        return;
    }
    flow = 0;
    if ((flow = forward(&srio, cfd, NULL)) == -1) {
        close(sfd);
        printf("<5> forward server to client Error, Abort\n");
        return;
    }

    close(sfd);
    char log[MAXLINE];
    format_log_entry(log, csock, uri, flow);
    printf("%s\n", log);
    return;
}

/*
 * forward - forward HTTP Request
 * @criop - client Rio pointer
 * @sfd - server file descriptor
 * return bytes forward
 */
size_t forward(rio_t *criop, int sfd, char *method)
{
    int stat = 1;
    size_t actsize = 0, flow = 0;
    long bodysize = 0; 
    char buf[MAXLINE], body[MAXBUF];
    while (1) {
        if ((stat = Rio_readlineb_w(criop, buf, MAXLINE - 1, &actsize)) == -1) return -1;
        if (actsize == 0 || stat == 0) return flow;
        printf("[%ld] %s",actsize, buf);    // DEBUG <
        parse_cnt_len(buf, &bodysize);
        if ((stat = Rio_writen_w(sfd, buf, actsize, &actsize)) != 1) return -1;
        flow += actsize;
        if (strcmp(buf, "\r\n") == 0) break;
    }
    printf("--- Header Forwarding Complete ----\n");
    int hasbody = 0;
    if (method == NULL) hasbody = bodysize == 0;
    else {
        hasbody = strcasecmp(method, "GET");
    }
    if (hasbody) {
        printf("Has body\n");
        while (1) {
            if ((stat = Rio_readnb_w(criop, body, MAXBUF - 1, &actsize)) == -1) return -1;
            if (actsize == 0) break;
            if ((stat = Rio_writen_w(sfd, body, actsize, &actsize)) != 1) return -1;
            body[actsize] = '\0';                  // DEBUG <
            printf("[%ld] %s", actsize, body);    // DEBUG <
            flow += actsize;
            if (actsize < MAXBUF - 1) break;
        }
        if ((stat = Rio_writen_w(sfd, "\r\n", strlen("\r\n"), &actsize)) != 1)
            return -1;
        return flow;
    }
    int readsize = MAXBUF - 1 > bodysize ? bodysize : MAXBUF - 1;
    printf("<Body>\n");
    while (bodysize > 0) {
        if ((stat = Rio_readnb_w(criop, body, readsize, &actsize)) == -1) return -1;
        if (actsize == 0) break;
        if ((stat = Rio_writen_w(sfd, body, actsize, &actsize)) != 1) return -1;
        body[actsize] = '\0';               // DEBUG <
        printf("[%ld] %s", actsize, body); // DEBUG <
        bodysize -= actsize;
        flow += actsize;
        if (actsize < readsize) break;
    }
    if ((stat = Rio_writen_w(sfd, "\r\n", strlen("\r\n"), &actsize)) != 1) return -1;
    return flow;
}

/*
 * parse_cnt_len - parse content length
 */
int parse_cnt_len(const char *hdr, long *len)
{
    if (strncasecmp(hdr, "Content-Length", 14) == 0) {
        char cnt[30];
        sscanf(hdr, "%s %ld", cnt, len);
        printf("Parse Content Length : %ld\n", *len);
        return 1;
    }
    return 0;
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
        fprintf(stderr, "! rio_readn Error !\n");
        fflush(stderr);
        *size = 0;
        return -1;
    } else if (stat == 0) {
        printf("<!> EOF encounter while rio_readn\n");
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
        fprintf(stderr, "! rio readnb Error !\n");
        fflush(stderr);
        *size = 0;
        return -1;
    } else if (stat == 0) {
        printf("<!> EOF\n");
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
        fprintf(stderr, "! rio readlineb Error !\n");
        fflush(stderr);
        *size = 0;
        return -1;
    }
    else if (stat == 0) {
        printf("<!> EOF\n");
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
        fprintf(stderr, "! rio writen Error !\n");
        fflush(stderr);
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


