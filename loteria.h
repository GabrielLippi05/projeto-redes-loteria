// Evitar duplicata de Arquivos
#ifndef LOTERIA_H
#define LOTERIA_H

#define QTD_NUMEROS 5
#define NUM_MIN 1
#define NUM_MAX 100

//Structs para usuário
typedef struct{
    int client_socket;
    int *numeros_aposta;
    int qtd_numeros;
}Aposta;

//Funções do Jogo
void inicializador_gerador();
void sortear_numeros(int *sorteados);
Aposta* criar_aposta(int client_socket, int qtd_numeros);
void liberar_aposta(Aposta *aposta);

#endif