# Planificador Dieciochero

Tarea 1 — Sistemas Operativos (UDP). Procesos, tuberías y señales.

Simulador que lee un plan de actividades descrito como un grafo acíclico dirigido (DAG) y
ejecuta cada actividad en un proceso distinto, respetando las dependencias, un límite de K
procesos simultáneos, el paso de mensajes por tuberías, el aislamiento de fallos y la
interrupción con Ctrl+C.

## Integrantes

- Roberto Ibaceta Cisternas — [RoberIbaZ](https://github.com/RoberIbaZ)
- Gabriel Varas — [gabovrs](https://github.com/gabovrs)

## Compilación

Con el Makefile:

```
make
```

O con las flags exigidas:

```
gcc -Wall -Wextra -std=c17 -lpthread -o planificador planificador.c
```

## Ejecución

```
./planificador plan.txt K [prob_fallo]
```

- `plan.txt`: archivo con el plan de actividades.
- `K`: máximo de procesos de actividades que pueden existir al mismo tiempo (entero mayor que 0).
- `prob_fallo`: porcentaje de 0 a 100 de probabilidad de que cada actividad falle
  internamente. Si no se indica, vale 0, es decir, ninguna actividad falla por sí sola.

Ejemplos:

```
./planificador plan.txt 3                      # plan de ejemplo con 3 procesos a la vez
./planificador pruebas/fallo.txt 3 50          # la mitad de las actividades falla
./planificador pruebas/plan_10000.txt 50       # prueba de estrés con 10000 actividades
```

Presionar **Ctrl+C** durante la ejecución simula la inspección de la Seremi: se abortan todas
las actividades y el programa termina ordenadamente.

### Formato de `plan.txt`

```
ID_Actividad : Nombre_Actividad : tiempo_ms : Dependencia1, Dependencia2, ...
```

- El ID es alfanumérico y debe ser único.
- Si el tiempo viene vacío, se asigna uno al azar entre 100 y 5000 ms.
- Las dependencias se pueden escribir con o sin corchetes (`1, 2` o `[1, 2]`).
- Se ignoran las líneas vacías y las que empiezan con `#`.

El programa rechaza el plan (y no ejecuta nada) si una línea está mal formada, si un ID se
repite, si el tiempo no es un número, si una actividad depende de un ID que no existe o de sí
misma, o si las dependencias forman un ciclo.

### Salida

Cada evento se imprime con los milisegundos transcurridos desde el inicio:

```
[  1202 ms] inicia  4 (asar_longaniza, 800 ms, pid 46493)
[  1204 ms] 4 recibe: "insumo de prender_carbon listo"
[  1204 ms] 4 recibe: "insumo de comprar_carne listo"
[  2005 ms] termina 4 (asar_longaniza) -> "insumo de asar_longaniza listo"
```

Al final se muestra un resumen con cuántas actividades se completaron, fallaron, se abortaron
o no alcanzaron a ejecutarse.

## Funciones implementadas

Todo el código está en `planificador.c`.

**Lectura del plan**

| Función | Qué hace |
|---|---|
| `leer_plan` | Abre el archivo y lo recorre línea por línea con `fgets`. |
| `leer_linea` | Separa una línea en sus cuatro campos, valida el ID y el tiempo, y guarda la actividad. |
| `siguiente_campo` | Corta el texto en el siguiente `:`. Se usa en vez de `strtok` porque `strtok` se salta los campos vacíos (y el tiempo puede venir vacío). |
| `recortar` | Quita los espacios al inicio y al final de un texto. |
| `leer_dependencias` / `agregar_dependencia` | Separan la lista de dependencias por comas y guardan cada ID. |
| `buscar_actividad` | Devuelve la posición de la actividad con un ID dado. |

**Modelado del DAG**

| Función | Qué hace |
|---|---|
| `armar_grafo` | Convierte los IDs de las dependencias en aristas: llena la lista de hijos (quienes dependen de la actividad), la de padres (de quienes depende) y el contador `pendientes`. Rechaza dependencias inexistentes o a sí misma, y cuenta una sola vez las repetidas. |
| `revisar_ciclos` | Verifica que el grafo no tenga ciclos con el algoritmo de Kahn. |
| `agregar_entero` | Agrega un valor a una lista dinámica de enteros (se usa para hijos y padres). |

**Ejecución**

| Función | Qué hace |
|---|---|
| `ejecutar_plan` | Ciclo principal del padre: lanza actividades listas mientras haya menos de K corriendo, espera con `waitpid` a que termine alguna, procesa su resultado y desbloquea a sus dependientes. |
| `lanzar_actividad` | Crea las dos tuberías de la actividad, hace `fork()` y le envía al hijo los insumos de sus dependencias. |
| `simular_actividad` | Código del proceso hijo: lee sus insumos, decide si falla, duerme el tiempo de la actividad y envía su insumo al padre. |
| `dormir_ms` | Duerme los milisegundos indicados con `nanosleep`, continuando si una señal lo interrumpe. |
| `abortar_rama` | Marca como abortadas, recursivamente, todas las actividades que dependen de una que falló. |
| `buscar_por_pid` | Encuentra la actividad asociada al pid que devolvió `waitpid`. |
| `manejador_sigint` | Manejador de SIGINT: solo activa la bandera `g_seremi`. |
| `manejador_sigchld` | Manejador vacío de SIGCHLD: solo sirve para despertar a `sigsuspend` cuando termina un hijo. |
| `tiempo_actual` | Milisegundos desde que partió la simulación, para los mensajes. |
| `liberar_plan` | Libera toda la memoria reservada. |

**Pruebas** (carpeta `pruebas/`)

| Archivo | Uso |
|---|---|
| `generar_plan.py` | Genera planes aleatorios de N actividades, siempre acíclicos: `python3 pruebas/generar_plan.py 10000 > plan_grande.txt`. |
| `plan_10000.txt` | Plan de 10000 actividades para la prueba de estrés. |
| `fallo.txt` | Plan para ver el aislamiento de errores. |
| `ciclo.txt`, `dependencia_inexistente.txt` | Planes inválidos que deben ser rechazados. |
| `sin_tiempo.txt` | Actividades sin tiempo y con dependencias entre corchetes. |

## Decisiones de diseño

**Un proceso por actividad, creado solo cuando está lista.** Cada actividad se simula en un
proceso hijo creado con `fork()`. No se crean todos los procesos al inicio: una actividad recién
se lanza cuando todas sus dependencias terminaron (su contador `pendientes` llega a 0) y hay
cupo. Así nunca existen más de K procesos de actividades, que es lo que pide el enunciado.

**Representación del DAG con arreglos.** Las actividades se guardan en un arreglo dinámico y el
grafo con listas de posiciones (hijos y padres) más un contador de dependencias pendientes. Es
lo más simple que permite, al terminar una actividad, saber en tiempo constante por arista qué
actividades se desbloquean. Los ciclos se detectan antes de ejecutar nada con el algoritmo de
Kahn, porque un ciclo haría que el planificador se quedara esperando para siempre.

**Control de concurrencia sin busy-waiting.** El padre lleva la cuenta de procesos corriendo.
Mientras sea menor que K y haya actividades listas en la cola, lanza una. Cuando no puede lanzar
más, recoge un hijo terminado con `waitpid(..., WNOHANG)` y, si no hay ninguno, se duerme con
`sigsuspend` hasta que llegue SIGCHLD (terminó un hijo) o SIGINT (Ctrl+C). No consume CPU
mientras espera: en una simulación de 6 segundos el programa usó unos 14 ms de CPU. Todo el estado del planificador lo maneja solo el proceso padre (los hijos no
comparten memoria con él), por lo que no hay variables compartidas que puedan generar race
conditions; la única comunicación es por tuberías y códigos de salida.

**Si el sistema no permite más procesos.** Con K muy grande, `fork` o `pipe` pueden fallar por
límites del sistema (procesos por usuario o archivos abiertos). En ese caso la actividad vuelve
a la cola, se muestra un único aviso y se reintenta cuando termine otra actividad. Solo si no hay
ningún proceso corriendo y aun así no se puede crear uno, la simulación se detiene.

**Paso de mensajes con dos tuberías por actividad.**

- *Hijo → padre*: al terminar, el hijo escribe su insumo (por ejemplo `"insumo de comprar_pan
  listo"`) y el padre lo guarda.
- *Padre → hijo*: al lanzar una actividad, el padre le reenvía los insumos de cada una de sus
  dependencias, y el hijo los lee antes de empezar a simular.

Se usa al padre como intermediario porque el dependiente todavía no existe cuando la dependencia
termina (se crea después, cuando está listo). Todos los mensajes miden 128 bytes fijos: así el
mensaje es acotado, cada `write` es atómico (menor que `PIPE_BUF`) y cada `read` obtiene un
mensaje completo. Solo hay tuberías abiertas para las actividades en curso, lo que mantiene bajo
el número de descriptores aun con 10000 actividades. Cada hijo cierra las tuberías que heredó de
las otras actividades, y el padre cierra el extremo de escritura de la tubería de entrada antes
de crear otro proceso, para que el hijo reciba EOF al terminar de leer. El padre ignora SIGPIPE
para no morir si un hijo termina antes de leer sus insumos.

**Fallos internos y aislamiento.** El enunciado no especifica cómo falla una actividad, por lo
que se agregó el argumento opcional `prob_fallo`. Cada hijo, con su propia semilla aleatoria,
decide si falla; si falla, termina a mitad de su tiempo con `exit(1)` y sin enviar su insumo. El
padre considera fallida cualquier actividad que no termine con código 0 o no entregue su insumo,
lo que incluye procesos muertos por una señal. Al detectar un fallo, recorre recursivamente sus
hijos y los marca como abortados; como nunca llegan a tener `pendientes` en 0, no se lanzan. Las
demás ramas siguen ejecutándose normalmente. El valor por defecto es 0 para que la invocación
`./planificador plan.txt K` del enunciado no aborte ramas por azar.

**Ctrl+C (SIGINT) sin race condition.** El manejador de SIGINT solo activa una bandera `volatile
sig_atomic_t`, porque dentro de un manejador solo es seguro hacer operaciones mínimas (no se
puede usar `printf` ni `malloc`). El ciclo principal revisa la bandera, envía SIGTERM a todos los
procesos en curso, los recoge con `waitpid` para no dejar zombies, muestra el resumen y termina.

Una versión ingenua (revisar la bandera y luego bloquearse en `waitpid`) tiene un race condition:
si el Ctrl+C llega justo entre la revisión y el `waitpid`, el padre se duerme sin haberlo visto y
no reacciona hasta que termine otro hijo. Para evitarlo, el padre mantiene SIGINT y SIGCHLD
bloqueadas con `sigprocmask` mientras trabaja, y solo las desbloquea dentro de `sigsuspend`, que
desbloquea y se duerme en un único paso atómico. Si una señal llega en cualquier otro momento
queda pendiente, y `sigsuspend` retorna de inmediato. En las pruebas, el programa reaccionó a
Ctrl+C enviados en momentos al azar en 3 ms o menos.

Los hijos limpian la máscara de señales heredada y conservan el manejador heredado, así que no
mueren por el Ctrl+C directamente: es el padre quien decide terminarlos, lo que hace el cierre
ordenado.

**Sin hilos.** Toda la concurrencia se logra con procesos (`fork`), tuberías (`pipe`) y señales;
no se usa ninguna función de `pthread`.

## Pruebas realizadas

- Plan de ejemplo con K = 1, 2 y 3: las actividades respetan sus dependencias.
- 10000 actividades con K = 4, 50, 500 y 10000: todas se completan, nunca hay más de K procesos
  a la vez, cada actividad recibe exactamente los insumos de sus dependencias y no quedan
  procesos zombie.
- Fallos con `prob_fallo` y matando procesos a mano con `kill -9`: solo se abortan las ramas
  dependientes.
- Ctrl+C en medio de la ejecución, también enviado en momentos al azar: se abortan todas las
  actividades en 3 ms o menos y sin dejar procesos vivos.
- Planes inválidos (ciclos, dependencias inexistentes, tiempos no numéricos): se rechazan con un
  mensaje de error.
