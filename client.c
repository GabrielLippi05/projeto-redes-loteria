#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define SERVER_IP_PADRAO "127.0.0.1"
#define BUFFER_SIZE 1024

// -------------------------------------------------------------
// Envia todos os bytes (trata envio parcial e EINTR).
// MSG_NOSIGNAL evita que o SIGPIPE derrube o cliente se o servidor
// fechar a conexao no meio de um envio: o erro vira retorno -1.
// -------------------------------------------------------------
static int enviar_tudo(int sock_fd, const char *dados, size_t n) {
    size_t enviados = 0;
    while (enviados < n) {
        ssize_t r = send(sock_fd, dados + enviados, n - enviados, MSG_NOSIGNAL);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        enviados += (size_t)r;
    }
    return 0;
}

// Verdadeiro se a linha digitada e exatamente o comando :quit
static int eh_quit(const char *linha) {
    size_t n = strlen(":quit");
    return strncmp(linha, ":quit", n) == 0 && (linha[n] == '\0' || linha[n] == '\n' || linha[n] == ' ');
}

// -------------------------------------------------------------
// THREAD 1: Lê comandos/apostas do teclado e envia para o Server
// -------------------------------------------------------------
static void* thread_enviar(void *arg) {
    int sock_fd = *(int*)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        // Ctrl+D (EOF) no teclado equivale a :quit
        if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
            enviar_tudo(sock_fd, ":quit\n", strlen(":quit\n"));
            break;
        }

        // Enter vazio é ignorado
        if (strcmp(buffer, "\n") == 0) continue;

        if (enviar_tudo(sock_fd, buffer, strlen(buffer)) < 0) {
            // Conexao perdida: derruba o socket para a thread_receber perceber e sair
            shutdown(sock_fd, SHUT_RDWR);
            break;
        }

        if (eh_quit(buffer)) break;
    }
    return NULL;
}

// -------------------------------------------------------------
// THREAD 2: Escuta respostas do Servidor e imprime na tela
// -------------------------------------------------------------
static void* thread_receber(void *arg) {
    int sock_fd = *(int*)arg;
    char buffer[BUFFER_SIZE];
    ssize_t bytes;

    while (1) {
        bytes = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes <= 0) break;   // 0 = servidor fechou; < 0 = erro de rede

        buffer[bytes] = '\0';
        printf("%s", buffer);
        fflush(stdout);
    }

    if (bytes < 0)
        printf("\n[Cliente] Erro na conexao: %s\n", strerror(errno));
    else
        printf("\n[Cliente] Conexao com o servidor encerrada.\n");
    fflush(stdout);
    return NULL;
}

// -------------------------------------------------------------
// MAIN: Cria o Socket, Conecta e dispara as 2 Threads
// Uso: ./client [IP_DO_SERVIDOR]   (padrao: 127.0.0.1)
// -------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc > 2) {
        fprintf(stderr, "Uso: %s [IP_DO_SERVIDOR]\n", argv[0]);
        return 1;
    }
    const char *ip = (argc == 2) ? argv[1] : SERVER_IP_PADRAO;

    // 1. Configuracao do IP e Porta de Destino
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Endereco IP invalido: %s\n", ip);
        return 1;
    }

    // 2. Criacao do Socket TCP
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Erro ao criar socket");
        return 1;
    }

    printf("Conectando ao servidor em %s:%d...\n", ip, PORT);

    // 3. Conexao com o Servidor (TCP Handshake)
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erro ao conectar no servidor");
        close(sock_fd);
        return 1;
    }

    // 4. Criacao das 2 Threads independentes exigidas no diagrama
    pthread_t t_envio, t_recepcao;
    if (pthread_create(&t_recepcao, NULL, thread_receber, &sock_fd) != 0 ||
        pthread_create(&t_envio, NULL, thread_enviar, &sock_fd) != 0) {
        fprintf(stderr, "Erro ao criar as threads do cliente\n");
        close(sock_fd);
        return 1;
    }

    // Aguarda a recepcao encerrar (servidor caiu, erro de rede ou :quit)
    pthread_join(t_recepcao, NULL);

    // A thread de envio pode estar presa no fgets (esperando o teclado). Ela e
    // cancelada aqui: o cancelamento libera o stdin, senao o exit() poderia travar.
    pthread_cancel(t_envio);
    pthread_join(t_envio, NULL);

    close(sock_fd);
    printf("[Cliente] Aplicacao finalizada.\n");
    return 0;
}
