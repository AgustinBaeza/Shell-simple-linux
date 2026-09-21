# Shell- Sistemas Operativos 

## Requisitos

* Linux
* GCC
* Make

## Compilación

Desde la carpeta donde se encuentran `miShell.c` y el `Makefile`, ejecutar:

```bash
make
```

Esto generará el ejecutable:

```text
miShell
```

## Ejecución

Ejecutar:

```bash
./miShell
```

La shell mostrará un prompt y permitirá ingresar comandos.

## Ejemplos

Comando normal:

```bash
ls
```

Cambiar directorio:

```bash
cd ..
```

Pipe:

```bash
ls | grep .c
```

Redirección:

```bash
ls > salida.txt
```

Ejecutar en segundo plano:

```bash
sleep 30 &
```

Ver jobs:

```bash
jobs
```

Monitorear procesos en segundo plano:

```bash
pmon
```

También se puede indicar el tiempo de actualización:

```bash
pmon 2
```

Para salir de `pmon`, presionar:

```text
Ctrl+C
```

Para cerrar la shell:

```bash
exit
```


