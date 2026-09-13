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

#define MAX_JOBS 64
#define MAX_COMANDO 256

typedef struct {
    int   numero;
    pid_t pid;
    char  comando[MAX_COMANDO];
    int   activo;
    int   terminado;
} Job;

static Job jobsList[MAX_JOBS];
static int siguienteNumeroJob = 1;
static volatile sig_atomic_t hayJobsTerminados = 0;


/*
Imprime el directorio actual de la shell
*/
static void mostrarPrompt(void) {
    char directorioActual[PATH_MAX];

    if (getcwd(directorioActual, sizeof(directorioActual)) != NULL){
        printf("miShell: %s$ ", directorioActual);
    }
    else{
        /* si getcwd falla */
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

/* Guarda el proceso en el arreglo de jobs */
int agregarJob(pid_t pid, char *argumentos[], int cantidadArgumentos) {
    for (int i = 0; i < MAX_JOBS; i++) {
        /* Buscamos un espacio libre en el arreglo */
        if (jobsList[i].activo == 0) {
            jobsList[i].numero = siguienteNumeroJob;
            siguienteNumeroJob++; /* aumento para el proximo */
            jobsList[i].pid = pid;
            jobsList[i].activo = 1;
            jobsList[i].terminado = 0;
            
            /* Armamos el comando como un solo string usando strcat */
            strcpy(jobsList[i].comando, ""); 
            for (int j = 0; j < cantidadArgumentos; j++) {
                strcat(jobsList[i].comando, argumentos[j]);
                strcat(jobsList[i].comando, " "); /* espacio entre argumentos */
            }
            
            return jobsList[i].numero;
        }
    }
    return -1; /* Retorna -1 si la lista esta llena */
}

/* Imprime los jobs que se estan ejecutando */
void listarJobs() {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobsList[i].activo == 1) {
            printf("[%d] %d Ejecutando %s\n", jobsList[i].numero, jobsList[i].pid, jobsList[i].comando);
        }
    }
}

/* Revisa la lista y avisa si alguno ya termino */
void notificarJobsTerminados() {
    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobsList[i].activo == 1 && jobsList[i].terminado == 1) {
            printf("[%d]+ Done %s\n", jobsList[i].numero, jobsList[i].comando);
            jobsList[i].activo = 0; /* Lo liberamos para que se pueda sobreescribir */
        }
    }
    hayJobsTerminados = 0; /* reseteamos la flag */
}

/* Esta funcion se ejecuta automaticamente cuando muere un proceso hijo */
void manejadorSigchld(int señal) {
    (void)señal;
    int estadoSalida;
    pid_t pid;

    /* Usamos WNOHANG para no bloquear la shell si no hay hijos muertos */
    while ((pid = waitpid(-1, &estadoSalida, WNOHANG)) > 0) {
        
        /* Buscamos cual de nuestros jobs fue el que murio */
        for (int i = 0; i < MAX_JOBS; i++) {
            if (jobsList[i].pid == pid) {
                jobsList[i].terminado = 1; /* lo marcamos para imprimirlo despues */
            }
        }
        hayJobsTerminados = 1; /* Le avisamos al main que hay algo para imprimir */
    }
}

/* Configura la captura de la señal */
void instalarManejadorSigchld() {
    /* Cuando un hijo termine (SIGCHLD), llama a manejadorSigchld. */
    signal(SIGCHLD, manejadorSigchld);
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

    if (strcmp(argumentos[0], "jobs") == 0) {
        listarJobs();
        return 1;
    }


    return 0;
}

/*
Ejecuta y crea un proceso con fork, y utilizando execvp para hacer otro proceso
y espera con waitpid
*/
static void ejecutarComandoExterno(char *argumentos[], int cantidadArgumentos, int esBackground) {
    pid_t pidHijo = fork();

    if (pidHijo < 0){
        perror("fork");
        exit(1);
    }

    if (pidHijo == 0){

        /* aplicamos las redirecciones si hay */
        int cantidadLimpia = aplicarRedirecciones(argumentos, cantidadArgumentos);
        if (cantidadLimpia < 0) {
          _exit(1);
        }

        /* proceso hijo: reemplazamos su imagen por el comando pedido */
        execvp(argumentos[0], argumentos);

        /* si execvp vuelve es porque fallo */
        fprintf(stderr, "miShell: %s: %s\n", argumentos[0], strerror(errno));
        exit(1);
    }

    if (esBackground) {
        int numero = agregarJob(pidHijo, argumentos, cantidadArgumentos);
        if (numero > 0) {
            printf("[%d] %d\n", numero, (int)pidHijo);
        }
    } else {
        int estadoSalida;
        waitpid(pidHijo, &estadoSalida, 0);
    }
}


int main(void) {
char lineaLeida[MAX_LINE];
char *argumentos[MAX_ARGS];

instalarManejadorSigchld();

while (1) {
    mostrarPrompt();

    if (hayJobsTerminados) {
        notificarJobsTerminados();
    }

    if (fgets(lineaLeida, sizeof(lineaLeida), stdin) == NULL) {
        printf("\n");
        break;
    }

    int cantidadArgumentos = separarEnTokens(lineaLeida, argumentos, MAX_ARGS);
    if (cantidadArgumentos == 0) {
        continue;
    }

    int esBackground = 0;
        if (strcmp(argumentos[cantidadArgumentos - 1], "&") == 0) {
        esBackground = 1;
        cantidadArgumentos--;
        argumentos[cantidadArgumentos] = NULL;
    }


    if (ejecutarComandoInterno(argumentos, cantidadArgumentos)) {
       continue;
    }

    ejecutarComandoExterno(argumentos, cantidadArgumentos, esBackground);
}

return 0;
}