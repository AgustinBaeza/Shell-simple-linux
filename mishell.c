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
#include <time.h>
#include <ctype.h>

#define MAX_LINE 1024
#define MAX_ARGS 64
#define MAX_COMANDOS 64

#define MAX_JOBS 64
#define MAX_COMANDO 256

typedef struct {
    int   numero;
    pid_t pid;

    pid_t pids[MAX_COMANDOS];
    int   cantidadProcesos;
    int   procesosPendientes;

    char  comando[MAX_COMANDO];
    int   activo;
    int   terminado;
} Job;

static Job jobsList[MAX_JOBS];
static int siguienteNumeroJob = 1;
static volatile sig_atomic_t hayJobsTerminados = 0;
static int leerStatPmon(pid_t pid, char *estado, unsigned long long *cpuTicks);
static const char *textoEstadoPmon(char estado);

/*
 * Variables utilizadas por pmon.
 * Las señales solo cambian estas banderas.
 */
static volatile sig_atomic_t pmonActualizar = 0;
static volatile sig_atomic_t pmonSalir = 0;

/*
 * Guarda la medición anterior de CPU de un proceso para poder
 * calcular el porcentaje aproximado en la siguiente actualización.
 */
typedef struct {
    pid_t pid;
    unsigned long long cpuTicks;
    struct timespec tiempo;
    int valido;
} MedicionPmon;


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
    char *estadoSeparacion;
    char *token = strtok_r(linea, " \t\n", &estadoSeparacion);

    while (token != NULL){
        if (cantidadArgumentos >= maxArgumentos - 1) {
            fprintf(stderr, "miShell: Demasiados argumentos. Límite de argumentos: %d", maxArgumentos);
            return -1;
        }
        argumentos[cantidadArgumentos++] = token;
        token = strtok_r(NULL, " \t\n", &estadoSeparacion);
    }

    argumentos[cantidadArgumentos] = NULL;

    return cantidadArgumentos;
}

/*
 Funcion que separa los comandos para poder correr varios comandos al mismo tiempo mediante el uso de tuberías.
 Separa los comandos según la aparición de '|'
 */
static int separarPipelines(char *linea, Comando comandos[], int maxComandos) {
    int contadorComandos = 0;

    char *saveptr = NULL;
    char *segmento = strtok_r(linea, "|", &saveptr);

    while (segmento != NULL) {

        if (contadorComandos >= maxComandos) {
            fprintf(stderr,
                    "miShell: Demasiados comandos. Límite: %d\n",
                    maxComandos);
            return -1;
        }

        comandos[contadorComandos].cantidadArgumentos =
            separarEnTokens(
                segmento,
                comandos[contadorComandos].arrayArgumentos,
                MAX_ARGS
            );

        if (comandos[contadorComandos].cantidadArgumentos <= 0) {
            fprintf(stderr,
                    "miShell: comando vacío en la tubería\n");
            return -1;
        }

        contadorComandos++;

        segmento = strtok_r(NULL, "|", &saveptr);
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

/* Guarda un comando o una tubería completa como un job */
static int agregarJob(pid_t pids[], int cantidadProcesos, const char *comando) {

    for (int i = 0; i < MAX_JOBS; i++) {

        if (jobsList[i].activo == 0) {

            jobsList[i].numero = siguienteNumeroJob++;
            jobsList[i].pid = pids[0];

            jobsList[i].cantidadProcesos = cantidadProcesos;
            jobsList[i].procesosPendientes = cantidadProcesos;

            jobsList[i].activo = 1;
            jobsList[i].terminado = 0;

            for (int j = 0; j < cantidadProcesos; j++) {
                jobsList[i].pids[j] = pids[j];
            }

            snprintf(
                jobsList[i].comando,
                sizeof(jobsList[i].comando),
                "%s",
                comando
            );

            return jobsList[i].numero;
        }
    }

    return -1;
}

/* Imprime los jobs que se estan ejecutando */
void listarJobs(void) {

    for (int i = 0; i < MAX_JOBS; i++) {

        if (jobsList[i].activo == 1) {

            char estado;
            unsigned long long cpuTicks;

            leerStatPmon(jobsList[i].pid, &estado, &cpuTicks);
                printf("[%d] %d %s %s\n",
                   jobsList[i].numero,
                   jobsList[i].pid,
                   textoEstadoPmon(estado),
                   jobsList[i].comando);
        }
    }
}

/* Revisa la lista y avisa si alguno ya termino */
void notificarJobsTerminados(void) {

    for (int i = 0; i < MAX_JOBS; i++) {

        if (jobsList[i].activo == 1 &&
            jobsList[i].terminado == 1) {

            printf("[%d]+ Done %s\n",
                   jobsList[i].numero,
                   jobsList[i].comando);

            jobsList[i].activo = 0;
        }
    }

    hayJobsTerminados = 0;
}

/* Esta funcion se ejecuta automaticamente cuando muere un proceso hijo */
void manejadorSigchld(int senal) {
    (void)senal;
    int estadoSalida;
    pid_t pid;

    while ((pid = waitpid(-1,
                          &estadoSalida,
                          WNOHANG)) > 0) {

        for (int i = 0; i < MAX_JOBS; i++) {

            if (jobsList[i].activo != 1) {
                continue;
            }

            for (int j = 0;
                 j < jobsList[i].cantidadProcesos;
                 j++) {

                if (jobsList[i].pids[j] == pid) {

                    if (jobsList[i].procesosPendientes > 0) {
                        jobsList[i].procesosPendientes--;
                    }

                    if (jobsList[i].procesosPendientes == 0) {

                        jobsList[i].terminado = 1;
                        hayJobsTerminados = 1;
                    }

                    break;
                }
            }
        }
    }
}

/* Configura la captura de la señal */
void instalarManejadorSigchld(void) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = manejadorSigchld;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART | SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &act, NULL) < 0) {
        perror("sigaction SIGCHLD");
        exit(1);
    }
}

static void bloquearSigchld(sigset_t *mascaraAnterior) {

    sigset_t mascara;

    sigemptyset(&mascara);
    sigaddset(&mascara, SIGCHLD);

    if (sigprocmask(SIG_BLOCK,
                    &mascara,
                    mascaraAnterior) < 0) {

        perror("sigprocmask");
        exit(1);
    }
}


static void restaurarMascara(
    const sigset_t *mascaraAnterior) {

    if (sigprocmask(SIG_SETMASK,
                    mascaraAnterior,
                    NULL) < 0) {

        perror("sigprocmask");
        exit(1);
    }
}
/*
Intenta ejecutar argumentos[0] como comando interno
Si no hay argumentos retorna 1
Si se ejecuta exitosamente retorna 1
Si no ejecuta el comando retorna 0
*/

/*
 * Manejador de SIGALRM.
 * Solo coloca una bandera para que el ciclo principal actualice pmon.
 */
static void manejadorSigalrmPmon(int senal) {
    (void)senal;
    pmonActualizar = 1;
}

/*
 * Manejador de Ctrl+C mientras pmon está funcionando.
 * Solo termina pmon; la shell sigue funcionando.
 */
static void manejadorSigintPmon(int senal) {
    (void)senal;
    pmonSalir = 1;
    pmonActualizar = 1;
}

/*
 * Lee directamente /proc/[pid]/stat.
 * Obtiene el estado del proceso y utime + stime.
 */
static int leerStatPmon(pid_t pid, char *estado,
                        unsigned long long *cpuTicks) {
    char ruta[64];
    char buffer[4096];

    snprintf(ruta, sizeof(ruta), "/proc/%d/stat", (int)pid);

    FILE *archivo = fopen(ruta, "r");
    if (archivo == NULL) {
        return -1;
    }

    if (fgets(buffer, sizeof(buffer), archivo) == NULL) {
        fclose(archivo);
        return -1;
    }

    fclose(archivo);

    /*
     * El nombre del proceso puede contener espacios.
     * Por eso buscamos el último ')' del segundo campo.
     */
    char *finComm = strrchr(buffer, ')');
    if (finComm == NULL || finComm[1] != ' ') {
        return -1;
    }

    *estado = finComm[2];

    /*
     * Aquí empieza el campo 4.
     * utime es el campo 14 y stime el campo 15.
     */
    char *resto = finComm + 4;
    char *guardar = NULL;
    char *token = strtok_r(resto, " ", &guardar);

    unsigned long long utime = 0;
    unsigned long long stime = 0;
    int numeroCampo = 4;

    while (token != NULL) {
        if (numeroCampo == 14) {
            utime = strtoull(token, NULL, 10);
        } else if (numeroCampo == 15) {
            stime = strtoull(token, NULL, 10);
            break;
        }

        numeroCampo++;
        token = strtok_r(NULL, " ", &guardar);
    }

    if (numeroCampo < 15) {
        return -1;
    }

    *cpuTicks = utime + stime;
    return 0;
}

/*
 * Lee directamente /proc/[pid]/status para obtener VmRSS.
 */
static long leerRssPmon(pid_t pid) {
    char ruta[64];
    char linea[256];

    snprintf(ruta, sizeof(ruta), "/proc/%d/status", (int)pid);

    FILE *archivo = fopen(ruta, "r");
    if (archivo == NULL) {
        return -1;
    }

    long rss = -1;

    while (fgets(linea, sizeof(linea), archivo) != NULL) {
        if (strncmp(linea, "VmRSS:", 6) == 0) {
            rss = strtol(linea + 6, NULL, 10);
            break;
        }
    }

    fclose(archivo);
    return rss;
}

/*
 * Busca la medición anterior de un PID.
 */
static MedicionPmon *buscarMedicionPmon(MedicionPmon mediciones[],
                                        int cantidad, pid_t pid) {
    for (int i = 0; i < cantidad; i++) {
        if (mediciones[i].valido && mediciones[i].pid == pid) {
            return &mediciones[i];
        }
    }

    return NULL;
}

/*
 * Guarda una medición nueva o actualiza una existente.
 */
static void guardarMedicionPmon(MedicionPmon mediciones[],
                                int *cantidad,
                                pid_t pid,
                                unsigned long long cpuTicks,
                                struct timespec ahora) {
    MedicionPmon *anterior =
        buscarMedicionPmon(mediciones, *cantidad, pid);

    if (anterior != NULL) {
        anterior->cpuTicks = cpuTicks;
        anterior->tiempo = ahora;
        return;
    }

    if (*cantidad < MAX_JOBS) {
        mediciones[*cantidad].pid = pid;
        mediciones[*cantidad].cpuTicks = cpuTicks;
        mediciones[*cantidad].tiempo = ahora;
        mediciones[*cantidad].valido = 1;
        (*cantidad)++;
    }
}

/*
 * Calcula %CPU usando la diferencia de tiempo de CPU del proceso
 * dividida por el tiempo real transcurrido.
 */
static double calcularCpuPmon(MedicionPmon *anterior,
                              unsigned long long cpuTicks,
                              struct timespec ahora,
                              long ticksPorSegundo) {
    if (anterior == NULL || ticksPorSegundo <= 0) {
        return 0.0;
    }

    unsigned long long deltaTicks = 0;

    if (cpuTicks >= anterior->cpuTicks) {
        deltaTicks = cpuTicks - anterior->cpuTicks;
    }

    double cpuSegundos =
        (double)deltaTicks / (double)ticksPorSegundo;

    double tiempoReal =
        (double)(ahora.tv_sec - anterior->tiempo.tv_sec) +
        (double)(ahora.tv_nsec - anterior->tiempo.tv_nsec) /
        1000000000.0;

    if (tiempoReal <= 0.0) {
        return 0.0;
    }

    return (cpuSegundos / tiempoReal) * 100.0;
}

/*
 * Convierte la letra de estado de /proc/[pid]/stat
 * en un texto más fácil de leer.
 */
static const char *textoEstadoPmon(char estado) {
    switch (estado) {
        case 'R':
            return "ejecutando";
        case 'S':
            return "durmiendo";
        case 'D':
            return "espera";
        case 'T':
            return "detenido";
        case 'Z':
            return "zombie";
        case 'X':
            return "muerto";
        default:
            return "desconocido";
    }
}

/*
 * Dibuja la tabla de pmon.
 */
static void mostrarPmon(MedicionPmon mediciones[],
                        int *cantidadMediciones,
                        long ticksPorSegundo) {
    struct timespec ahora;
    clock_gettime(CLOCK_MONOTONIC, &ahora);

    printf("\033[H\033[J");

    printf("PID\tCOMANDO\t\t\tESTADO\t\t%%CPU(aprox)\tRSS(KB)\n");
    printf("-------------------------------------------------------------------------------\n");

    for (int i = 0; i < MAX_JOBS; i++) {
        if (jobsList[i].activo != 1) {
            continue;
        }


        /*
         Realizamos un for para iterar sobre todos los procesos
         Es decir, tenemos en cuenta al pipeline, si hay varios comandos separados poe '|', mostramos a cada uno en jobs
         */

        for (int j = 0; j < jobsList[i].cantidadProcesos; j++) {

        pid_t pid = jobsList[i].pids[j];
        char estado;
        unsigned long long cpuTicks;

        if (leerStatPmon(pid, &estado, &cpuTicks) != 0) {
            /*
             * Si ya no existe, simplemente no se muestra en esta
             * actualización.
             */
            continue;
        }

        long rss = leerRssPmon(pid);

        MedicionPmon *anterior =
            buscarMedicionPmon(mediciones, *cantidadMediciones, pid);

        double cpu =
            calcularCpuPmon(anterior, cpuTicks, ahora, ticksPorSegundo);

        printf("%d\t%-24s %-16s %8.2f\t%ld\n",
               (int)pid,
               jobsList[i].comando,
               textoEstadoPmon(estado),
               cpu,
               rss);

        guardarMedicionPmon(mediciones,
                            cantidadMediciones,
                            pid,
                            cpuTicks,
                            ahora);
        }
    }


    fflush(stdout);
}

/*
 * Implementación del comando interno:
 *
 *     pmon
 *     pmon [segundos]
 *
 * Por defecto actualiza cada 2 segundos.
 */
static void ejecutarPmon(int segundos) {
    if (segundos <= 0) {
        segundos = 2;
    }

    struct sigaction accionAlarma;
    struct sigaction accionInt;

    /*
     * SIGALRM se utiliza para pedir una nueva actualización.
     */
    memset(&accionAlarma, 0, sizeof(accionAlarma));
    accionAlarma.sa_handler = manejadorSigalrmPmon;
    sigemptyset(&accionAlarma.sa_mask);
    accionAlarma.sa_flags = SA_RESTART;

    if (sigaction(SIGALRM, &accionAlarma, NULL) < 0) {
        perror("sigaction SIGALRM");
        return;
    }

    /*
     * La shell ignora SIGINT, por lo que mientras pmon está activo
     * cambiamos temporalmente SIGINT para poder salir con Ctrl+C.
     */
    memset(&accionInt, 0, sizeof(accionInt));
    accionInt.sa_handler = manejadorSigintPmon;
    sigemptyset(&accionInt.sa_mask);
    accionInt.sa_flags = 0;

    if (sigaction(SIGINT, &accionInt, NULL) < 0) {
        perror("sigaction SIGINT");
        return;
    }

    pmonActualizar = 1;
    pmonSalir = 0;

    MedicionPmon mediciones[MAX_JOBS];
    memset(mediciones, 0, sizeof(mediciones));

    int cantidadMediciones = 0;

    long ticksPorSegundo = sysconf(_SC_CLK_TCK);

    if (ticksPorSegundo <= 0) {
        ticksPorSegundo = 100;
    }

    while (!pmonSalir) {
        if (pmonActualizar) {
            pmonActualizar = 0;

            mostrarPmon(mediciones,
                        &cantidadMediciones,
                        ticksPorSegundo);

            /*
             * Si SIGCHLD detectó un job terminado, lo quitamos de
             * la lista para que desaparezca de la siguiente tabla.
             */
            if (hayJobsTerminados) {
                notificarJobsTerminados();
            }

            if (!pmonSalir) {
                alarm((unsigned int)segundos);
            }
        }
        if (!pmonSalir) {
            pause();
        }
    }

    alarm(0);

    /*
     * Restauramos SIGINT al comportamiento original de la shell:
     * ignorarlo.
     */
    struct sigaction ignorar;
    memset(&ignorar, 0, sizeof(ignorar));
    ignorar.sa_handler = SIG_IGN;
    sigemptyset(&ignorar.sa_mask);
    ignorar.sa_flags = SA_RESTART;

    if (sigaction(SIGINT, &ignorar, NULL) < 0) {
        perror("sigaction SIGINT");
    }

    /*
     * Restauramos SIGALRM a su comportamiento por defecto.
     */
    memset(&accionAlarma, 0, sizeof(accionAlarma));
    accionAlarma.sa_handler = SIG_DFL;
    sigemptyset(&accionAlarma.sa_mask);
    accionAlarma.sa_flags = 0;

    if (sigaction(SIGALRM, &accionAlarma, NULL) < 0) {
        perror("sigaction SIGALRM");
    }

    printf("\n");
}

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

    if (strcmp(argumentos[0], "pmon") == 0) {
        int segundos = (cantidadArgumentos > 1)
            ? atoi(argumentos[1])
            : 2;

        if (cantidadArgumentos > 2) {
            fprintf(stderr, "pmon: uso: pmon [segundos]\n");
            return 1;
        }

        ejecutarPmon(segundos);
        return 1;
    }


    return 0;
}

/*
Ejecuta y crea un proceso con fork, y utilizando execvp para hacer otro proceso
y espera con waitpid
*/
static void ejecutarComandoExterno(
    char *argumentos[],
    int cantidadArgumentos,
    int esBackground,
    const char *comandoOriginal) {
    sigset_t mascaraAnterior;
    bloquearSigchld(&mascaraAnterior);
    pid_t pidHijo = fork();

    if (pidHijo < 0) {

        restaurarMascara(&mascaraAnterior);

        perror("fork");
        exit(1);
    }


    if (pidHijo == 0) {

        restaurarMascara(&mascaraAnterior);

        /*
         * Los comandos foreground deben volver
         * al comportamiento normal de Ctrl+C.
         */
        if (!esBackground) {

            restaurarSignalPorDefecto(SIGINT);
            restaurarSignalPorDefecto(SIGQUIT);
        }


        int cantidadLimpia =
            aplicarRedirecciones(
                argumentos,
                cantidadArgumentos
            );

        if (cantidadLimpia <= 0) {
            _exit(1);
        }


        execvp(argumentos[0], argumentos);


        fprintf(stderr,
                "miShell: %s: %s\n",
                argumentos[0],
                strerror(errno));

        _exit(127);
    }


    if (esBackground) {

        pid_t pids[1] = { pidHijo };

        int numero =
            agregarJob(
                pids,
                1,
                comandoOriginal
            );

        if (numero > 0) {

            printf("[%d] %d\n",
                   numero,
                   (int)pidHijo);

        } else {

            fprintf(stderr,
                    "miShell: lista de jobs llena\n");
        }

        restaurarMascara(&mascaraAnterior);

    } else {

        int estadoSalida;

        while (waitpid(pidHijo,
                       &estadoSalida,
                       0) < 0 &&
               errno == EINTR) {
        }

        restaurarMascara(&mascaraAnterior);
    }
}
/*
    Funcion para ejecutar varios comandos al mismo tiempo.

 */
static void ejecutarPipes(
    Comando comandos[],
    int cantidadComandos,
    int esBackground,
    const char *comandoOriginal) {

    if (cantidadComandos < 2) {
        return;
    }

    /* Verificamos los comandos */
    for (int i = 0; i < cantidadComandos; i++) {

        if (comandos[i].cantidadArgumentos <= 0) {

            fprintf(stderr,
                    "miShell: comando vacío en la tubería\n");

            return;
        }


        if (esComandoInterno(
                comandos[i].arrayArgumentos[0])) {

            fprintf(stderr,
                    "miShell: el comando interno '%s' "
                    "no se admite dentro de una tubería\n",
                    comandos[i].arrayArgumentos[0]);

            return;
        }
    }

    int cantidadPipes = cantidadComandos - 1;

    int (*pipes)[2] =
        malloc(sizeof(int[2]) * cantidadPipes);


    if (pipes == NULL) {
        perror("malloc");
        return;
    }


    /* Creamos todos los pipes */
    for (int i = 0; i < cantidadPipes; i++) {

        if (pipe(pipes[i]) < 0) {

            perror("pipe");

            for (int j = 0; j < i; j++) {

                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            free(pipes);

            return;
        }
    }


    sigset_t mascaraAnterior;
    bloquearSigchld(&mascaraAnterior);
    pid_t pids[MAX_COMANDOS];

    int creados = 0;


    /*
     * Creamos un hijo por cada comando
     */
    for (int i = 0;
         i < cantidadComandos;
         i++) {

        pid_t pid = fork();


        if (pid < 0) {

            perror("fork");


            for (int j = 0;
                 j < cantidadPipes;
                 j++) {

                close(pipes[j][0]);
                close(pipes[j][1]);
            }


            for (int j = 0;
                 j < creados;
                 j++) {

                kill(pids[j], SIGTERM);

                waitpid(
                    pids[j],
                    NULL,
                    0
                );
            }


            free(pipes);

            restaurarMascara(
                &mascaraAnterior
            );

            exit(1);
        }


        /*
         * PROCESO HIJO
         */
        if (pid == 0) {

            restaurarMascara(
                &mascaraAnterior
            );


            if (!esBackground) {

                restaurarSignalPorDefecto(
                    SIGINT
                );

                restaurarSignalPorDefecto(
                    SIGQUIT
                );
            }


            /*
             * Si no es el primer comando,
             * recibe stdin del pipe anterior.
             */
            if (i > 0) {

                if (dup2(
                        pipes[i - 1][0],
                        STDIN_FILENO) < 0) {

                    perror("dup2 stdin");
                    _exit(1);
                }
            }


            /*
             * Si no es el último comando,
             * stdout va al siguiente pipe.
             */
            if (i < cantidadComandos - 1) {

                if (dup2(
                        pipes[i][1],
                        STDOUT_FILENO) < 0) {

                    perror("dup2 stdout");
                    _exit(1);
                }
            }


            /*
             * Después de dup2 todos estos
             * descriptores deben cerrarse.
             */
            for (int j = 0;
                 j < cantidadPipes;
                 j++) {

                close(pipes[j][0]);
                close(pipes[j][1]);
            }


            /*
             * Aplicamos <, > y >>.
             */
            int cantidadLimpia =
                aplicarRedirecciones(
                    comandos[i].arrayArgumentos,
                    comandos[i].cantidadArgumentos
                );


            if (cantidadLimpia <= 0) {
                _exit(1);
            }


            execvp(
                comandos[i].arrayArgumentos[0],
                comandos[i].arrayArgumentos
            );


            fprintf(
                stderr,
                "miShell: %s: %s\n",
                comandos[i].arrayArgumentos[0],
                strerror(errno)
            );


            _exit(127);
        }


        /*
         * PADRE
         */
        pids[creados++] = pid;
    }


    /*
     * La shell tampoco necesita los pipes.
     */
    for (int i = 0;
         i < cantidadPipes;
         i++) {

        close(pipes[i][0]);
        close(pipes[i][1]);
    }


    free(pipes);


    /*
     * PIPELINE EN BACKGROUND
     */
    if (esBackground) {

        int numero =
            agregarJob(
                pids,
                cantidadComandos,
                comandoOriginal
            );


        if (numero > 0) {

            printf("[%d] %d\n", numero, (int)pids[0]);

        } else {

            fprintf(
                stderr,
                "miShell: lista de jobs llena\n"
            );
        }


        restaurarMascara(
            &mascaraAnterior
        );
    }


    /*
     * PIPELINE EN FOREGROUND
     */
    else {

        for (int i = 0;
             i < cantidadComandos;
             i++) {

            while (waitpid(
                       pids[i],
                       NULL,
                       0) < 0 &&
                   errno == EINTR) {
            }
        }


        restaurarMascara(
            &mascaraAnterior
        );
    }
}


int main(void) {

    char lineaLeida[MAX_LINE];
    char *argumentos[MAX_ARGS];

    /*
     * La shell no debe morir con
     * Ctrl+C ni Ctrl+\.
     */
    ignorarSignal(SIGINT);
    ignorarSignal(SIGQUIT);
    instalarManejadorSigchld();


    while (1) {
        /*
         * Primero mostramos jobs terminados.
         */
        if (hayJobsTerminados) {
            notificarJobsTerminados();
        }

        mostrarPrompt();

        /*
         * Ctrl+D
         */
        if (fgets(
                lineaLeida,
                sizeof(lineaLeida),
                stdin) == NULL) {

            printf("\n");
            break;
        }

        /*
         * Quitamos el salto de línea.
         */
        lineaLeida[
            strcspn(lineaLeida, "\n")
        ] = '\0';

        /*
         * Quitamos espacios finales.
         */
        size_t largo =
            strlen(lineaLeida);


        while (largo > 0 &&
               isspace(
                   (unsigned char)
                   lineaLeida[largo - 1])) {

            lineaLeida[--largo] = '\0';
        }

        /*
         * Revisamos si termina en &
         */
        int esBackground = 0;

        if (largo > 0 &&
            lineaLeida[largo - 1] == '&') {

            esBackground = 1;

            lineaLeida[--largo] = '\0';


            while (largo > 0 &&
                   isspace(
                       (unsigned char)
                       lineaLeida[largo - 1])) {

                lineaLeida[--largo] = '\0';
            }
        }

        /*
         * Línea vacía.
         */
        if (largo == 0) {
            continue;
        }

        /*
         * Guardamos el texto original
         * antes de modificarlo con strtok.
         */
        char comandoOriginal[MAX_LINE];

        snprintf(
            comandoOriginal,
            sizeof(comandoOriginal),
            "%s",
            lineaLeida
        );

        /*
         * Si contiene |, usamos
         * la implementación de pipelines.
         */
        if (strchr(lineaLeida, '|') != NULL) {

            Comando comandos[MAX_COMANDOS];

            memset(
                comandos,
                0,
                sizeof(comandos)
            );

            int cantidadComandos =
                separarPipelines(
                    lineaLeida,
                    comandos,
                    MAX_COMANDOS
                );


            if (cantidadComandos < 0) {
                continue;
            }


            if (cantidadComandos < 2) {

                fprintf(
                    stderr,
                    "miShell: tubería inválida\n"
                );

                continue;
            }


            ejecutarPipes(
                comandos,
                cantidadComandos,
                esBackground,
                comandoOriginal
            );


            continue;
        }

        /*
         * Comando normal.
         */
        int cantidadArgumentos =
            separarEnTokens(
                lineaLeida,
                argumentos,
                MAX_ARGS
            );


        if (cantidadArgumentos <= 0) {
            continue;
        }

        /*
         * Comandos internos.
         */
        if (ejecutarComandoInterno(
                argumentos,
                cantidadArgumentos)) {

            continue;
        }

        /*
         * Comando externo.
         */
        ejecutarComandoExterno(
            argumentos,
            cantidadArgumentos,
            esBackground,
            comandoOriginal
        );
    }
    return 0;
}