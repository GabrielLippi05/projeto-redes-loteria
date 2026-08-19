#include <stdio.h>
#include <stdlib.h>
#include <locale.h>

#include "client.h"
#include "loteria.h"


// Função que lê os números do teclado garantindo que são válidos e sem repetição
void ler_aposta_usuario(int *aposta) 
{
    printf("!!MINI-SENA!!\n");
    printf("Digite %d numeros entre %d e %d:\n\n", QTD_NUMEROS, NUM_MIN, NUM_MAX);

    for (int i = 0; i < QTD_NUMEROS; i++) 
    {
        int numero;
        int valido = 0;

        // Repete até o usuário digitar um número correto para a posição 'i'
        while (!valido) 
        {
            printf("Digite o %dº numero: ", i + 1);
            if (scanf("%d", &numero) != 1) 
            {
                // Limpa o buffer caso o usuário digite letras ou caracteres inválidos
                while (getchar() != '\n');
                printf("Entrada invalida! Digite apenas numeros inteiros.\n");
                continue;
            }

            // 1. Checa se o número está dentro do intervalo permitido
            if (numero < NUM_MIN || numero > NUM_MAX) 
            {
                printf("Erro: O numero deve estar entre %d e %d!\n", NUM_MIN, NUM_MAX);
                continue;
            }

            // 2. Checa se o número já foi digitado anteriormente no mesmo bilhete
            int repetido = 0;
            for (int j = 0; j < i; j++) 
            {
                if (aposta[j] == numero) 
                {
                    repetido = 1;
                    break;
                }
            }

            if (repetido) 
            {
                printf("Erro: Voce ja digitou o numero %d! Escolha outro.\n", numero);
                continue;
            }

            // Se passou por todas as checagens, o número é válido
            aposta[i] = numero;
            valido = 1;
        }
    }

    printf("\n Aposta preenchida com sucesso!\n");
}

int main() 
{
    setlocale(LC_ALL, "Portuguese");

    int minha_aposta[QTD_NUMEROS];
    int sorteados[QTD_NUMEROS];

    //Testando a leitura da aposta
    ler_aposta_usuario(minha_aposta);

    //Exibe os números escolhidos pelo usuario
    printf("\nSua aposta foi: ");
    for (int i = 0; i < QTD_NUMEROS; i++) 
    {
        printf("[%d] ", minha_aposta[i]);
    }
    printf("\n");

    //Sorteia os números da loteria para teste
    inicializador_gerador();
    sortear_numeros(sorteados);

    printf("Numeros sorteados: ");
    for (int i = 0; i < QTD_NUMEROS; i++) 
    {
        printf("[%d] ", sorteados[i]);
    }
    printf("\n\n");

    return 0;
}