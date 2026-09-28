#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define MAX_LINEA 8192
#define MAX_ID 64
#define MAX_NOMBRE 128
#define TIEMPO_MIN 100
#define TIEMPO_MAX 5000
#define MAX_MENSAJE 128    /* tamaño fijo de cada mensaje que viaja por las tuberías */

/* Estados de una actividad */
#define PENDIENTE 0
#define COMPLETADA 1
#define FALLIDA 2
#define ABORTADA 3

typedef struct {
    char id[MAX_ID];
    char nombre[MAX_NOMBRE];
    int tiempo;        /* en milisegundos */
    int num_deps;
    char **deps;       /* IDs de las dependencias, tal como vienen en el archivo */
    int *hijos;        /* posiciones de las actividades que dependen de esta */
    int num_hijos;
    int *padres;       /* posiciones de las actividades de las que depende */
    int num_padres;
    int pendientes;    /* dependencias que aún no terminan */
    pid_t pid;         /* pid del proceso que la simula (0 si no ha partido) */
    int pipe_salida[2];           /* tubería hijo -> padre con el insumo que produce */
    char mensaje[MAX_MENSAJE];    /* insumo producido, se reenvía a los dependientes */
    int estado_final;             /* PENDIENTE, COMPLETADA, FALLIDA o ABORTADA */
} Actividad;

Actividad *actividades = NULL;
int num_actividades = 0;
int capacidad = 0;
int prob_fallo = 0;  /* porcentaje de probabilidad de que una actividad falle */

struct timespec inicio_simulacion;
static volatile sig_atomic_t g_seremi = 0;

void manejador_sigint(int sig) {
    (void)sig;
    g_seremi = 1;
}

/* Quita espacios al inicio y al final. Modifica el string. */
char *recortar(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *fin = s + strlen(s);
    while (fin > s && isspace((unsigned char)fin[-1])) fin--;
    *fin = '\0';
    return s;
}

/* Corta s en el primer ':' y devuelve lo que viene después (o NULL si no hay). */
char *siguiente_campo(char *s) {
    char *p = strchr(s, ':');
    if (p == NULL) return NULL;
    *p = '\0';
    return p + 1;
}

int buscar_actividad(const char *id) {
    for (int i = 0; i < num_actividades; i++) {
        if (strcmp(actividades[i].id, id) == 0) return i;
    }
    return -1;
}

void agregar_dependencia(Actividad *a, const char *id) {
    a->deps = realloc(a->deps, (a->num_deps + 1) * sizeof(char *));
    if (a->deps == NULL) {
        perror("realloc");
        exit(1);
    }
    a->deps[a->num_deps] = strdup(id);
    a->num_deps++;
}

/* Lee la lista "1, 2, 3" (con o sin corchetes) y la guarda en a. */
void leer_dependencias(Actividad *a, char *texto) {
    for (char *c = texto; *c; c++) {
        if (*c == '[' || *c == ']') *c = ' ';
    }
    char *token = strtok(texto, ",");
    while (token != NULL) {
        token = recortar(token);
        if (strlen(token) > 0) agregar_dependencia(a, token);
        token = strtok(NULL, ",");
    }
}

/* Procesa una línea del archivo. Devuelve 0 si está bien, -1 si hay error. */
int leer_linea(char *linea, int num_linea) {
    char *id = linea;
    char *nombre = siguiente_campo(id);
    char *tiempo = nombre ? siguiente_campo(nombre) : NULL;
    char *deps = tiempo ? siguiente_campo(tiempo) : NULL;

    if (nombre == NULL || tiempo == NULL) {
        fprintf(stderr, "Linea %d: formato invalido\n", num_linea);
        return -1;
    }

    id = recortar(id);
    nombre = recortar(nombre);
    tiempo = recortar(tiempo);

    if (strlen(id) == 0 || strlen(id) >= MAX_ID) {
        fprintf(stderr, "Linea %d: ID invalido\n", num_linea);
        return -1;
    }
    if (buscar_actividad(id) != -1) {
        fprintf(stderr, "Linea %d: ID '%s' repetido\n", num_linea, id);
        return -1;
    }

    if (num_actividades == capacidad) {
        capacidad = capacidad == 0 ? 64 : capacidad * 2;
        actividades = realloc(actividades, capacidad * sizeof(Actividad));
        if (actividades == NULL) {
            perror("realloc");
            exit(1);
        }
    }

    Actividad *a = &actividades[num_actividades];
    memset(a, 0, sizeof(Actividad));
    strcpy(a->id, id);
    strncpy(a->nombre, nombre, MAX_NOMBRE - 1);

    if (strlen(tiempo) == 0) {
        a->tiempo = TIEMPO_MIN + rand() % (TIEMPO_MAX - TIEMPO_MIN + 1);
    } else {
        char *fin;
        long t = strtol(tiempo, &fin, 10);
        if (*fin != '\0' || t < 0) {
            fprintf(stderr, "Linea %d: tiempo '%s' invalido\n", num_linea, tiempo);
            return -1;
        }
        a->tiempo = (int)t;
    }

    if (deps != NULL) leer_dependencias(a, deps);

    num_actividades++;
    return 0;
}

int leer_plan(const char *ruta) {
    FILE *f = fopen(ruta, "r");
    if (f == NULL) {
        perror(ruta);
        return -1;
    }

    char linea[MAX_LINEA];
    int num_linea = 0;
    while (fgets(linea, sizeof(linea), f) != NULL) {
        num_linea++;
        char *l = recortar(linea);
        if (strlen(l) == 0 || l[0] == '#') continue;  /* líneas vacías o comentarios */
        if (leer_linea(l, num_linea) != 0) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* Agrega valor al final de una lista dinámica de enteros. */
void agregar_entero(int **lista, int *cantidad, int valor) {
    *lista = realloc(*lista, (*cantidad + 1) * sizeof(int));
    if (*lista == NULL) {
        perror("realloc");
        exit(1);
    }
    (*lista)[*cantidad] = valor;
    (*cantidad)++;
}

/* Convierte los IDs de las dependencias en aristas del grafo:
   si i depende de d, entonces i es hijo de d y d le suma 1 a los pendientes de i. */
int armar_grafo(void) {
    for (int i = 0; i < num_actividades; i++) {
        Actividad *a = &actividades[i];
        for (int j = 0; j < a->num_deps; j++) {
            int d = buscar_actividad(a->deps[j]);
            if (d == -1) {
                fprintf(stderr, "Actividad '%s' depende de '%s', que no existe\n", a->id, a->deps[j]);
                return -1;
            }
            if (d == i) {
                fprintf(stderr, "Actividad '%s' depende de si misma\n", a->id);
                return -1;
            }

            /* Si la dependencia viene repetida (ej: "1, 1") se cuenta una sola vez */
            int repetida = 0;
            for (int h = 0; h < actividades[d].num_hijos; h++) {
                if (actividades[d].hijos[h] == i) repetida = 1;
            }
            if (repetida) continue;

            agregar_entero(&actividades[d].hijos, &actividades[d].num_hijos, i);
            agregar_entero(&a->padres, &a->num_padres, d);
            a->pendientes++;
        }
    }
    return 0;
}

/* Revisa que el grafo no tenga ciclos (algoritmo de Kahn).
   Se van "sacando" las actividades sin pendientes; si al final quedan
   actividades sin sacar, es porque forman un ciclo. */
int revisar_ciclos(void) {
    int *pendientes = malloc(num_actividades * sizeof(int));
    int *cola = malloc(num_actividades * sizeof(int));
    if (num_actividades > 0 && (pendientes == NULL || cola == NULL)) {
        perror("malloc");
        exit(1);
    }

    int inicio = 0, fin = 0;
    for (int i = 0; i < num_actividades; i++) {
        pendientes[i] = actividades[i].pendientes;
        if (pendientes[i] == 0) cola[fin++] = i;
    }

    while (inicio < fin) {
        int actual = cola[inicio++];
        for (int h = 0; h < actividades[actual].num_hijos; h++) {
            int hijo = actividades[actual].hijos[h];
            pendientes[hijo]--;
            if (pendientes[hijo] == 0) cola[fin++] = hijo;
        }
    }

    int resultado = 0;
    if (fin < num_actividades) {
        fprintf(stderr, "El plan tiene un ciclo entre las actividades:");
        for (int i = 0; i < num_actividades; i++) {
            if (pendientes[i] > 0) fprintf(stderr, " %s", actividades[i].id);
        }
        fprintf(stderr, "\n");
        resultado = -1;
    }

    free(pendientes);
    free(cola);
    return resultado;
}

/* Milisegundos transcurridos desde que partió la simulación. */
long tiempo_actual(void) {
    struct timespec ahora;
    clock_gettime(CLOCK_MONOTONIC, &ahora);
    return (ahora.tv_sec - inicio_simulacion.tv_sec) * 1000
         + (ahora.tv_nsec - inicio_simulacion.tv_nsec) / 1000000;
}

void dormir_ms(int ms) {
    struct timespec espera;
    espera.tv_sec = ms / 1000;
    espera.tv_nsec = (long)(ms % 1000) * 1000000;
    while (nanosleep(&espera, &espera) == -1 && errno == EINTR) {
        /* si una señal interrumpe el sueño, se sigue durmiendo lo que falta */
    }
}

/* Código que ejecuta el proceso hijo de la actividad i.
   Lee los insumos de sus dependencias desde "entrada", simula el trabajo
   y envía su propio insumo al padre por pipe_salida. */
void simular_actividad(int i, int entrada) {
    Actividad *a = &actividades[i];

    /* El hijo hereda las tuberías de las otras actividades en curso; no las usa */
    for (int j = 0; j < num_actividades; j++) {
        if (j != i && actividades[j].pid > 0 && actividades[j].estado_final == PENDIENTE) {
            close(actividades[j].pipe_salida[0]);
        }
    }
    close(a->pipe_salida[0]);

    char mensaje[MAX_MENSAJE];
    while (read(entrada, mensaje, MAX_MENSAJE) == MAX_MENSAJE) {
        mensaje[MAX_MENSAJE - 1] = '\0';
        printf("[%6ld ms] %s recibe: \"%s\"\n", tiempo_actual(), a->id, mensaje);
    }
    close(entrada);
    fflush(stdout);

    /* Cada hijo usa su propia semilla para decidir si falla */
    srand(time(NULL) ^ getpid());
    if (rand() % 100 < prob_fallo) {
        dormir_ms(a->tiempo / 2);  /* falla a mitad de camino, sin entregar su insumo */
        _exit(1);
    }

    dormir_ms(a->tiempo);

    memset(mensaje, 0, sizeof(mensaje));
    snprintf(mensaje, sizeof(mensaje), "insumo de %.100s listo", a->nombre);
    if (write(a->pipe_salida[1], mensaje, MAX_MENSAJE) != MAX_MENSAJE) _exit(1);
    close(a->pipe_salida[1]);
    _exit(0);
}

/* Crea el proceso de la actividad i y le envía los insumos de sus dependencias.
   Devuelve 0 si se pudo, -1 si falló pipe o fork. */
int lanzar_actividad(int i) {
    Actividad *a = &actividades[i];
    int entrada[2];  /* tubería padre -> hijo con los insumos de las dependencias */

    if (pipe(a->pipe_salida) == -1) {
        perror("pipe");
        return -1;
    }
    if (pipe(entrada) == -1) {
        perror("pipe");
        close(a->pipe_salida[0]);
        close(a->pipe_salida[1]);
        return -1;
    }

    fflush(stdout);  /* para que el hijo no herede texto sin imprimir */
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        close(a->pipe_salida[0]);
        close(a->pipe_salida[1]);
        close(entrada[0]);
        close(entrada[1]);
        return -1;
    }
    if (pid == 0) {
        close(entrada[1]);
        simular_actividad(i, entrada[0]);
    }

    close(entrada[0]);
    close(a->pipe_salida[1]);
    a->pid = pid;
    printf("[%6ld ms] inicia  %s (%s, %d ms, pid %d)\n",
           tiempo_actual(), a->id, a->nombre, a->tiempo, (int)pid);
    fflush(stdout);  /* así "inicia" aparece antes de lo que imprima el hijo */

    /* Reenviar al hijo el insumo de cada dependencia. Se cierra antes de
       volver para que ningún otro hijo herede este extremo de escritura. */
    for (int j = 0; j < a->num_padres; j++) {
        Actividad *p = &actividades[a->padres[j]];
        if (write(entrada[1], p->mensaje, MAX_MENSAJE) != MAX_MENSAJE) break;
    }
    close(entrada[1]);
    return 0;
}

int buscar_por_pid(pid_t pid) {
    for (int i = 0; i < num_actividades; i++) {
        if (actividades[i].pid == pid) return i;
    }
    return -1;
}

/* Marca como abortadas todas las actividades que dependen (directa o
   indirectamente) de la actividad i. */
void abortar_rama(int i, int *terminadas) {
    Actividad *a = &actividades[i];
    for (int h = 0; h < a->num_hijos; h++) {
        int hijo = a->hijos[h];
        if (actividades[hijo].estado_final == PENDIENTE) {
            actividades[hijo].estado_final = ABORTADA;
            (*terminadas)++;
            printf("[%6ld ms] [ABORTADA] Actividad %s cancelada por dependencia insatisfecha\n",
                   tiempo_actual(), actividades[hijo].id);
            abortar_rama(hijo, terminadas);
        }
    }
}

void ejecutar_plan(int k) {
    int *cola = malloc(num_actividades * sizeof(int));
    if (num_actividades > 0 && cola == NULL) {
        perror("malloc");
        exit(1);
    }
    int inicio = 0, fin = 0;
    for (int i = 0; i < num_actividades; i++) {
        if (actividades[i].pendientes == 0) cola[fin++] = i;
    }

    int corriendo = 0;
    int terminadas = 0;  /* completadas + fallidas + abortadas */
    clock_gettime(CLOCK_MONOTONIC, &inicio_simulacion);

    while (terminadas < num_actividades) {
        /* Fiscalización de la Seremi */
        if (g_seremi) {
            printf("\n[%6ld ms] [SEREMI] ¡Fiscalizacion! Abortando actividades en ejecucion...\n",
                   tiempo_actual());
            for (int i = 0; i < num_actividades; i++) {
                Actividad *a = &actividades[i];
                if (a->pid > 0 && a->estado_final == PENDIENTE) {
                    kill(a->pid, SIGTERM);
                    close(a->pipe_salida[0]);
                    a->estado_final = ABORTADA;
                }
            }
            while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {}
            printf("[%6ld ms] [SEREMI] Clausura completada.\n", tiempo_actual());
            break;
        }

        /* Lanzar hasta completar límite K */
        while (inicio < fin && corriendo < k) {
            if (lanzar_actividad(cola[inicio]) != 0) break;
            inicio++;
            corriendo++;
        }

        if (corriendo == 0) {
            if (inicio == fin && terminadas < num_actividades) {
                fprintf(stderr, "No hay mas actividades listas para ejecutar\n");
            }
            break;
        }

        int estado;
        pid_t pid = waitpid(-1, &estado, 0);
        if (pid == -1) {
            if (errno == EINTR) continue;
            perror("waitpid");
            break;
        }

        int i = buscar_por_pid(pid);
        if (i == -1) continue;
        corriendo--;
        terminadas++;
        Actividad *a = &actividades[i];

        /* La actividad cuenta como completada solo si terminó bien y entregó su insumo */
        int ok = WIFEXITED(estado) && WEXITSTATUS(estado) == 0;
        if (ok) {
            ssize_t bytes;
            do {
                bytes = read(a->pipe_salida[0], a->mensaje, MAX_MENSAJE);
            } while (bytes == -1 && errno == EINTR);
            ok = bytes == MAX_MENSAJE;
            a->mensaje[MAX_MENSAJE - 1] = '\0';
        }
        close(a->pipe_salida[0]);

        if (ok) {
            a->estado_final = COMPLETADA;
            printf("[%6ld ms] termina %s (%s) -> \"%s\"\n", tiempo_actual(), a->id, a->nombre, a->mensaje);

            for (int h = 0; h < a->num_hijos; h++) {
                int hijo = a->hijos[h];
                if (actividades[hijo].estado_final == PENDIENTE) {
                    actividades[hijo].pendientes--;
                    if (actividades[hijo].pendientes == 0) cola[fin++] = hijo;
                }
            }
        } else {
            a->estado_final = FALLIDA;
            printf("[%6ld ms] [FALLO] Actividad %s finalizo de forma anormal\n", tiempo_actual(), a->id);
            abortar_rama(i, &terminadas);
        }
    }

    int cuenta[4] = {0, 0, 0, 0};
    for (int i = 0; i < num_actividades; i++) cuenta[actividades[i].estado_final]++;
    printf("Simulacion terminada en %ld ms: %d completadas, %d fallidas, %d abortadas, %d sin ejecutar (total %d)\n",
           tiempo_actual(), cuenta[COMPLETADA], cuenta[FALLIDA], cuenta[ABORTADA], cuenta[PENDIENTE],
           num_actividades);
    free(cola);
}

void liberar_plan(void) {
    for (int i = 0; i < num_actividades; i++) {
        for (int j = 0; j < actividades[i].num_deps; j++) free(actividades[i].deps[j]);
        free(actividades[i].deps);
        free(actividades[i].hijos);
        free(actividades[i].padres);
    }
    free(actividades);
}

int main(int argc, char *argv[]) {
    if (argc != 3 && argc != 4) {
        fprintf(stderr, "Uso: %s plan.txt K [prob_fallo]\n", argv[0]);
        return 1;
    }

    char *fin;
    long k = strtol(argv[2], &fin, 10);
    if (*fin != '\0' || k <= 0) {
        fprintf(stderr, "K debe ser un entero mayor que 0\n");
        return 1;
    }

    if (argc == 4) {
        long p = strtol(argv[3], &fin, 10);
        if (*fin != '\0' || p < 0 || p > 100) {
            fprintf(stderr, "prob_fallo debe ser un entero entre 0 y 100\n");
            return 1;
        }
        prob_fallo = (int)p;
    }

    srand(time(NULL) ^ getpid());

    /* Configurar manejo de SIGINT */
    struct sigaction sa;
    sa.sa_handler = manejador_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    /* Si un hijo muere antes de leer sus insumos, write devuelve error en vez de matar al padre */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    if (leer_plan(argv[1]) != 0 || armar_grafo() != 0 || revisar_ciclos() != 0) {
        liberar_plan();
        return 1;
    }

    printf("Plan cargado: %d actividades, K = %ld, probabilidad de fallo = %d%%\n",
           num_actividades, k, prob_fallo);
    ejecutar_plan((int)k);
    liberar_plan();
    return 0;
}
