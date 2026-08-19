#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>
#include "loteria.h"


void inicializador_gerador()
{
    srand(time(NULL)); /*gera uma seed aleatoria, cada seed possui uma sequencia 
                       diferente de numeros aleatorios, ou seja, a seed 123, tera
                       uma sequencia 51 - 42 - 12 - 02..., uma seed 321 tera outra
                       sequencia 23 - 12 - 32 - 01... e assim por diante*/          
}

// Sorteia os números sem repetição e salva no vetor passado por referência
void sortear_numeros(int *sorteados)
{
    int i = 0;
    while(i < QTD_NUMEROS) // enquanto i < 5 faz:
    {
        int num = ((rand() % NUM_MAX) + NUM_MIN); 
        int repetido = 0;

    // Verifica se o numero salvo em "num" ja esta presente no vetor "sorteados"
        for(int j = 0; j < i; j++)
        {
            if(sorteados[j] == num)
            {
                repetido = 1;
                break;
            }
        }

        // Caso repetido = 0, significa que o numero nao foi sorteado ainda, logo:
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
