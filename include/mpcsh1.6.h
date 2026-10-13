#ifndef MPCSH16_H
#define MPCSH16_H

#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>

/* Version definitions */
#define MPCSH_VERSION_MAJOR 1
#define MPCSH_VERSION_MINOR 6
#define MPCSH_VERSION_PATCH 0
#define MPCSH_VERSION_STRING "1.6.0"

/* Constants */
#define MPCSH_MAX_PIPELINE 64
#define MPCSH_MAX_WINDOWS 64
#define MPCSH_TASKBAR_HEIGHT 40
#define MPCSH_DEFAULT_WINDOW_WIDTH 800
#define MPCSH_DEFAULT_WINDOW_HEIGHT 600

/* Window states */
typedef enum {
    WINDOW_STATE_NORMAL,
    WINDOW_STATE_MINIMIZED,
    WINDOW_STATE_MAXIMIZED,
    WINDOW_STATE_FULLSCREEN,
    WINDOW_STATE_HIDDEN
} WindowState;

/* Window structure */
typedef struct _MpcshWindow {
    int id;                          /* wlid - window launch ID */
    GtkWindow *gtk_window;           /* GTK4 window handle */
    char *title;                     /* Window title */
    char *command;                   /* Command to execute */
    int x, y;                        /* Window position */
    int width, height;               /* Window size */
    WindowState state;               /* Window state */
    char *icon_path;                 /* Window icon path */
    GdkRGBA accent_color;            /* Window accent color */
    int blur_level;                  /* Glass blur level (0-100) */
    gboolean color_loop_enabled;     /* Color loop enabled */
    int color_loop_index;            /* Current color loop index */
    struct _MpcshWindow *parent;     /* Parent window */
    GSList *children;                /* Child windows */
    struct _MpcshWindow *next;       /* Next window in list */
} MpcshWindow;

/* Addon structure */
typedef struct _MpcshAddon {
    char *name;                      /* Addon name */
    char *version;                   /* Addon version */
    char *description;               /* Addon description */
    char *author;                    /* Addon author */
    GList *commands;                 /* List of commands */
    GList *windows;                  /* List of windows */
    struct _MpcshAddon *next;        /* Next addon */
} MpcshAddon;

/* Taskbar structure */
typedef struct _MpcshTaskbar {
    GtkWidget *gtk_widget;           /* GTK4 taskbar widget */
    GList *windows;                  /* Windows in taskbar */
    GtkWidget *start_menu;           /* Start menu button */
    GtkWidget *clock;                /* Clock widget */
    GtkWidget *notification_area;    /* Notification area */
} MpcshTaskbar;

/* Global variables */
extern MpcshWindow *windows_list;
extern MpcshAddon *addons_list;
extern MpcshTaskbar *taskbar;
extern int next_window_id;
extern GdkRGBA default_accent_color;

/* Function prototypes */
MpcshWindow* create_window(const char *title, const char *command, int width, int height, int x, int y);
void destroy_window(int window_id);
void show_window(int window_id);
void hide_window(int window_id);
void minimize_window(int window_id);
void maximize_window(int window_id);
void configure_window(int window_id, const char *title, const char *accent_color, int blur_level, gboolean color_loop);
void window_launch(const char *title, const char *command, int width, int height, int x, int y, gboolean persist, const char *accent_color, int blur_level);
void window_terminate(int window_id);
void window_pick(void);
void setup_taskbar(GtkWidget *parent_container);
void update_taskbar(void);
void load_addons(void);
void unload_addon(const char *name);
void addon_command(const char *addon_name, const char *command_name, const char *args);
void setup_configuration(void);
void save_configuration(const char *filename);
void load_configuration(const char *filename);
void set_accent_color(const char *color_name);
void toggle_color_loop(gboolean enabled);
void set_blur_level(int level);

#endif /* MPCSH16_H */
