#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 8080
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024
static pthread_t t_envio;

// -------------------------------------------------------------
// THREAD 1: Lê comandos/apostas do teclado e envia para o Server
// -------------------------------------------------------------
void* thread_enviar(void *arg) {
    int sock_fd = *(int*)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        // Lê a linha inteira digitada pelo usuário no terminal
        if (fgets(buffer, sizeof(buffer), stdin) != NULL) 
        {
            // Se o usuário apertar apenas Enter vazio, ignora
            if (strcmp(buffer, "\n") == 0) continue;

            // Envia o texto pela rede para o Servidor
            if(send(sock_fd, buffer, strlen(buffer), 0) <= 0) 
            {
                break; 
            }
            //FASE 2: Se o usuario digitou :quit, encerra o loop de envio
            if(strncmp(buffer, ":quit", 5) == 0)
            {
                break;
            }
        }
        else
        {
            break;
        }
      
    }
    return NULL;
}

// -------------------------------------------------------------
// THREAD 2: Escuta respostas do Servidor e imprime na tela
// -------------------------------------------------------------
void* thread_receber(void *arg) {
    int sock_fd = *(int*)arg;
    char buffer[BUFFER_SIZE];

    while (1) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(sock_fd, buffer, sizeof(buffer) - 1, 0);

        // Se o servidor fechou a conexão ou deu erro
        if (bytes <= 0) {
            printf("\n[Cliente] Conexão com o servidor encerrada.\n");
            //FASE 2: Cancela a thread_enviar para ela destravar o fgets
            pthread_cancel(t_envio);
            break;
        }

        // Imprime a mensagem vinda do servidor na tela
        printf("%s", buffer);
        fflush(stdout);
    }
    return NULL;
}

// -------------------------------------------------------------
// MAIN: Cria o Socket, Conecta e dispara as 2 Threads
// -------------------------------------------------------------
int main() {
    int sock_fd;
    struct sockaddr_in server_addr;
    pthread_t t_envio, t_recepcao;

    // 1. Criação do Socket TCP
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("Erro ao criar socket");
        return 1;
    }

    // 2. Configuração do IP e Porta de Destino
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &server_addr.sin_addr);

    printf("Conectando ao servidor em %s:%d...\n", SERVER_IP, PORT);

    // 3. Conexão com o Servidor (TCP Handshake)
    if (connect(sock_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erro ao conectar no servidor");
        close(sock_fd);
        return 1;
    }

    // 4. Criação das 2 Threads independentes exigidas no diagrama
    pthread_t t_recepcao; //FASE2
    pthread_create(&t_envio, NULL, thread_enviar, (void*)&sock_fd);
    pthread_create(&t_recepcao, NULL, thread_receber, (void*)&sock_fd);
    

    // Aguarda a recepção encerrar primeiro (desconexão ou :quit)
    pthread_join(t_recepcao, NULL);

    //Fecha o socket e limpa a thread de envio
    close(sock_fd);
    pthread_join(t_envio, NULL);

    printf("[Cliente] Aplicação finalizada com sucesso.\n");
    return 0;
}