#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "loteria.h"

// Define a semente aleatória com base no relógio do sistema
void inicializador_gerador()
{
    srand(time(NULL));
}

// Sorteia os números sem repetição e salva no vetor passado por referência
void sortear_numeros(int *sorteados)
{
    int i = 0;
    while(i < QTD_NUMEROS)
    {
        int num = ((rand() % NUM_MAX) + NUM_MIN);
        int repetido = 0;

        // Verifica se o número já foi sorteado nesta rodada
        for(int j = 0; j < i; j++)
        {
            if(sorteados[j] == num)
            {
                repetido = 1;
                break;
            }
        }

        // Se for inédito, salva no vetor e avança o índice
        if(!repetido)
        {
            sorteados[i] = num;
            i++;
        }
    }
}

// Aloca dinamicamente a struct Aposta e o vetor interno de números
Aposta* criar_aposta(int client_socket, int qtd_numeros)
{
    // Aloca a struct principal
    Aposta *aposta = (Aposta*)malloc(sizeof(Aposta));
    if (aposta == NULL) {
        perror("Erro ao alocar struct Aposta");
        return NULL;
    }

    aposta->client_socket = client_socket;
    aposta->qtd_numeros = qtd_numeros;
    
    // Aloca o array de números com o tamanho exato da aposta
    aposta->numeros_aposta = (int*)malloc(qtd_numeros * sizeof(int));
    if (aposta->numeros_aposta == NULL) {
        perror("Erro ao alocar array de numeros");
        free(aposta); // Libera a struct se o array falhar
        return NULL;
    }

    return aposta;
}

// Libera a memória alocada dinamicamente para evitar vazamentos (memory leak)
void liberar_aposta(Aposta *aposta)
{
    if (aposta != NULL) {
        // Libera primeiro o array interno, depois a struct
        if (aposta->numeros_aposta != NULL) {
            free(aposta->numeros_aposta);
            aposta->numeros_aposta = NULL;
        }
        free(aposta);
    }
}