#define _POSIX_C_SOURCE 200809L


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

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
} Actividad;

Actividad *actividades = NULL;
int num_actividades = 0;
int capacidad = 0;

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

void mostrar_plan(void) {
    printf("Plan con %d actividades:\n", num_actividades);
    for (int i = 0; i < num_actividades; i++) {
        Actividad *a = &actividades[i];
        printf("  [%s] %s (%d ms) deps:", a->id, a->nombre, a->tiempo);
        for (int j = 0; j < a->num_deps; j++) printf(" %s", a->deps[j]);
        printf("\n");
    }
}

void liberar_plan(void) {
    for (int i = 0; i < num_actividades; i++) {
        for (int j = 0; j < actividades[i].num_deps; j++) free(actividades[i].deps[j]);
        free(actividades[i].deps);
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

    if (leer_plan(argv[1]) != 0) {
        liberar_plan();
        return 1;
    }

    mostrar_plan();
    liberar_plan();
    return 0;
}
