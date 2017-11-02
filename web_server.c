/*
  simple web server for sonix board

  based on tserver:

  https://github.com/tridge/junkcode/tree/master/tserver
*/
#include "web_server.h"
#include "includes.h"
#include "web_files.h"
#ifdef SYSTEM_FREERTOS
#include <libmid_nvram/snx_mid_nvram.h>
#else
#include <arpa/inet.h>
#include <termios.h>
#endif

#ifndef SYSTEM_FREERTOS
static pthread_mutex_t lock;
static int serial_port_fd = -1;
static int fc_udp_in_fd = -1;
static int udp_out_fd = -1;

struct sockaddr_in fc_addr;
socklen_t fc_addrlen;

struct sockaddr_in udp_out_addr;
socklen_t udp_out_addrlen;

unsigned baudrate = 57600;

struct {
    uint64_t packet_count_from_fc;
} stats;

#endif

static int num_sockets_open;
static int debug_level;

// public web-site that will be allowed. Can be edited with NVRAM editor
static const char *public_origin = "fly.example.com";

#ifndef bool
#define bool int
#endif

void web_server_set_debug(int level)
{
    debug_level = level;
}

void web_debug(int level, const char *fmt, ...)
{
    if (level > debug_level) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    console_vprintf(fmt, ap);
    va_end(ap);
}

#ifdef SYSTEM_FREERTOS
static void lock_state(void)
{
}

static void unlock_state(void)
{
}
#else
static void lock_state(void)
{
    pthread_mutex_lock(&lock);
}

static void unlock_state(void)
{
    pthread_mutex_unlock(&lock);
}
#endif

/*
  destroy socket buffer, writing any pending data
 */
static int sock_buf_destroy(struct sock_buf *sock)
{
    if (sock->add_content_length) {
        /*
          support dynamic content by delaying the content-length
          header. This is needed to keep some anti-virus programs
          (eg. AVG Free) happy. Without a content-length the load of
          dynamic json can hang
         */
        uint32_t body_size = talloc_get_size(sock->buf) - sock->header_length;
        write(sock->fd, sock->buf, sock->header_length);
        char *clen = print_printf(sock, "Content-Length: %u\r\n\r\n", body_size);
        if (clen) {
            write(sock->fd, clen, talloc_get_size(clen));
        }
        write(sock->fd, sock->buf + sock->header_length, body_size);
    } else {
        size_t size = talloc_get_size(sock->buf);
        if (size > 0) {
            write(sock->fd, sock->buf, size);
        }
    }

    lock_state();
    web_debug(3,"closing fd %d num_sockets_open=%d\n", sock->fd, num_sockets_open);
    num_sockets_open--;
    unlock_state();
    
    close(sock->fd);
    return 0;
}

/*
  write to sock_buf
 */
ssize_t sock_write(struct sock_buf *sock, const char *s, size_t size)
{
    size_t current_size = talloc_get_size(sock->buf);
    ssize_t ret;
    if (!sock->add_content_length &&
        (size >= 1000 || (size >= 200 && current_size == 0))) {
        if (current_size > 0) {
            ret = write(sock->fd, sock->buf, current_size);
            if (ret != current_size) {
                return -1;
            }
            talloc_free(sock->buf);
            sock->buf = NULL;
        }
        ret = write(sock->fd, s, size);
    } else {
        sock->buf = talloc_realloc_size(sock, sock->buf, current_size + size);
        if (sock->buf) {
            memcpy(sock->buf + current_size, s, size);
        }
        ret = size;
    }
    return ret;
}

/*
  print to socket buffer
 */
void sock_printf(struct sock_buf *sock, const char *fmt, ...)
{
    if (strchr(fmt, '%') == NULL) {
        // simple string
        sock_write(sock, fmt, strlen(fmt));
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    char *buf2 = print_vprintf(sock, fmt, ap);
#ifdef SYSTEM_FREERTOS
    sock_write(sock, buf2, talloc_get_size(buf2));
#else
    size_t size = talloc_get_size(buf2);
    if (buf2 && size > 0 && buf2[size-1] == 0) {
        // cope with differences in print_vprintf between platforms
        size--;
    }
    sock_write(sock, buf2, size);
#endif
    talloc_free(buf2);
    va_end(ap);
}


/*
  destroy a connection
*/
void connection_destroy(struct connection_state *c)
{
    talloc_free(c);
#ifdef SYSTEM_FREERTOS
    vTaskDelete(NULL);
#else
    pthread_exit(0);
#endif
}


#ifndef SYSTEM_FREERTOS
/*
  write some data to the flight controller
 */
void mavlink_fc_write(const uint8_t *buf, size_t size)
{
    if (serial_port_fd != -1) {
        write(serial_port_fd, buf, size);
    }
    if (fc_udp_in_fd != -1) {
        if (fc_addrlen != 0) {
            sendto(fc_udp_in_fd, buf, size, 0, (struct sockaddr*)&fc_addr, fc_addrlen);
        }
    }
}

/*
  send a mavlink message to flight controller
 */
void mavlink_fc_send(mavlink_message_t *msg)
{
    if (serial_port_fd != -1) {
        _mavlink_resend_uart(MAVLINK_COMM_FC, msg);
    }
    if (fc_udp_in_fd != -1) {
        uint8_t buf[600];
        uint16_t len = mavlink_msg_to_send_buffer(buf, msg);
        mavlink_fc_write(buf, len);
    }
}

/*
  send a mavlink msg over WiFi
 */
static void mavlink_broadcast(int fd, mavlink_message_t *msg)
{
    uint8_t buf[300];
    uint16_t len = mavlink_msg_to_send_buffer(buf, msg);
    if (len > 0) {
        struct sockaddr_in addr;
        memset(&addr, 0x0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        addr.sin_port = htons(14550);
        
        sendto(fd, buf, len, 0, (struct sockaddr*)&addr, sizeof(addr));
    }
}
#endif

/*
  send a mavlink msg over WiFi to a single target
 */
static void mavlink_send_udp_out(mavlink_message_t *msg)
{
    if (udp_out_fd == -1) {
        return;
    }
    uint8_t buf[300];
    uint16_t len = mavlink_msg_to_send_buffer(buf, msg);
    if (len > 0) {
        sendto(udp_out_fd, buf, len, 0, (struct sockaddr*)&udp_out_addr, sizeof(udp_out_addr));
    }
}

/*
  process input on a connection
*/
static void connection_process(struct connection_state *c)
{
    if (!c->cgi->setup(c->cgi)) {
        connection_destroy(c);
        return;
    }
    c->cgi->load_variables(c->cgi);
    web_debug(3, "processing '%s' on %d num_sockets_open=%d\n", c->cgi->pathinfo, c->cgi->sock->fd, num_sockets_open);
    c->cgi->download(c->cgi, c->cgi->pathinfo);
    web_debug(3, "destroying '%s' fd=%d\n", c->cgi->pathinfo, c->cgi->sock->fd);
    connection_destroy(c);
}

/*
  check origin header to prevent attacks by javascript in other web pages
 */
static bool check_origin(const char *origin)
{
    if (strcmp(origin, "http://10.0.1.128") == 0) {
        // always accept
        return true;
    }

#ifdef SYSTEM_FREERTOS
    // could be a different local IP
    char local_ip[16];
    get_local_ip(local_ip, sizeof(local_ip));
    if (strncmp(origin, "http://", 7) == 0 &&
        strcmp(origin+7, local_ip) == 0) {
        return true;
    }
#else
    // TODO: check all IPs on all interfaces?
#endif

    // also allow file:// URLs which produce a 'null' origin
    if (strcmp(origin, "null") == 0) {
        return true;
    }
    char *allowed_origin;
#ifdef SYSTEM_FREERTOS
    unsigned origin_length = 0;
    if (snx_nvram_get_data_len("SkyViper", "AllowedOrigin", &origin_length) != NVRAM_SUCCESS) {
        return false;
    }
    allowed_origin = talloc_zero_size(NULL, origin_length);
    if (allowed_origin == NULL) {
        return false;
    }
    if (snx_nvram_string_get("SkyViper", "AllowedOrigin", allowed_origin) != NVRAM_SUCCESS) {
        talloc_free(allowed_origin);
        return false;
    }
#else
    allowed_origin = talloc_zero_size(NULL, 1);
#endif
    // check for wildcard allowed origin
    if (strcmp(allowed_origin, "*") == 0) {
        talloc_free(allowed_origin);
        return true;
    }

    // allow for http:// or https://
    if (strncmp(origin, "http://", 7) == 0) {
        origin += 7;
    } else if (strncmp(origin, "https://", 8) == 0) {
        origin += 8;
    } else {
        console_printf("Denied origin protocol: [%s]\n", origin);
        talloc_free(allowed_origin);
        return false;
    }
    
    if (strcmp(allowed_origin, origin) != 0) {
        console_printf("Denied origin: [%s] allowed: [%s]\n", origin, allowed_origin);
        talloc_free(allowed_origin);
        return false;
    }
    talloc_free(allowed_origin);
    return true;
}

/*
  setup AllowedOrigin if not set already
 */
static void setup_origin(const char *origin)
{
#ifdef SYSTEM_FREERTOS
    unsigned origin_length = 0;
    if (snx_nvram_get_data_len("SkyViper", "AllowedOrigin", &origin_length) != NVRAM_SUCCESS ||
        origin_length == 0) {
        snx_nvram_string_set("SkyViper", "AllowedOrigin", __DECONST(char *,origin));
    }
#endif
}

void sig_pipe_handler(int signum)
{
    web_debug(3, "Caught signal SIGPIPE %d\n",signum);
}

/*
  task for web_server
*/
#ifdef SYSTEM_FREERTOS
static void web_server_connection_process(void *pvParameters)
{
    struct connection_state *c = pvParameters;
    c->cgi = cgi_init(c, c->sock);
    if (!c->cgi) {
        connection_destroy(c);
        return;
    }

    c->cgi->check_origin = check_origin;

    connection_process(c);
}
#else
static void *web_server_connection_process(void *arg)
{
    int fd = (intptr_t)arg;
    // new talloc tree per thread
    struct connection_state *c = talloc_zero(NULL, struct connection_state);
    c->sock = talloc_zero(c, struct sock_buf);
    c->sock->fd = fd;

    lock_state();

    num_sockets_open++;
    
    web_debug(4,"Opened connection %d num_sockets_open=%d\n", fd, num_sockets_open);

    unlock_state();
    
    talloc_set_destructor(c->sock, sock_buf_destroy);
    c->cgi = cgi_init(c, c->sock);
    if (!c->cgi) {
        connection_destroy(c);
        return NULL;
    }

    c->cgi->check_origin = check_origin;

    connection_process(c);
    return NULL;
}
#endif

#ifdef SYSTEM_FREERTOS
/*
  task for web_server
*/
void web_server_task_process(void *pvParameters)
{
    int listen_sock;

    struct sockaddr_in addr;

    // setup default allowed origin
    setup_origin(public_origin);
    
    if ((listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        goto end;
    }

    memset(&addr, 0x0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(WEB_SERVER_PORT);

    if (bind(listen_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        goto end;
    }

    if (listen(listen_sock, 20) < 0) {
        goto end;
    }

    while (1)
    {
        fd_set fds;
        struct timeval tv;
        int numfd = listen_sock+1;

        FD_ZERO(&fds);
        FD_SET(listen_sock, &fds);

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int res = select(numfd, &fds, NULL, NULL, &tv);
        if (res <= 0) {
            continue;
        }

        if (FD_ISSET(listen_sock, &fds)) {
            // new connection
            struct sockaddr_in addr;
            int len = sizeof(struct sockaddr_in);
            int fd = accept(listen_sock, (struct sockaddr *)&addr, (socklen_t *)&len);
            if (fd != -1) {
                struct connection_state *c = talloc_zero(NULL, struct connection_state);
                if (c == NULL) {
                    close(fd);
                    continue;
                }
                c->sock = talloc_zero(c, struct sock_buf);
                if (!c->sock) {
                    talloc_free(c);
                    close(fd);
                    continue;
                }
                c->sock->fd = fd;
                num_sockets_open++;
                talloc_set_destructor(c->sock, sock_buf_destroy);
                xTaskCreate(web_server_connection_process, "http_connection", STACK_SIZE_4K, c, 10, &c->task);
            }
        }
    }

end:
    vTaskDelete(NULL);
}
#else
void do_http_accept(const int sockfd)
{
    int fd = accept(sockfd, NULL,0);
    if (fd == -1) {
        console_printf("accept failed: %s\n", strerror(errno));
        goto BAD_ACCEPT;
    };

    // use a thread per connection. This allows for sending MAVLink messages
    // via mavlink_fc_send() from connections
    pthread_t thread_id;
    pthread_attr_t thread_attr;

    int perrno;
    if ((perrno = pthread_attr_init(&thread_attr)) != 0) {
        console_printf("pthread_attr_init failed: %s\n", strerror(perrno));
        goto BAD_PTHREAD_INIT;
    }
    if ((perrno = pthread_attr_setdetachstate(&thread_attr, PTHREAD_CREATE_DETACHED)) != 0) {
        console_printf("pthread_attr_setdetachstate failed: %s\n", strerror(perrno));
        goto BAD_PTHREAD_SETDETEACHSTATE;
    }
    if ((perrno = pthread_create(&thread_id, &thread_attr, web_server_connection_process, (void*)(intptr_t)fd)) != 0) {
        console_printf("pthread_create failed: %s\n", strerror(perrno));
        goto BAD_PTHREAD_CREATE;
    }
    pthread_attr_destroy(&thread_attr);
    return;

BAD_PTHREAD_CREATE:
BAD_PTHREAD_SETDETEACHSTATE:
    pthread_attr_destroy(&thread_attr);
BAD_PTHREAD_INIT:
    close(fd);
BAD_ACCEPT:
    return;
}

int uart2_get_baudrate()
{
    return baudrate;
}

unsigned mavlink_fc_pkt_count()
{
    return stats.packet_count_from_fc;
}

/*
  main select loop
 */
static void select_loop(int http_socket_fd, int udp_socket_fd)
{    
    while (1) {
        fd_set fds;
        struct timeval tv;
        int numfd = 0;

        FD_ZERO(&fds);
        if (http_socket_fd != -1) {
            FD_SET(http_socket_fd, &fds);
            if (http_socket_fd >= numfd) {
                numfd = http_socket_fd+1;
            }
        }
        if (udp_socket_fd != -1) {
            FD_SET(udp_socket_fd, &fds);
            if (udp_socket_fd >= numfd) {
                numfd = udp_socket_fd+1;
            }
        }
        if (serial_port_fd != -1) {
            FD_SET(serial_port_fd, &fds);
            if (serial_port_fd >= numfd) {
                numfd = serial_port_fd+1;
            }
        }

        if (fc_udp_in_fd != -1) {
            FD_SET(fc_udp_in_fd, &fds);
            if (fc_udp_in_fd >= numfd) {
                numfd = fc_udp_in_fd+1;
            }
        }

        if (udp_out_fd != -1) {
            FD_SET(udp_out_fd, &fds);
            if (udp_out_fd >= numfd) {
                numfd = udp_out_fd+1;
            }
        }

        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int res = select(numfd, &fds, NULL, NULL, &tv);
        if (res <= 0) {
            continue;
        }

        // check for new incoming tcp connection
        if (http_socket_fd != -1 &&
            FD_ISSET(http_socket_fd, &fds)) {
            do_http_accept(http_socket_fd);
        }

        // check for incoming UDP packet (broadcast)
        if (udp_socket_fd != -1 &&
            FD_ISSET(udp_socket_fd, &fds)) {
            // we have data pending
            uint8_t buf[300];
            ssize_t nread = read(udp_socket_fd, buf, sizeof(buf));
            if (nread > 0) {
                // send the data straight to the flight controller
                mavlink_fc_write(buf, nread);
            }
        }

        // check for incoming UDP packet from udp-out connection:
        if (udp_out_fd != -1 &&
            FD_ISSET(udp_out_fd, &fds)) {
            // we have data pending
            uint8_t buf[1024];
            fc_addrlen = sizeof(fc_addr);
            ssize_t nread = recvfrom(udp_out_fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&udp_out_addr, &udp_out_addrlen);
            if (nread <= 0) {
                /* printf("Read error from udp out connection\n"); */
            } else {
                // send to flight controller
                mavlink_fc_write(buf, nread);
            }
        }

        if (fc_udp_in_fd != -1 &&
            FD_ISSET(fc_udp_in_fd, &fds)) {
            // we have data pending
            uint8_t buf[3000];
            fc_addrlen = sizeof(fc_addr);
            ssize_t nread = recvfrom(fc_udp_in_fd, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&fc_addr, &fc_addrlen);
            if (nread > 0) {
                mavlink_message_t msg;
                mavlink_status_t status;
                for (uint16_t i=0; i<nread; i++) {
                    if (mavlink_parse_char(MAVLINK_COMM_FC, buf[i], &msg, &status)) {
                        stats.packet_count_from_fc++;
                        if (!mavlink_handle_msg(&msg)) {
                            // forward to network connection as a udp broadcast packet
                            if (udp_socket_fd != -1) {
                                mavlink_broadcast(udp_socket_fd, &msg);
                            }
                            // send to udp-out connection
                            mavlink_send_udp_out(&msg);
                        }
                    }
                }
            }
        }

        // check for incoming bytes from flight controller
        if (serial_port_fd != -1 &&
            FD_ISSET(serial_port_fd, &fds)) {
            uint8_t buf[200];
            ssize_t nread = read(serial_port_fd, buf, sizeof(buf));
            if (nread <= 0) {
                printf("Read error from flight controller: %s\n", strerror(errno));
                // we should re-open serial port
                /* exit(1); */
            } else {
                mavlink_message_t msg;
                mavlink_status_t status;
                for (uint16_t i=0; i<nread; i++) {
                    if (mavlink_parse_char(MAVLINK_COMM_FC, buf[i], &msg, &status)) {
                        stats.packet_count_from_fc++;
                        if (!mavlink_handle_msg(&msg)) {
                            // forward to network connection as a udp broadcast packet
                            if (udp_socket_fd != -1) {
                                mavlink_broadcast(udp_socket_fd, &msg);
                            }
                            // send to udp-out connection
                            mavlink_send_udp_out(&msg);
                        }
                    }
                }
            }
        }
    }
}

/*
  open MAVLink serial port
 */
static int mavlink_serial_open(const char *path, unsigned baudrate)
{
    int fd = open(path, O_RDWR);
    if (fd == -1) {
        perror(path);
        return -1;
    }

    struct termios t;
    memset(&t, 0, sizeof(t));

    tcgetattr(fd, &t);
    
    cfsetspeed(&t, baudrate);

    t.c_iflag &= ~(BRKINT | ICRNL | IMAXBEL | IXON | IXOFF);
    t.c_oflag &= ~(OPOST | ONLCR);
    t.c_lflag &= ~(ISIG | ICANON | IEXTEN | ECHO | ECHOE | ECHOK | ECHOCTL | ECHOKE);
    t.c_cc[VMIN] = 0;
    t.c_cflag &= ~CRTSCTS;

    tcsetattr(fd, TCSANOW, &t);

    return fd;
}

/*
  open a TCP listening socket
 */
static int tcp_open(const char *ip, const unsigned port)
{
    struct sockaddr_in sock;
    int listen_sock;
    int one=1;
    
    memset((char *)&sock, 0, sizeof(sock));
    sock.sin_port = htons(port);
    sock.sin_family = AF_INET;
    if (ip != NULL) {
        if (inet_pton(AF_INET, ip, &sock.sin_addr.s_addr) != 1) {
            printf("Failed to convert IP address\n");
            exit(1);
        }
    }
    listen_sock = socket(AF_INET, SOCK_STREAM, 0);

    setsockopt(listen_sock, SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));

    if (bind(listen_sock, (struct sockaddr * ) &sock,sizeof(sock)) != 0) {
        close(listen_sock);
        return -1;
    }

    if (listen(listen_sock, 10) != 0) {
        close(listen_sock);
        return -1;        
    }

    return listen_sock;
}


/*
  open a UDP socket for broadcasting on port 14550

  We will listen on a emphemeral port, and send to the broadcast
  address
 */
static int udp_open(void)
{
    struct sockaddr_in sock;
    int res;
    int one=1;
    
    memset(&sock,0,sizeof(sock));

    sock.sin_port = 0;
    sock.sin_family = AF_INET;

    res = socket(AF_INET, SOCK_DGRAM, 0);
    if (res == -1) { 
        return -1;
    }

    setsockopt(res,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));
    setsockopt(res,SOL_SOCKET,SO_BROADCAST,(char *)&one,sizeof(one));

    if (bind(res, (struct sockaddr *)&sock, sizeof(sock)) < 0) { 
        return(-1); 
    }

    return res;
}

/*
  open a UDP socket for taking messages from the flight controller
 */
static int udp_in_open(int port)
{
    struct sockaddr_in sock;
    int res;
    int one=1;

    memset(&sock,0,sizeof(sock));

    sock.sin_port = htons(port);
    /* sock.sin_addr.s_addr = htonl(INADDR_LOOPBACK); */
    sock.sin_family = AF_INET;

    res = socket(AF_INET, SOCK_DGRAM, 17);
    if (res == -1) { 
        return -1;
    }

    setsockopt(res,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));

    if (bind(res, (struct sockaddr *)&sock, sizeof(sock)) < 0) { 
        return(-1); 
    }

    return res;
}

static int udp_out_open(const char *ip, const int port)
{
    int one=1;

    // prepare sending socket
    struct sockaddr_in send_addr;

    memset(&send_addr,0,sizeof(send_addr));

    send_addr.sin_port = 0;
    send_addr.sin_family = AF_INET;

    int res = socket(AF_INET, SOCK_DGRAM, 0);
    if (res == -1) {
        return -1;
    }

    setsockopt(res,SOL_SOCKET,SO_REUSEADDR,(char *)&one,sizeof(one));

    if (bind(res, (struct sockaddr *)&send_addr, sizeof(send_addr)) < 0) {
        return(-1);
    }

    // prepare destination address
    memset(&udp_out_addr,0x0,sizeof(udp_out_addr));
    udp_out_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip, &udp_out_addr.sin_addr.s_addr) != 1) {
        printf("Failed to convert IP address\n");
        exit(1);
    }
    udp_out_addr.sin_family = AF_INET;
    udp_out_addr.sin_port = htons(port);

    return res;
}

void test_config();

/* main program, start listening and answering queries */
int main(int argc, char *argv[])
{
    extern char *optarg;
    int opt;
    const char *serial_port = NULL;
    const char *usage = "Usage: web_server -p http_port -b baudrate -s serial_port -d debug_level -u -f fc_udp_in -O udp-out-address:port";
    bool do_udp_broadcast = 0;
    int fc_udp_in_port = -1;
    const char *udp_out_arg = NULL; // e.g. 1.2.3.4:6543
    const char *http_port_arg = NULL; // e.g. 1.2.3.4:6543 or 6543

    // setup default allowed origin
    setup_origin(public_origin);

    /* test_config(); */
    /* exit(1); */

    while ((opt=getopt(argc, argv, "p:s:b:hd:uf:O:")) != -1) {
        switch (opt) {
        case 'p':
            http_port_arg = optarg;
            break;
        case 's':
            serial_port = optarg;
            break;
        case 'b':
            baudrate = atoi(optarg);
            break;
        case 'd':
            web_server_set_debug(atoi(optarg));
            break;
        case 'u':
            do_udp_broadcast = 1;
            break;
        case 'f':
            fc_udp_in_port = atoi(optarg);
            break;
        case 'O':
            udp_out_arg = optarg;
            break;
        case 'h':
        default:
            printf("%s\n", usage);
            exit(1);
            break;
        }
    }

    if (fc_udp_in_port !=-1 && serial_port != NULL) {
        // don't want to muck around with multiple mavlink channels
        console_printf("Only one of serial port and udp-in-port (-s and -u) can be supplied");
        exit(1);
    }
    if (serial_port == NULL) {
        baudrate = -1;
    }

    // summarily ignore SIGPIPE; without this, if a download is
    // interrupted sock_write's write() call will kill the process
    // with SIGPIPE
    web_debug(4, "Ignoring sig pipe\n");
    if (signal(SIGPIPE, sig_pipe_handler) == SIG_ERR) {
        console_printf("Failed to ignore SIGPIPE: %m\n");
    }

    pthread_mutex_init(&lock, NULL);
    
    if (serial_port) {
        serial_port_fd = mavlink_serial_open(serial_port, baudrate);
        if (serial_port_fd == -1) {
            printf("Failed to open mavlink serial port %s\n", serial_port);
            exit(1);
        }
    }

    int udp_socket_fd = -1;
    if (do_udp_broadcast) {
        udp_socket_fd = udp_open();
        if (udp_socket_fd == -1) {
            printf("Failed to open UDP socket\n");
            exit(1);
        }
    }

    int http_socket_fd = -1;
    if (http_port_arg != NULL) {
        char *colon = strchr(http_port_arg, ':');
        if (colon == NULL) {
            // just a port number
            http_socket_fd = tcp_open(NULL, atoi(http_port_arg));
        } else {
            *colon = '\0';
            http_socket_fd = tcp_open(http_port_arg, atoi(colon+1));
        }

        if (http_socket_fd == -1) {
            printf("Failed to open TCP socket\n");
            exit(1);
        }
    }

    if (fc_udp_in_port != -1) {
        fc_udp_in_fd = udp_in_open(fc_udp_in_port);
        if (fc_udp_in_fd == -1) {
            printf("Failed to open UDP-in socket\n");
            exit(1);
        }
    }

    if (udp_out_arg != NULL) {
        char *colon = strchr(udp_out_arg, ':');
        if (colon == NULL) {
            printf("udp-out address should be e.g. 1.2.3.4:6543\n");
            exit(1);
        }
        *colon = '\0';
        udp_out_fd = udp_out_open(udp_out_arg, atoi(colon+1));
        if (udp_out_fd == -1) {
            printf("Failed to open UDP-out (%s:%u)\n", udp_out_arg, atoi(colon+1));
            exit(1);
        }
    }

    select_loop(http_socket_fd, udp_socket_fd);

    return 0;
}

#endif
