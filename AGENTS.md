# AGENTS.md - SRTPB (Reproducción en Tiempo Real de SeedLink)

## Descripción del Proyecto
**SRTPB** es una herramienta en Python y C diseñada para simular la reproducción en tiempo real de datos sísmicos archivados, reenviándolos a un servidor SeedLink / Datalink (como Iris Ringserver).
Lee formas de onda sísmicas, las divide en pequeños intervalos temporales (`PacketTimeInterval`) y las transmite en tiempo real o acelerado, respetando la línea de tiempo natural y permitiendo configurar latencias de telemetría simuladas por estación.

## Arquitectura y Componentes Clave
- **`src/srtpb.py`**: CLI principal en Python. Conserva todas las opciones históricas y lanza al motor C `srtpbc`, que hace el corte, empaquetado y envío en tiempo real.
- **`src/srtpbc.c`**: Motor C de replay. Lee MiniSEED con `libmseed`, construye trazas continuas, corta por índice de muestra, empaqueta registros STEIM2 de 512 bytes y los envía por DataLink en lotes. Implementa `-m`, `-l`, `-f`, `-O`, `--test` y `-d`.
- **`src/srtpblib.i` y `src/srtpblib.c`**: Interfaz SWIG histórica (se sigue compilando por compatibilidad; el camino principal usa `srtpbc`).
- **`src/msrtslice.c`**: Utilidad en C puro para el corte eficiente de datos MiniSEED en ventanas temporales.
- **`ringserver/`**: Servidor Iris Ringserver integrado, junto con `slinktool` y `dalitool`.

## Compilación y Configuración
- **Requisitos previos**: `swig`, Python 3 (`numpy`, `obspy`), `gcc`, `make`.
- **Configuración inicial**: `./INSTALL` (extrae los tarballs empaquetados para `ringserver`, `dalitool`, `slinktool` y parchea los Makefiles con `-fPIC`).
- **Compilación**: `make` (compila `srtpbc`, las bibliotecas de C, el contenedor SWIG `_srtpblib.so`, `msrtslice` y coloca todo en `bin/`). Si se edita el motor C, ejecutar `make` o al menos `make -C src srtpbc && cp src/srtpbc bin/` para desplegarlo en `bin/`.

## Ejecución y Verificación
1. **Iniciar el Servidor**: Depende de un ambiente virtual llamado srtpb. Se deben iniciar tres terminales

- **terminal 1**: Levanta el ringserver
cd Dev/srtpb/
source srtpb/bin/activate
rm -rf ring/*
bin/ringserver bin/ring.conf

- **terminal 2**: ejecuta el envío de los datos, la opción -s ajusta la ventana del slice en segundos
cd Dev/srtpb/
source srtpb/bin/activate
python bin/srtpb.py -s 1.0 -m test2.mseed

- **terminal 3**: herramienta slinktool para monitorear el envío de los datos
cd Dev/srtpb/
source srtpb/bin/activate
bin/slinktool -p localhost:18000