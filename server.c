#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "loteria.h"     // struct Aposta, criar_aposta, liberar_aposta, constantes padrao

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_NUMEROS 100          // maximo de numeros por aposta
#define MAX_APOSTAS_RODADA 1000  // maximo de apostas por cliente em uma rodada (protege a memoria)
#define LIMITE_VALOR 1000000     // |valor| maximo aceito em :inicio, :fim e nas apostas
#define TIMEOUT_ENVIO_SEG 5      // prazo total para enviar uma mensagem; estourou, o cliente e derrubado
#define POLL_ACCEPT_MS 500       // intervalo para o accept checar o pedido de encerramento
#define PAUSA_LINHA_MS 50        // espera por mais dados quando a linha chegou sem '\n'

#ifndef INTERVALO_SORTEIO
#define INTERVALO_SORTEIO 60     // segundos entre sorteios (pode ser trocado com -DINTERVALO_SORTEIO=N)
#endif

// =============================================================================
// ESTRUTURAS
// =============================================================================

typedef struct ApostaNode {
    Aposta *aposta;
    struct ApostaNode *proximo;
} ApostaNode;

// Contexto de um cliente: tudo que e dele vive aqui (isolamento total).
typedef struct ClienteCtx {
    int socket;
    int aceito;                  // 1 = dentro do limite, 0 = limite excedido
    char ip[INET_ADDRSTRLEN];    // so para logs
    int porta;

    int cfg_inicio;
    int cfg_fim;
    int cfg_qtd;

    ApostaNode *lista_apostas;   // apostas da rodada atual
    ApostaNode *fim_lista;
    int qtd_apostas;

    pthread_mutex_t lock;        // protege cfg_*, lista_apostas, qtd_apostas e encerrar
    pthread_cond_t  cond;        // acorda o temporizador quando o cliente sai
    int encerrar;
    int morto;                   // 1 = conexao condenada (falha de envio); acessado com __atomic

    pthread_mutex_t lock_envio;  // serializa send() no socket (2 threads enviam)
    unsigned int seed;           // semente propria para rand_r

    struct ClienteCtx *proximo_ctx;  // lista global de clientes ativos
} ClienteCtx;

// =============================================================================
// ESTADO GLOBAL DO SERVIDOR (tudo protegido por lock_clientes)
// =============================================================================
static pthread_mutex_t lock_clientes = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cond_threads  = PTHREAD_COND_INITIALIZER;
static ClienteCtx *lista_clientes = NULL;  // clientes aceitos e ainda ativos
static int clientes_ativos = 0;            // vagas ocupadas
static int threads_vivas = 0;              // threads de trabalho em execucao (aceitas + recusadas)
static int max_clientes = 0;

static int encerrando = 0;   // acessado so com __atomic (seguro dentro de handler de sinal)

static void tratar_sinal(int sig) {
    (void)sig;
    int e = errno;                                   // handler preserva errno
    __atomic_store_n(&encerrando, 1, __ATOMIC_RELAXED);
    errno = e;
}

static int pediu_encerrar(void) {
    return __atomic_load_n(&encerrando, __ATOMIC_RELAXED);
}

// =============================================================================
// FUNCOES AUXILIARES
// =============================================================================

// Milissegundos que faltam ate o prazo (inicio + TIMEOUT_ENVIO_SEG)
static long restante_ms(const struct timespec *ini) {
    struct timespec agora;
    clock_gettime(CLOCK_MONOTONIC, &agora);
    long gasto = (agora.tv_sec - ini->tv_sec) * 1000L + (agora.tv_nsec - ini->tv_nsec) / 1000000L;
    return TIMEOUT_ENVIO_SEG * 1000L - gasto;
}

// Conexao ja condenada (envio falhou antes) ou servidor encerrando?
static int conexao_morta(ClienteCtx *c) {
    return pediu_encerrar() || __atomic_load_n(&c->morto, __ATOMIC_RELAXED);
}

// Envia a mensagem inteira (trata envio parcial e EINTR) com prazo TOTAL de
// TIMEOUT_ENVIO_SEG: um cliente que nao le (ou le a conta-gotas) e derrubado.
// Se falhar, a conexao e derrubada: o recv da outra thread retorna e o cliente
// e encerrado normalmente. Retorna 1 em sucesso, 0 em falha.
static int enviar(ClienteCtx *c, const char *msg) {
    if (conexao_morta(c)) return 0;

    size_t total = 0, n = strlen(msg);
    int erro = 0;

    pthread_mutex_lock(&c->lock_envio);
    struct timespec ini;
    clock_gettime(CLOCK_MONOTONIC, &ini);
    while (total < n) {
        ssize_t r = send(c->socket, msg + total, n - total, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (r >= 0) { total += (size_t)r; continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            long resta = restante_ms(&ini);
            if (resta <= 0 || pediu_encerrar()) { erro = ETIMEDOUT; break; }
            struct pollfd p = { c->socket, POLLOUT, 0 };
            int pr = poll(&p, 1, resta < 200 ? (int)resta : 200);  // fatias curtas: percebe o encerramento
            if (pr < 0 && errno != EINTR) { erro = errno; break; }
            continue;
        }
        erro = errno;
        break;
    }
    pthread_mutex_unlock(&c->lock_envio);

    if (erro) {
        if (!__atomic_exchange_n(&c->morto, 1, __ATOMIC_RELAXED))   // loga so na primeira falha
            printf("[Socket %d] Falha ao enviar (%s). Derrubando conexao.\n", c->socket, strerror(erro));
        shutdown(c->socket, SHUT_RDWR);
        return 0;
    }
    return 1;
}

// Anexa texto formatado a um buffer dinamico. Se faltar memoria, o texto e descartado.
static void anexar(char **buf, size_t *len, size_t *cap, const char *fmt, ...) {
    va_list ap;
    while (1) {
        va_start(ap, fmt);
        int n = vsnprintf(*buf + *len, *cap - *len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n < *cap - *len) { *len += (size_t)n; return; }

        size_t novo_cap = (*cap + (size_t)n + 1) * 2;
        char *novo = realloc(*buf, novo_cap);
        if (!novo) return;          // *buf e *cap continuam consistentes
        *buf = novo;
        *cap = novo_cap;
    }
}

static void liberar_lista(ApostaNode *no) {
    while (no) {
        ApostaNode *prox = no->proximo;
        liberar_aposta(no->aposta);
        free(no);
        no = prox;
    }
}

// Converte string em int, rejeitando lixo ("12abc"), vazio e valores absurdos.
static int parse_int(const char *s, int *out) {
    if (!s || !*s) return 0;
    char *fim;
    errno = 0;
    long v = strtol(s, &fim, 10);
    if (errno != 0 || *fim != '\0' || v < -LIMITE_VALOR || v > LIMITE_VALOR) return 0;
    *out = (int)v;
    return 1;
}

// Desfaz o cadastro do cliente na lista global e libera a vaga. Chamar com lock_clientes.
static void remover_cliente_locked(ClienteCtx *c) {
    ClienteCtx **pp = &lista_clientes;
    while (*pp && *pp != c) pp = &(*pp)->proximo_ctx;
    if (*pp) *pp = c->proximo_ctx;
    clientes_ativos--;
}

// Libera tudo que pertence ao cliente (socket, apostas, mutexes, memoria).
static void destruir_ctx(ClienteCtx *c) {
    close(c->socket);
    liberar_lista(c->lista_apostas);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->lock_envio);
    free(c);
}

// Ultima acao de toda thread de trabalho: avisa a main que ela terminou.
static void fim_da_thread(void) {
    pthread_mutex_lock(&lock_clientes);
    threads_vivas--;
    pthread_cond_broadcast(&cond_threads);
    pthread_mutex_unlock(&lock_clientes);
}

// =============================================================================
// THREAD 2 (por cliente): TEMPORIZADOR E SORTEIO
// =============================================================================
static void* thread_temporizador(void *arg) {
    ClienteCtx *c = arg;

    while (1) {
        struct timespec limite;
        clock_gettime(CLOCK_MONOTONIC, &limite);
        limite.tv_sec += INTERVALO_SORTEIO;

        pthread_mutex_lock(&c->lock);
        int rc = 0;
        while (!c->encerrar && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&c->cond, &c->lock, &limite);

        if (c->encerrar) {
            pthread_mutex_unlock(&c->lock);
            break;
        }

        int inicio = c->cfg_inicio, fim = c->cfg_fim, qtd = c->cfg_qtd;
        ApostaNode *lista = c->lista_apostas;
        c->lista_apostas = NULL;
        c->fim_lista = NULL;
        c->qtd_apostas = 0;
        pthread_mutex_unlock(&c->lock);

        // Defesa: config incoerente nunca deve gerar loop infinito
        if (qtd < 1 || qtd > MAX_NUMEROS || fim < inicio || fim - inicio + 1 < qtd) {
            liberar_lista(lista);
            continue;
        }

        int *sorteados = malloc((size_t)qtd * sizeof(int));
        if (!sorteados) { liberar_lista(lista); continue; }

        int total = 0;
        while (total < qtd) {
            int n = (int)(rand_r(&c->seed) % (unsigned)(fim - inicio + 1)) + inicio;
            int repetido = 0;
            for (int j = 0; j < total; j++)
                if (sorteados[j] == n) { repetido = 1; break; }
            if (!repetido) sorteados[total++] = n;
        }

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
        enviar(c, rel);   // se falhar, a conexao e derrubada e o cliente sai

        free(rel);
        free(sorteados);
        liberar_lista(lista);
    }
    return NULL;
}

// =============================================================================
// PROCESSAMENTO DE UMA LINHA (comando ou aposta)
// Retorna 1 se o cliente pediu :quit, 0 caso contrario.
// =============================================================================
static int processar_linha(ClienteCtx *c, const char *linha_original) {
    char linha[BUFFER_SIZE];
    snprintf(linha, sizeof(linha), "%s", linha_original);
    char *save = NULL;

    // ------------------------- COMANDOS (':') -------------------------
    if (linha[0] == ':') {
        char resp[BUFFER_SIZE];
        char *cmd   = strtok_r(linha, " \t", &save);
        char *arg   = strtok_r(NULL, " \t", &save);
        char *extra = strtok_r(NULL, " \t", &save);

        if (strcmp(cmd, ":quit") == 0) {
            enviar(c, "[Banca] Desconectando. Ate logo!\n");
            return 1;
        }

        int valor = 0;
        int numero_ok = (arg != NULL && extra == NULL && parse_int(arg, &valor));

        pthread_mutex_lock(&c->lock);
        if (strcmp(cmd, ":inicio") == 0) {
            if (!numero_ok)
                snprintf(resp, sizeof(resp), "[Erro] Uso: :inicio <NUMERO> (inteiro ate %d)\n", LIMITE_VALOR);
            else if (valor >= c->cfg_fim)
                snprintf(resp, sizeof(resp), "[Erro] Inicio deve ser menor que o fim (%d)\n", c->cfg_fim);
            else if (c->cfg_fim - valor + 1 < c->cfg_qtd)
                snprintf(resp, sizeof(resp), "[Erro] Intervalo menor que a quantidade de numeros (%d)\n", c->cfg_qtd);
            else {
                c->cfg_inicio = valor;
                snprintf(resp, sizeof(resp), "[Config] Inicio alterado para: %d\n", valor);
            }
        }
        else if (strcmp(cmd, ":fim") == 0) {
            if (!numero_ok)
                snprintf(resp, sizeof(resp), "[Erro] Uso: :fim <NUMERO> (inteiro ate %d)\n", LIMITE_VALOR);
            else if (valor <= c->cfg_inicio)
                snprintf(resp, sizeof(resp), "[Erro] Fim deve ser maior que o inicio (%d)\n", c->cfg_inicio);
            else if (valor - c->cfg_inicio + 1 < c->cfg_qtd)
                snprintf(resp, sizeof(resp), "[Erro] Intervalo menor que a quantidade de numeros (%d)\n", c->cfg_qtd);
            else {
                c->cfg_fim = valor;
                snprintf(resp, sizeof(resp), "[Config] Fim alterado para: %d\n", valor);
            }
        }
        else if (strcmp(cmd, ":qtd") == 0) {
            if (!numero_ok)
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
        return 0;
    }

    // ------------------------- APOSTA -------------------------
    int numeros[MAX_NUMEROS];
    int contador = 0, invalido = 0;

    for (char *tok = strtok_r(linha, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        if (contador >= MAX_NUMEROS) { contador = MAX_NUMEROS + 1; break; }  // excesso: cai no erro de quantidade
        if (!parse_int(tok, &numeros[contador])) { invalido = 1; break; }
        contador++;
    }

    if (invalido) {
        enviar(c, "[Erro] Aposta invalida: use apenas numeros inteiros separados por espaco!\n");
        return 0;
    }

    // Valida e registra na mesma secao critica: a config nao muda no meio da validacao
    char erro[BUFFER_SIZE] = "";
    pthread_mutex_lock(&c->lock);

    if (contador != c->cfg_qtd) {
        snprintf(erro, sizeof(erro), "[Erro] Voce deve apostar exatamente %d numeros!\n", c->cfg_qtd);
    } else if (c->qtd_apostas >= MAX_APOSTAS_RODADA) {
        snprintf(erro, sizeof(erro), "[Erro] Limite de %d apostas por rodada atingido.\n", MAX_APOSTAS_RODADA);
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
            if (c->fim_lista) c->fim_lista->proximo = no;
            else c->lista_apostas = no;
            c->fim_lista = no;
            c->qtd_apostas++;
        }
    }
    pthread_mutex_unlock(&c->lock);

    if (erro[0])
        enviar(c, erro);
    else
        enviar(c, "[Banca] Aposta registrada com sucesso! Aguarde o sorteio da rodada.\n");
    return 0;
}

// =============================================================================
// THREAD 1 (por cliente): RECEPTOR
// TCP e um fluxo de bytes: uma leitura pode trazer meia linha ou varias linhas.
// Por isso as linhas sao montadas num buffer e separadas por '\n'.
// Retorna o motivo do fim: 0 = :quit, 1 = cliente caiu sem :quit, 2 = erro de rede,
// 3 = servidor derrubou a conexao (encerrando ou falha de envio)
// =============================================================================
static int receptor(ClienteCtx *c) {
    char acc[BUFFER_SIZE];
    size_t len = 0;          // bytes pendentes em acc (linha ainda incompleta)
    int descartando = 0;     // 1 = jogando fora o resto de uma linha longa demais

    while (1) {
        if (conexao_morta(c)) return 3;

        ssize_t bytes = recv(c->socket, acc + len, sizeof(acc) - 1 - len, 0);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            printf("[Thread 1 | socket %d] Erro de conexao: %s\n", c->socket, strerror(errno));
            return 2;
        }
        if (bytes == 0) return 1;   // FIM da conexao sem :quit

        for (ssize_t i = 0; i < bytes; i++) {   // NUL e '\r' viram espaco: nao truncam nem sujam a linha
            char *p = acc + len + i;
            if (*p == '\0' || *p == '\r') *p = ' ';
        }
        len += (size_t)bytes;

        if (descartando) {
            char *nl = memchr(acc, '\n', len);
            if (!nl) { len = 0; continue; }
            len -= (size_t)(nl + 1 - acc);
            memmove(acc, nl + 1, len);
            descartando = 0;
        }

        while (len > 0) {
            char *nl = memchr(acc, '\n', len);
            size_t llen, consumir;

            if (nl) {
                llen = (size_t)(nl - acc);
                consumir = llen + 1;
            } else if (len >= sizeof(acc) - 1) {
                enviar(c, "[Erro] Linha muito longa. Ela foi descartada.\n");
                len = 0;
                descartando = 1;
                break;
            } else {
                // Linha sem '\n': espera um instante por mais dados; se nada vier,
                // trata o que chegou como uma linha completa (cliente que nao envia '\n')
                struct pollfd p = { c->socket, POLLIN, 0 };
                int pr = poll(&p, 1, PAUSA_LINHA_MS);
                if (pr > 0) break;                          // veio mais: volta ao recv
                if (pr < 0 && errno != EINTR) return 2;
                llen = len;
                consumir = len;
            }

            acc[llen] = '\0';
            if (llen > 0) {
                if (conexao_morta(c)) return 3;   // nao processa o resto do que ja chegou
                printf("[Socket %d recebeu]: \"%s\"\n", c->socket, acc);
                if (processar_linha(c, acc)) return 0;
            }
            len -= consumir;
            memmove(acc, acc + consumir, len);
        }
    }
}

// =============================================================================
// THREAD DE TRABALHO (uma por cliente conectado)
// =============================================================================
static void* thread_cliente(void *arg) {
    ClienteCtx *c = arg;

    // --- Limite de clientes excedido ---
    if (!c->aceito) {
        enviar(c, "[Servidor] Limite de clientes atingido. Tente novamente mais tarde.\n");
        destruir_ctx(c);
        fim_da_thread();
        return NULL;
    }

    // --- MSG1: "<HH:MM:SS>: CONECTADO!!" ---
    time_t agora = time(NULL);
    struct tm tm_info;
    localtime_r(&agora, &tm_info);
    char msg1[100];
    snprintf(msg1, sizeof(msg1), "<%02d:%02d:%02d>: CONECTADO!!\n",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);

    int motivo = 2;
    pthread_t t_temp;
    if (enviar(c, msg1)) {
        if (pthread_create(&t_temp, NULL, thread_temporizador, c) == 0) {
            motivo = receptor(c);

            pthread_mutex_lock(&c->lock);
            c->encerrar = 1;
            pthread_cond_signal(&c->cond);
            pthread_mutex_unlock(&c->lock);
            pthread_join(t_temp, NULL);
        } else {
            printf("[Socket %d] Erro ao criar thread do temporizador.\n", c->socket);
            enviar(c, "[Servidor] Erro interno. Tente novamente.\n");
        }
    }

    const char *txt = (motivo == 0) ? "saiu com :quit"
                    : (motivo == 1) ? "desconectou sem :quit"
                    : (motivo == 3) ? "foi desconectado pelo servidor"
                                    : "perdeu a conexao (erro)";

    // --- Libera a vaga e retira da lista antes de fechar o socket ---
    pthread_mutex_lock(&lock_clientes);
    remover_cliente_locked(c);
    printf("[Servidor] Cliente %s:%d %s. Vagas ocupadas: %d/%d\n",
           c->ip, c->porta, txt, clientes_ativos, max_clientes);
    pthread_mutex_unlock(&lock_clientes);

    destruir_ctx(c);
    fim_da_thread();
    return NULL;
}

// =============================================================================
// MAIN
// =============================================================================
static void configurar_socket_cliente(int fd) {
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));   // respostas curtas saem na hora
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));   // detecta cliente sumido (cabo, queda de rede)
#ifdef TCP_KEEPIDLE
    int idle = 30, intvl = 10, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    char *fim_arg = NULL;
    long maxc = (argc == 2) ? strtol(argv[1], &fim_arg, 10) : 0;
    if (argc != 2 || *argv[1] == '\0' || *fim_arg != '\0' || maxc < 1 || maxc > 10000) {
        fprintf(stderr, "Uso: %s <max_clientes>   (1 a 10000)\n", argv[0]);
        return 1;
    }
    max_clientes = (int)maxc;

    // SIGPIPE ignorado (send ja usa MSG_NOSIGNAL); SIGINT/SIGTERM pedem encerramento limpo
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sa.sa_handler = tratar_sinal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

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
    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("Erro no listen");
        close(server_fd);
        return 1;
    }
    printf("[Servidor] Porta %d, limite de %d cliente(s). Ctrl+C para encerrar.\n", PORT, max_clientes);

    while (!pediu_encerrar()) {
        // poll com timeout: o servidor percebe o pedido de encerramento mesmo sem conexoes novas
        struct pollfd pfd = { server_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, POLL_ACCEPT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("Erro no poll");
            break;
        }
        if (pr == 0) continue;

        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_socket = accept(server_fd, (struct sockaddr*)&cli_addr, &cli_len);
        if (client_socket < 0) {
            // Erro de uma conexao nao derruba o servidor
            if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("Erro no accept");
                usleep(100000);   // evita laco apertado em EMFILE/ENFILE/ENOMEM
            }
            continue;
        }

        configurar_socket_cliente(client_socket);

        ClienteCtx *c = calloc(1, sizeof(ClienteCtx));
        if (!c) { close(client_socket); continue; }

        c->socket = client_socket;
        c->cfg_inicio = NUM_MIN_PADRAO;
        c->cfg_fim = NUM_MAX_PADRAO;
        c->cfg_qtd = QTD_NUMEROS_PADRAO;
        c->seed = (unsigned int)time(NULL) ^ (unsigned int)client_socket;
        c->porta = ntohs(cli_addr.sin_port);
        if (!inet_ntop(AF_INET, &cli_addr.sin_addr, c->ip, sizeof(c->ip)))
            snprintf(c->ip, sizeof(c->ip), "?");

        pthread_mutex_init(&c->lock, NULL);
        pthread_mutex_init(&c->lock_envio, NULL);
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   // espera imune a mudanca de relogio
        pthread_cond_init(&c->cond, &ca);
        pthread_condattr_destroy(&ca);

        // Reserva a vaga de forma atomica e cadastra o cliente
        pthread_mutex_lock(&lock_clientes);
        if (clientes_ativos < max_clientes) {
            clientes_ativos++;
            c->aceito = 1;
            c->proximo_ctx = lista_clientes;
            lista_clientes = c;
        }
        threads_vivas++;
        printf("[Servidor] Conexao de %s:%d %s. Vagas ocupadas: %d/%d\n",
               c->ip, c->porta, c->aceito ? "aceita" : "recusada (lotado)", clientes_ativos, max_clientes);
        pthread_mutex_unlock(&lock_clientes);

        pthread_t t;
        if (pthread_create(&t, NULL, thread_cliente, c) != 0) {
            fprintf(stderr, "Erro ao criar thread de cliente\n");
            pthread_mutex_lock(&lock_clientes);
            if (c->aceito) remover_cliente_locked(c);
            threads_vivas--;
            pthread_mutex_unlock(&lock_clientes);
            destruir_ctx(c);
            continue;
        }
        pthread_detach(t);
    }

    // --- Encerramento limpo: derruba as conexoes e espera todas as threads sairem ---
    printf("[Servidor] Encerrando...\n");
    close(server_fd);

    pthread_mutex_lock(&lock_clientes);
    for (ClienteCtx *p = lista_clientes; p; p = p->proximo_ctx)
        shutdown(p->socket, SHUT_RDWR);   // o recv de cada cliente retorna e a thread se encerra
    while (threads_vivas > 0)
        pthread_cond_wait(&cond_threads, &lock_clientes);
    pthread_mutex_unlock(&lock_clientes);

    printf("[Servidor] Finalizado.\n");
    return 0;
}
