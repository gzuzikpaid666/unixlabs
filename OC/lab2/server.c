#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define DEFAULT_PORT 5000
#define BACKLOG      16

static volatile sig_atomic_t was_sighup = 0;
static volatile sig_atomic_t was_stop   = 0;

static void on_signal(int sig)
{
    if (sig == SIGHUP)
        was_sighup = 1;
    else
        was_stop = 1;
}

int main(int argc, char *argv[])
{
    int port = (argc > 1) ? atoi(argv[1]) : DEFAULT_PORT;
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Некорректный порт: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }
    if (listen(server_fd, BACKLOG) == -1) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sa.sa_flags   = 0;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGHUP,  &sa, NULL) == -1 ||
        sigaction(SIGINT,  &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        close(server_fd);
        return EXIT_FAILURE;
    }

    sigset_t blocked_mask, original_mask;
    sigemptyset(&blocked_mask);
    sigaddset(&blocked_mask, SIGHUP);
    sigaddset(&blocked_mask, SIGINT);
    sigaddset(&blocked_mask, SIGTERM);

    if (sigprocmask(SIG_BLOCK, &blocked_mask, &original_mask) == -1) {
        perror("sigprocmask");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Сервер слушает порт %d (PID %d)\n", port, (int)getpid());
    fflush(stdout);

    int client_fd = -1;

    while (!was_stop) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        int max_fd = server_fd;

        if (client_fd != -1) {
            FD_SET(client_fd, &read_fds);
            if (client_fd > max_fd)
                max_fd = client_fd;
        }

        int ready = pselect(max_fd + 1, &read_fds, NULL, NULL, NULL, &original_mask);

        if (ready == -1 && errno != EINTR) {
            perror("pselect");
            break;
        }

        if (was_sighup) {
            was_sighup = 0;
            printf("Получен сигнал SIGHUP\n");
            fflush(stdout);
        }
        if (was_stop) {
            printf("Получен сигнал завершения, выходим\n");
            break;
        }

        if (ready == -1)
            continue;

        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int new_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_len);

            if (new_fd == -1) {
                if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED)
                    perror("accept");
            } else {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                printf("Новое соединение от %s:%d (fd=%d)\n",
                       ip, ntohs(peer.sin_port), new_fd);

                if (client_fd != -1) {
                    printf("  клиент уже есть, соединение fd=%d закрыто\n", new_fd);
                    close(new_fd);
                } else {
                    client_fd = new_fd;
                }
                fflush(stdout);
            }
        }

        if (client_fd != -1 && FD_ISSET(client_fd, &read_fds)) {
            char buffer[1024];
            ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);

            if (n > 0) {
                printf("Получено байт: %zd\n", n);
            } else if (n == 0) {
                printf("Клиент закрыл соединение (fd=%d)\n", client_fd);
                close(client_fd);
                client_fd = -1;
            } else if (errno != EINTR) {
                perror("recv");
                close(client_fd);
                client_fd = -1;
            }
            fflush(stdout);
        }
    }

    if (client_fd != -1)
        close(client_fd);
    close(server_fd);
    sigprocmask(SIG_SETMASK, &original_mask, NULL);
    return EXIT_SUCCESS;
}