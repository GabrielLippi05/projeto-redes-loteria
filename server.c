#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "loteria.h"     // struct Aposta, criar_aposta, liberar_aposta, constantes padrao

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_NUMEROS 100          // limite de numeros por aposta (tamanho de numeros_lidos)
#define INTERVALO_SORTEIO 60     // segundos entre sorteios

// =============================================================================
// ESTRUTURAS
// =============================================================================

// No da lista encadeada de apostas (uma lista POR CLIENTE)
typedef struct ApostaNode {
    Aposta *aposta;
    struct ApostaNode *proximo;
} ApostaNode;

// Contexto de um cliente: tudo que antes era global agora vive aqui,
// entao cada cliente e totalmente independente dos outros.
typedef struct {
    int socket;
    int aceito;                  // 1 = dentro do limite, 0 = limite excedido

    // Configuracao da loteria deste cliente
    int cfg_inicio;
    int cfg_fim;
    int cfg_qtd;

    // Apostas da rodada atual (memoria compartilhada entre as 2 threads do cliente)
    ApostaNode *lista_apostas;
    ApostaNode *fim_lista;

    pthread_mutex_t lock;        // protege cfg_*, lista_apostas e encerrar
    pthread_cond_t  cond;        // acorda o temporizador quando o cliente sai
    int encerrar;                // flag: 1 = temporizador deve terminar

    pthread_mutex_t lock_envio;  // evita que 2 threads misturem send() no mesmo socket
    unsigned int seed;           // semente propria para rand_r (thread-safe)
} ClienteCtx;

// =============================================================================
// ESTADO GLOBAL DO SERVIDOR (controle de vagas)
// =============================================================================
static pthread_mutex_t lock_clientes = PTHREAD_MUTEX_INITIALIZER;
static int clientes_ativos = 0;
static int max_clientes = 0;

// =============================================================================
// FUNCOES AUXILIARES
// =============================================================================

// Envia texto ao cliente com exclusao mutua no socket
static void enviar(ClienteCtx *c, const char *msg) {
    pthread_mutex_lock(&c->lock_envio);
    send(c->socket, msg, strlen(msg), MSG_NOSIGNAL);
    pthread_mutex_unlock(&c->lock_envio);
}

// Anexa texto formatado a um buffer dinamico (cresce sozinho)
static void anexar(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    while (1) {
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n < *cap - *len) { *len += n; return; }
        *cap = (*cap + n + 1) * 2;
        char *novo = realloc(*buf, *cap);
        if (!novo) return;
        *buf = novo;
    }
}

// Libera todos os nos (e apostas) de uma lista
static void liberar_lista(ApostaNode *no) {
    while (no) {
        ApostaNode *prox = no->proximo;
        liberar_aposta(no->aposta);
        free(no);
        no = prox;
    }
}

// =============================================================================
// THREAD 2 (por cliente): TEMPORIZADOR E SORTEIO A CADA 60s
// =============================================================================
static void* thread_temporizador(void *arg) {
    ClienteCtx *c = (ClienteCtx*)arg;

    while (1) {
        // --- Espera 60s, mas acorda antes se o cliente desconectar ---
        struct timespec limite;
        clock_gettime(CLOCK_REALTIME, &limite);
        limite.tv_sec += INTERVALO_SORTEIO;

        pthread_mutex_lock(&c->lock);
        int rc = 0;
        while (!c->encerrar && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&c->cond, &c->lock, &limite);

        if (c->encerrar) {
            pthread_mutex_unlock(&c->lock);
            break;
        }

        // Tira uma "foto" da config e pega a lista inteira (zera para a proxima rodada)
        int inicio = c->cfg_inicio, fim = c->cfg_fim, qtd = c->cfg_qtd;
        ApostaNode *lista = c->lista_apostas;
        c->lista_apostas = NULL;
        c->fim_lista = NULL;
        pthread_mutex_unlock(&c->lock);

        // --- Sorteio de numeros unicos em [inicio, fim] ---
        int *sorteados = malloc(qtd * sizeof(int));
        if (!sorteados) { liberar_lista(lista); continue; }

        int total = 0;
        while (total < qtd) {
            int n = (rand_r(&c->seed) % (fim - inicio + 1)) + inicio;
            int repetido = 0;
            for (int j = 0; j < total; j++)
                if (sorteados[j] == n) { repetido = 1; break; }
            if (!repetido) sorteados[total++] = n;
        }

        // --- Monta o relatorio ---
        size_t cap = 1024, len = 0;
        char *rel = malloc(cap);
        if (!rel) { free(sorteados); liberar_lista(lista); continue; }
        rel[0] = '\0';

        anexar(&rel, &len, &cap, "\n================ SORTEIO DA RODADA ================\nSorteados: ");
        for (int k = 0; k < qtd; k++)
            anexar(&rel, &len, &cap, "[%d] ", sorteados[k]);
        anexar(&rel, &len, &cap, "\n");

        if (lista != NULL) {
            int numero = 1;
            for (ApostaNode *no = lista; no; no = no->proximo, numero++) {
                Aposta *ap = no->aposta;
                int acertos = 0;
                anexar(&rel, &len, &cap, "Aposta %d: ", numero);
                for (int u = 0; u < ap->qtd_numeros; u++) {
                    anexar(&rel, &len, &cap, "[%d] ", ap->numeros_aposta[u]);
                    for (int s = 0; s < qtd; s++)
                        if (ap->numeros_aposta[u] == sorteados[s]) { acertos++; break; }
                }
                anexar(&rel, &len, &cap, "\nAcertos: %d\n\n", acertos);
            }
            anexar(&rel, &len, &cap, "====================================================\n\n");
        } else {
            anexar(&rel, &len, &cap,
                   "Voce nao realizou nenhuma aposta nesta rodada.\n"
                   "====================================================\n\n");
        }

        printf("[Thread 2 | socket %d] Sorteio realizado.\n", c->socket);
        enviar(c, rel);

        free(rel);
        free(sorteados);
        liberar_lista(lista);
    }
    return NULL;
}

// =============================================================================
// THREAD 1 (por cliente): RECEPTOR DE COMANDOS E APOSTAS
// Retorna quando o cliente desconecta ou envia :quit
// =============================================================================
static void receptor(ClienteCtx *c) {
    char buffer[BUFFER_SIZE];

    while (1) {
        memset(buffer, 0, sizeof(buffer));
        int bytes = recv(c->socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes <= 0) {
            printf("[Thread 1 | socket %d] Cliente desconectou.\n", c->socket);
            break;
        }

        buffer[strcspn(buffer, "\r\n")] = '\0';
        if (strlen(buffer) == 0) continue;

        printf("[Socket %d recebeu]: \"%s\"\n", c->socket, buffer);

        // ------------------------- COMANDOS (':') -------------------------
        if (buffer[0] == ':') {
            char resp[BUFFER_SIZE];

            if (strcmp(buffer, ":quit") == 0) {
                enviar(c, "[Banca] Desconectando. Ate logo!\n");
                break;
            }

            int valor;
            pthread_mutex_lock(&c->lock);
            if (strncmp(buffer, ":inicio", 7) == 0) {
                if (sscanf(buffer, ":inicio %d", &valor) != 1)
                    snprintf(resp, sizeof(resp), "[Erro] Uso: :inicio <NUMERO>\n");
                else if (valor >= c->cfg_fim)
                    snprintf(resp, sizeof(resp), "[Erro] Inicio deve ser menor que o fim (%d)\n", c->cfg_fim);
                else if (c->cfg_fim - valor + 1 < c->cfg_qtd)
                    snprintf(resp, sizeof(resp), "[Erro] Intervalo menor que a quantidade de numeros (%d)\n", c->cfg_qtd);
                else {
                    c->cfg_inicio = valor;
                    snprintf(resp, sizeof(resp), "[Config] Inicio alterado para: %d\n", valor);
                }
            }
            else if (strncmp(buffer, ":fim", 4) == 0) {
                if (sscanf(buffer, ":fim %d", &valor) != 1)
                    snprintf(resp, sizeof(resp), "[Erro] Uso: :fim <NUMERO>\n");
                else if (valor <= c->cfg_inicio)
                    snprintf(resp, sizeof(resp), "[Erro] Fim deve ser maior que o inicio (%d)\n", c->cfg_inicio);
                else if (valor - c->cfg_inicio + 1 < c->cfg_qtd)
                    snprintf(resp, sizeof(resp), "[Erro] Intervalo menor que a quantidade de numeros (%d)\n", c->cfg_qtd);
                else {
                    c->cfg_fim = valor;
                    snprintf(resp, sizeof(resp), "[Config] Fim alterado para: %d\n", valor);
                }
            }
            else if (strncmp(buffer, ":qtd", 4) == 0) {
                if (sscanf(buffer, ":qtd %d", &valor) != 1)
                    snprintf(resp, sizeof(resp), "[Erro] Uso: :qtd <NUMERO>\n");
                else if (valor < 1 || valor > MAX_NUMEROS)
                    snprintf(resp, sizeof(resp), "[Erro] Quantidade deve estar entre 1 e %d\n", MAX_NUMEROS);
                else if (valor > c->cfg_fim - c->cfg_inicio + 1)
                    snprintf(resp, sizeof(resp), "[Erro] Quantidade maior que o tamanho do intervalo\n");
                else {
                    c->cfg_qtd = valor;
                    snprintf(resp, sizeof(resp), "[Config] Quantidade de numeros alterada para: %d\n", valor);
                }
            }
            else {
                snprintf(resp, sizeof(resp), "[Erro] Comando invalido. Use :inicio, :fim, :qtd ou :quit\n");
            }
            pthread_mutex_unlock(&c->lock);

            enviar(c, resp);
            continue;
        }

        // ------------------------- APOSTA -------------------------
        int numeros[MAX_NUMEROS];
        int contador = 0;
        char *token = strtok(buffer, " ");
        while (token != NULL && contador < MAX_NUMEROS) {
            numeros[contador++] = atoi(token);
            token = strtok(NULL, " ");
        }

        // Valida tudo e registra dentro da mesma secao critica,
        // assim a config nao muda no meio da validacao.
        char erro[BUFFER_SIZE] = "";
        pthread_mutex_lock(&c->lock);

        if (contador != c->cfg_qtd) {
            snprintf(erro, sizeof(erro), "[Erro] Voce deve apostar exatamente %d numeros!\n", c->cfg_qtd);
        } else {
            for (int i = 0; i < contador && !erro[0]; i++) {
                if (numeros[i] < c->cfg_inicio || numeros[i] > c->cfg_fim) {
                    snprintf(erro, sizeof(erro), "[Erro] Os numeros devem estar entre %d e %d!\n",
                             c->cfg_inicio, c->cfg_fim);
                    break;
                }
                for (int j = i + 1; j < contador; j++) {
                    if (numeros[i] == numeros[j]) {
                        snprintf(erro, sizeof(erro), "[Erro] A aposta nao pode ter numeros repetidos!\n");
                        break;
                    }
                }
            }
        }

        if (!erro[0]) {
            Aposta *nova = criar_aposta(c->socket, c->cfg_qtd);
            ApostaNode *no = nova ? malloc(sizeof(ApostaNode)) : NULL;
            if (!nova || !no) {
                if (nova) liberar_aposta(nova);
                snprintf(erro, sizeof(erro), "[Erro] Falha ao registrar aposta (memoria).\n");
            } else {
                for (int i = 0; i < c->cfg_qtd; i++)
                    nova->numeros_aposta[i] = numeros[i];
                no->aposta = nova;
                no->proximo = NULL;
                // insere no FIM para manter a ordem cronologica
                if (c->fim_lista) c->fim_lista->proximo = no;
                else c->lista_apostas = no;
                c->fim_lista = no;
            }
        }
        pthread_mutex_unlock(&c->lock);

        if (erro[0])
            enviar(c, erro);
        else
            enviar(c, "[Banca] Aposta registrada com sucesso! Aguarde o sorteio da rodada.\n");
    }
}

// =============================================================================
// THREAD DE TRABALHO (uma por cliente conectado)
// =============================================================================
static void* thread_cliente(void *arg) {
    ClienteCtx *c = (ClienteCtx*)arg;

    // --- Limite de clientes excedido: avisa, fecha e libera recursos ---
    if (!c->aceito) {
        enviar(c, "[Servidor] Limite de clientes atingido. Tente novamente mais tarde.\n");
        close(c->socket);
        pthread_mutex_destroy(&c->lock);
        pthread_cond_destroy(&c->cond);
        pthread_mutex_destroy(&c->lock_envio);
        free(c);
        return NULL;
    }

    // --- MSG1: "<HH:MM:SS>: CONECTADO!!" ---
    time_t agora = time(NULL);
    struct tm tm_info;
    localtime_r(&agora, &tm_info);
    char msg1[100];
    snprintf(msg1, sizeof(msg1), "<%02d:%02d:%02d>: CONECTADO!!\n",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    enviar(c, msg1);

    // --- Thread 2 do cliente (temporizador) ---
    pthread_t t_temp;
    int temp_ok = (pthread_create(&t_temp, NULL, thread_temporizador, c) == 0);

    // --- Thread 1 (esta propria thread): recebe comandos/apostas ---
    receptor(c);

    // --- Encerramento: acorda o temporizador e espera ele terminar ---
    pthread_mutex_lock(&c->lock);
    c->encerrar = 1;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->lock);
    if (temp_ok) pthread_join(t_temp, NULL);

    // --- Desaloca os dados deste cliente ---
    close(c->socket);
    liberar_lista(c->lista_apostas);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->lock_envio);

    // --- Libera a vaga ---
    pthread_mutex_lock(&lock_clientes);
    clientes_ativos--;
    printf("[Servidor] Cliente saiu. Vagas ocupadas: %d/%d\n", clientes_ativos, max_clientes);
    pthread_mutex_unlock(&lock_clientes);

    free(c);
    return NULL;
}

// =============================================================================
// MAIN: socket, bind, listen e loop de accept
// =============================================================================
int main(int argc, char *argv[]) {
    if (argc != 2 || (max_clientes = atoi(argv[1])) < 1) {
        fprintf(stderr, "Uso: %s <max_clientes>\n", argv[0]);
        return 1;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("Erro ao criar socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Erro no bind");
        close(server_fd);
        return 1;
    }
    if (listen(server_fd, 5) < 0) {
        perror("Erro no listen");
        close(server_fd);
        return 1;
    }
    printf("[Servidor] Porta %d, limite de %d cliente(s). Aguardando conexoes...\n", PORT, max_clientes);

    while (1) {
        int client_socket = accept(server_fd, NULL, NULL);
        if (client_socket < 0) {
            perror("Erro no accept");
            continue;
        }

        ClienteCtx *c = calloc(1, sizeof(ClienteCtx));
        if (!c) { close(client_socket); continue; }

        c->socket = client_socket;
        c->cfg_inicio = NUM_MIN_PADRAO;
        c->cfg_fim = NUM_MAX_PADRAO;
        c->cfg_qtd = QTD_NUMEROS_PADRAO;
        c->seed = (unsigned int)time(NULL) ^ (unsigned int)client_socket;
        pthread_mutex_init(&c->lock, NULL);
        pthread_cond_init(&c->cond, NULL);
        pthread_mutex_init(&c->lock_envio, NULL);

        // Reserva a vaga de forma atomica (verifica + incrementa sob o mesmo lock)
        pthread_mutex_lock(&lock_clientes);
        if (clientes_ativos < max_clientes) {
            clientes_ativos++;
            c->aceito = 1;
        }
        printf("[Servidor] Nova conexao (%s). Vagas ocupadas: %d/%d\n",
               c->aceito ? "aceita" : "recusada", clientes_ativos, max_clientes);
        pthread_mutex_unlock(&lock_clientes);

        // Thread de trabalho: recebe o contexto (que contem a conexao) e volta direto ao accept()
        pthread_t t;
        if (pthread_create(&t, NULL, thread_cliente, c) != 0) {
            perror("Erro ao criar thread de cliente");
            if (c->aceito) {
                pthread_mutex_lock(&lock_clientes);
                clientes_ativos--;
                pthread_mutex_unlock(&lock_clientes);
            }
            close(client_socket);
            free(c);
            continue;
        }
        pthread_detach(t);   // nao precisa de join: a thread se limpa sozinha
    }

    close(server_fd);
    return 0;
}