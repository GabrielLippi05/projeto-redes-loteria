#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "loteria.h"

// Inicializa a semente pseudoaleatória
void inicializador_gerador(void) {
    srand(time(NULL));
}

// Aloca dinamicamente a struct e o vetor de números da aposta
Aposta* criar_aposta(int client_socket, int qtd_numeros) {
    // Sem numeros nao ha aposta; malloc(0) poderia devolver ponteiro ambiguo
    if (qtd_numeros < 1) return NULL;

    Aposta *nova_aposta = (Aposta*)malloc(sizeof(Aposta));
    if (nova_aposta == NULL) {
        perror("Erro ao alocar Aposta");
        return NULL;
    }

    nova_aposta->client_socket = client_socket;
    nova_aposta->qtd_numeros = qtd_numeros;
    nova_aposta->numeros_aposta = (int*)malloc(qtd_numeros * sizeof(int));

    if (nova_aposta->numeros_aposta == NULL) {
        perror("Erro ao alocar numeros da aposta");
        free(nova_aposta);
        return NULL;
    }

    return nova_aposta;
}

// Libera com segurança a memória alocada
void liberar_aposta(Aposta *aposta) {
    if (aposta != NULL) {
        if (aposta->numeros_aposta != NULL) {
            free(aposta->numeros_aposta);
        }
        free(aposta);
    }
}