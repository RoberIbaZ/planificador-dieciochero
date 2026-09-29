#!/usr/bin/env python3
# Genera un plan.txt aleatorio (siempre es un DAG) para las pruebas de estrés.
# Uso: python3 pruebas/generar_plan.py N [max_deps] [tiempo_min] [tiempo_max] > plan_grande.txt
# Cada actividad solo depende de actividades anteriores, así nunca hay ciclos.
# Si tiempo_min es -1, el tiempo se deja vacío para que el planificador lo asigne al azar.
import random
import sys

if len(sys.argv) < 2:
    print("Uso: python3 generar_plan.py N [max_deps] [tiempo_min] [tiempo_max]", file=sys.stderr)
    sys.exit(1)

n = int(sys.argv[1])
max_deps = int(sys.argv[2]) if len(sys.argv) > 2 else 3
tiempo_min = int(sys.argv[3]) if len(sys.argv) > 3 else 1
tiempo_max = int(sys.argv[4]) if len(sys.argv) > 4 else 5

for i in range(n):
    deps = random.sample(range(i), min(i, random.randint(0, max_deps)))
    tiempo = "" if tiempo_min == -1 else str(random.randint(tiempo_min, tiempo_max))
    print(f"{i} : actividad_{i} : {tiempo} : " + ", ".join(map(str, deps)))
