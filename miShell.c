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
#define MAX_COMANDOS 64

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
 Definimos una estructura en la que se agruparan los comandos a la hora de crear pipes.
 */
typedef struct {
    char *arrayArgumentos[MAX_ARGS];
    int cantidadArgumentos;
} Comando;

/*
 Verifica si el comando entregado es interno o externo.
 Retorna 1 si es interno, 0 si es externo.
 */
static int esComandoInterno(const char *nombreComando) {
    if (strcmp(nombreComando, "cd") == 0 ||
        strcmp(nombreComando, "exit") == 0 ||
        strcmp(nombreComando, "jobs") == 0 ||
        strcmp(nombreComando, "pmon") == 0) {
        return 1;
    }

    return 0;
}
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

    while (token != NULL){
        if (cantidadArgumentos > maxArgumentos - 1) {
            fprintf(stderr, "miShell: Demasiados argumentos. Límite de argumentos: %d", maxArgumentos);
            return -1;
        }
        argumentos[cantidadArgumentos++] = token;
        token = strtok(NULL, " \t\n");
    }

    argumentos[cantidadArgumentos] = NULL;

    return cantidadArgumentos;
}

/*
 Funcion que separa los comandos para poder correr varios comandos al mismo tiempo mediante el uso de tuberías.
 Separa los comandos según la aparición de '|'
 */
static int separarPipelines (char *linea,  Comando comandos[], int maxComandos) {

    int contadorComandos = 0;
    char *segmento = strtok(linea, "|");

    while (segmento != NULL){
        if (contadorComandos > maxComandos - 1) {
            fprintf(stderr, "miShell: Demasiados comandos. Límite de comandos: %d", maxComandos);
            return -1;
        }
        //Separamos en tokens al segmento guardando la cantidad de argumentos y el array de argumentos-
        comandos[contadorComandos].cantidadArgumentos =
            separarEnTokens(segmento,
                            comandos[contadorComandos].arrayArgumentos,
                            MAX_ARGS);
        contadorComandos++;
        segmento = strtok(NULL, "|");
    }

    return contadorComandos;

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
Si no hay argumentos retorna 1
Si se ejecuta exitosamente retorna 1
Si no ejecuta el comando retorna 0
*/
static int ejecutarComandoInterno(char *argumentos[], int cantidadArgumentos) {
    if (cantidadArgumentos == 0) {
      return 1; /* línea vacía: no hay nada que ejecutar */
    }

    else if (strcmp(argumentos[0], "cd") == 0) {
        const char *destino = (cantidadArgumentos > 1) ? argumentos[1] : getenv("HOME");
        if (destino == NULL) {
         destino = "/";
        }
        if (chdir(destino) != 0) {
          fprintf(stderr, "cd: %s: %s\n", destino, strerror(errno));
        }
        return 1;
    }

    else if (strcmp(argumentos[0], "exit") == 0) {
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
/*
    Funcion para ejecutar varios comandos al mismo tiempo.

 */
static void ejecutarPipes(Comando comandos[], int cantidadComandos) {

    /*
     Primero verificamos que existan almenos dos comandos.
     Si solo existe un comando, o no existen comandos, entonces se llamó a la función equivocada.
     */
    if (cantidadComandos <2 || comandos == NULL) {
        return;
    }
    //Verificamos que los comandos NO sean internos.

    for (int i = 0; i < cantidadComandos; i++) {
        if (esComandoInterno(comandos[i].arrayArgumentos[0])) {
            fprintf(stderr, "miShell: %s es un comando interno, y, por lo tanto, no debe ejecutarse mediante fork()+exec()\n", comandos[i].arrayArgumentos[0]);
            return;
        }
    }

    int cantidadPipes = cantidadComandos - 1;
    //Reservamos el espacio para todos los pipes
    int (*pipes)[2] = malloc(sizeof(int[2]) * cantidadPipes);

    if (pipes == NULL) {
        perror("malloc");
        return;
    }
    for (int i = 0; i < cantidadPipes; i++) {

        //Revisamos si hubo algun error al crear las pipes
        if (pipe(pipes[i]) == -1) {
            perror("pipe");
            // limpiamos los pipes si hubo un error
            for (int j = 0; j < i; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }
            free(pipes);
            return;
        }
    }
/*
 Creamos un proceso hijo para cada comando
 */
    for (int i = 0; i < cantidadComandos; i++) {
        pid_t pid = fork();
        //Revisamos que el proceso hijo haya sido creado correctamente
        if (pid < 0) {
            perror("fork");
            // Limpiar recursos en el proceso padre
            for (int j = 0; j < cantidadPipes; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
                }
            free(pipes);
            return;
            }

        else if (pid == 0) {

            // Restaurar señales para que SIGINT y SIGQUIT maten a los procesos hijos
            restaurarSignalPorDefecto(SIGINT);
            restaurarSignalPorDefecto(SIGQUIT);

            /*
             conectamos los extremos de las pipes mediante dup2
             Ello se utiliza para que la pipe 'i' tenga acceso o pueda leer el output de la pipe 'i-1'
             */
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) {
                    perror("dup2 stdin");
                    _exit(1);
                }
            }
            /*
             Ahora conectamos el stdout de 'i-1' con el pipe 'i'
             */
            if (i < cantidadComandos - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) == -1) {
                    perror("dup2 stdout");
                    _exit(1);
                }
            }
            /*
             Cerramos los pipes antes de ejecutar los comandos
             STDOUT_FILENO ahora apunta al pipe
             y al ejecutar los comandos fork crea una copia de los padres
             por lo cual cerramos los pipes antes de ejecutar los comandos
             */
            for (int j = 0; j < cantidadPipes; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            /*
             *EJECUTAMOS LOS COMANDOS
             *Aplicar redirecciones de archivos si es que las hay
             */
            int cantidadLimpia = aplicarRedirecciones(comandos[i].arrayArgumentos, comandos[i].cantidadArgumentos);
            //Si hubo un error terminamos a miShell
            if (cantidadLimpia < 0) {
                _exit(1);
            }

            // Ejecutar el comando
            execvp(comandos[i].arrayArgumentos[0], comandos[i].arrayArgumentos);

            fprintf(stderr, "miShell: %s: %s\n", comandos[i].arrayArgumentos[0], strerror(errno));
            _exit(1);
        }
    }

    /*
     Esta parte del codigo está dentro del proceso padre
     para finalizar cerramos todos los pipes y los liberamos
     */
    for (int j = 0; j < cantidadPipes; j++) {
        close(pipes[j][0]);
        close(pipes[j][1]);
    }
    free(pipes);

    /*
     Agregamos un waitpid para esperar a que todos los procesos terminen
     en específico se esperan a los hijos del proceso actual
     */
    for (int i = 0; i < cantidadComandos; i++) {
        waitpid(-1, NULL, 0);
    }
}


int main(void) {
char lineaLeida[MAX_LINE];
char *argumentos[MAX_ARGS];

    /*
     Ignoramos las señales SIGINT y SIGQUIT para que ellas no terminen miShell
     */

    ignorarSignal(SIGINT);
    ignorarSignal(SIGQUIT);
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

    /*
     Verificamos si la linea leida presenta varios comandos separados por |
     */

    if (strchr(lineaLeida, '|')!= NULL) {
        Comando comandos[MAX_COMANDOS];
        int cantidadComandos = 0;
        cantidadComandos = separarPipelines(lineaLeida, comandos, MAX_COMANDOS);
        if (cantidadComandos > 0) {
            ejecutarPipes(comandos, cantidadComandos);
        }
        continue;
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