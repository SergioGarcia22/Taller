# Editor de Texto en C - Taller 1
Proyecto para la asignatura de Sistemas Operativos. Desarrollado por Sergio Daniel Garcia y Julian David Escobar.

## Funcionalidades
- Procesador de texto en consola con arquitectura multihilo.
- Comunicación entre procesos mediante tuberías nombradas (FIFOs).
- Persistencia de datos con auto-guardado en disco.
- Interfaz dinámica con configuración personalizable.

## Cómo ejecutar
1. Compilar: `gcc editor.c -o editor -lpthread`
2. Ejecutar: `./editor`