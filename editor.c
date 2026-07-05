#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <signal.h>

// --- Variables Globales y Recursos Compartidos ---
pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
char original_filename[256];
FILE *temp_file;
volatile int running = 1;
volatile int ui_needs_update = 1;
// --- Estructura de Configuración ---
typedef struct {
    char default_filename[256];
    int header_color;
    int autosave_interval;
    int show_clock;
} Config;

Config app_config; // Instancia global

// --- Manejo de Señales (Para salir limpiamente con Ctrl+C) ---
void handle_sigint(int sig) {
   running = 0;
   printf("\033[?25h");     // Restaurar el cursor de la terminal
   printf("\033[2J\033[H"); // Limpiar pantalla al salir
   printf("Saliendo del editor y guardando estado final...\n");
   exit(0);
}

// --- Función Auxiliar: Dibujar la Interfaz ---
void draw_screen() {
   pthread_mutex_lock(&file_mutex);

   // Obtener la hora actual
   time_t t = time(NULL);
   struct tm tm = *localtime(&t);

   // --- Contar líneas del archivo temporal ---
   int line_count = 0;
   fseek(temp_file, 0, SEEK_SET);
   int c;
   int last_char_was_newline = 1;
   while ((c = fgetc(temp_file)) != EOF) {
       if (c == '\n') {
           line_count++;
           last_char_was_newline = 1;
       } else {
           last_char_was_newline = 0;
       }
   }
   if (!last_char_was_newline) line_count++;

   // Mover el cursor al inicio de la pantalla
   printf("\033[H");

   // --- Cabecera con color configurable ---
   printf("\033[%dm", app_config.header_color);   // <-- ahora sí usa el color del config

   if (app_config.show_clock) {
       printf(" Archivo: %-30s | Líneas: %-5d | Hora: %02d:%02d ",
              original_filename, line_count, tm.tm_hour, tm.tm_min);
   } else {
       printf(" Archivo: %-30s | Líneas: %-5d ",
              original_filename, line_count);
   }

   printf("\033[K");
   printf("\033[0m\n");

   // Leer el contenido del archivo temporal y mostrarlo
   fseek(temp_file, 0, SEEK_SET);
   char ch;
   int line = 2;

   while ((ch = fgetc(temp_file)) != EOF && line <= 24) {
       putchar(ch);
       if (ch == '\n') line++;
   }

   printf("\033[J");

   fflush(stdout);
   pthread_mutex_unlock(&file_mutex);
}
// --- Hilo 1: Interfaz y Reloj ---
void* ui_thread(void* arg) {
   // Limpiar la pantalla completa una vez al inicio y ocultar cursor
   printf("\033[2J\033[?25l");
   int last_min = -1;

   while (running) {
       time_t t = time(NULL);
       struct tm tm = *localtime(&t);

       // Actualizamos si cambió el minuto o si el hilo 3 nos avisó de nuevo texto
       if (tm.tm_min != last_min || ui_needs_update) {
           draw_screen();
           last_min = tm.tm_min;
           ui_needs_update = 0;
       }
       usleep(100000); // Dormir 100ms para no saturar la CPU
   }
   return NULL;
}

// --- Hilo 2: Auto-guardado (Persistencia) ---
void* autosave_thread(void* arg) {
   while (running) {
       sleep(10); // Despertar cada 10 segundos
     
       pthread_mutex_lock(&file_mutex);
       FILE *out = fopen(original_filename, "w");
       if (out != NULL) {
           fseek(temp_file, 0, SEEK_SET);
           char buf[1024];
           size_t bytes;
           // Volcar buffer temporal al archivo final
           while ((bytes = fread(buf, 1, sizeof(buf), temp_file)) > 0) {
               fwrite(buf, 1, bytes, out);
           }
           fclose(out);
       }
       pthread_mutex_unlock(&file_mutex);
   }
   return NULL;
}

// --- Hilo 3: Escucha del FIFO (Receptor de Texto) ---
void* fifo_thread(void* arg) {
   const char* fifo_path = "/tmp/editor_fifo";
 
   // Crear el FIFO. Ignoramos el error si ya existe.
   mkfifo(fifo_path, 0666);

   while (running) {
       // Esto bloqueará hasta que algún proceso abra el FIFO para escritura (ej. echo "Hola" > /tmp/editor_fifo)
       int fd = open(fifo_path, O_RDONLY);
       if (fd < 0) continue;

       char buffer[256];
       ssize_t bytes_read;
       while ((bytes_read = read(fd, buffer, sizeof(buffer) - 1)) > 0) {
           // Sección Crítica: Escribir en el archivo temporal
           pthread_mutex_lock(&file_mutex);
           fseek(temp_file, 0, SEEK_END);
           fwrite(buffer, 1, bytes_read, temp_file);
         
           // Notificamos a la interfaz que hay que repintar
           ui_needs_update = 1;
           pthread_mutex_unlock(&file_mutex);
       }
       close(fd);
   }
   return NULL;
}

// --- Configuración por defecto (a prueba de fallos) ---
void set_default_config() {
    strncpy(app_config.default_filename, "sin_nombre.txt", sizeof(app_config.default_filename) - 1);
    app_config.header_color = 7;      // video inverso, como el original
    app_config.autosave_interval = 10;
    app_config.show_clock = 1;
}

// --- Cargar configuración desde archivo ---
void load_config(const char* config_path) {
    set_default_config();

    FILE *fp = fopen(config_path, "r");
    if (fp == NULL) {
        printf("Aviso: no se encontró %s, usando configuración por defecto.\n", config_path);
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (line[0] == '\n' || line[0] == '#') continue;

        char key[128], value[128];
        if (sscanf(line, "%127[^=]=%127[^\n]", key, value) == 2) {

            // --- Limpieza de \r (retorno de carro, típico de archivos CRLF/Windows) ---
            size_t len = strlen(value);
            if (len > 0 && value[len - 1] == '\r') {
                value[len - 1] = '\0';
            }

            if (strcmp(key, "DEFAULT_FILENAME") == 0) {
                strncpy(app_config.default_filename, value, sizeof(app_config.default_filename) - 1);
            } else if (strcmp(key, "HEADER_COLOR") == 0) {
                app_config.header_color = atoi(value);
            } else if (strcmp(key, "AUTOSAVE_INTERVAL") == 0) {
                app_config.autosave_interval = atoi(value);
            } else if (strcmp(key, "SHOW_CLOCK") == 0) {
                app_config.show_clock = atoi(value);
            }
        }
    }

    fclose(fp);
}

// --- Programa Principal ---
int main(int argc, char* argv[]) {

    // Registrar manejador de señal para Ctrl+C
    signal(SIGINT, handle_sigint);

    // 1. Cargar configuración PRIMERO
    load_config("config.txt");

    // 2. Si no hay argumento, usar el nombre por defecto del config
    if (argc < 2) {
        strncpy(original_filename, app_config.default_filename, sizeof(original_filename) - 1);
        printf("No se especificó archivo. Usando: %s\n", original_filename);
    } else {
        strncpy(original_filename, argv[1], sizeof(original_filename) - 1);
    }

   // Función segura POSIX para archivos temporales (se borra al cerrar el programa)
   temp_file = tmpfile();
   if (!temp_file) {
       perror("Error creando el buffer temporal");
       return 1;
   }

   // Cargar el contenido existente del archivo al buffer temporal si el archivo ya existe
   FILE *in = fopen(original_filename, "r");
   if (in) {
       char buf[1024];
       size_t bytes;
       while ((bytes = fread(buf, 1, sizeof(buf), in)) > 0) {
           fwrite(buf, 1, bytes, temp_file);
       }
       fclose(in);
   }

   // Declarar e inicializar los 3 hilos requeridos
   pthread_t t_ui, t_save, t_fifo;
 
   pthread_create(&t_ui, NULL, ui_thread, NULL);
   pthread_create(&t_save, NULL, autosave_thread, NULL);
   pthread_create(&t_fifo, NULL, fifo_thread, NULL);

   // Esperamos a los hilos (aunque correrán hasta recibir SIGINT)
   pthread_join(t_ui, NULL);
   pthread_join(t_save, NULL);
   pthread_join(t_fifo, NULL);

   fclose(temp_file);
   return 0;
}

