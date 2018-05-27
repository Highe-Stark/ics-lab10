/*
 * proxy.c - ICS Web proxy
 *
 *
 */

#include "csapp.h"
#include <stdarg.h>
#include <sys/select.h>

sem_t mutout;

/*
 * Function prototypes
 */
void *thread(void *vargp);
int build_sockaddr_in(struct sockaddr_in *sap, const char *hostname, const char *port);
void doit(int cfd, struct sockaddr_in *csock);
int record(FILE *fp, const char *rcd);
int Rio_readn_w(int fd, void *buf, size_t maxsize, size_t* size);
int Rio_readnb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size);
int Rio_readlineb_w(rio_t *rp, void *buf, size_t maxsize, size_t *size);
int Rio_writen_w(int fd, void *buf, size_t n, size_t *size);
int parse_uri(char *uri, char *target_addr, char *path, char *port);
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, size_t size);

typedef struct  {
    int fd;
    struct sockaddr_in sa;
} fd_addr_t ;

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

    signal(EPIPE, SIG_IGN);
    Sem_init(&mutout, 0, 1);

    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage client_addr;
    pthread_t tid;
    fd_addr_t *fd_addr_p;
    listenfd = Open_listenfd(argv[1]);

    while (1) {
        clientlen = sizeof(struct sockaddr_storage);
        fd_addr_p = Malloc(sizeof(fd_addr_t));
        //connfdp = Malloc(sizeof(int));
        connfd = Accept(listenfd, (SA *) &client_addr, &clientlen);
        int flags = NI_NUMERICHOST | NI_NUMERICSERV;
        Getnameinfo((SA *)&client_addr, clientlen, hostname, MAXLINE, port, MAXLINE, flags);
        struct sockaddr_in client_sock_in;
        build_sockaddr_in(&client_sock_in, hostname, port);
        fd_addr_p->sa = client_sock_in;
        fd_addr_p->fd = connfd;
        Pthread_create(&tid, NULL, thread, fd_addr_p);
    }
    exit(0);
}

/*
 * build_sockaddr_in - construct a sockaddr_in with hostname and port
 */
int build_sockaddr_in(struct sockaddr_in *sap, const char *hostname, const char *port)
{
    memset(sap, 0, sizeof(struct sockaddr_in));
    sap->sin_family = AF_INET;
    inet_pton(AF_INET, hostname, &(sap->sin_addr.s_addr));
    sap->sin_port = htons((short) atoi(port));
    return 0;
}

/* Thread routine */
void *thread(void *vargp)
{
    fd_addr_t fd_addr = *((fd_addr_t *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(fd_addr.fd, &(fd_addr.sa));
    Close(fd_addr.fd);
    return NULL;
}

/*
 * doit - get server url and forward request to server
 */
void doit(int cfd, struct sockaddr_in *csock)
{
    FILE *logger = fopen("myproxy.log", "a+");
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char body[MAXBUF];
    rio_t crio;
    size_t actsize;
    int stat = 1;

    /* Read request line and headers */
    Rio_readinitb(&crio, cfd);
    stat = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize);
    record(logger, buf);
    sscanf(buf, "%s %s %s", method, uri, version);
    /* Connect to server */
    char shostname[MAXLINE], sport[MAXLINE], spath[MAXLINE];
    parse_uri(uri, shostname, sport, spath);
    printf("Connecting server -> hostname : %s, port : %s\n", shostname, sport);
    int sfd = Open_clientfd(shostname, sport);
    rio_t srio;
    Rio_readinitb(&srio, sfd);

    /* Forward Request to Server */
    int bodySize = 0;
    sprintf(buf, "%s /%s %s\r\n", method, spath, version);
    while (strcmp(buf, "\r\n")) {
        if (strncasecmp(buf, "Content-Length", 14) == 0) {
            char tmp[30];
            sscanf(buf, "%s %d", tmp, &bodySize);
            sprintf(tmp, "get content length : %d\n", bodySize);
            record(logger, tmp);
        }
        stat = Rio_writen_w(sfd, buf, sizeof(buf), &actsize);
        if (stat != 1) {record(logger, "! Rio_writen_w Error !\n"); break;}
        stat = Rio_readlineb_w(&crio, buf, MAXLINE, &actsize);
        if (stat != 1) {record(logger, "! Rio_readlineb_w Error !\n"); break;}
    }
    if ((stat = Rio_writen_w(sfd, "\r\n", sizeof("\r\n"), &actsize)) != 1) {record(logger, "! Rio_writen_w Error !\n");}
    while (bodySize > 0) {
        int readSize = MAXBUF - 1 > bodySize ? bodySize : MAXBUF - 1;
        if ((stat = Rio_readnb_w(&crio, body, readSize, &actsize)) != 1) {record(logger, "! Rio_readnb_w Error !\n"); break;}
        bodySize -= actsize;
        if ((stat = Rio_writen_w(sfd, body, sizeof(body), &actsize)) != 1) {record(logger, "! Rio_writen_w Error !\n"); break;}
    }

    /* Forward Response to Server */
    bodySize = 0;
    int flow = 0;
    if ((stat = Rio_readlineb_w(&srio, buf, MAXLINE, &actsize)) != 1) {record(logger, "! Rio_readlineb_w Error !\n");}
    flow += actsize;
    while (strcmp(buf, "\r\n")) {
        if (strncasecmp(buf, "Content-Length", 14) == 0) {
            char tmp[30];
            sscanf(buf, "%s %d", tmp, &bodySize);
            sprintf(tmp, "get content length : %d\n", bodySize);
            record(logger, tmp);
        }
        if ((stat = Rio_writen_w(cfd, buf, sizeof(buf), &actsize)) != 1) {record(logger, "! Rio_writen_w Error !\n"); break;}
        if ((stat = Rio_readlineb_w(&srio, buf, MAXLINE, &actsize)) != 1){record(logger, "! Rio_readlineb_w Error !\n"); break;}
        flow += actsize;
    }
    if ((stat = Rio_writen_w(sfd, "\r\n", sizeof("\r\n"), &actsize)) != 1) {record(logger, "! Rio_writen_w Error !\n");}
    while (bodySize > 0) {
        int readSize = MAXBUF - 1 > bodySize ? bodySize : MAXBUF - 1;
        if ((stat = Rio_readnb_w(&srio, body, readSize, &actsize)) != 1) {record(logger, "! Rio_readnb_w Error !\n"); break;}
        bodySize -= actsize;
        flow += actsize;
        if ((stat = Rio_writen_w(cfd, body, sizeof(body), &actsize)) != 1) {record(logger, "! Rio_writen_w Error !\n"); break;}
    }

    char log[MAXLINE];
    format_log_entry(log, csock, shostname, flow);
    P(&mutout);
    printf("%s\n", log);
    fflush(stdout);
    V(&mutout);

    fclose(logger);
    return ;
}

/*
 * record - write record to file
 */
int record(FILE *fp, const char *rcd)
{
    int tid = pthread_self();
    fprintf(fp, "Thread %d >> %s", tid, rcd);
    fflush(fp);
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


