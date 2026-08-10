#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "loteria.h"

void inicializador_gerador()
{
    srand(time(NULL)); //define a seed inicial, ex: 123 -> a sequencia vai ser X, 1234 a sequencia dos numeros sera Y(mesma logica do minecraft)
}

void sortear_numeros(int *sorteados)
{
    int i = 0;
    while(i < QTD_NUMEROS) //Como estamos usando a logica da Mega-Sena enquanto i < 6:
    {
        int num = ((rand() % NUM_MAX) + NUM_MIN; ); //Qualquer numero inteiro dividido pelo NUM_MAX (60) o resto resultara num valor entre 0 e 59, logo somamos o resto com o NUM_MIN (1) para o resultado ficar entre 1 e
        int repetido = 0;

        for(int j = 0; j < i; j++)
        {
            if(sorteados[j] == num)
            {
                repetido = 1;
                break; //Sai do loop caso o numero sorteado seja repetido
            }
        }

        if(!repetido) //Caso nao seja repetido
        {
            sorteados[i] = num; 
            i++;
        }
        //Finaliza o While so quando os 6 numeros forem sorteados
    }
}




