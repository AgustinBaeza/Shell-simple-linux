#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
 
#define MAX_LINE 1024
#define MAX_ARGS 64
 
/*
Imprime el directorio actual de la shell
*/
static void mostrarPrompt(void) {
    char directorioActual[PATH_MAX];
 
    if (getcwd(directorioActual, sizeof(directorioActual)) != NULL){
        printf("miShell: %s$ ", directorioActual);
    }
    else{
        // si getcwd falla
        printf("miShell:?$ ");
    }
    fflush(stdout);
}

/*
Separa las lineas en tokens
*/
static int separarEnTokens(char *linea, char *argumentos[], int maxArgumentos){

    int cantidadArgumentos = 0;
    char *token = strtok(linea, " \t\n");
 
    while (token != NULL && cantidadArgumentos < maxArgumentos - 1){
        argumentos[cantidadArgumentos++] = token;
        token = strtok(NULL, " \t\n");
    }

    argumentos[cantidadArgumentos] = NULL;
 
    return cantidadArgumentos;
}
 

/*
Intenta ejecutar argumentos[0] como comando interno
*/
static int ejecutarComandoInterno(char *argumentos[], int cantidadArgumentos) {
    if (cantidadArgumentos == 0) {
        return 1; /* línea vacía: no hay nada que ejecutar */
    }
 
    if (strcmp(argumentos[0], "cd") == 0) {
        const char *destino = (cantidadArgumentos > 1) ? argumentos[1] : getenv("HOME");
        if (destino == NULL) {
            destino = "/";
        }
        if (chdir(destino) != 0) {
            fprintf(stderr, "cd: %s: %s\n", destino, strerror(errno));
        }
        return 1;
    }
 
    if (strcmp(argumentos[0], "exit") == 0) {
        int codigoSalida = (cantidadArgumentos > 1) ? atoi(argumentos[1]) : 0;
        exit(codigoSalida);
    }


    return 0;
}

/*
Ejecuta y crea un proceso con fork, y utilizando execvp para hacer otro proceso
y espera con waitpid
*/
static void ejecutarComandoExterno(char *argumentos[]) {
    pid_t pidHijo = fork();
 
    if (pidHijo < 0){
        perror("fork");
        exit(1);
    }
 
    if (pidHijo == 0){

        // proceso hijo: reemplazamos su imagen por el comando pedido
        execvp(argumentos[0], argumentos);
 
        // si execvp vuelve es porque fallo
        fprintf(stderr, "miShell: %s: %s\n", argumentos[0], strerror(errno));
        _exit(127);
    }
 
    // sheel espera que el proceso hijo termine
    int estado_salida;
    waitpid(pidHijo, &estado_salida, 0);
}


int main(void) {
    char lineaLeida[MAX_LINE];
    char *argumentos[MAX_ARGS];

    while (1) {
        mostrarPrompt();

        if (fgets(lineaLeida, sizeof(lineaLeida), stdin) == NULL) {
            printf("\n");
            break;
        }

        int cantidadArgumentos = separarEnTokens(lineaLeida, argumentos, MAX_ARGS);
        if (cantidadArgumentos == 0) {
            continue;
        }

        if (ejecutarComandoInterno(argumentos, cantidadArgumentos)) {
            continue;
        }

        ejecutarComandoExterno(argumentos);
    }

    return 0;
}