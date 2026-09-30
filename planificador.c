/* Tarea 1 - Sistemas Operativos
 * Planificador Dieciochero: ejecuta un DAG de actividades usando procesos.
 * uso: ./planificador plan.txt K
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

#define MAX_LINEA 2048
#define MAX_ID 64
#define MAX_NOMBRE 128
#define MAX_MENSAJE 256
#define MAX_INSUMOS 4096

typedef enum {
    PENDIENTE,
    EJECUTANDO,
    COMPLETADA,
    FALLIDA,
    ABORTADA
} Estado;

typedef struct {
    char id[MAX_ID];
    char nombre[MAX_NOMBRE];
    int tiempo_ms;

    char **dep_ids;
    int n_deps_raw;
    int *dep_idx;

    int *dependientes;
    int n_dependientes;
    int cap_dependientes;

    int indegree;
    Estado estado;

    pid_t pid;
    int out_read_fd;
    char mensaje[MAX_MENSAJE];
} Actividad;

/* tabla hash para id (texto) -> indice, open addressing simple */
typedef struct {
    char *key;
    int idx;
    int usado;
} HashEntry;

static HashEntry *tabla = NULL;
static size_t cap_tabla = 0;

static unsigned long hash_str(const char *s) {
    unsigned long h = 5381;
    int c;
    while ((c = *s++)) h = ((h << 5) + h) + (unsigned long)c;
    return h;
}

static void tabla_init(size_t n) {
    cap_tabla = 8;
    while (cap_tabla < n * 4) cap_tabla <<= 1;
    tabla = calloc(cap_tabla, sizeof(HashEntry));
    if (!tabla) { perror("calloc tabla"); exit(1); }
}

static void tabla_insertar(const char *key, int idx) {
    unsigned long h = hash_str(key) % cap_tabla;
    while (tabla[h].usado) h = (h + 1) % cap_tabla;
    tabla[h].key = strdup(key);
    tabla[h].idx = idx;
    tabla[h].usado = 1;
}

static int tabla_buscar(const char *key) {
    unsigned long h = hash_str(key) % cap_tabla;
    size_t inicio = h;
    while (tabla[h].usado) {
        if (strcmp(tabla[h].key, key) == 0) return tabla[h].idx;
        h = (h + 1) % cap_tabla;
        if (h == inicio) break;
    }
    return -1;
}

static Actividad *acts = NULL;
static int n = 0;
static int K = 1;
static int running = 0;
static long completadas = 0, fallidas = 0, abortadas = 0;

static int *cola = NULL;
static int cola_head = 0, cola_tail = 0, cola_size = 0;

static int *pila_abort = NULL;

typedef struct { pid_t pid; int idx; int usado; } PidEntry;
static PidEntry *tabla_pid = NULL;
static size_t cap_tabla_pid = 0;

static volatile sig_atomic_t sigint_recibido = 0;

static void trim(char *s) {
    char *inicio = s;
    while (*inicio == ' ' || *inicio == '\t') inicio++;
    if (inicio != s) memmove(s, inicio, strlen(inicio) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
}

static void cola_push(int idx) {
    cola[cola_tail] = idx;
    cola_tail = (cola_tail + 1) % (n + 1);
    cola_size++;
}

static int cola_pop(void) {
    int idx = cola[cola_head];
    cola_head = (cola_head + 1) % (n + 1);
    cola_size--;
    return idx;
}

static void pid_tabla_init(int cap_K) {
    cap_tabla_pid = 8;
    while ((int)cap_tabla_pid < cap_K * 4) cap_tabla_pid <<= 1;
    tabla_pid = calloc(cap_tabla_pid, sizeof(PidEntry));
}

static void pid_tabla_insertar(pid_t pid, int idx) {
    unsigned long h = ((unsigned long)pid) % cap_tabla_pid;
    while (tabla_pid[h].usado) h = (h + 1) % cap_tabla_pid;
    tabla_pid[h].pid = pid;
    tabla_pid[h].idx = idx;
    tabla_pid[h].usado = 1;
}

static int pid_tabla_extraer(pid_t pid) {
    unsigned long h = ((unsigned long)pid) % cap_tabla_pid;
    size_t inicio = h;
    while (tabla_pid[h].usado) {
        if (tabla_pid[h].pid == pid) {
            int idx = tabla_pid[h].idx;
            tabla_pid[h].usado = 0;
            return idx;
        }
        h = (h + 1) % cap_tabla_pid;
        if (h == inicio) break;
    }
    return -1;
}

/* linea: ID : Nombre : tiempo_ms : [dep1, dep2] (corchetes opcionales) */
static int parse_linea(char *linea, Actividad *act) {
    size_t len = strlen(linea);
    while (len > 0 && (linea[len - 1] == '\n' || linea[len - 1] == '\r')) {
        linea[--len] = '\0';
    }

    char *p = linea;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '#') return 0;

    char *campos[4];
    char *cursor = p;
    for (int i = 0; i < 3; i++) {
        char *dp = strchr(cursor, ':');
        if (!dp) {
            fprintf(stderr, "linea mal formada, se ignora: %s\n", linea);
            return -1;
        }
        *dp = '\0';
        campos[i] = cursor;
        cursor = dp + 1;
    }
    campos[3] = cursor;

    for (int i = 0; i < 4; i++) trim(campos[i]);

    if (strlen(campos[0]) == 0) return -1;

    strncpy(act->id, campos[0], MAX_ID - 1);
    act->id[MAX_ID - 1] = '\0';
    strncpy(act->nombre, campos[1], MAX_NOMBRE - 1);
    act->nombre[MAX_NOMBRE - 1] = '\0';

    if (strlen(campos[2]) == 0) {
        act->tiempo_ms = 100 + rand() % 4901;
    } else {
        act->tiempo_ms = atoi(campos[2]);
        if (act->tiempo_ms <= 0) act->tiempo_ms = 100 + rand() % 4901;
    }

    char *dep_str = campos[3];
    size_t dl = strlen(dep_str);
    if (dl >= 2 && dep_str[0] == '[' && dep_str[dl - 1] == ']') {
        dep_str[dl - 1] = '\0';
        dep_str++;
    }

    act->dep_ids = NULL;
    act->n_deps_raw = 0;
    if (strlen(dep_str) > 0) {
        char *copia = strdup(dep_str);
        char *tok = strtok(copia, ",");
        while (tok) {
            trim(tok);
            if (strlen(tok) > 0) {
                act->dep_ids = realloc(act->dep_ids, sizeof(char *) * (act->n_deps_raw + 1));
                act->dep_ids[act->n_deps_raw++] = strdup(tok);
            }
            tok = strtok(NULL, ",");
        }
        free(copia);
    }

    act->dep_idx = NULL;
    act->dependientes = NULL;
    act->n_dependientes = 0;
    act->cap_dependientes = 0;
    act->indegree = 0;
    act->estado = PENDIENTE;
    act->pid = -1;
    act->out_read_fd = -1;
    act->mensaje[0] = '\0';

    return 1;
}

static void agregar_dependiente(int idx_dep, int idx_dependiente) {
    Actividad *a = &acts[idx_dep];
    if (a->n_dependientes == a->cap_dependientes) {
        a->cap_dependientes = a->cap_dependientes ? a->cap_dependientes * 2 : 4;
        a->dependientes = realloc(a->dependientes, sizeof(int) * a->cap_dependientes);
    }
    a->dependientes[a->n_dependientes++] = idx_dependiente;
}

static void cargar_plan(const char *ruta) {
    FILE *f = fopen(ruta, "r");
    if (!f) {
        fprintf(stderr, "no se pudo abrir '%s': %s\n", ruta, strerror(errno));
        exit(1);
    }

    int cap = 64;
    acts = malloc(sizeof(Actividad) * cap);
    n = 0;

    char linea[MAX_LINEA];
    while (fgets(linea, sizeof(linea), f)) {
        if (n == cap) {
            cap *= 2;
            acts = realloc(acts, sizeof(Actividad) * cap);
        }
        int r = parse_linea(linea, &acts[n]);
        if (r == 1) n++;
    }
    fclose(f);

    if (n == 0) {
        fprintf(stderr, "plan sin actividades validas\n");
        exit(1);
    }

    tabla_init((size_t)n);
    for (int i = 0; i < n; i++) tabla_insertar(acts[i].id, i);

    for (int i = 0; i < n; i++) {
        Actividad *a = &acts[i];
        a->dep_idx = malloc(sizeof(int) * (a->n_deps_raw > 0 ? a->n_deps_raw : 1));
        for (int k = 0; k < a->n_deps_raw; k++) {
            int d = tabla_buscar(a->dep_ids[k]);
            if (d < 0) {
                fprintf(stderr, "dependencia desconocida '%s' en '%s'\n", a->dep_ids[k], a->id);
                exit(1);
            }
            if (d == i) {
                fprintf(stderr, "'%s' depende de si misma\n", a->id);
                exit(1);
            }
            a->dep_idx[k] = d;
            agregar_dependiente(d, i);
        }
        a->indegree = a->n_deps_raw;
    }
}

/* aborta toda la descendencia de una actividad fallida, sin recursion
   (una cadena de 10000 nodos podria reventar el stack si fuera recursivo) */
static void abortar_descendencia(int idx_inicial) {
    int sp = 0;
    pila_abort[sp++] = idx_inicial;
    while (sp > 0) {
        int idx = pila_abort[--sp];
        Actividad *a = &acts[idx];
        for (int j = 0; j < a->n_dependientes; j++) {
            int dep = a->dependientes[j];
            if (acts[dep].estado == PENDIENTE) {
                acts[dep].estado = ABORTADA;
                abortadas++;
                pila_abort[sp++] = dep;
            }
        }
    }
}

static void manejador_sigint(int signo) {
    (void)signo;
    sigint_recibido = 1;
}

static void abortar_todo_por_sigint(void) {
    fprintf(stderr, "\nSIGINT recibido, abortando actividades en ejecucion...\n");
    for (int i = 0; i < n; i++) {
        if (acts[i].estado == EJECUTANDO) kill(acts[i].pid, SIGTERM);
    }
    for (int i = 0; i < n; i++) {
        if (acts[i].estado == EJECUTANDO) {
            int status;
            waitpid(acts[i].pid, &status, 0);
            if (acts[i].out_read_fd >= 0) close(acts[i].out_read_fd);
            acts[i].estado = ABORTADA;
            abortadas++;
        }
    }
}

static void ejecutar_actividad_hijo(Actividad *act, int in_read_fd, int out_write_fd) {
    signal(SIGINT, SIG_IGN); /* que sea el padre el que decida como abortar */

    char buffer_in[MAX_INSUMOS];
    ssize_t total = 0, r;
    while ((r = read(in_read_fd, buffer_in + total, sizeof(buffer_in) - (size_t)total - 1)) > 0) {
        total += r;
        if ((size_t)total >= sizeof(buffer_in) - 1) break;
    }
    buffer_in[total] = '\0';
    close(in_read_fd);

    printf("[PID %d] iniciando '%s' (%s) - %d ms\n", getpid(), act->nombre, act->id, act->tiempo_ms);
    if (total > 0) printf("[PID %d]   insumos: %s\n", getpid(), buffer_in);
    fflush(stdout);

    struct timespec ts;
    ts.tv_sec = act->tiempo_ms / 1000;
    ts.tv_nsec = (act->tiempo_ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);

    double tasa_falla = 0.0;
    const char *env_fr = getenv("PLANIFICADOR_FAIL_RATE");
    if (env_fr) tasa_falla = atof(env_fr);

    srand((unsigned)(getpid() ^ (int)time(NULL)));
    int fallo = (tasa_falla > 0.0) && (((double)rand() / (double)RAND_MAX) < tasa_falla);

    char msg[MAX_MENSAJE];
    if (fallo) {
        snprintf(msg, sizeof(msg), "FAIL:%s", act->id);
        write(out_write_fd, msg, strlen(msg));
        close(out_write_fd);
        printf("[PID %d] '%s' fallo\n", getpid(), act->nombre);
        fflush(stdout);
        exit(1);
    }

    snprintf(msg, sizeof(msg), "OK:%s completado en %dms", act->id, act->tiempo_ms);
    write(out_write_fd, msg, strlen(msg));
    close(out_write_fd);
    printf("[PID %d] '%s' completado\n", getpid(), act->nombre);
    fflush(stdout);
    exit(0);
}

static void lanzar_actividad(int idx) {
    Actividad *act = &acts[idx];

    char insumos[MAX_INSUMOS];
    insumos[0] = '\0';
    for (int k = 0; k < act->n_deps_raw; k++) {
        Actividad *dep = &acts[act->dep_idx[k]];
        strncat(insumos, dep->mensaje, sizeof(insumos) - strlen(insumos) - 2);
        strncat(insumos, ";", sizeof(insumos) - strlen(insumos) - 1);
    }

    int in_fd[2], out_fd[2];
    if (pipe(in_fd) == -1 || pipe(out_fd) == -1) {
        perror("pipe");
        exit(1);
    }

    /* si no hago flush aca, el hijo hereda el buffer de stdout sin
       vaciar y lo reimprime al hacer exit() -> salida duplicada */
    fflush(stdout);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }

    if (pid == 0) {
        close(in_fd[1]);
        close(out_fd[0]);
        ejecutar_actividad_hijo(act, in_fd[0], out_fd[1]);
        _exit(1);
    }

    close(in_fd[0]);
    close(out_fd[1]);

    if (strlen(insumos) > 0) write(in_fd[1], insumos, strlen(insumos));
    close(in_fd[1]);

    act->pid = pid;
    act->estado = EJECUTANDO;
    act->out_read_fd = out_fd[0];
    pid_tabla_insertar(pid, idx);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "uso: %s plan.txt K\n", argv[0]);
        return 1;
    }

    K = atoi(argv[2]);
    if (K < 1) {
        fprintf(stderr, "K tiene que ser >= 1\n");
        return 1;
    }

    srand((unsigned)time(NULL));
    cargar_plan(argv[1]);

    cola = malloc(sizeof(int) * (size_t)(n + 1));
    pila_abort = malloc(sizeof(int) * (size_t)n);
    pid_tabla_init(K);

    for (int i = 0; i < n; i++) {
        if (acts[i].indegree == 0) cola_push(i);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* sin SA_RESTART, para que corte el waitpid */
    sigaction(SIGINT, &sa, NULL);

    printf("plan cargado: %d actividades, K=%d\n", n, K);
    fflush(stdout);

    long terminadas = 0;
    while (terminadas < n) {
        while (running < K && cola_size > 0) {
            int idx = cola_pop();
            if (acts[idx].estado != PENDIENTE) continue;
            lanzar_actividad(idx);
            running++;
        }

        if (running == 0) break; /* nada corriendo y nada listo, no se puede avanzar mas */

        int status;
        pid_t pid = waitpid(-1, &status, 0);

        if (pid < 0) {
            if (errno == EINTR) {
                if (sigint_recibido) {
                    abortar_todo_por_sigint();
                    terminadas = completadas + fallidas + abortadas;
                    break;
                }
                continue;
            }
            perror("waitpid");
            break;
        }

        running--;
        int idx = pid_tabla_extraer(pid);
        if (idx < 0) continue;

        char buf[MAX_MENSAJE];
        ssize_t rd = read(acts[idx].out_read_fd, buf, sizeof(buf) - 1);
        if (rd < 0) rd = 0;
        buf[rd] = '\0';
        close(acts[idx].out_read_fd);
        acts[idx].out_read_fd = -1;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            acts[idx].estado = COMPLETADA;
            strncpy(acts[idx].mensaje, buf, MAX_MENSAJE - 1);
            completadas++;

            for (int j = 0; j < acts[idx].n_dependientes; j++) {
                int dep = acts[idx].dependientes[j];
                if (acts[dep].estado == PENDIENTE) {
                    acts[dep].indegree--;
                    if (acts[dep].indegree == 0) cola_push(dep);
                }
            }
        } else {
            acts[idx].estado = FALLIDA;
            strncpy(acts[idx].mensaje, buf, MAX_MENSAJE - 1);
            fallidas++;
            abortar_descendencia(idx);
        }

        terminadas = completadas + fallidas + abortadas;
    }

    printf("\n--- resumen ---\n");
    printf("total: %d, completadas: %ld, fallidas: %ld, abortadas: %ld\n",
           n, completadas, fallidas, abortadas);

    return sigint_recibido ? 130 : 0;
}
