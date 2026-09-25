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

#define MAX_LINEA 8192
#define MAX_ID 64
#define MAX_NOMBRE 128
#define TIEMPO_MIN 100
#define TIEMPO_MAX 5000

typedef struct {
    char id[MAX_ID];
    char nombre[MAX_NOMBRE];
    int tiempo;        /* en milisegundos */
    int num_deps;
    char **deps;       /* IDs de las dependencias, tal como vienen en el archivo */
    int *hijos;        /* posiciones de las actividades que dependen de esta */
    int num_hijos;
    int pendientes;    /* dependencias que aún no terminan */
    pid_t pid;         /* pid del proceso que la simula (0 si no ha partido) */
} Actividad;

Actividad *actividades = NULL;
int num_actividades = 0;
int capacidad = 0;

struct timespec inicio_simulacion;

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

void agregar_hijo(Actividad *a, int hijo) {
    a->hijos = realloc(a->hijos, (a->num_hijos + 1) * sizeof(int));
    if (a->hijos == NULL) {
        perror("realloc");
        exit(1);
    }
    a->hijos[a->num_hijos] = hijo;
    a->num_hijos++;
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

            agregar_hijo(&actividades[d], i);
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

/* Código que ejecuta el proceso hijo: duerme el tiempo de la actividad y termina. */
void simular_actividad(Actividad *a) {
    struct timespec espera;
    espera.tv_sec = a->tiempo / 1000;
    espera.tv_nsec = (long)(a->tiempo % 1000) * 1000000;
    while (nanosleep(&espera, &espera) == -1 && errno == EINTR) {
        /* si una señal interrumpe el sueño, se sigue durmiendo lo que falta */
    }
    _exit(0);
}

/* Crea el proceso de la actividad i. Devuelve 0 si se pudo, -1 si fork falló. */
int lanzar_actividad(int i) {
    Actividad *a = &actividades[i];
    fflush(stdout);  /* para que el hijo no herede texto sin imprimir */
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        simular_actividad(a);
    }
    a->pid = pid;
    printf("[%6ld ms] inicia  %s (%s, %d ms, pid %d)\n",
           tiempo_actual(), a->id, a->nombre, a->tiempo, (int)pid);
    return 0;
}

int buscar_por_pid(pid_t pid) {
    for (int i = 0; i < num_actividades; i++) {
        if (actividades[i].pid == pid) return i;
    }
    return -1;
}

/* Ejecuta el plan: lanza las actividades listas y espera a que terminen.
   Cuando una termina, se descuenta de los pendientes de sus hijos y los
   que quedan en 0 pasan a la cola de listas. */
void ejecutar_plan(void) {
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
    int terminadas = 0;
    clock_gettime(CLOCK_MONOTONIC, &inicio_simulacion);

    while (terminadas < num_actividades) {
        while (inicio < fin) {
            if (lanzar_actividad(cola[inicio]) != 0) break;
            inicio++;
            corriendo++;
        }

        if (corriendo == 0) {
            fprintf(stderr, "No se pudo crear ningun proceso, se detiene la simulacion\n");
            break;
        }

        /* waitpid bloquea al padre hasta que termine algún hijo (sin busy-waiting) */
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
        printf("[%6ld ms] termina %s (%s)\n", tiempo_actual(), a->id, a->nombre);

        for (int h = 0; h < a->num_hijos; h++) {
            int hijo = a->hijos[h];
            actividades[hijo].pendientes--;
            if (actividades[hijo].pendientes == 0) cola[fin++] = hijo;
        }
    }

    printf("Simulacion terminada: %d de %d actividades completadas en %ld ms\n",
           terminadas, num_actividades, tiempo_actual());
    free(cola);
}

void liberar_plan(void) {
    for (int i = 0; i < num_actividades; i++) {
        for (int j = 0; j < actividades[i].num_deps; j++) free(actividades[i].deps[j]);
        free(actividades[i].deps);
        free(actividades[i].hijos);
    }
    free(actividades);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s plan.txt K\n", argv[0]);
        return 1;
    }

    char *fin;
    long k = strtol(argv[2], &fin, 10);
    if (*fin != '\0' || k <= 0) {
        fprintf(stderr, "K debe ser un entero mayor que 0\n");
        return 1;
    }

    srand(time(NULL) ^ getpid());

    if (leer_plan(argv[1]) != 0 || armar_grafo() != 0 || revisar_ciclos() != 0) {
        liberar_plan();
        return 1;
    }

    printf("Plan cargado: %d actividades, K = %ld\n", num_actividades, k);
    ejecutar_plan();
    liberar_plan();
    return 0;
}
