#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

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
Ignora la señal recibida en el argumento
Utilizado para que la shell no se cierre con la señal IGINT

*/

static void ignorarSignal(int sig) {
    /*
    Definimos la estructra de sigaction
    */
    struct sigaction act;
    memset(&act, 0, sizeof(act)); // deja inicialmente todos en 0
    act.sa_handler = SIG_IGN; //SIG_IGN es utilizado para ignorar la señal
    sigemptyset(&act.sa_mask); //+mientras se ejecuta el manejador, no bloqueamos ninguna señal
    /*
   Ya que hemos de ignorar la señal, agregamos la flag SA_RESTART
   Ella reinicia las llamadas al sistema o syscalls que han sido interrumpidas
    */
    act.sa_flags = SA_RESTART;

    //En caso de algun error
    if (sigaction(sig, &act, NULL) < 0) {
        perror("sigaction");
        exit(1);
    }

}

/*
Restaura una señal a su valor por defecto
Es utilizado en procesos hijos para que, mientras señales como SIGNIN no terminan a miShell, SÍ termnen a procesos iiciados dentro de miShell

*/

static void restaurarSignalPorDefecto(int sig) {
    /*
    Definimos la estructra de sigaction
    */
    struct sigaction act;
    memset(&act, 0, sizeof(act)); // deja inicialmente todos en 0
    act.sa_handler = SIG_DFL; //SIG_DFL restaura la accion por defecto de la señal
    sigemptyset(&act.sa_mask); //+mientras se ejecuta el manejador, no bloqueamos ninguna señal
    /*
   D
   Dejamos sin flags
    */
    act.sa_flags = 0;

    //En caso de algun error
    if (sigaction(sig, &act, NULL) < 0) {
        perror("sigaction");
        exit(1);
    }

}

/*
Recorre argumentos buscando '<', '>' y '>>'.
Por cada operador encontrado, abre el archivo correspondiente y usa dup2
para redirigir stdin o stdout, y saca el operador y el archivo de argumentos
para que execvp no los reciba como parte del comando.
Devuelve la nueva cantidad de argumentos, o -1 si hubo un error.
*/
static int aplicarRedirecciones(char *argumentos[], int cantidadArgumentos) {
    char *argumentosLimpios[MAX_ARGS];
    int nuevaCantidad = 0;

    for (int i = 0; i < cantidadArgumentos; i++) {

        if (strcmp(argumentos[i], "<") == 0) {
            if (argumentos[i + 1] == NULL) {
                fprintf(stderr, "miShell: se esperaba un archivo despues de '<'\n");
                return -1;
        }

        int fdEntrada = open(argumentos[i + 1], O_RDONLY);
        if (fdEntrada < 0) {
            fprintf(stderr, "miShell: %s: %s\n", argumentos[i + 1], strerror(errno));
            return -1;
        }

        if (dup2(fdEntrada, STDIN_FILENO) < 0) {
            perror("dup2");
            close(fdEntrada);
            return -1;
        }

        close(fdEntrada);
        i++;
        }
        else if (strcmp(argumentos[i], ">") == 0 || strcmp(argumentos[i], ">>") == 0) {
            if (argumentos[i + 1] == NULL) {
                fprintf(stderr, "miShell: se esperaba un archivo despues de '%s'\n", argumentos[i]);
                return -1;
            }

            int flags = O_WRONLY | O_CREAT;
            flags |= (strcmp(argumentos[i], ">>") == 0) ? O_APPEND : O_TRUNC;

            int fdSalida = open(argumentos[i + 1], flags, 0644);
            if (fdSalida < 0) {
                fprintf(stderr, "miShell: %s: %s\n", argumentos[i + 1], strerror(errno));
                return -1;
            }

            if (dup2(fdSalida, STDOUT_FILENO) < 0) {
                perror("dup2");
                close(fdSalida);
                return -1;
            }

            close(fdSalida);
            i++;
        }
        else {
            argumentosLimpios[nuevaCantidad++] = argumentos[i];
        }
    }

    for (int i = 0; i < nuevaCantidad; i++) {
        argumentos[i] = argumentosLimpios[i];
    }
    argumentos[nuevaCantidad] = NULL;

    return nuevaCantidad;
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
static void ejecutarComandoExterno(char *argumentos[], int cantidadArgumentos) {
    pid_t pidHijo = fork();

    if (pidHijo < 0){
        perror("fork");
        exit(1);
    }

    if (pidHijo == 0){

        /*
         Agregamos comportamiento respecto a las señales de SIGINT y SIGQUIT
         Estas señales han de terminar los rocesos dentro de miShell
         */
        restaurarSignalPorDefecto(SIGINT);
        restaurarSignalPorDefecto(SIGQUIT);

        // aplicamos las redirecciones si hay
        int cantidadLimpia = aplicarRedirecciones(argumentos, cantidadArgumentos);
        if (cantidadLimpia < 0) {
          _exit(1);
        }

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

    /*
     Ignoramos las señales SIGINT y SIGQUIT para que ellas no terminen miShell
     */

    ignorarSignal(SIGINT);
    ignorarSignal(SIGQUIT);

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

    ejecutarComandoExterno(argumentos, cantidadArgumentos);
}

return 0;
}