#ifndef LOTERIA_H
#define LOTERIA_H

// Valores padrão da especificação
#define NUM_MIN_PADRAO 0
#define NUM_MAX_PADRAO 100
#define QTD_NUMEROS_PADRAO 5

typedef struct {
    int client_socket;
    int qtd_numeros;
    int *numeros_aposta;
} Aposta;

void inicializador_gerador(void);
Aposta* criar_aposta(int client_socket, int qtd_numeros);
void liberar_aposta(Aposta *aposta);

#endif