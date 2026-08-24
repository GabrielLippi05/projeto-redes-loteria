#include <stdio.h>       // Entrada e saída padrão (printf, perror, snprintf)
#include <stdlib.h>      // Funções utilitárias (malloc, free, rand, atoi, exit)
#include <string.h>      // Manipulação de strings (memset, strlen, strncmp, strtok)
#include <unistd.h>      // Chamadas POSIX do sistema (sleep, close)
#include <time.h>        // Funções de data/hora (time, localtime, strftime)
#include <pthread.h>     // Biblioteca de Threads POSIX e controle de Mutex
#include <sys/socket.h>  // Biblioteca de sockets de rede (socket, bind, listen, accept, send, recv)
#include <netinet/in.h>  // Estruturas de endereçamento de rede (struct sockaddr_in)

#include "loteria.h"     // Cabeçalho com a struct Aposta e inicializador_gerador

#define PORT 8080        // Porta TCP padrão utilizada para escutar conexões
#define BUFFER_SIZE 1024 // Tamanho padrão dos buffers de mensagens

// =============================================================================
// VARIÁVEIS GLOBAIS E MEMÓRIA COMPARTILHADA
// =============================================================================

// Mutex para garantir Exclusão Mútua no acesso à memória compartilhada (aposta_atual)
pthread_mutex_t lock_memoria = PTHREAD_MUTEX_INITIALIZER;

// Parâmetros da Loteria configurados dinamicamente via comandos
int cfg_inicio = NUM_MIN_PADRAO;     // Limite inferior do intervalo (padrão: 0)
int cfg_fim = NUM_MAX_PADRAO;        // Limite superior do intervalo (padrão: 100)
int cfg_qtd = QTD_NUMEROS_PADRAO;    // Quantidade de números por bilhete (padrão: 5)

// Ponteiro global que guarda a aposta ativa do usuário
Aposta *aposta_atual = NULL;

// =============================================================================
// THREAD 1: RECEPTOR DE DADOS (Leitura contínua do Socket)
// =============================================================================
void* thread_receptor(void *arg) {
    // Converte o argumento genérico de volta para o descritor de socket inteiro
    int client_socket = *(int*)arg;
    char buffer[BUFFER_SIZE];

    printf("[Thread 1] Receptor ativo. Aguardando comandos ou apostas...\n");

    // Loop infinito para manter a escuta contínua das mensagens do cliente
    while (1) {
        // Zera o buffer com bytes nulos para não carregar lixo de mensagens antigas
        memset(buffer, 0, sizeof(buffer));

        // recv() bloqueia a thread até o cliente enviar bytes pela rede
        int bytes = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        // Se retornar <= 0, o cliente encerrou a conexão ou ocorreu erro fatal
        if (bytes <= 0) {
            printf("[Thread 1] Conexao com o cliente foi encerrada.\n");
            break; // Sai do loop para encerrar a thread
        }

        // Remove quebras de linha '\r' e '\n' deixadas pelo fgets do cliente
        buffer[strcspn(buffer, "\r\n")] = '\0';

        // Se o usuário apenas apertou Enter vazio, ignora e continua ouvindo
        if (strlen(buffer) == 0) continue;

        printf("[Servidor recebeu]: \"%s\"\n", buffer);

        // -------------------------------------------------------------
        // CENÁRIO A: Processamento de Comandos (Começam com ':')
        // -------------------------------------------------------------
        if (buffer[0] == ':') {
            char resposta[BUFFER_SIZE];

            if (strncmp(buffer, ":inicio", 7) == 0) {
                sscanf(buffer, ":inicio %d", &cfg_inicio);
                snprintf(resposta, sizeof(resposta), "[Config] Inicio alterado para: %d\n", cfg_inicio);
            } 
            else if (strncmp(buffer, ":fim", 4) == 0) {
                sscanf(buffer, ":fim %d", &cfg_fim);
                snprintf(resposta, sizeof(resposta), "[Config] Fim alterado para: %d\n", cfg_fim);
            } 
            else if (strncmp(buffer, ":qtd", 4) == 0) {
                sscanf(buffer, ":qtd %d", &cfg_qtd);
                snprintf(resposta, sizeof(resposta), "[Config] Quantidade de numeros alterada para: %d\n", cfg_qtd);
            } 
            else {
                snprintf(resposta, sizeof(resposta), "[Erro] Comando invalido. Use :inicio, :fim ou :qtd\n");
            }

            // Devolve a confirmação da alteração de configuração para o terminal do cliente
            send(client_socket, resposta, strlen(resposta), 0);
        }
        // -------------------------------------------------------------
        // CENÁRIO B: Processamento de Apostas (Números separados por espaços)
        // -------------------------------------------------------------
        else {
            int numeros_lidos[100];
            int contador = 0;

            // strtok() fatia a string buffer utilizando o caractere de espaço como delimitador
            char *token = strtok(buffer, " ");
            while (token != NULL && contador < 100) {
                numeros_lidos[contador++] = atoi(token); // Converte string para int
                token = strtok(NULL, " ");               // Pega o próximo token
            }

            // Valida se a quantidade de números bate exatamente com a configuração atual
            if (contador != cfg_qtd) {
                char aviso_erro[BUFFER_SIZE];
                snprintf(aviso_erro, sizeof(aviso_erro), "[Erro] Voce deve apostar exatamente %d numeros!\n", cfg_qtd);
                send(client_socket, aviso_erro, strlen(aviso_erro), 0);
                continue; // Volta para o início do loop sem salvar a aposta
            }

            // --- REGIÃO CRÍTICA (Escrita na memória compartilhada com Mutex) ---
            pthread_mutex_lock(&lock_memoria); // Tranca o acesso exclusivo

            // Se já existia uma aposta anterior da rodada atual, desaloca a antiga
            if (aposta_atual != NULL) {
                liberar_aposta(aposta_atual);
            }

            // Aloca a nova aposta dinamicamente com base na quantidade configurada
            aposta_atual = criar_aposta(client_socket, cfg_qtd);
            if (aposta_atual != NULL) {
                for (int i = 0; i < cfg_qtd; i++) {
                    aposta_atual->numeros_aposta[i] = numeros_lidos[i];
                }
            }

            pthread_mutex_unlock(&lock_memoria); // Destranca o acesso

            char *msg_sucesso = "[Banca] Aposta registrada com sucesso! Aguarde o sorteio da rodada.\n";
            send(client_socket, msg_sucesso, strlen(msg_sucesso), 0);
        }
    }

    return NULL;
}

// =============================================================================
// THREAD 2: TEMPORIZADOR E SORTEIO AUTOMÁTICO (Execução a cada 60s)
// =============================================================================
void* thread_temporizador(void *arg) {
    int client_socket = *(int*)arg;

    // Loop contínuo das rodadas de sorteio
    while (1) {
        // Pausa a execução da Thread 2 durante 60 segundos (1 minuto)
        sleep(60);

        printf("[Thread 2] 1 minuto decorrido! Realizando o sorteio...\n");

        // Aloca espaço temporário para guardar os números sorteados da rodada
        int *sorteados = (int*)malloc(cfg_qtd * sizeof(int));
        if (sorteados == NULL) continue;

        // Inicializa a semente pseudoaleatória com base no relógio do sistema
        inicializador_gerador();

        // Algoritmo para sortear números únicos dentro do intervalo [cfg_inicio, cfg_fim]
        int total_sorteados = 0;
        while (total_sorteados < cfg_qtd) {
            int sorteado = (rand() % (cfg_fim - cfg_inicio + 1)) + cfg_inicio;
            
            // Verifica se o número já foi sorteado nesta mesma rodada
            int repetido = 0;
            for (int j = 0; j < total_sorteados; j++) {
                if (sorteados[j] == sorteado) {
                    repetido = 1;
                    break;
                }
            }

            // Só insere no array se não houver repetição
            if (!repetido) {
                sorteados[total_sorteados++] = sorteado;
            }
        }

        // Constrói o cabeçalho do relatório com os números premiados
        char relatorio[BUFFER_SIZE];
        char temp[128];
        snprintf(relatorio, sizeof(relatorio), "\n================ SORTEIO DA RODADA ================\nSorteados: ");
        for (int k = 0; k < cfg_qtd; k++) {
            snprintf(temp, sizeof(temp), "[%d] ", sorteados[k]);
            strncat(relatorio, temp, sizeof(relatorio) - strlen(relatorio) - 1);
        }
        strncat(relatorio, "\n", sizeof(relatorio) - strlen(relatorio) - 1);

        // --- REGIÃO CRÍTICA (Leitura e limpeza da memória compartilhada) ---
        pthread_mutex_lock(&lock_memoria); // Tranca a memória contra alterações da Thread 1

        // Caso o usuário tenha realizado uma aposta antes do sorteio
        if (aposta_atual != NULL) {
            int total_acertos = 0;
            char acertos_str[BUFFER_SIZE] = "Numeros que voce acertou: ";

            // Compara os números da aposta com o vetor de números sorteados
            for (int u = 0; u < aposta_atual->qtd_numeros; u++) {
                for (int s = 0; s < cfg_qtd; s++) {
                    if (aposta_atual->numeros_aposta[u] == sorteados[s]) {
                        total_acertos++;
                        snprintf(temp, sizeof(temp), "[%d] ", aposta_atual->numeros_aposta[u]);
                        strncat(acertos_str, temp, sizeof(acertos_str) - strlen(acertos_str) - 1);
                    }
                }
            }

            if (total_acertos == 0) {
                strncat(acertos_str, "Nenhum", sizeof(acertos_str) - strlen(acertos_str) - 1);
            }

            snprintf(temp, sizeof(temp), "\nTotal de acertos: %d\n====================================================\n\n", total_acertos);
            strncat(relatorio, acertos_str, sizeof(relatorio) - strlen(relatorio) - 1);
            strncat(relatorio, temp, sizeof(relatorio) - strlen(relatorio) - 1);

            // Libera a memória da aposta conferida e zera o ponteiro para o próximo minuto
            liberar_aposta(aposta_atual);
            aposta_atual = NULL;
        } 
        // Caso o minuto tenha passado sem nenhuma aposta registrada
        else {
            strncat(relatorio, "Voce nao realizou nenhuma aposta nesta rodada.\n====================================================\n\n", sizeof(relatorio) - strlen(relatorio) - 1);
        }

        pthread_mutex_unlock(&lock_memoria); // Destranca a memória compartilhada

        // Envia o boletim completo com o sorteio e conferência para o cliente
        send(client_socket, relatorio, strlen(relatorio), 0);

        // Desaloca o vetor dinâmico de sorteados da rodada
        free(sorteados);
    }

    return NULL;
}

// =============================================================================
// FUNÇÃO PRINCIPAL (Configuração do Socket TCP e Inicialização)
// =============================================================================
int main() {
    int server_fd, client_socket;
    struct sockaddr_in server_addr;
    pthread_t t_receptor, t_temporizador;

    // 1. Criação do Socket TCP (AF_INET = IPv4, SOCK_STREAM = TCP)
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Erro ao criar socket");
        return 1;
    }

    // Configura a opção SO_REUSEADDR para permitir reiniciar o servidor sem travar a porta
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 2. Preenchimento da estrutura com o endereço IP e a porta de escuta
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Aceita conexões em qualquer interface local
    server_addr.sin_port = htons(PORT);        // Converte a porta para Network Byte Order

    // 3. Associa o socket ao IP e Porta configurados (bind)
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erro no bind");
        close(server_fd);
        return 1;
    }

    // 4. Coloca o socket em modo passivo de escuta (listen)
    listen(server_fd, 5);
    printf("[Servidor] Aguardando conexao de cliente na porta %d...\n", PORT);

    // 5. Bloqueia a execução aguardando a chegada da conexão do cliente (accept)
    client_socket = accept(server_fd, NULL, NULL);
    if (client_socket < 0) {
        perror("Erro no accept");
        close(server_fd);
        return 1;
    }

    // 6. Monta e transmite a MSG1 obrigatória: "<HH:MM:SS>: CONECTADO!!\n"
    time_t agora = time(NULL);
    struct tm *t = localtime(&agora);
    char msg1[100];
    snprintf(msg1, sizeof(msg1), "<%02d:%02d:%02d>: CONECTADO!!\n", t->tm_hour, t->tm_min, t->tm_sec);
    send(client_socket, msg1, strlen(msg1), 0);

    // 7. Criação das duas threads concorrentes passando o client_socket por referência
    pthread_create(&t_receptor, NULL, thread_receptor, (void*)&client_socket);
    pthread_create(&t_temporizador, NULL, thread_temporizador, (void*)&client_socket);

    // 8. pthread_join segura a main() até a thread_receptor terminar (cliente desconectar)
    pthread_join(t_receptor, NULL);

    // 9. Com a saída do cliente, cancela o temporizador que estava dormindo no loop
    pthread_cancel(t_temporizador);

    // 10. Fechamento seguro dos sockets
    close(client_socket);
    close(server_fd);
    printf("[Servidor] Conexoes encerradas e servidor finalizado com sucesso.\n");

    return 0;
}