# Planificador Dieciochero - Tarea 1 SO

Simulador de un DAG de actividades con procesos (fork), pipes y señales.
Sin hilos, según lo pide el enunciado.

## Compilar

```
make
```

o directamente:

```
gcc -Wall -Wextra -std=c17 -o planificador planificador.c -lpthread
```

El -lpthread va porque la pauta de compilación lo pide así, pero no se usa
ningún hilo en el programa.

## Correr

```
./planificador plan.txt K
```

K es el máximo de actividades corriendo al mismo tiempo.

Para probar el aislamiento de errores se puede forzar que las actividades
fallen con cierta probabilidad:

```
PLANIFICADOR_FAIL_RATE=0.3 ./planificador plan.txt 3
```

(0 a 1, por defecto 0 = nadie falla)

Para generar un plan grande y probar la carga de 10000 actividades:

```
python3 generar_stress.py 10000 plan_stress.txt
./planificador plan_stress.txt 50
```

## Cómo funciona

El programa principal (el "director") es un solo proceso que va haciendo
fork() de cada actividad cuando ya cumplió todas sus dependencias. Nunca
hace dos cosas a la vez, por lo que no hay condiciones de carrera en su
lógica: la única concurrencia real pasa entre los procesos hijos, que no
comparten memoria entre ellos.

Cada actividad tiene dos pipes: uno para recibir los mensajes de sus
dependencias antes de empezar, y otro para dejar su propio mensaje cuando
termina. El padre concatena los mensajes de las dependencias y se los pasa
al hijo por el pipe de entrada antes del fork.

Para el límite K: mientras hay cupo y actividades listas, se lanzan más.
Cuando se llega al límite, en vez de hacer polling se usa
`waitpid(-1, &status, 0)` bloqueante, que hace que el proceso quede dormido
hasta que cualquier hijo cambie de estado. Así se evita el busy-waiting.

Si una actividad falla (exit distinto de 0), no se decrementa el contador
de dependencias de sus hijos en el grafo (el "indegree"): en cambio se
marcan directamente como abortados, recursivamente, a todos los que
dependían de ella. El resto del DAG sigue corriendo normal. Esto se hace
con una pila en vez de recursión real por si la cadena de actividades es
muy larga (con 10000 nodos una recursión podría llegar a desbordar el
stack).

Para el Ctrl+C: el handler de SIGINT solo prende una bandera, no hace nada
pesado. Como no se usa SA_RESTART, el waitpid bloqueante se corta con
EINTR y ahí el director manda SIGTERM a todos los hijos vivos, los espera
con waitpid para que no queden zombies, y termina.

Para resolver los IDs de texto (que son alfanuméricos, no números) a
índices del arreglo se usa una tabla hash simple, porque buscar linealmente
con 10000 actividades sería muy lento.

## Pruebas que hice

- El ejemplo del enunciado con K=1 y K=2, revisando que el orden de
  ejecución respete las dependencias.
- FAIL_RATE=1.0 para confirmar que si falla una raíz se abortan solo sus
  descendientes y el programa no se cae.
- Ctrl+C en medio de la ejecución, revisando con `ps aux` que no queden
  procesos colgados.
- plan de 10000 actividades generado con generar_stress.py, con K=50:
  terminó bien en unos 12 segundos.
- Lo mismo pero con FAIL_RATE=0.3, para revisar que completadas + fallidas
  + abortadas siempre sume el total.
