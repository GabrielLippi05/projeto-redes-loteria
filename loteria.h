// Evitar duplicata de Arquivos
#ifndef LOTERIA_H
#define LOTERIA_H

#define QTD_NUMEROS 6
#define NUM_MIN 1
#define NUM_MAX 60

//Funções do Jogo
void inicializador_gerador();
void sortear_numeros(int *sorteados);