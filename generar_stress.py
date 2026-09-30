#!/usr/bin/env python3
"""
Genera un plan.txt de estres con N actividades formando un DAG valido
(sin ciclos): cada actividad solo puede depender de actividades con un
ID numerico menor, lo que garantiza que el grafo sea aciclico.

Uso: python3 generar_stress.py [N] [salida]
"""
import random
import sys

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
salida = sys.argv[2] if len(sys.argv) > 2 else "plan_stress.txt"

with open(salida, "w") as f:
    for i in range(1, N + 1):
        deps = []
        if i > 1:
            num_deps = random.randint(0, min(3, i - 1))
            deps = random.sample(range(1, i), num_deps)
        deps_str = ", ".join(str(d) for d in deps)
        tiempo = random.randint(10, 100)  # ms cortos para que la prueba no demore horas
        f.write(f"{i} : actividad_{i} : {tiempo} : {deps_str}\n")

print(f"{salida} generado con {N} actividades")
