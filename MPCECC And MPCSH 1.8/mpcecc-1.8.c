/*
 * MarekPC Extended Capabilities Console (MPCECC) 1.8
 * GTK4 + VTE terminal console with profiles, wallpaper,
 * tabs, clipboard shortcuts, context menu, and settings.
 *
 * Clipboard shortcuts (VTE-GTK4 does NOT bind these itself, so MPCECC
 * installs them as capture-phase window shortcuts):
 *   Ctrl+Shift+C = copy selected text
 *   Ctrl+Shift+V = paste
 *   Ctrl+V       = paste
 *
 * 1.8 fixes:
 *   - MPSP only unlinks genuine mpcimgcat temp files (tmp dir, prefix
 *     "mpcimgcat-", suffix ".png", regular file, owned by you).
 *   - Every tab has an id, exported to its shell as $MPCECC_TAB. Control
 *     requests may start with "@ID " so LAUNCH and MPSP land in the tab
 *     that sent them instead of whichever tab is selected.
 *   - MPSP overlay close button uses gtk_overlay_remove_overlay().
 *   - Control requests: up to 64 KB, read until newline, 2 s timeout.
 *
 * Fixes / features in this revision (1.8):
 *   - MULTI-WINDOW PixelWindows (wlid registry): multiple apps can live
 *     in the same tab as independent overlays; F3 star-map + cycling.
 *   - Window cycle is Ctrl+Shift+Tab (and Ctrl+Shift+` ) so the WM/DE
 *     never steals Alt+Tab or plain Ctrl+Tab. Shift reverses order.
 *   - Default framebuffer raised to 1440×900; in-tab frame cap raised
 *     to 1920×1200 so tool-heavy UIs (Krita, GIMP, …) are not cut off.
 *   - Aspect-correct CONTAIN fit kept (letterbox, never stretch).
 *   - windowlaunch / PixelWindow NEVER touches your real Firefox
 *     profile. Isolated MPCECCWLPROFILE under ~/.mozilla/firefox/.
 *   - Pixel window resizable with LXQt-style title bar
 *     (minimize / maximize / restore / close), clickable/typeable
 *     via xdotool / XTest.
 *   - MPCECC Pixel Sending Protocol (MPSP): mpcimgcat can push real
 *     images into the current tab as a floating, draggable overlay
 *     (title bar + close button). Falls back to Kitty / iTerm2 /
 *     aspect-corrected half-blocks when not running under MPCECC.
 *   - (carried from 1.6) GTK4 wallpaper surface conversion, capture-
 *     phase shortcuts, crash dialog + Restart Shell, clean close path.
 *   - GTK4 removed gdk_cairo_surface_create_from_pixbuf(); the wallpaper
 *     is now converted to a cairo surface by hand
 *     (mpcecc_surface_from_pixbuf).
 *   - Fixed shortcut accelerator typos (<Control><Shift>t / w).
 *   - Shortcut controller now runs in the CAPTURE phase, so Ctrl+Shift+T,
 *     Ctrl+Shift+W, Ctrl+, and Ctrl+? work while the VTE terminal has
 *     focus instead of being swallowed by it.
 *   - File encoding repaired (clean UTF-8: arrows, bullets, emoji).
 *   - Removed a bogus g_object_unref() of the old dropdown model, which
 *     would over-release it and could crash when profiles are edited.
 *   - Right-click context menu is now installed on every tab.
 *   - If the shell of a tab dies (crash, signal, non-zero exit),
 *     MPCECC shows a dialog: "<shell> crashed" with a
 *     "Restart Shell" button (respawns the shell in the same tab)
 *     and a "Close Tab" button.
 *   - Spawn errors are reported the same way.
 *   - Closing a tab (✕, Ctrl+Shift+W, crash-dialog "Close Tab") no
 *     longer triggers the crash dialog: the tab is marked "closing"
 *     and the child-exited handler is disconnected before removal,
 *     since VTE legitimately kills the shell with SIGHUP then.
 *
 * NOTE on VTE versions: this file targets the libvte-2.91-gtk4 API
 * where "child-exited" passes the wait status as a signal argument
 * and VteTerminalSpawnAsyncCallback is
 *   void (*)(VteTerminal *, int status, GError *, gpointer).
 *
 * Build on CachyOS / Arch (no pkg-config required):
 *
 *   gcc -O2 -pipe -std=gnu11 \\
 *       -I/usr/include/gtk-4.0 \\
 *       -I/usr/include/gdk-pixbuf-2.0 \\
 *       -I/usr/include/glib-2.0 \\
 *       -I/usr/lib/glib-2.0/include \\
 *       -I/usr/include/cairo \\
 *       -I/usr/include/pango-1.0 \\
 *       -I/usr/include/harfbuzz \\
 *       -I/usr/include/graphene-1.0 \\
 *       -I/usr/lib/graphene-1.0/include \\
 *       -I/usr/include/vte-2.91 -I/usr/include/vte-2.91-gtk4 \\
 *       -I/usr/include/libpng16 \\
 *       -I/usr/include/pixman-1 \\
 *       -I/usr/include/freetype2 \\
 *       -I/usr/include/fribidi \\
 *       -I/usr/include/libmount \\
 *       -I/usr/include/blkid \\
 *       -I/usr/include/sysprof-6 \\
 *       mpcecc-1.8.c -o mpcecc \\
 *       -lgtk-4 -lvte-2.91-gtk4 -lgdk_pixbuf-2.0 \\
 *       -lglib-2.0 -lgobject-2.0 -lpangocairo-1.0 -lpango-1.0 \\
 *       -lharfbuzz -lcairo -lgraphene-1.0 -lX11 -lXtst -ldl -lm
 *
 *   Shorter form that usually works on CachyOS/Arch:
 *
 *   gcc -O2 -pipe mpcecc-1.8.c -o mpcecc \\
 *       -lgtk-4 -lvte-2.91-gtk4 -lgdk_pixbuf-2.0 \\
 *       -lglib-2.0 -lgobject-2.0 -lX11 -lXtst -ldl -lm
 */

#include <gtk/gtk.h>
#include <math.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <vte/vte.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>
#include <dlfcn.h>

#define MPCECC_DEFAULT_WALLPAPER "/bin/mpcshresources/wallpaper.png"
/* 0 = contain (whole image visible, edges smeared into the empty space)
 * 1 = cover   (fills the window, crops the overflowing edges) */
#define MPCECC_WALLPAPER_COVER 0

/* How dark the smeared padding gets at the very edge of the window:
 * 0.0 = no darkening, 1.0 = solid near-black. */
#define MPCECC_PAD_DARKNESS 0.90

#define MPCECC_CONFIG_DIR "mpcecc"
#define MPCECC_PROFILE_FILE "profiles.conf"

typedef struct {
    GtkApplication *app;
    GtkWindow *window;
    GtkNotebook *tabs;
    GtkDropDown *profile_selector;
    GPtrArray *profiles;
    char *active_profile;
} MPCECC;

typedef struct {
    char *name;
    char *wallpaper;
    char *font;
    char *shell;
} MpcEccProfile;

typedef struct {
    MPCECC *ecc;
    GtkWidget *container;
    GtkWidget *picture;      /* wallpaper: custom-drawn, see below */
    GdkPixbuf *wallpaper_pixbuf;
    cairo_surface_t *wallpaper_surface;
    VteTerminal *terminal;
    GtkWidget *pixel_surface; /* GtkPicture overlay for in-tab FreeDOS-style pixels */
    char *profile_name;
    char *shell;
    /* TRUE once the user asked to close this tab: child-exited must
     * not be treated as a crash (VTE kills the shell with SIGHUP
     * when the terminal widget is destroyed). */
    gboolean closing;
    int id;   /* stable per-tab id, exported to the shell as $MPCECC_TAB */
} MpcEccTab;

static int mpcecc_next_tab_id = 1;

typedef struct {
    MPCECC *ecc;
    GtkDropDown *selector;
    GtkEntry *name;
    GtkEntry *wallpaper;
    GtkEntry *font;
    GtkEntry *shell;
} MpcEccSettings;

/* Integrated MPC PixelWindow: application framebuffer rendered inside VTE. */
static char **mpcecc_window_command = NULL;
static int mpcecc_window_command_argc = 0;
static gboolean mpcecc_window_terminal = FALSE;
static gboolean mpcecc_window_pixels = TRUE;
static const char *mpcecc_window_cwd = NULL;
static const char *mpcecc_window_title = NULL;
static const char *mpcecc_window_size = NULL;
static const char *mpcecc_window_fps = NULL;
static GPid mpcecc_pixel_xvfb_pid = 0;
static GPid mpcecc_pixel_app_pid = 0;
static guint mpcecc_pixel_ready_id = 0;
static guint mpcecc_pixel_capture_id = 0;
static VteTerminal *mpcecc_pixel_terminal = NULL;
static char *mpcecc_pixel_display = NULL;
static char *mpcecc_pixel_frame_path = NULL;
static int mpcecc_pixel_frame = 0;
static int mpcecc_pixel_fps = 15;
static int mpcecc_pixel_width = 1440;
static int mpcecc_pixel_height = 900;
static void mpcecc_open_command_window(MPCECC *ecc, char **command);
static MPCECC *mpcecc_main_ecc = NULL;
static GSocketService *mpcecc_control_service = NULL;
static GtkWidget *mpcecc_active_pixel_surface = NULL; /* the picture currently showing pixels */

/* ---- Multi-window registry (wlid = WindowLaunch ID) ---- */
typedef struct {
    int wlid;
    char *command;       /* argv[0] basename or full */
    char *title;
    char *display;       /* :N for this window's Xvfb */
    GPid xvfb_pid;
    GPid app_pid;
    char *frame_path;
    GtkWidget *frame;    /* in-tab overlay frame */
    GtkWidget *picture;  /* this window's picture inside its tab */
    MpcEccTab *tab;      /* tab that owns the overlay */
    guint capture_id;
    gboolean capture_busy;
    gboolean terminating;
    Display *input_display;       /* direct XTest connection for low-latency input */
    guint mouse_buttons;         /* held X buttons, so drags remain held */
    double pending_x, pending_y;
    guint motion_id;             /* coalesce pointer motion to one update per frame */
    int width, height, fps;
    guint32 color;       /* star color for picker */
} MpcEccWlid;

static GList *mpcecc_wlid_list = NULL;   /* of MpcEccWlid* */
static int mpcecc_next_wlid = 1;
static int mpcecc_focused_wlid = 0;

static MpcEccWlid *mpcecc_wlid_find(int id)
{
    for (GList *l = mpcecc_wlid_list; l; l = l->next) {
        MpcEccWlid *w = l->data;
        if (w->wlid == id) return w;
    }
    return NULL;
}

static MpcEccWlid *mpcecc_wlid_for_picture(GtkWidget *picture)
{
    for (GList *l = mpcecc_wlid_list; l; l = l->next) {
        MpcEccWlid *w = l->data;
        if (w->picture == picture) return w;
    }
    return NULL;
}

/* The XTest connection talks to this window's private Xvfb, which we SIGTERM
 * when the window closes. XCloseDisplay()/XFlush() on a dead server triggers
 * Xlib's fatal I/O error handler, which calls exit() and takes ALL of MPCECC
 * down. So never touch the Display again: close its socket and leak the small
 * Display struct instead. */
static void mpcecc_wlid_drop_input(MpcEccWlid *w)
{
    if (!w || !w->input_display) return;
    int fd = ConnectionNumber(w->input_display);
    w->input_display = NULL;
    if (fd >= 0) close(fd);
}

static void mpcecc_wlid_free(MpcEccWlid *w)
{
    if (!w) return;
    g_free(w->command);
    g_free(w->title);
    g_free(w->display);
    g_free(w->frame_path);
    if (w->motion_id) g_source_remove(w->motion_id);
    mpcecc_wlid_drop_input(w);
    g_free(w);
}

static gboolean mpcecc_pixel_delayed_capture_start(gpointer user_data);
static gboolean mpcecc_command_is_gui(char **command);
static gboolean mpcecc_pixel_on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data);
static void mpcecc_pixel_on_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
static void mpcecc_pixel_attach_wheel(GtkWidget *picture);
static void mpcecc_hide_pixels_on_current_tab(void);
static void mpcecc_hide_pixels_clicked(GtkButton *button, gpointer user_data);
static void mpcecc_pixel_win_minimize(GtkButton *button, gpointer user_data);
static void mpcecc_pixel_win_maximize(GtkButton *button, gpointer user_data);
static void mpcecc_pixel_win_restore(GtkButton *button, gpointer user_data);
static void mpcecc_pixel_win_apply_state(GtkWidget *frame);
static void mpcecc_pixel_drag_begin(GtkGestureDrag *g, double x, double y, gpointer u);
static void mpcecc_pixel_drag_update(GtkGestureDrag *g, double ox, double oy, gpointer u);
static void mpcecc_wlid_focus(MpcEccWlid *win);
static gboolean mpcecc_global_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                                  GdkModifierType state, gpointer user_data);

static char *
mpcecc_profile_path(void)
{
    static char path[4096];
    const char *home = g_get_home_dir();

    snprintf(path, sizeof(path), "%s/.config/%s/%s",
             home ? home : ".",
             MPCECC_CONFIG_DIR,
             MPCECC_PROFILE_FILE);

    return path;
}

static void
mpcecc_free_profile(MpcEccProfile *profile)
{
    g_free(profile->name);
    g_free(profile->wallpaper);
    g_free(profile->font);
    g_free(profile->shell);
    g_free(profile);
}

static GPtrArray *
mpcecc_load_profiles(void)
{
    GPtrArray *profiles = g_ptr_array_new_with_free_func(
        (GDestroyNotify)mpcecc_free_profile);

    MpcEccProfile *purple = g_new0(MpcEccProfile, 1);
    purple->name = g_strdup("MarekPC Purple");
    purple->wallpaper = g_strdup(MPCECC_DEFAULT_WALLPAPER);
    purple->font = g_strdup("FantasqueSansM Nerd Font Propo 11");
    purple->shell = g_strdup("mpcsh");
    g_ptr_array_add(profiles, purple);

    MpcEccProfile *violet = g_new0(MpcEccProfile, 1);
    violet->name = g_strdup("Deep Violet");
    violet->wallpaper = g_strdup(MPCECC_DEFAULT_WALLPAPER);
    violet->font = g_strdup("Monospace 11");
    violet->shell = g_strdup("fish");
    g_ptr_array_add(profiles, violet);

    GKeyFile *keyfile = g_key_file_new();

    if (g_key_file_load_from_file(keyfile,
        mpcecc_profile_path(),
                                  G_KEY_FILE_NONE,
                                  NULL)) {
        gsize group_count = 0;
    gchar **groups = g_key_file_get_groups(keyfile, &group_count);

    for (gsize i = 0; i < group_count; ++i) {
        MpcEccProfile *profile = g_new0(MpcEccProfile, 1);

        profile->name = g_strdup(groups[i]);
        profile->wallpaper = g_key_file_get_string(
            keyfile, groups[i], "Wallpaper", NULL);
        profile->font = g_key_file_get_string(
            keyfile, groups[i], "Font", NULL);
        profile->shell = g_key_file_get_string(
            keyfile, groups[i], "Shell", NULL);

        if (!profile->wallpaper || !*profile->wallpaper) {
            g_free(profile->wallpaper);
            profile->wallpaper = g_strdup(MPCECC_DEFAULT_WALLPAPER);
        }

        if (!profile->font || !*profile->font) {
            g_free(profile->font);
            profile->font = g_strdup("Monospace 11");
        }

        if (!profile->shell || !*profile->shell) {
            g_free(profile->shell);
            profile->shell = g_strdup("fish");
        }

        g_ptr_array_add(profiles, profile);
    }

    g_strfreev(groups);
                                  }

                                  g_key_file_unref(keyfile);
                                  return profiles;
}

static void
mpcecc_save_profiles(MPCECC *ecc)
{
    GKeyFile *keyfile = g_key_file_new();

    for (guint i = 0; i < ecc->profiles->len; ++i) {
        MpcEccProfile *profile = g_ptr_array_index(ecc->profiles, i);

        g_key_file_set_string(keyfile, profile->name,
                              "Wallpaper", profile->wallpaper);
        g_key_file_set_string(keyfile, profile->name,
                              "Font", profile->font);
        g_key_file_set_string(keyfile, profile->name,
                              "Shell", profile->shell);
    }

    char *directory = g_path_get_dirname(mpcecc_profile_path());
    g_mkdir_with_parents(directory, 0755);
    g_free(directory);

    g_key_file_save_to_file(keyfile, mpcecc_profile_path(), NULL);
    g_key_file_unref(keyfile);
}

static MpcEccProfile *
mpcecc_profile_by_name(MPCECC *ecc, const char *name)
{
    for (guint i = 0; i < ecc->profiles->len; ++i) {
        MpcEccProfile *profile = g_ptr_array_index(ecc->profiles, i);

        if (g_strcmp0(profile->name, name) == 0)
            return profile;
    }

    return g_ptr_array_index(ecc->profiles, 0);
}

static GtkStringList *
mpcecc_make_profile_model(MPCECC *ecc)
{
    GtkStringList *model = gtk_string_list_new(NULL);

    for (guint i = 0; i < ecc->profiles->len; ++i) {
        MpcEccProfile *profile = g_ptr_array_index(ecc->profiles, i);
        gtk_string_list_append(model, profile->name);
    }

    return model;
}

static void
mpcecc_refresh_profile_selector(MPCECC *ecc)
{
    GtkStringList *model = mpcecc_make_profile_model(ecc);

    /* gtk_drop_down_set_model() takes its own reference and releases
     * the old model by itself; we only drop our creation reference. */
    gtk_drop_down_set_model(ecc->profile_selector, G_LIST_MODEL(model));
    g_object_unref(model);

    guint selected_index = 0;

    for (guint i = 0; i < ecc->profiles->len; ++i) {
        MpcEccProfile *profile = g_ptr_array_index(ecc->profiles, i);

        if (g_strcmp0(profile->name, ecc->active_profile) == 0) {
            selected_index = i;
            break;
        }
    }

    gtk_drop_down_set_selected(ecc->profile_selector, selected_index);
}

/* Fill the rectangle (rx, ry, rw, rh) with a dark gradient that is fully
 * transparent at (gx0, gy0) and reaches MPCECC_PAD_DARKNESS at (gx1, gy1). */
static void
mpcecc_edge_fade(cairo_t *cr, double rx, double ry, double rw, double rh,
                 double gx0, double gy0, double gx1, double gy1)
{
    cairo_pattern_t *gradient =
    cairo_pattern_create_linear(gx0, gy0, gx1, gy1);

    cairo_pattern_add_color_stop_rgba(gradient, 0.0, 0.03, 0.0, 0.08, 0.0);
    cairo_pattern_add_color_stop_rgba(gradient, 1.0, 0.03, 0.0, 0.08,
                                      MPCECC_PAD_DARKNESS);

    cairo_set_source(cr, gradient);
    cairo_rectangle(cr, rx, ry, rw, rh);
    cairo_fill(cr);

    cairo_pattern_destroy(gradient);
}

static void
mpcecc_wallpaper_draw(GtkDrawingArea *area, cairo_t *cr,
                      int width, int height, gpointer user_data)
{
    MpcEccTab *tab = user_data;
    cairo_surface_t *surface = tab->wallpaper_surface;

    /* Fill the background with a deep violet so any area not
     * covered by the wallpaper still looks like MPCECC. */
    cairo_set_source_rgb(cr, 0.12, 0.05, 0.22);
    cairo_paint(cr);

    if (!surface)
        return;

    int img_w = cairo_image_surface_get_width(surface);
    int img_h = cairo_image_surface_get_height(surface);
    if (img_w <= 0 || img_h <= 0)
        return;

    /* Aspect-preserving scaling, centered. Fitting by width alone made
     * the image taller than the window on wide screens, so the top and
     * bottom got cropped. Now we pick the scale from BOTH axes:
     *   contain (default): whole image visible, bars filled with violet
     *   cover:             window fully covered, edges cropped */
    double scale_w = (double)width / (double)img_w;
    double scale_h = (double)height / (double)img_h;
    #if MPCECC_WALLPAPER_COVER
    double scale = scale_w > scale_h ? scale_w : scale_h;
    #else
    double scale = scale_w < scale_h ? scale_w : scale_h;
    #endif
    double off_x = ((double)width - img_w * scale) / 2.0;
    double off_y = ((double)height - img_h * scale) / 2.0;

    cairo_save(cr);
    cairo_translate(cr, off_x, off_y);
    cairo_scale(cr, scale, scale);
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    /* PAD smears the image's edge pixels outward, so the areas the
     * image doesn't reach get filled with its own edge colours
     * (magenta on the left, violet on the right) instead of a flat
     * bar. cairo_paint() fills the whole window, not just the image. */
    cairo_pattern_set_extend(cairo_get_source(cr), CAIRO_EXTEND_PAD);
    cairo_paint(cr);
    cairo_restore(cr);

    /* Darken ONLY the smeared areas, progressively: no change where the
     * real image ends, up to MPCECC_PAD_DARKNESS at the window edge. This
     * keeps the prompt readable on the bright magenta/violet padding. */
    double img_dw = img_w * scale;
    double img_dh = img_h * scale;

    if (off_x > 1.0) {
        /* left: clear at the image edge -> dark at x = 0 */
        mpcecc_edge_fade(cr, 0, 0, off_x, height,
                         off_x, 0, 0, 0);
        /* right: clear at the image edge -> dark at x = width */
        mpcecc_edge_fade(cr, off_x + img_dw, 0,
                         width - (off_x + img_dw), height,
                         off_x + img_dw, 0, width, 0);
    }

    if (off_y > 1.0) {
        /* top */
        mpcecc_edge_fade(cr, 0, 0, width, off_y,
                         0, off_y, 0, 0);
        /* bottom */
        mpcecc_edge_fade(cr, 0, off_y + img_dh,
                         width, height - (off_y + img_dh),
                         0, off_y + img_dh, 0, height);
    }
}

/* GTK4 removed gdk_cairo_surface_create_from_pixbuf(), so convert
 * the GdkPixbuf (RGB/RGBA, non-premultiplied, byte order R,G,B,A)
 * into a cairo ARGB32 surface (premultiplied, native-endian) by hand. */
static cairo_surface_t *
mpcecc_surface_from_pixbuf(GdkPixbuf *pixbuf)
{
    int width = gdk_pixbuf_get_width(pixbuf);
    int height = gdk_pixbuf_get_height(pixbuf);
    int channels = gdk_pixbuf_get_n_channels(pixbuf);
    int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
    gboolean has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
    const guchar *src = gdk_pixbuf_read_pixels(pixbuf);

    cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
        return surface;

    cairo_surface_flush(surface);

    guchar *dst = cairo_image_surface_get_data(surface);
    int dst_stride = cairo_image_surface_get_stride(surface);

    for (int y = 0; y < height; ++y) {
        const guchar *s = src + (gsize)y * src_stride;
        guint32 *d = (guint32 *)(dst + (gsize)y * dst_stride);

        for (int x = 0; x < width; ++x) {
            guint r = s[0], g = s[1], b = s[2];
            guint a = has_alpha ? s[3] : 255;

            if (a != 255) {
                r = (r * a + 127) / 255;
                g = (g * a + 127) / 255;
                b = (b * a + 127) / 255;
            }

            d[x] = (a << 24) | (r << 16) | (g << 8) | b;
            s += channels;
        }
    }

    cairo_surface_mark_dirty(surface);
    return surface;
}

static void
mpcecc_apply_profile(MpcEccTab *tab)
{
    MpcEccProfile *profile =
    mpcecc_profile_by_name(tab->ecc, tab->profile_name);

    /* Load the wallpaper and draw it ourselves: scaled smoothly
     * (linear-filtered) to the FULL window width, anchored to the
     * bottom of the window, aspect ratio preserved. This shows the
     * complete image at its maximum possible size — no letterbox
     * bars on the sides, no cropping, and it scales up and down. */
    if (tab->wallpaper_surface) {
        cairo_surface_destroy(tab->wallpaper_surface);
        tab->wallpaper_surface = NULL;
    }
    if (tab->wallpaper_pixbuf) {
        g_object_unref(tab->wallpaper_pixbuf);
        tab->wallpaper_pixbuf = NULL;
    }
    tab->wallpaper_pixbuf =
    gdk_pixbuf_new_from_file(profile->wallpaper, NULL);
    if (tab->wallpaper_pixbuf)
        tab->wallpaper_surface =
        mpcecc_surface_from_pixbuf(tab->wallpaper_pixbuf);

    gtk_widget_queue_draw(tab->picture);

    /* Font fallback: try the profile font first.  If Pango cannot
     * resolve a usable family (or the string is empty/garbage), fall
     * back to the hard-coded safe default. */
    static const char *const MPCECC_FALLBACK_FONT = "Monospace 11";

    const char *font_str = (profile->font && *profile->font)
    ? profile->font : MPCECC_FALLBACK_FONT;

    PangoFontDescription *font =
    pango_font_description_from_string(font_str);

    if (!pango_font_description_get_family(font)) {
        pango_font_description_free(font);
        font = pango_font_description_from_string(MPCECC_FALLBACK_FONT);
    }

    vte_terminal_set_font(tab->terminal, font);
    vte_terminal_set_clear_background(tab->terminal, FALSE);

    pango_font_description_free(font);

    /* Install the transparent-terminal CSS only once per process. */
    static gboolean css_installed = FALSE;

    if (!css_installed) {
        GtkCssProvider *provider = gtk_css_provider_new();
        gtk_css_provider_load_from_string(
            provider,
            ".mpcecc-terminal { background: transparent; }");

        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

        g_object_unref(provider);
        css_installed = TRUE;
    }
}

static void
mpcecc_free_tab(MpcEccTab *tab)
{
    /* A tab owns its embedded application sessions; closing the tab stops only those sessions. */
    for (GList *l = mpcecc_wlid_list, *next = NULL; l; l = next) {
        next = l->next;
        MpcEccWlid *win = l->data;
        if (win->tab != tab) continue;
        win->terminating = TRUE;
        if (win->capture_id) { g_source_remove(win->capture_id); win->capture_id = 0; }
        if (win->app_pid) kill((pid_t)win->app_pid, SIGTERM);
        if (win->xvfb_pid) kill((pid_t)win->xvfb_pid, SIGTERM);
        if (win->frame_path) unlink(win->frame_path);
        mpcecc_wlid_list = g_list_delete_link(mpcecc_wlid_list, l);
        if (mpcecc_focused_wlid == win->wlid) mpcecc_focused_wlid = 0;
        mpcecc_wlid_free(win);
    }
    g_free(tab->profile_name);
    g_free(tab->shell);
    if (tab->wallpaper_surface)
        cairo_surface_destroy(tab->wallpaper_surface);
    if (tab->wallpaper_pixbuf)
        g_object_unref(tab->wallpaper_pixbuf);
    g_free(tab);
}

/* ------------------------------------------------------------
 * Crash handling: "<shell> crashed" dialog + Restart Shell
 * ---------------------------------------------------------- */

enum {
    MPCECC_CRASH_RESPONSE_RESTART = 1,
    MPCECC_CRASH_RESPONSE_CLOSE   = 2,
    MPCECC_CRASH_RESPONSE_DISMISS = 3
};

static void
mpcecc_spawn_shell(VteTerminal *terminal, const char *shell, MpcEccTab *tab);

static void mpcecc_close_tab(MPCECC *ecc, MpcEccTab *tab);

/* Helper: verify that a tab pointer is still alive by walking the
 * notebook pages.  Returns the live tab or NULL if it has already
 * been destroyed. */
static MpcEccTab *
mpcecc_find_live_tab(MPCECC *ecc, MpcEccTab *candidate)
{
    if (!ecc || !candidate || !ecc->tabs)
        return NULL;
    int n = gtk_notebook_get_n_pages(ecc->tabs);
    for (int i = 0; i < n; ++i) {
        GtkWidget *page = gtk_notebook_get_nth_page(ecc->tabs, i);
        MpcEccTab *t = g_object_get_data(G_OBJECT(page), "mpcecc-tab");
        if (t == candidate)
            return t;
    }
    return NULL;
}

static void
mpcecc_crash_dialog_response(GtkButton *button, gpointer user_data)
{
    int response = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
                                                     "mpcecc-response"));
    GtkWidget *dialog = GTK_WIDGET(user_data);
    MpcEccTab *maybe_tab = g_object_get_data(G_OBJECT(dialog), "mpcecc-tab");
    MPCECC *ecc = g_object_get_data(G_OBJECT(dialog), "mpcecc-ecc");

    gtk_window_destroy(GTK_WINDOW(dialog));

    MpcEccTab *tab = mpcecc_find_live_tab(ecc, maybe_tab);
    if (!tab)
        return;

    if (response == MPCECC_CRASH_RESPONSE_RESTART) {
        mpcecc_spawn_shell(tab->terminal, tab->shell, tab);
        return;
    }

    if (response == MPCECC_CRASH_RESPONSE_CLOSE) {
        mpcecc_close_tab(tab->ecc, tab);
    }
}

static void
mpcecc_crash_dialog(MpcEccTab *tab, const char *detail)
{
    char title[512];

    snprintf(title, sizeof(title), "%s crashed",
             tab->shell ? tab->shell : "The shell");

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), tab->ecc->window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 160);

    g_object_set_data(G_OBJECT(dialog), "mpcecc-tab", tab);
    g_object_set_data(G_OBJECT(dialog), "mpcecc-ecc", tab->ecc);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);
    gtk_window_set_child(GTK_WINDOW(dialog), root);

    GtkWidget *heading = gtk_label_new(title);
    gtk_widget_add_css_class(heading, "title-4");
    gtk_box_append(GTK_BOX(root), heading);

    GtkWidget *message = gtk_label_new(detail);
    gtk_label_set_wrap(GTK_LABEL(message), TRUE);
    gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
    gtk_box_append(GTK_BOX(root), message);

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(button_box, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(root), button_box);

    struct {
        const char *label;
        int response;
    } buttons[] = {
        { "_Dismiss",        MPCECC_CRASH_RESPONSE_DISMISS },
        { "_Close Tab",      MPCECC_CRASH_RESPONSE_CLOSE   },
        { "_Restart Shell",  MPCECC_CRASH_RESPONSE_RESTART },
    };

    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        GtkWidget *button = gtk_button_new_with_mnemonic(buttons[i].label);
        g_object_set_data(G_OBJECT(button), "mpcecc-response",
                          GINT_TO_POINTER(buttons[i].response));
        g_signal_connect(button, "clicked",
                         G_CALLBACK(mpcecc_crash_dialog_response), dialog);
        gtk_box_append(GTK_BOX(button_box), button);
    }

    gtk_window_present(GTK_WINDOW(dialog));
}

static void
mpcecc_child_exited(VteTerminal *terminal, int status, gpointer user_data)
{
    MpcEccTab *tab = user_data;
    char detail[256];

    (void)terminal;

    /* The user closed this tab on purpose: VTE sends the shell a
     * SIGHUP while tearing the terminal down, which arrives here as
     * a non-zero status. That is not a crash. */
    if (tab->closing)
        return;

    /* Clean exit (user typed 'exit'): not a crash. */
    if (status == 0)
        return;

    /* This VTE passes the wait status directly in the signal.
     * A negative value means the process was killed by signal -status
     * on some builds, so handle both shapes. */
    if (status < 0) {
        snprintf(detail, sizeof(detail),
                 "The process was killed by signal %d.", -status);
    } else if (WIFSIGNALED(status)) {
        snprintf(detail, sizeof(detail),
                 "The process was killed by signal %d.",
                 WTERMSIG(status));
    } else if (WIFEXITED(status)) {
        snprintf(detail, sizeof(detail),
                 "The process exited with status %d.",
                 WEXITSTATUS(status));
    } else {
        snprintf(detail, sizeof(detail),
                 "The process terminated abnormally (status %d).", status);
    }

    mpcecc_crash_dialog(tab, detail);
}

static void
mpcecc_spawn_callback(VteTerminal *terminal, int status,
                      GError *error, gpointer user_data)
{
    MpcEccTab *tab = user_data;

    (void)terminal;
    (void)status;

    if (error) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Failed to start: %s", error->message);
        mpcecc_crash_dialog(tab, detail);
        g_error_free(error);
    }
}

/* Sanitize shell path: only allow absolute paths under /usr/bin, /bin,
 * /usr/local/bin, or a simple basename that will be searched via PATH.
 * Reject anything containing shell metacharacters or relative ".." tricks. */
static gboolean
mpcecc_shell_is_safe(const char *shell)
{
    if (!shell || !*shell)
        return FALSE;
    if (strpbrk(shell, ";&|<>`$(){}[]*?\\\"'") != NULL)
        return FALSE;
        if (strstr(shell, "..") != NULL)
            return FALSE;
    if (shell[0] == '/') {
        static const char *safe_prefixes[] = {
            "/usr/bin/", "/bin/", "/usr/local/bin/", NULL
        };
        for (int i = 0; safe_prefixes[i]; ++i) {
            if (g_str_has_prefix(shell, safe_prefixes[i]))
                return TRUE;
        }
        return FALSE;
    }
    if (strchr(shell, '/') == NULL)
        return TRUE;
    return FALSE;
}

static void
mpcecc_spawn_shell(VteTerminal *terminal, const char *shell, MpcEccTab *tab)
{
    if (!mpcecc_shell_is_safe(shell)) {
        char detail[512];
        snprintf(detail, sizeof(detail),
                 "Refused to spawn unsafe shell path: %s\\n"
                 "Only /usr/bin/*, /bin/*, /usr/local/bin/* or a plain basename are allowed.",
                 shell ? shell : "(null)");
        mpcecc_crash_dialog(tab, detail);
        return;
    }
    if (tab->closing || !terminal)
        return;

    const char *arguments[2] = { shell, NULL };
    gchar **environment = g_get_environ();

    static const char *drop[] = {
        "SSH_AUTH_SOCK", "GPG_AGENT_INFO", "PASSWORD", "PASSWD",
        "TOKEN", "SECRET", "API_KEY", "AWS_SECRET_ACCESS_KEY", NULL
    };
    for (int i = 0; drop[i]; ++i)
        environment = g_environ_unsetenv(environment, drop[i]);

    environment = g_environ_setenv(environment, "MPCECC", "1", TRUE);
    if (tab) {
        char idbuf[16];
        snprintf(idbuf, sizeof idbuf, "%d", tab->id);
        environment = g_environ_setenv(environment, "MPCECC_TAB", idbuf, TRUE);
    }

    vte_terminal_spawn_async(
        terminal,
        VTE_PTY_DEFAULT,
        NULL,
        (char **)arguments,
                             environment,
                             G_SPAWN_SEARCH_PATH,
                             NULL,
                             NULL,
                             NULL,
                             -1,
                             NULL,
                             mpcecc_spawn_callback,
                             tab);

    g_strfreev(environment);
}

static MpcEccTab *
mpcecc_current_tab(MPCECC *ecc)
{
    int page = gtk_notebook_get_current_page(ecc->tabs);

    if (page < 0)
        return NULL;

    GtkWidget *child = gtk_notebook_get_nth_page(ecc->tabs, page);

    if (!child)
        return NULL;

    return g_object_get_data(G_OBJECT(child), "mpcecc-tab");
}

/* Remove a tab the user explicitly closed. Marking the tab as
 * "closing" before removal prevents the child-exited handler from
 * popping up the crash dialog: when the page is removed, the VTE
 * terminal is destroyed and the shell dies with SIGHUP, which is
 * expected, not a crash. */
static void
mpcecc_close_tab(MPCECC *ecc, MpcEccTab *tab)
{
    if (!tab || tab->closing)
        return;

    tab->closing = TRUE;

    g_signal_handlers_disconnect_by_func(
        tab->terminal, G_CALLBACK(mpcecc_child_exited), tab);

    int page_count = gtk_notebook_get_n_pages(ecc->tabs);

    for (int i = 0; i < page_count; ++i) {
        if (gtk_notebook_get_nth_page(ecc->tabs, i) == tab->container) {
            gtk_notebook_remove_page(ecc->tabs, i);
            break;
        }
    }
}

static void
mpcecc_close_tab_button_clicked(GtkButton *button, gpointer user_data)
{
    MpcEccTab *tab = user_data;
    (void)button;
    mpcecc_close_tab(tab->ecc, tab);
}

static GtkWidget *
mpcecc_make_tab_label(MpcEccTab *tab, const char *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new(title);
    GtkWidget *close_button =
    gtk_button_new_from_icon_name("window-close-symbolic");

    gtk_button_set_has_frame(GTK_BUTTON(close_button), FALSE);

    g_signal_connect(close_button, "clicked",
                     G_CALLBACK(mpcecc_close_tab_button_clicked), tab);

    gtk_box_append(GTK_BOX(box), label);
    gtk_box_append(GTK_BOX(box), close_button);

    return box;
}

static void
mpcecc_install_context_menu(MpcEccTab *tab);

static MpcEccTab *
mpcecc_new_tab(MPCECC *ecc, const char *profile_name,
               const char *shell_override)
{
    MpcEccTab *tab = g_new0(MpcEccTab, 1);
    tab->id = mpcecc_next_tab_id++;

    tab->ecc = ecc;
    tab->profile_name = g_strdup(profile_name);

    MpcEccProfile *profile =
    mpcecc_profile_by_name(ecc, profile_name);

    tab->container = gtk_overlay_new();
    tab->picture = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(tab->picture),
                                   mpcecc_wallpaper_draw, tab, NULL);

    gtk_widget_set_hexpand(tab->picture, TRUE);
    gtk_widget_set_vexpand(tab->picture, TRUE);

    tab->terminal = VTE_TERMINAL(vte_terminal_new());

    gtk_widget_add_css_class(GTK_WIDGET(tab->terminal),
                             "mpcecc-terminal");
    gtk_widget_set_hexpand(GTK_WIDGET(tab->terminal), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(tab->terminal), TRUE);

    GtkScrolledWindow *scroll =
    GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());

    gtk_scrolled_window_set_child(scroll, GTK_WIDGET(tab->terminal));
    gtk_scrolled_window_set_policy(scroll,
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    gtk_overlay_set_child(GTK_OVERLAY(tab->container), tab->picture);
    gtk_overlay_add_overlay(GTK_OVERLAY(tab->container),
                            GTK_WIDGET(scroll));

    /* FreeDOS-style in-tab pixel surface (starts hidden) – drawn OVER the shell */
    /* FreeDOS-style in-tab LXQt-like window: titlebar with
     * min / max / restore / close, resizable modes, pixel body. */
    {
        GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_add_css_class(frame, "mpcecc-pixel-frame");
        gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
        gtk_widget_set_visible(frame, FALSE);
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state",
                          GINT_TO_POINTER(0)); /* NORMAL */

        /* —— title bar (LXQt order: icon+title … min max close) —— */
        GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        gtk_widget_add_css_class(titlebar, "mpcecc-pixel-titlebar");

        GtkWidget *icon = gtk_label_new("▣");
        gtk_widget_add_css_class(icon, "mpcecc-pixel-icon");

        GtkWidget *title = gtk_label_new("PixelWindow");
        gtk_widget_set_hexpand(title, TRUE);
        gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
        gtk_widget_add_css_class(title, "mpcecc-pixel-title");
        g_object_set_data(G_OBJECT(frame), "mpcecc-pixel-title", title);

        /* LXQt layout: minimize | maximize(toggle) | close  — no 4th button */
        GtkWidget *btn_min = gtk_button_new_with_label("–");
        GtkWidget *btn_max = gtk_button_new_with_label("□");
        GtkWidget *btn_close = gtk_button_new_with_label("×");
        gtk_widget_add_css_class(btn_min, "mpcecc-pixel-btn");
        gtk_widget_add_css_class(btn_max, "mpcecc-pixel-btn");
        gtk_widget_add_css_class(btn_close, "mpcecc-pixel-btn");
        gtk_widget_add_css_class(btn_close, "mpcecc-pixel-close");
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-maxbtn", btn_max);
        g_signal_connect(btn_min, "clicked", G_CALLBACK(mpcecc_pixel_win_minimize), frame);
        g_signal_connect(btn_max, "clicked", G_CALLBACK(mpcecc_pixel_win_maximize), frame);
        g_signal_connect(btn_close, "clicked", G_CALLBACK(mpcecc_hide_pixels_clicked), frame);

        gtk_box_append(GTK_BOX(titlebar), icon);
        gtk_box_append(GTK_BOX(titlebar), title);
        gtk_box_append(GTK_BOX(titlebar), btn_min);
        gtk_box_append(GTK_BOX(titlebar), btn_max);
        gtk_box_append(GTK_BOX(titlebar), btn_close);

        {
            GtkGestureDrag *drag = GTK_GESTURE_DRAG(gtk_gesture_drag_new());
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
            g_signal_connect(drag, "drag-begin", G_CALLBACK(mpcecc_pixel_drag_begin), frame);
            g_signal_connect(drag, "drag-update", G_CALLBACK(mpcecc_pixel_drag_update), frame);
            gtk_widget_add_controller(titlebar, GTK_EVENT_CONTROLLER(drag));
        }

        /* —— body: pixel surface —— */
        GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_hexpand(body, TRUE);
        gtk_widget_set_vexpand(body, TRUE);
        g_object_set_data(G_OBJECT(frame), "mpcecc-pixel-body", body);

        GtkWidget *picture = gtk_picture_new();
        /* CONTAIN: keep aspect ratio (no stretch). Letterbox bars are preferable
         * to distorted/cut-off UI in Krita, GIMP, browsers, etc. */
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_hexpand(picture, TRUE);
        gtk_widget_set_vexpand(picture, TRUE);
        gtk_widget_set_halign(picture, GTK_ALIGN_FILL);
        gtk_widget_set_valign(picture, GTK_ALIGN_FILL);
        gtk_widget_set_can_focus(picture, TRUE);
        gtk_widget_set_focusable(picture, TRUE);
        g_object_set_data(G_OBJECT(frame), "mpcecc-pixel-picture", picture);

        {
            GtkGestureClick *click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
            gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
            g_signal_connect(click, "pressed", G_CALLBACK(mpcecc_pixel_on_click), NULL);
            gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click)); mpcecc_pixel_attach_wheel(picture);
            GtkEventControllerKey *keys = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
            gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(keys), GTK_PHASE_CAPTURE);
            g_signal_connect(keys, "key-pressed", G_CALLBACK(mpcecc_pixel_on_key), NULL);
            gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(keys));
        }

        gtk_box_append(GTK_BOX(body), picture);
        gtk_box_append(GTK_BOX(frame), titlebar);
        gtk_box_append(GTK_BOX(frame), body);

        static gboolean pixel_css = FALSE;
        if (!pixel_css) {
            GtkCssProvider *css = gtk_css_provider_new();
            /* Square, barely-rounded, solid purple — no KDE chrome, no flashy gradients.
             * Titlebar is the drag handle; window stays trapped inside the tab. */
            gtk_css_provider_load_from_string(css,
                                              ".mpcecc-pixel-frame {"
                                              "  background-color: #160e22;"
                                              "  border: 1px solid #5a3d82;"
                                              "  border-radius: 3px;"
                                              "  box-shadow: 0 4px 16px alpha(black, 0.35);"
                                              "}"
                                              ".mpcecc-pixel-normal {"
                                              "  margin: 0;"
                                              "}"
                                              ".mpcecc-pixel-max {"
                                              "  margin: 0;"
                                              "  border-radius: 0;"
                                              "}"
                                              ".mpcecc-pixel-titlebar {"
                                              "  background-color: #2a1a40;"
                                              "  background-image: none;"
                                              "  min-height: 28px;"
                                              "  padding: 0 6px;"
                                              "  border-bottom: 1px solid #4a3568;"
                                              "  border-radius: 2px 2px 0 0;"
                                              "}"
                                              ".mpcecc-pixel-icon { color: #a890d0; font-size: 12px; padding-right: 4px; }"
                                              ".mpcecc-pixel-title {"
                                              "  color: #e8dcf8;"
                                              "  font-weight: 500;"
                                              "  font-size: 11px;"
                                              "}"
                                              ".mpcecc-pixel-btn {"
                                              "  color: #c8b8e0;"
                                              "  background: transparent;"
                                              "  border: none;"
                                              "  min-width: 28px;"
                                              "  min-height: 24px;"
                                              "  border-radius: 2px;"
                                              "  padding: 0 6px;"
                                              "  font-weight: 500;"
                                              "  font-size: 13px;"
                                              "}"
                                              ".mpcecc-pixel-btn:hover {"
                                              "  background-color: #3d2a58;"
                                              "  color: #ffffff;"
                                              "}"
                                              ".mpcecc-pixel-close:hover {"
                                              "  background-color: #9a2a38;"
                                              "  color: #ffffff;"
                                              "}");
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                                       GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(css);
            pixel_css = TRUE;
        }

        tab->pixel_surface = frame;
        mpcecc_pixel_win_apply_state(frame);
        gtk_overlay_add_overlay(GTK_OVERLAY(tab->container), frame);
        gtk_overlay_set_measure_overlay(GTK_OVERLAY(tab->container), frame, FALSE);
    }

    g_object_set_data_full(G_OBJECT(tab->container), "mpcecc-tab",
                           tab, (GDestroyNotify)mpcecc_free_tab);

    mpcecc_apply_profile(tab);
    mpcecc_install_context_menu(tab);

    const char *shell = shell_override ? shell_override : profile->shell;
    tab->shell = g_strdup(shell);

    /*
     * Track child exits so a crashed shell produces a visible
     * "<shell> crashed" dialog with a Restart Shell button instead
     * of a silently dead tab.
     */
    g_signal_connect(tab->terminal, "child-exited",
                     G_CALLBACK(mpcecc_child_exited), tab);

    mpcecc_spawn_shell(tab->terminal, shell, tab);

    char title[256];
    snprintf(title, sizeof(title), "%s — %s", profile_name, shell);

    int page = gtk_notebook_append_page(
        ecc->tabs,
        tab->container,
        mpcecc_make_tab_label(tab, title));

    gtk_notebook_set_current_page(ecc->tabs, page);
    gtk_notebook_set_tab_reorderable(ecc->tabs, tab->container, TRUE);

    return tab;
}

static const char *
mpcecc_selected_profile_name(MPCECC *ecc)
{
    GtkStringList *model =
    GTK_STRING_LIST(gtk_drop_down_get_model(ecc->profile_selector));
    guint selected = gtk_drop_down_get_selected(ecc->profile_selector);

    if (selected == GTK_INVALID_LIST_POSITION ||
        selected >= g_list_model_get_n_items(G_LIST_MODEL(model)))
        return "MarekPC Purple";

    return gtk_string_list_get_string(model, selected);
}

static void
mpcecc_new_tab_button_clicked(GtkButton *button, gpointer user_data)
{
    MPCECC *ecc = user_data;
    const char *profile_name = mpcecc_selected_profile_name(ecc);

    (void)button;

    g_free(ecc->active_profile);
    ecc->active_profile = g_strdup(profile_name);

    mpcecc_new_tab(ecc, profile_name, NULL);
}

static void
mpcecc_run_command_in_current_tab(MPCECC *ecc, const char *command)
{
    MpcEccTab *tab = mpcecc_current_tab(ecc);

    if (!tab || !tab->terminal)
        return;

    char line[512];
    snprintf(line, sizeof(line), "%s\n", command);

    vte_terminal_feed_child(tab->terminal, line, -1);
}

static void
mpcecc_interpreter_button_clicked(GtkButton *button, gpointer user_data)
{
    MPCECC *ecc = user_data;
    const char *command = g_object_get_data(G_OBJECT(button), "command");
    const char *mode = g_object_get_data(G_OBJECT(button), "mode");

    if (g_strcmp0(mode, "tab") == 0) {
        mpcecc_new_tab(ecc, mpcecc_selected_profile_name(ecc), command);
    } else {
        mpcecc_run_command_in_current_tab(ecc, command);
    }
}

static void
mpcecc_add_interpreter_button(GtkWidget *box, MPCECC *ecc,
                              const char *label, const char *command,
                              const char *mode)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_object_set_data_full(G_OBJECT(button), "command",
                           g_strdup(command), g_free);
    g_object_set_data_full(G_OBJECT(button), "mode",
                           g_strdup(mode), g_free);

    g_signal_connect(button, "clicked",
                     G_CALLBACK(mpcecc_interpreter_button_clicked), ecc);

    gtk_box_append(GTK_BOX(box), button);
}

static void
mpcecc_show_help(GtkButton *button, gpointer user_data)
{
    MPCECC *ecc = user_data;

    (void)button;

    GtkWidget *help_window = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(help_window), "MPCECC Help");
    gtk_window_set_transient_for(GTK_WINDOW(help_window), ecc->window);
    gtk_window_set_modal(GTK_WINDOW(help_window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(help_window), 720, 560);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(help_window), root);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(help_window), header);

    GtkWidget *close_button = gtk_button_new_with_label("Close");

    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_destroy), help_window);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), close_button);

    GtkScrolledWindow *scroll =
    GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());

    gtk_widget_set_vexpand(GTK_WIDGET(scroll), TRUE);
    gtk_scrolled_window_set_policy(scroll,
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    GtkWidget *help_label = gtk_label_new(
        "MarekPC Extended Capabilities Console\n\n"
        "• Every tab is a real terminal running its own shell\n"
        "• Close tabs with ✕, Ctrl+Shift+W, or the right-click menu\n"
        "• Copy selected text: Ctrl+Shift+C or right-click → Copy\n"
        "• Paste: Ctrl+Shift+V or Ctrl+V, or right-click → Paste\n"
        "• New tab: Ctrl+Shift+T\n"
        "• Profile settings: Ctrl+comma\n"
        "• The wallpaper is a real GTK image behind the terminal\n"
        "• Python, Node, Fish, and mpcsh open persistent interpreter tabs\n"
        "• If a shell crashes, MPCECC shows a dialog with Restart Shell\n\n"
        "Profiles are stored in:\n"
        "~/.config/mpcecc/profiles.conf\n\n"
        "Example:\n"
        "[MarekPC Purple]\n"
        "Wallpaper=/bin/mpcshresources/wallpaper.png\n"
        "Font=FantasqueSansM Nerd Font Propo 11\n"
        "Shell=/bin/mpcsh\n");

    gtk_label_set_wrap(GTK_LABEL(help_label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(help_label), 0.0f);

    gtk_widget_set_margin_top(help_label, 18);
    gtk_widget_set_margin_bottom(help_label, 18);
    gtk_widget_set_margin_start(help_label, 18);
    gtk_widget_set_margin_end(help_label, 18);

    gtk_scrolled_window_set_child(scroll, help_label);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(scroll));

    gtk_window_present(GTK_WINDOW(help_window));
}

static void
mpcecc_copy_action(GSimpleAction *action, GVariant *parameter,
                   gpointer user_data)
{
    MPCECC *ecc = user_data;
    MpcEccTab *tab = mpcecc_current_tab(ecc);

    (void)action;
    (void)parameter;

    if (tab && tab->terminal)
        vte_terminal_copy_clipboard_format(tab->terminal, VTE_FORMAT_TEXT);
}

static void
mpcecc_paste_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data)
{
    MPCECC *ecc = user_data;
    MpcEccTab *tab = mpcecc_current_tab(ecc);

    (void)action;
    (void)parameter;

    if (tab && tab->terminal)
        vte_terminal_paste_clipboard(tab->terminal);
}

static void
mpcecc_select_all_action(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data)
{
    MPCECC *ecc = user_data;
    MpcEccTab *tab = mpcecc_current_tab(ecc);

    (void)action;
    (void)parameter;

    if (tab && tab->terminal)
        vte_terminal_select_all(tab->terminal);
}

static void
mpcecc_clear_action(GSimpleAction *action, GVariant *parameter,
                    gpointer user_data)
{
    MPCECC *ecc = user_data;
    MpcEccTab *tab = mpcecc_current_tab(ecc);

    (void)action;
    (void)parameter;

    if (tab && tab->terminal)
        vte_terminal_reset(tab->terminal, TRUE, TRUE);
}

static void
mpcecc_new_tab_action(GSimpleAction *action, GVariant *parameter,
                      gpointer user_data)
{
    MPCECC *ecc = user_data;

    (void)action;
    (void)parameter;

    mpcecc_new_tab(ecc, mpcecc_selected_profile_name(ecc), NULL);
}

static void
mpcecc_close_tab_action(GSimpleAction *action, GVariant *parameter,
                        gpointer user_data)
{
    MPCECC *ecc = user_data;
    int page = gtk_notebook_get_current_page(ecc->tabs);

    (void)action;
    (void)parameter;

    if (page < 0)
        return;

    GtkWidget *child = gtk_notebook_get_nth_page(ecc->tabs, page);

    if (!child)
        return;

    MpcEccTab *tab = g_object_get_data(G_OBJECT(child), "mpcecc-tab");
    mpcecc_close_tab(ecc, tab);
}

static void
mpcecc_help_action(GSimpleAction *action, GVariant *parameter,
                   gpointer user_data)
{
    (void)action;
    (void)parameter;
    mpcecc_show_help(NULL, user_data);
}

static void
mpcecc_context_menu_pressed(GtkGestureClick *gesture, int press_count,
                            double x, double y, gpointer user_data)
{
    GtkWidget *popover = user_data;
    GdkRectangle rectangle = { (int)x, (int)y, 1, 1 };

    (void)gesture;
    (void)press_count;

    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &rectangle);
    gtk_popover_popup(GTK_POPOVER(popover));
}

static void
mpcecc_install_context_menu(MpcEccTab *tab)
{
    const char *menu_xml =
    "<?xml version='1.0'?>"
    "<interface>"
    "  <menu id='mpcecc-context-menu'>"
    "    <section>"
    "      <item>"
    "        <attribute name='label'>Copy</attribute>"
    "        <attribute name='action'>win.copy</attribute>"
    "        <attribute name='accel'>&lt;Control&gt;&lt;Shift&gt;c</attribute>"
    "      </item>"
    "      <item>"
    "        <attribute name='label'>Paste</attribute>"
    "        <attribute name='action'>win.paste</attribute>"
    "        <attribute name='accel'>&lt;Control&gt;&lt;Shift&gt;v</attribute>"
    "      </item>"
    "      <item>"
    "        <attribute name='label'>Select All</attribute>"
    "        <attribute name='action'>win.select-all</attribute>"
    "      </item>"
    "    </section>"
    "    <section>"
    "      <item>"
    "        <attribute name='label'>Clear Terminal</attribute>"
    "        <attribute name='action'>win.clear</attribute>"
    "      </item>"
    "      <item>"
    "        <attribute name='label'>New Tab</attribute>"
    "        <attribute name='action'>win.new-tab</attribute>"
    "        <attribute name='accel'>&lt;Control&gt;&lt;Shift&gt;t</attribute>"
    "      </item>"
    "      <item>"
    "        <attribute name='label'>Close Tab</attribute>"
    "        <attribute name='action'>win.close-tab</attribute>"
    "        <attribute name='accel'>&lt;Control&gt;&lt;Shift&gt;w</attribute>"
    "      </item>"
    "    </section>"
    "    <section>"
    "      <item>"
    "        <attribute name='label'>Profile Settings</attribute>"
    "        <attribute name='action'>win.settings</attribute>"
    "      </item>"
    "      <item>"
    "        <attribute name='label'>Help</attribute>"
    "        <attribute name='action'>win.help</attribute>"
    "      </item>"
    "    </section>"
    "  </menu>"
    "</interface>";

    GtkBuilder *builder = gtk_builder_new_from_string(menu_xml, -1);
    GMenuModel *menu = G_MENU_MODEL(
        gtk_builder_get_object(builder, "mpcecc-context-menu"));

    GtkWidget *popover = gtk_popover_menu_new_from_model(menu);

    gtk_widget_set_parent(popover, GTK_WIDGET(tab->terminal));
    gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);

    GtkGestureClick *right_click =
    GTK_GESTURE_CLICK(gtk_gesture_click_new());

    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(right_click),
                                  GDK_BUTTON_SECONDARY);

    g_signal_connect(right_click, "pressed",
                     G_CALLBACK(mpcecc_context_menu_pressed), popover);

    gtk_widget_add_controller(GTK_WIDGET(tab->terminal),
                              GTK_EVENT_CONTROLLER(right_click));

    g_object_unref(builder);
}

static void
mpcecc_install_shortcuts(MPCECC *ecc)
{
    GtkShortcutController *shortcuts =
    GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());

    gtk_shortcut_controller_set_scope(shortcuts, GTK_SHORTCUT_SCOPE_LOCAL);

    /* VTE's own key controller consumes (and forwards to the shell) every
     * key press while the terminal has focus.  A shortcut controller in the
     * default bubble phase therefore never sees Ctrl+Shift+T/W, Ctrl+, or
     * Ctrl+? (Ctrl+, even got typed into the shell as a literal ",").
     * Run the shortcuts in the capture phase so the window handles them
     * BEFORE the focused terminal gets the event. */
    gtk_event_controller_set_propagation_phase(
        GTK_EVENT_CONTROLLER(shortcuts), GTK_PHASE_CAPTURE);

    struct {
        const char *accelerator;
        const char *action;
    } bindings[] = {
        { "<Control><Shift>c",  "win.copy" },
        { "<Control>v",         "win.paste" },
        { "<Control><Shift>v",  "win.paste" },
        { "<Control><Shift>t",  "win.new-tab" },
        { "<Control><Shift>w",  "win.close-tab" },
        { "<Control>comma",     "win.settings" },
        { "<Control>question",  "win.help" },
        { "<Control><Shift>slash", "win.help" }
    };

    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); ++i) {
        GtkShortcut *shortcut = gtk_shortcut_new(
            gtk_shortcut_trigger_parse_string(bindings[i].accelerator),
                                                 gtk_named_action_new(bindings[i].action));

        gtk_shortcut_controller_add_shortcut(shortcuts, shortcut);
    }

    gtk_widget_add_controller(GTK_WIDGET(ecc->window),
                              GTK_EVENT_CONTROLLER(shortcuts));
}

static void
mpcecc_add_action(MPCECC *ecc, const char *name,
                  void (*callback)(GSimpleAction *, GVariant *, gpointer))
{
    GSimpleAction *action = g_simple_action_new(name, NULL);

    g_signal_connect(action, "activate", G_CALLBACK(callback), ecc);
    g_action_map_add_action(G_ACTION_MAP(ecc->window), G_ACTION(action));
    g_object_unref(action);
}

/* The settings window has its own profile dropdown (settings->selector),
 * so it reads the selection from there, not from the header selector. */
static const char *
mpcecc_settings_selected_name(MpcEccSettings *settings)
{
    GtkStringList *model =
    GTK_STRING_LIST(gtk_drop_down_get_model(settings->selector));
    guint selected = gtk_drop_down_get_selected(settings->selector);

    if (selected == GTK_INVALID_LIST_POSITION ||
        selected >= g_list_model_get_n_items(G_LIST_MODEL(model)))
        return "MarekPC Purple";

    return gtk_string_list_get_string(model, selected);
}

static void
mpcecc_settings_load_selected(MpcEccSettings *settings)
{
    MPCECC *ecc = settings->ecc;
    const char *profile_name = mpcecc_settings_selected_name(settings);
    MpcEccProfile *profile = mpcecc_profile_by_name(ecc, profile_name);

    gtk_editable_set_text(GTK_EDITABLE(settings->name), profile->name);
    gtk_editable_set_text(GTK_EDITABLE(settings->wallpaper),
                          profile->wallpaper);
    gtk_editable_set_text(GTK_EDITABLE(settings->font), profile->font);
    gtk_editable_set_text(GTK_EDITABLE(settings->shell), profile->shell);
}

static void
mpcecc_settings_selection_changed(GtkDropDown *selector,
                                  GParamSpec *parameter,
                                  gpointer user_data)
{
    (void)selector;
    (void)parameter;
    mpcecc_settings_load_selected(user_data);
}

static void
mpcecc_settings_refresh_selector(MpcEccSettings *settings,
                                 const char *select_name)
{
    MPCECC *ecc = settings->ecc;
    GtkStringList *model = mpcecc_make_profile_model(ecc);
    guint index = 0;

    gtk_drop_down_set_model(settings->selector, G_LIST_MODEL(model));
    g_object_unref(model);

    for (guint i = 0; i < ecc->profiles->len; ++i) {
        MpcEccProfile *profile = g_ptr_array_index(ecc->profiles, i);

        if (g_strcmp0(profile->name, select_name) == 0) {
            index = i;
            break;
        }
    }

    gtk_drop_down_set_selected(settings->selector, index);
    mpcecc_settings_load_selected(settings);
}

static void
mpcecc_settings_save_clicked(GtkButton *button, gpointer user_data)
{
    MpcEccSettings *settings = user_data;
    MPCECC *ecc = settings->ecc;

    (void)button;

    const char *old_name = mpcecc_settings_selected_name(settings);
    MpcEccProfile *profile = mpcecc_profile_by_name(ecc, old_name);

    const char *name =
    gtk_editable_get_text(GTK_EDITABLE(settings->name));
    const char *wallpaper =
    gtk_editable_get_text(GTK_EDITABLE(settings->wallpaper));
    const char *font =
    gtk_editable_get_text(GTK_EDITABLE(settings->font));
    const char *shell =
    gtk_editable_get_text(GTK_EDITABLE(settings->shell));

    /* Copy before freeing: 'name' etc. point into the entries, but
     * old_name points into the dropdown model and profile->name is
     * about to be replaced. */
    char *new_name = g_strdup(name && *name ? name : "Untitled");
    char *new_wallpaper = g_strdup(wallpaper && *wallpaper
    ? wallpaper : MPCECC_DEFAULT_WALLPAPER);
    char *new_font = g_strdup(font && *font ? font : "Monospace 11");
    char *new_shell = g_strdup(shell && *shell ? shell : "fish");

    g_free(profile->name);
    g_free(profile->wallpaper);
    g_free(profile->font);
    g_free(profile->shell);

    profile->name = new_name;
    profile->wallpaper = new_wallpaper;
    profile->font = new_font;
    profile->shell = new_shell;

    g_free(ecc->active_profile);
    ecc->active_profile = g_strdup(profile->name);

    mpcecc_save_profiles(ecc);
    mpcecc_refresh_profile_selector(ecc);
    mpcecc_settings_refresh_selector(settings, profile->name);
}

static void
mpcecc_settings_apply_clicked(GtkButton *button, gpointer user_data)
{
    MpcEccSettings *settings = user_data;
    MPCECC *ecc = settings->ecc;
    MpcEccTab *tab = mpcecc_current_tab(ecc);

    (void)button;

    if (!tab)
        return;

    g_free(tab->profile_name);
    tab->profile_name = g_strdup(mpcecc_settings_selected_name(settings));

    mpcecc_apply_profile(tab);
}

static void
mpcecc_settings_new_clicked(GtkButton *button, gpointer user_data)
{
    MpcEccSettings *settings = user_data;
    MPCECC *ecc = settings->ecc;

    (void)button;

    MpcEccProfile *profile = g_new0(MpcEccProfile, 1);

    profile->name = g_strdup("New Profile");
    profile->wallpaper = g_strdup(MPCECC_DEFAULT_WALLPAPER);
    profile->font = g_strdup("FantasqueSansM Nerd Font Propo 11");
    profile->shell = g_strdup("mpcsh");

    g_ptr_array_add(ecc->profiles, profile);

    g_free(ecc->active_profile);
    ecc->active_profile = g_strdup(profile->name);

    mpcecc_save_profiles(ecc);
    mpcecc_refresh_profile_selector(ecc);
    mpcecc_settings_refresh_selector(settings, profile->name);
}

static void
mpcecc_settings_delete_clicked(GtkButton *button, gpointer user_data)
{
    MpcEccSettings *settings = user_data;
    MPCECC *ecc = settings->ecc;

    (void)button;

    if (ecc->profiles->len <= 1)
        return;

    char *profile_name = g_strdup(mpcecc_settings_selected_name(settings));

    for (guint i = 0; i < ecc->profiles->len; ++i) {
        MpcEccProfile *profile = g_ptr_array_index(ecc->profiles, i);

        if (g_strcmp0(profile->name, profile_name) == 0) {
            g_ptr_array_remove_index(ecc->profiles, i);
            break;
        }
    }

    g_free(profile_name);

    MpcEccProfile *first_profile =
    g_ptr_array_index(ecc->profiles, 0);

    g_free(ecc->active_profile);
    ecc->active_profile = g_strdup(first_profile->name);

    mpcecc_save_profiles(ecc);
    mpcecc_refresh_profile_selector(ecc);
    mpcecc_settings_refresh_selector(settings, first_profile->name);
}

static void
mpcecc_show_settings(GtkButton *button, gpointer user_data)
{
    MPCECC *ecc = user_data;

    (void)button;

    GtkWidget *settings_window = gtk_window_new();

    gtk_window_set_title(GTK_WINDOW(settings_window),
                         "MPCECC Profile Settings");
    gtk_window_set_transient_for(GTK_WINDOW(settings_window), ecc->window);
    gtk_window_set_modal(GTK_WINDOW(settings_window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(settings_window), 720, 520);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    gtk_widget_set_margin_top(root, 18);
    gtk_widget_set_margin_bottom(root, 18);
    gtk_widget_set_margin_start(root, 18);
    gtk_widget_set_margin_end(root, 18);

    gtk_window_set_child(GTK_WINDOW(settings_window), root);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(settings_window), header);

    GtkWidget *close_button = gtk_button_new_with_label("Close");

    g_signal_connect_swapped(close_button, "clicked",
                             G_CALLBACK(gtk_window_destroy),
                             settings_window);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), close_button);

    MpcEccSettings *settings = g_new0(MpcEccSettings, 1);
    settings->ecc = ecc;

    /* Free the settings struct when the window goes away. */
    g_object_set_data_full(G_OBJECT(settings_window), "mpcecc-settings",
                           settings, g_free);

    GtkWidget *profile_label = gtk_label_new("Profile");
    gtk_widget_set_halign(profile_label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(root), profile_label);

    GtkStringList *profile_model = mpcecc_make_profile_model(ecc);

    settings->selector = GTK_DROP_DOWN(
        gtk_drop_down_new(G_LIST_MODEL(profile_model), NULL));

    gtk_drop_down_set_selected(settings->selector, 0);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(settings->selector));

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_append(GTK_BOX(root), grid);

    struct {
        const char *label;
        GtkEntry **entry;
    } fields[] = {
        { "Profile name", &settings->name },
        { "Wallpaper image", &settings->wallpaper },
        { "Terminal font", &settings->font },
        { "Shell", &settings->shell }
    };

    for (guint i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        GtkWidget *label = gtk_label_new(fields[i].label);
        gtk_widget_set_halign(label, GTK_ALIGN_START);

        *fields[i].entry = GTK_ENTRY(gtk_entry_new());
        gtk_widget_set_hexpand(GTK_WIDGET(*fields[i].entry), TRUE);

        gtk_grid_attach(GTK_GRID(grid), label, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid),
                        GTK_WIDGET(*fields[i].entry), 1, i, 1, 1);
    }

    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(button_box, 16);
    gtk_box_append(GTK_BOX(root), button_box);

    GtkWidget *save_button = gtk_button_new_with_label("Save Profile");
    GtkWidget *apply_button =
    gtk_button_new_with_label("Apply to Current Tab");
    GtkWidget *new_button = gtk_button_new_with_label("New Profile");
    GtkWidget *delete_button =
    gtk_button_new_with_label("Delete Profile");

    gtk_box_append(GTK_BOX(button_box), save_button);
    gtk_box_append(GTK_BOX(button_box), apply_button);
    gtk_box_append(GTK_BOX(button_box), new_button);
    gtk_box_append(GTK_BOX(button_box), delete_button);

    g_signal_connect(save_button, "clicked",
                     G_CALLBACK(mpcecc_settings_save_clicked), settings);
    g_signal_connect(apply_button, "clicked",
                     G_CALLBACK(mpcecc_settings_apply_clicked), settings);
    g_signal_connect(new_button, "clicked",
                     G_CALLBACK(mpcecc_settings_new_clicked), settings);
    g_signal_connect(delete_button, "clicked",
                     G_CALLBACK(mpcecc_settings_delete_clicked), settings);
    g_signal_connect(settings->selector, "notify::selected",
                     G_CALLBACK(mpcecc_settings_selection_changed),
                     settings);

    mpcecc_settings_load_selected(settings);

    gtk_window_present(GTK_WINDOW(settings_window));
}

static void
mpcecc_settings_action(GSimpleAction *action, GVariant *parameter,
                       gpointer user_data)
{
    (void)action;
    (void)parameter;
    mpcecc_show_settings(NULL, user_data);
}

static gboolean
mpcecc_command_is_gui(char **command)
{
    if (!command || !command[0]) return FALSE;
    const char *base = strrchr(command[0], '/');
    base = base ? base + 1 : command[0];
    /* Deliberately conservative: ordinary CLI programs stay in VTE.
     * Add more desktop application names here as needed. */
    static const char *const gui_names[] = {
        "firefox", "firefox-bin", "firefox-esr", "firefox-nightly",
        "firefox-developer-edition", "chromium", "chromium-browser",
        "google-chrome", "google-chrome-stable", "brave", "brave-browser",
        "librewolf", "zen", "gimp", "inkscape", "libreoffice",
        "thunar", "pcmanfm-qt", "dolphin", "kate", "mousepad", "featherpad",
        "pavucontrol", "virt-manager", "org.gnome.Nautilus", NULL
    };
    for (guint i = 0; gui_names[i]; ++i)
        if (!strcmp(base, gui_names[i])) return TRUE;
        return FALSE;
}


/* ---- MPC PixelWindow: private Xvfb + frame capture shown in a GtkPicture ---- */
static GtkWidget *mpcecc_pixel_window = NULL;
static GtkWidget *mpcecc_pixel_picture = NULL;
static gboolean mpcecc_pixel_capture_busy = FALSE;
static int mpcecc_pixel_wait_tries = 0;
static gboolean mpcecc_pixel_close_on_exit = FALSE;

/* ---- keyboard forwarding state (private Xvfb has NO window manager, so the
 * app never gets X input focus on its own; we focus it ourselves and send
 * keys strictly in order through a queue) ---- */
static char *mpcecc_pixel_class = NULL;        /* WM_CLASS regex of the launched app */
static char *mpcecc_pixel_xwin = NULL;         /* cached X window id of the app */
static GQueue mpcecc_pixel_keyq = G_QUEUE_INIT;
static gboolean mpcecc_pixel_key_busy = FALSE;

static void mpcecc_pixel_set_class(const char *argv0)
{
    g_clear_pointer(&mpcecc_pixel_class, g_free);
    g_clear_pointer(&mpcecc_pixel_xwin, g_free);
    if (!argv0 || !*argv0) return;
    const char *base = strrchr(argv0, '/');
    base = base ? base + 1 : argv0;
    mpcecc_pixel_class = g_str_has_prefix(base, "firefox") ? g_strdup("firefox") : g_strdup(base);
}

static void mpcecc_pixel_cleanup(void)
{
    if (mpcecc_pixel_ready_id) { g_source_remove(mpcecc_pixel_ready_id); mpcecc_pixel_ready_id = 0; }
    if (mpcecc_pixel_capture_id) { g_source_remove(mpcecc_pixel_capture_id); mpcecc_pixel_capture_id = 0; }
    if (mpcecc_pixel_app_pid) { kill((pid_t)mpcecc_pixel_app_pid, SIGTERM); g_spawn_close_pid(mpcecc_pixel_app_pid); mpcecc_pixel_app_pid = 0; }
    if (mpcecc_pixel_xvfb_pid) { kill((pid_t)mpcecc_pixel_xvfb_pid, SIGTERM); g_spawn_close_pid(mpcecc_pixel_xvfb_pid); mpcecc_pixel_xvfb_pid = 0; }
    if (mpcecc_pixel_frame_path) unlink(mpcecc_pixel_frame_path);
    g_queue_clear_full(&mpcecc_pixel_keyq, g_free);
    mpcecc_pixel_key_busy = FALSE;
    g_clear_pointer(&mpcecc_pixel_xwin, g_free);
    g_clear_pointer(&mpcecc_pixel_display, g_free);
    g_clear_pointer(&mpcecc_pixel_frame_path, g_free);
}

static void mpcecc_pixel_feed(const char *text)
{
    if (!mpcecc_pixel_terminal || !text) return;
    gtk_widget_set_visible(GTK_WIDGET(mpcecc_pixel_terminal), TRUE);
    vte_terminal_feed(mpcecc_pixel_terminal, text, -1);
}

static void mpcecc_pixel_feed_error(const char *message)
{
    char *line = g_strdup_printf("\r\n\033[31mmpcecc: %s\033[0m\r\n", message ? message : "unknown error");
    mpcecc_pixel_feed(line); g_free(line);
}

static void mpcecc_pixel_capture_done(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    g_spawn_close_pid(pid);
    mpcecc_pixel_capture_busy = FALSE;
    if (!mpcecc_pixel_frame_path || !mpcecc_pixel_picture) return;   /* window already torn down */
        GError *error = NULL;
    if (!g_spawn_check_wait_status(status, &error)) {
        /* Xvfb may simply be gone because the app just exited: only complain if the app is still alive. */
        if (mpcecc_pixel_app_pid) { mpcecc_pixel_feed_error(error ? error->message : "frame capture failed"); mpcecc_pixel_cleanup(); }
        g_clear_error(&error); return;
    }
    GdkTexture *tex = gdk_texture_new_from_filename(mpcecc_pixel_frame_path, &error);
    if (!tex) { g_clear_error(&error); return; }                     /* half-written frame: skip, try next tick */
        gtk_picture_set_paintable(GTK_PICTURE(mpcecc_pixel_picture), GDK_PAINTABLE(tex));
        g_object_unref(tex);
        ++mpcecc_pixel_frame;
}

static gboolean mpcecc_pixel_capture_tick(gpointer user_data)
{
    (void)user_data;
    if (!mpcecc_pixel_frame_path || !mpcecc_pixel_display) { mpcecc_pixel_capture_id = 0; return G_SOURCE_REMOVE; }
    if (mpcecc_pixel_capture_busy) return G_SOURCE_CONTINUE;         /* previous capture still running */
        gchar *argv[] = { "import", "-silent", "-display", mpcecc_pixel_display, "-window", "root", mpcecc_pixel_frame_path, NULL };
    GError *error = NULL; GPid pid = 0;
    gchar **cenv = g_get_environ();
    cenv = g_environ_setenv(cenv, "DISPLAY", mpcecc_pixel_display, TRUE);   /* import can prefer $DISPLAY over -display */
    gboolean cok = g_spawn_async(NULL, argv, cenv, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &pid, &error);
    g_strfreev(cenv);
    if (!cok) {
        mpcecc_pixel_capture_id = 0;
        mpcecc_pixel_feed_error(error ? error->message : "frame capture failed; install imagemagick");
        g_clear_error(&error); mpcecc_pixel_cleanup(); return G_SOURCE_REMOVE;
    }
    mpcecc_pixel_capture_busy = TRUE;
    g_child_watch_add(pid, mpcecc_pixel_capture_done, NULL);
    return G_SOURCE_CONTINUE;
}

static void mpcecc_pixel_app_exited(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    g_spawn_close_pid(pid);
    if (pid != mpcecc_pixel_app_pid) return;                         /* already cleaned up */
        mpcecc_pixel_app_pid = 0;                                        /* reaped by GLib: never kill() this pid again */
        gboolean clean = g_spawn_check_wait_status(status, NULL);
    GtkWidget *window = mpcecc_pixel_window;
    if (clean && mpcecc_pixel_close_on_exit && window) { mpcecc_pixel_cleanup(); gtk_window_close(GTK_WINDOW(window)); return; }
    mpcecc_pixel_feed(clean ? "\r\n\033[33mmpcecc: application exited.\033[0m\r\n" : "\r\n\033[31mmpcecc: application exited with an error.\033[0m\r\n");
    mpcecc_pixel_cleanup();
}

static void mpcecc_pixel_xvfb_exited(GPid pid, gint status, gpointer user_data)
{
    (void)status; (void)user_data;
    g_spawn_close_pid(pid);                                          /* reaps the zombie */
    if (pid != mpcecc_pixel_xvfb_pid) return;                        /* we killed it ourselves in cleanup */
        mpcecc_pixel_xvfb_pid = 0;
    mpcecc_pixel_feed_error("private X server (Xvfb) exited unexpectedly");
    mpcecc_pixel_cleanup();
}


/* Build a launch argv for Firefox that uses ONLY the isolated
 * MPCECCWLPROFILE directory. Never touches the user's default profile,
 * bookmarks, cookies, or login sessions. Returns a newly allocated
 * argv that the caller must free element-wise, or NULL if not Firefox. */
static char **
mpcecc_firefox_isolated_argv(char **orig_command, const char *home)
{
    if (!orig_command || !orig_command[0] || !home)
        return NULL;

    char *base = g_path_get_basename(orig_command[0]);
    gboolean is_firefox =
    (g_strcmp0(base, "firefox") == 0 ||
    g_strcmp0(base, "firefox-bin") == 0 ||
    g_str_has_prefix(base, "firefox"));
    g_free(base);
    if (!is_firefox)
        return NULL;

    char *profile_dir = g_build_filename(home, ".mozilla", "firefox",
                                         "MPCECCWLPROFILE", NULL);
    g_mkdir_with_parents(profile_dir, 0700);

    char *lock1 = g_build_filename(profile_dir, "lock", NULL);
    char *lock2 = g_build_filename(profile_dir, ".parentlock", NULL);
    unlink(lock1);
    unlink(lock2);
    g_free(lock1);
    g_free(lock2);

    char *prefs = g_build_filename(profile_dir, "user.js", NULL);
    if (TRUE) { /* refresh isolated-profile prefs every launch */
        const char *js =
        "user_pref(\"browser.shell.checkDefaultBrowser\", false);\n"
        "user_pref(\"datareporting.policy.dataSubmissionEnabled\", false);\n"
        "user_pref(\"toolkit.telemetry.enabled\", false);\n"
        "user_pref(\"browser.startup.homepage_override.mstone\", \"ignore\");\n"
        "user_pref(\"browser.aboutwelcome.enabled\", false);\n"
        "user_pref(\"browser.tabs.warnOnClose\", false);\n"
        "user_pref(\"browser.sessionstore.resume_from_crash\", false);\n"
        "user_pref(\"browser.sessionstore.max_resumed_crashes\", 0);\n"
        "user_pref(\"browser.startup.page\", 0);\n"
        "user_pref(\"toolkit.startup.max_resumed_crashes\", -1);\n";
        g_file_set_contents(prefs, js, -1, NULL);
    }
    g_free(prefs);

    int orig_count = 0;
    while (orig_command[orig_count])
        ++orig_count;

    char **owned = g_new0(char *, orig_count + 8);
    owned[0] = g_strdup(orig_command[0]);
    owned[1] = g_strdup("--no-remote");
    owned[2] = g_strdup("--new-instance");
    owned[3] = g_strdup("-profile");
    owned[4] = profile_dir;
    for (int i = 1; i < orig_count; ++i)
        owned[4 + i] = g_strdup(orig_command[i]);
    return owned;
}

static void mpcecc_pixel_start_application(void)
{
    if (mpcecc_pixel_ready_id) { g_source_remove(mpcecc_pixel_ready_id); mpcecc_pixel_ready_id = 0; }
    if (!mpcecc_pixel_terminal || !mpcecc_pixel_display) return;

    /* Private runtime dir owned by the user */
    char runtime[256];
    snprintf(runtime, sizeof(runtime), "/tmp/mpc-runtime-%d", (int)getuid());
    g_mkdir_with_parents(runtime, 0700);

    const char *home = g_get_home_dir() ? g_get_home_dir() : "/tmp";
    gchar **environment = NULL;
    environment = g_environ_setenv(environment, "DISPLAY", mpcecc_pixel_display, TRUE);
    environment = g_environ_setenv(environment, "HOME", home, TRUE);
    environment = g_environ_setenv(environment, "USER", g_get_user_name() ? g_get_user_name() : "user", TRUE);
    environment = g_environ_setenv(environment, "LOGNAME", g_get_user_name() ? g_get_user_name() : "user", TRUE);
    environment = g_environ_setenv(environment, "PATH", "/usr/local/bin:/usr/bin:/bin", TRUE);
    environment = g_environ_setenv(environment, "LANG", "C.UTF-8", TRUE);
    environment = g_environ_setenv(environment, "GDK_BACKEND", "x11", TRUE);
    environment = g_environ_setenv(environment, "QT_QPA_PLATFORM", "xcb", TRUE);
    environment = g_environ_setenv(environment, "SDL_VIDEODRIVER", "x11", TRUE);
    environment = g_environ_setenv(environment, "MOZ_ENABLE_WAYLAND", "0", TRUE);
    environment = g_environ_setenv(environment, "MOZ_DISABLE_WAYLAND", "1", TRUE);
    environment = g_environ_setenv(environment, "GDK_SYNCHRONIZE", "1", TRUE);
    environment = g_environ_setenv(environment, "MPCECC_PIXEL_WINDOW", "1", TRUE);
    environment = g_environ_setenv(environment, "XDG_SESSION_TYPE", "x11", TRUE);
    environment = g_environ_setenv(environment, "XDG_RUNTIME_DIR", runtime, TRUE);
    environment = g_environ_setenv(environment, "LIBGL_ALWAYS_SOFTWARE", "1", TRUE);
    environment = g_environ_setenv(environment, "GALLIUM_DRIVER", "llvmpipe", TRUE);
    environment = g_environ_setenv(environment, "QT_XCB_GL_INTEGRATION", "none", TRUE);

    /* Firefox: isolated MPCECCWLPROFILE only — never the real default. */
    mpcecc_pixel_set_class(mpcecc_window_command ? mpcecc_window_command[0] : NULL);
    char **launch_argv = mpcecc_window_command;
    char **owned_argv = mpcecc_firefox_isolated_argv(mpcecc_window_command, home);
    if (owned_argv)
        launch_argv = owned_argv;

    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL, launch_argv, environment,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CHILD_INHERITS_STDIN,
                                NULL, NULL, &mpcecc_pixel_app_pid, &error);

    if (owned_argv) {
        for (int i = 0; owned_argv[i]; ++i) g_free(owned_argv[i]);
        g_free(owned_argv);
    }
    g_strfreev(environment);

    if (!ok) {
        mpcecc_pixel_feed_error(error ? error->message : "could not start application");
        g_clear_error(&error);
        mpcecc_pixel_cleanup();
        return;
    }
    g_child_watch_add(mpcecc_pixel_app_pid, mpcecc_pixel_app_exited, NULL);

    /* Give the application time to create its window and draw before capturing */
    g_timeout_add_seconds(3, (GSourceFunc)mpcecc_pixel_delayed_capture_start, NULL);
}



static gboolean
mpcecc_pixel_delayed_capture_start(gpointer user_data)
{
    (void)user_data;
    if (mpcecc_pixel_app_pid && !mpcecc_pixel_capture_id)
        mpcecc_pixel_capture_id = g_timeout_add(1000 / mpcecc_pixel_fps, mpcecc_pixel_capture_tick, NULL);
    return G_SOURCE_REMOVE;
}




static gboolean mpcecc_pixel_wait_display(gpointer user_data)
{
    (void)user_data;
    if (!mpcecc_pixel_display) { mpcecc_pixel_ready_id = 0; return G_SOURCE_REMOVE; }
    if (++mpcecc_pixel_wait_tries > 100) {                            /* ~10 s */
        mpcecc_pixel_ready_id = 0;
        mpcecc_pixel_feed_error("private X display never came up (is Xvfb working?)");
        mpcecc_pixel_cleanup(); return G_SOURCE_REMOVE;
    }
    gchar *argv[] = { "xdpyinfo", "-display", mpcecc_pixel_display, NULL };
    gint status = 0; GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, &status, &error);
    if (ok && g_spawn_check_wait_status(status, &error)) { mpcecc_pixel_ready_id = 0; mpcecc_pixel_start_application(); return G_SOURCE_REMOVE; }
    g_clear_error(&error); return G_SOURCE_CONTINUE;
}

static gchar **mpcecc_pixel_x_env(void)
{
    gchar **env = g_get_environ();
    return g_environ_setenv(env, "DISPLAY", mpcecc_pixel_display, TRUE);
}

/* Find (and cache) the X window of the launched app so we can give it focus. */
static void mpcecc_pixel_find_window(void)
{
    if (mpcecc_pixel_xwin || !mpcecc_pixel_class || !mpcecc_pixel_display) return;
    gchar *argv[] = { "xdotool", "search", "--onlyvisible", "--class", mpcecc_pixel_class, NULL };
    gchar **env = mpcecc_pixel_x_env();
    gchar *out = NULL; gint status = 0; GError *error = NULL;
    if (g_spawn_sync(NULL, argv, env, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL, NULL, &out, NULL, &status, &error) && out) {
        g_strstrip(out);
    gchar *nl = strchr(out, '\n');
    if (nl) *nl = 0;                                   /* first match */
        if (*out && strspn(out, "0123456789") == strlen(out))
            mpcecc_pixel_xwin = g_strdup(out);
        }
        g_clear_error(&error); g_free(out); g_strfreev(env);
}

static void mpcecc_pixel_key_pump(void);

static void mpcecc_pixel_key_done(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    g_spawn_close_pid(pid);
    if (!g_spawn_check_wait_status(status, NULL))
        g_clear_pointer(&mpcecc_pixel_xwin, g_free);       /* stale id: look it up again */
        mpcecc_pixel_key_busy = FALSE;
    mpcecc_pixel_key_pump();
}

/* Send queued keys ONE AT A TIME so fast typing arrives in order. */
static void mpcecc_pixel_key_pump(void)
{
    if (mpcecc_pixel_key_busy || !mpcecc_pixel_display) return;
    gchar *combo = g_queue_pop_head(&mpcecc_pixel_keyq);
    if (!combo) return;
    mpcecc_pixel_find_window();

    gchar *argv[8]; int n = 0;
    argv[n++] = "xdotool";
    if (mpcecc_pixel_xwin) { argv[n++] = "windowfocus"; argv[n++] = mpcecc_pixel_xwin; }
    argv[n++] = "key"; argv[n++] = combo; argv[n] = NULL;

    gchar **env = mpcecc_pixel_x_env();
    GPid pid = 0; GError *error = NULL;
    gboolean ok = g_spawn_async(NULL, argv, env,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, &pid, &error);
    g_strfreev(env); g_clear_error(&error); g_free(combo);
    if (ok) {
        mpcecc_pixel_key_busy = TRUE;
        g_child_watch_add(pid, mpcecc_pixel_key_done, NULL);
    } else {
        mpcecc_pixel_key_pump();
    }
}

static gboolean mpcecc_pixel_on_key(GtkEventControllerKey *controller, guint keyval, guint keycode, GdkModifierType state, gpointer user_data)
{
    (void)keycode; (void)user_data;
    GtkWidget *event_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    MpcEccWlid *event_win = mpcecc_wlid_for_picture(event_widget);
    if (event_win) mpcecc_wlid_focus(event_win);
    if (!mpcecc_pixel_display) return FALSE;
    if (keyval == GDK_KEY_Shift_L || keyval == GDK_KEY_Shift_R || keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R ||
        keyval == GDK_KEY_Alt_L || keyval == GDK_KEY_Alt_R || keyval == GDK_KEY_Super_L || keyval == GDK_KEY_Super_R ||
        keyval == GDK_KEY_Caps_Lock || keyval == GDK_KEY_Num_Lock || keyval == GDK_KEY_ISO_Level3_Shift ||
        keyval == GDK_KEY_Meta_L || keyval == GDK_KEY_Meta_R) return TRUE;
    /* Shift+Tab arrives as ISO_Left_Tab */
    const char *name = (keyval == GDK_KEY_ISO_Left_Tab) ? "Tab" : gdk_keyval_name(keyval);
    if (!name || !*name) return FALSE;
    GString *combo = g_string_new(NULL);
    if (state & GDK_CONTROL_MASK) g_string_append(combo, "ctrl+");
    if (state & GDK_ALT_MASK) g_string_append(combo, "alt+");
    if (state & GDK_SUPER_MASK) g_string_append(combo, "super+");
    /* Shift is implied by printable keysyms ("A", "exclam"), but must be sent
     * explicitly for Shift+Arrow / Shift+Tab / Shift+Home etc. */
    if ((state & GDK_SHIFT_MASK) && (keyval == GDK_KEY_ISO_Left_Tab || gdk_keyval_to_unicode(keyval) == 0))
        g_string_append(combo, "shift+");
    g_string_append(combo, name);
    if (g_queue_get_length(&mpcecc_pixel_keyq) < 256)
        g_queue_push_tail(&mpcecc_pixel_keyq, g_string_free(combo, FALSE));
    else
        g_string_free(combo, TRUE);
    mpcecc_pixel_key_pump();
    return TRUE;
}

static void mpcecc_pixel_on_click(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data)
{
    (void)n_press; (void)user_data;
    GtkWidget *event_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    MpcEccWlid *event_win = mpcecc_wlid_for_picture(event_widget);
    if (event_win) mpcecc_wlid_focus(event_win);
    GtkWidget *pic = event_widget ? event_widget : (mpcecc_pixel_picture ? mpcecc_pixel_picture : mpcecc_active_pixel_surface);
    if (!mpcecc_pixel_display || !pic) return;
    double aw = gtk_widget_get_width(pic), ah = gtk_widget_get_height(pic);
    if (aw < 1 || ah < 1) return;
    /* Map widget coords → Xvfb framebuffer. Always CONTAIN (letterbox-aware)
     * so clicks land correctly when the picture keeps aspect ratio. */
    int tx, ty;
    double scale = MIN(aw / (double)mpcecc_pixel_width, ah / (double)mpcecc_pixel_height);
    if (scale < 1e-6) return;
    double ox = (aw - mpcecc_pixel_width * scale) / 2.0;
    double oy = (ah - mpcecc_pixel_height * scale) / 2.0;
    tx = (int)((x - ox) / scale);
    ty = (int)((y - oy) / scale);
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    if (tx >= mpcecc_pixel_width) tx = mpcecc_pixel_width - 1;
    if (ty >= mpcecc_pixel_height) ty = mpcecc_pixel_height - 1;
    gtk_widget_grab_focus(pic);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (button < 1 || button > 3) button = 1;
    gchar *sx = g_strdup_printf("%d", tx), *sy = g_strdup_printf("%d", ty), *sb = g_strdup_printf("%u", button);
    gchar *argv[] = { "xdotool", "mousemove", sx, sy, "click", sb, NULL };
    GError *error = NULL; gchar **env = g_get_environ();
    env = g_environ_setenv(env, "DISPLAY", mpcecc_pixel_display, TRUE);
    g_spawn_async(NULL, argv, env, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, &error);
    g_strfreev(env); g_clear_error(&error); g_free(sx); g_free(sy); g_free(sb);
}

/* ---- mouse wheel / touchpad scroll forwarding ----
 * The pointer position is tracked with a motion controller, and wheel events
 * become X buttons 4/5 (vertical) and 6/7 (horizontal) at that spot.  Smooth
 * touchpad deltas are accumulated and sent in coalesced batches. */
static double mpcecc_pixel_ptr_x = -1, mpcecc_pixel_ptr_y = -1;
static double mpcecc_pixel_scroll_acc_x = 0, mpcecc_pixel_scroll_acc_y = 0;
static int mpcecc_pixel_scroll_v = 0, mpcecc_pixel_scroll_h = 0;   /* pending clicks, signed */
static guint mpcecc_pixel_scroll_id = 0;

static gboolean mpcecc_pixel_map_xy(double x, double y, int *tx, int *ty)
{
    GtkWidget *pic = mpcecc_pixel_picture ? mpcecc_pixel_picture : mpcecc_active_pixel_surface;
    if (!mpcecc_pixel_display || !pic) return FALSE;
    double aw = gtk_widget_get_width(pic), ah = gtk_widget_get_height(pic);
    if (aw < 1 || ah < 1) return FALSE;
    /* Always CONTAIN mapping (letterbox-aware). */
    double scale = MIN(aw / (double)mpcecc_pixel_width, ah / (double)mpcecc_pixel_height);
    if (scale < 1e-6) return FALSE;
    double ox = (aw - mpcecc_pixel_width * scale) / 2.0;
    double oy = (ah - mpcecc_pixel_height * scale) / 2.0;
    *tx = (int)((x - ox) / scale);
    *ty = (int)((y - oy) / scale);
    if (*tx < 0) *tx = 0;
    if (*ty < 0) *ty = 0;
    if (*tx >= mpcecc_pixel_width) *tx = mpcecc_pixel_width - 1;
    if (*ty >= mpcecc_pixel_height) *ty = mpcecc_pixel_height - 1;
    return TRUE;
}

static gboolean mpcecc_pixel_scroll_flush(gpointer user_data)
{
    (void)user_data;
    mpcecc_pixel_scroll_id = 0;
    int v = mpcecc_pixel_scroll_v, h = mpcecc_pixel_scroll_h, tx, ty;
    mpcecc_pixel_scroll_v = mpcecc_pixel_scroll_h = 0;
    if ((!v && !h) || !mpcecc_pixel_display) return G_SOURCE_REMOVE;
    if (mpcecc_pixel_ptr_x < 0 || !mpcecc_pixel_map_xy(mpcecc_pixel_ptr_x, mpcecc_pixel_ptr_y, &tx, &ty))
        return G_SOURCE_REMOVE;

    gchar *sx = g_strdup_printf("%d", tx), *sy = g_strdup_printf("%d", ty);
    gchar *vn = g_strdup_printf("%d", v < 0 ? -v : v), *hn = g_strdup_printf("%d", h < 0 ? -h : h);
    gchar *argv[20]; int n = 0;
    argv[n++] = "xdotool"; argv[n++] = "mousemove"; argv[n++] = sx; argv[n++] = sy;
    if (v) { argv[n++] = "click"; argv[n++] = "--repeat"; argv[n++] = vn; argv[n++] = (v < 0) ? "4" : "5"; }
    if (h) { argv[n++] = "click"; argv[n++] = "--repeat"; argv[n++] = hn; argv[n++] = (h < 0) ? "6" : "7"; }
    argv[n] = NULL;
    gchar **env = mpcecc_pixel_x_env();
    GError *error = NULL;
    g_spawn_async(NULL, argv, env, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, &error);
    g_strfreev(env); g_clear_error(&error);
    g_free(sx); g_free(sy); g_free(vn); g_free(hn);
    return G_SOURCE_REMOVE;
}

static gboolean mpcecc_pixel_on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer user_data)
{
    GtkWidget *event_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    MpcEccWlid *event_win = mpcecc_wlid_for_picture(event_widget);
    if (event_win) mpcecc_wlid_focus(event_win);
    (void)controller; (void)user_data;
    if (!mpcecc_pixel_display) return FALSE;
    mpcecc_pixel_scroll_acc_x += dx;
    mpcecc_pixel_scroll_acc_y += dy;
    int cy = (int)mpcecc_pixel_scroll_acc_y, cx = (int)mpcecc_pixel_scroll_acc_x;   /* whole steps only */
    mpcecc_pixel_scroll_acc_y -= cy;
    mpcecc_pixel_scroll_acc_x -= cx;
    mpcecc_pixel_scroll_v += cy;
    mpcecc_pixel_scroll_h += cx;
    if ((mpcecc_pixel_scroll_v || mpcecc_pixel_scroll_h) && !mpcecc_pixel_scroll_id)
        mpcecc_pixel_scroll_id = g_timeout_add(25, mpcecc_pixel_scroll_flush, NULL);
    return TRUE;
}

static void mpcecc_pixel_on_motion(GtkEventControllerMotion *controller, double x, double y, gpointer user_data)
{
    GtkWidget *event_widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    MpcEccWlid *event_win = mpcecc_wlid_for_picture(event_widget);
    if (event_win) mpcecc_wlid_focus(event_win);
    (void)controller; (void)user_data;
    mpcecc_pixel_ptr_x = x; mpcecc_pixel_ptr_y = y;
}

static void mpcecc_pixel_attach_wheel(GtkWidget *picture)
{
    GtkEventControllerScroll *scroll = GTK_EVENT_CONTROLLER_SCROLL(
        gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES));
    g_signal_connect(scroll, "scroll", G_CALLBACK(mpcecc_pixel_on_scroll), NULL);
    gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(scroll));
    GtkEventControllerMotion *motion = GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());
    g_signal_connect(motion, "motion", G_CALLBACK(mpcecc_pixel_on_motion), NULL);
    g_signal_connect(motion, "enter", G_CALLBACK(mpcecc_pixel_on_motion), NULL);
    gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(motion));
}

static gboolean mpcecc_pixel_window_closed(GtkWindow *window, gpointer user_data)
{
    (void)window; (void)user_data;
    mpcecc_pixel_close_on_exit = FALSE; mpcecc_pixel_cleanup();
    mpcecc_pixel_picture = NULL; mpcecc_pixel_window = NULL; mpcecc_pixel_terminal = NULL;
    return FALSE;
}

static void
mpcecc_open_command_window(MPCECC *ecc, char **command)
{
    if (!command || !command[0] || !*command[0]) return;
    if (mpcecc_window_size) { unsigned w = 0, h = 0; if (sscanf(mpcecc_window_size, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 64 && w <= 8192 && h <= 8192) { mpcecc_pixel_width = (int)w; mpcecc_pixel_height = (int)h; } }
    if (mpcecc_window_fps) { int n = atoi(mpcecc_window_fps); if (n > 0 && n <= 60) mpcecc_pixel_fps = n; }

    GtkWidget *window = gtk_application_window_new(ecc->app);
    mpcecc_pixel_window = window;

    /* Client-side decorations with a real, draggable LXQt-style title bar */
    gtk_window_set_decorated(GTK_WINDOW(window), TRUE);   /* allow CSD */
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    char *fallback = g_strdup_printf("MPC PixelWindow — %s", command[0]);
    const char *win_title = mpcecc_window_title ? mpcecc_window_title : fallback;
    gtk_window_set_title(GTK_WINDOW(window), win_title);

    /* Custom dark purple header bar – this is the control bar from the sketch */
    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(header), "icon:minimize,maximize,close");
    GtkWidget *title_label = gtk_label_new(win_title);
    gtk_widget_add_css_class(title_label, "title");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title_label);
    gtk_window_set_titlebar(GTK_WINDOW(window), header);

    /* Pure MarekPC purple – hard override so CachyOS Nord cannot touch it */
    {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_string(css,
                                          "window, window.background, window.csd {"
                                          "  background-color: #120a1c;"
                                          "  background-image: none;"
                                          "  border: 1px solid #5a3a8a;"
                                          "  border-radius: 6px;"
                                          "  box-shadow: 0 8px 24px alpha(black, 0.55);"
                                          "}"
                                          "headerbar, headerbar.default-decoration, .titlebar {"
                                          "  background-color: #1a1028;"
                                          "  background-image: none;"
                                          "  color: #e8d8ff;"
                                          "  border: none;"
                                          "  border-bottom: 1px solid #6a4a9a;"
                                          "  min-height: 40px;"
                                          "  padding: 0 8px;"
                                          "  box-shadow: none;"
                                          "  border-radius: 6px 6px 0 0;"
                                          "}"
                                          "headerbar .title, headerbar label {"
                                          "  color: #e8d8ff;"
                                          "  font-weight: 700;"
                                          "  font-size: 13px;"
                                          "  text-shadow: none;"
                                          "}"
                                          "headerbar button, headerbar button.titlebutton,"
                                          "headerbar button.minimize, headerbar button.maximize, headerbar button.close {"
                                          "  color: #d0c0f0;"
                                          "  background-color: transparent;"
                                          "  background-image: none;"
                                          "  border: none;"
                                          "  box-shadow: none;"
                                          "  min-width: 30px;"
                                          "  min-height: 30px;"
                                          "  border-radius: 4px;"
                                          "  padding: 0;"
                                          "}"
                                          "headerbar button:hover, headerbar button.titlebutton:hover {"
                                          "  background-color: #4a3a6a;"
                                          "  color: #ffffff;"
                                          "}"
                                          "headerbar button.close:hover {"
                                          "  background-color: #c04060;"
                                          "  color: #ffffff;"
                                          "}"
                                          "headerbar button:active {"
                                          "  background-color: #3a2a5a;"
                                          "}"
        );
        gtk_style_context_add_provider_for_display(
            gdk_display_get_default(),
                                                   GTK_STYLE_PROVIDER(css),
                                                   GTK_STYLE_PROVIDER_PRIORITY_USER);
        g_object_unref(css);
    }

    g_free(fallback);
    gtk_window_set_default_size(GTK_WINDOW(window), mpcecc_pixel_width, mpcecc_pixel_height);
    g_signal_connect(window, "close-request", G_CALLBACK(mpcecc_pixel_window_closed), NULL);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *terminal = vte_terminal_new();
    gtk_widget_set_hexpand(terminal, TRUE); gtk_widget_set_vexpand(terminal, TRUE);
    mpcecc_pixel_terminal = VTE_TERMINAL(terminal);
    gtk_window_set_child(GTK_WINDOW(window), root);

    if (mpcecc_window_terminal) {
        /* --terminal: plain VTE running the program, no Xvfb needed */
        gtk_box_append(GTK_BOX(root), terminal);
        gchar **environment = g_get_environ();
        environment = g_environ_setenv(environment, "MPCECC", "1", TRUE);
        environment = g_environ_setenv(environment, "MPCECC_MANAGED", "1", TRUE);
        vte_terminal_spawn_async(VTE_TERMINAL(terminal), VTE_PTY_DEFAULT, NULL, command, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, -1, NULL, NULL, NULL);
        g_strfreev(environment); gtk_window_present(GTK_WINDOW(window)); return;
    }

    /* pixel mode: picture on top (aspect-correct contain, never stretch).
     * Window is resizable with LXQt-style minimize/maximize/close. */
    GtkWidget *picture = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(picture, TRUE);
    gtk_widget_set_vexpand(picture, TRUE);
    gtk_widget_set_halign(picture, GTK_ALIGN_FILL);
    gtk_widget_set_valign(picture, GTK_ALIGN_FILL);
    gtk_widget_set_can_focus(picture, TRUE);
    gtk_widget_set_focusable(picture, TRUE);
    mpcecc_pixel_picture = picture;
    gtk_box_append(GTK_BOX(root), picture);
    gtk_widget_set_visible(terminal, FALSE);
    gtk_widget_set_size_request(terminal, -1, 90);
    gtk_widget_set_vexpand(terminal, FALSE);
    gtk_box_append(GTK_BOX(root), terminal);
    mpcecc_pixel_close_on_exit = TRUE;

    /* Keys forwarded to private X display via xdotool so typing works. */
    GtkEventControllerKey *keys = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(keys), GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(mpcecc_pixel_on_key), NULL);
    gtk_widget_add_controller(window, GTK_EVENT_CONTROLLER(keys));
    /* Clicks mapped into Xvfb coordinates (letterbox-aware). */
    GtkGestureClick *click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(mpcecc_pixel_on_click), NULL);
    gtk_widget_add_controller(picture, GTK_EVENT_CONTROLLER(click)); mpcecc_pixel_attach_wheel(picture);
    gtk_window_present(GTK_WINDOW(window));
    gtk_widget_grab_focus(picture);

    const char *tools[] = { "Xvfb", "xdpyinfo", "import", "xdotool", NULL };
    const char *pkgs[] = { "xorg-server-xvfb", "xorg-xdpyinfo", "imagemagick", "xdotool", NULL };
    for (int t = 0; tools[t]; ++t) {
        if (!g_find_program_in_path(tools[t])) {
            char *msg = g_strdup_printf("missing tool '%s' (Arch package: %s)", tools[t], pkgs[t]);
            mpcecc_pixel_feed_error(msg); g_free(msg); return;
        }
    }
    int display_number = -1;
    for (int n = 100; n < 300; ++n) {
        gchar *sock = g_strdup_printf("/tmp/.X11-unix/X%d", n), *lock = g_strdup_printf("/tmp/.X%d-lock", n);
        gboolean used = g_file_test(sock, G_FILE_TEST_EXISTS) || g_file_test(lock, G_FILE_TEST_EXISTS);
        g_free(sock); g_free(lock);
        if (!used) { display_number = n; break; }
    }
    if (display_number < 0) { mpcecc_pixel_feed_error("no free private display number"); return; }
    mpcecc_pixel_display = g_strdup_printf(":%d", display_number);
    mpcecc_pixel_frame_path = g_strdup_printf("/tmp/mpc-pixel-%d.png", (int)getpid());
    gchar *geometry = g_strdup_printf("%dx%dx24", mpcecc_pixel_width, mpcecc_pixel_height);
    gchar *xvfb_args[] = { "Xvfb", mpcecc_pixel_display, "-screen", "0", geometry, "-ac", "-nolisten", "tcp", "+extension", "GLX", "+iglx", "-noreset", NULL };
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL, xvfb_args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &mpcecc_pixel_xvfb_pid, &error);
    g_free(geometry);
    if (!ok) { mpcecc_pixel_feed_error(error ? error->message : "could not start Xvfb; install xorg-server-xvfb"); g_clear_error(&error); mpcecc_pixel_cleanup(); return; }
    g_child_watch_add(mpcecc_pixel_xvfb_pid, mpcecc_pixel_xvfb_exited, NULL);
    mpcecc_pixel_wait_tries = 0;
    mpcecc_pixel_ready_id = g_timeout_add(100, mpcecc_pixel_wait_display, NULL);
}


/* ============================================================
 * FreeDOS-style in-tab pixel embedding + control socket
 * ============================================================ */

static char *
mpcecc_control_sock_path(void)
{
    static char path[4096];
    const char *runtime = g_get_user_runtime_dir();
    if (runtime && *runtime)
        snprintf(path, sizeof(path), "%s/mpcecc-control.sock", runtime);
    else
        snprintf(path, sizeof(path), "/tmp/mpcecc-control-%d.sock", (int)getuid());
    return path;
}

static MpcEccTab *
mpcecc_get_current_tab(void)
{
    if (!mpcecc_main_ecc || !mpcecc_main_ecc->tabs) return NULL;
    int page = gtk_notebook_get_current_page(mpcecc_main_ecc->tabs);
    if (page < 0) return NULL;
    GtkWidget *w = gtk_notebook_get_nth_page(mpcecc_main_ecc->tabs, page);
    if (!w) return NULL;
    return g_object_get_data(G_OBJECT(w), "mpcecc-tab");
}

static void
mpcecc_feed_current_tab(const char *msg)
{
    MpcEccTab *tab = mpcecc_get_current_tab();
    if (!tab || !tab->terminal || !msg) return;
    char *line = g_strdup_printf("\r\n\033[1;35m[mpcecc]\033[0m %s\r\n", msg);
    vte_terminal_feed(tab->terminal, line, -1);
    g_free(line);
}



static void
mpcecc_show_pixels_on_current_tab(void)
{
    MpcEccTab *tab = mpcecc_get_current_tab();
    if (!tab || !tab->pixel_surface) return;
    GtkWidget *picture = g_object_get_data(G_OBJECT(tab->pixel_surface), "mpcecc-pixel-picture");
    if (!picture) picture = tab->pixel_surface;
    mpcecc_active_pixel_surface = picture;
    /* Default NORMAL: fixed-size box you can drag by the titlebar (move, not resize). */
    g_object_set_data(G_OBJECT(tab->pixel_surface), "mpcecc-pw-state",
                      GINT_TO_POINTER(0)); /* NORMAL */
    g_object_set_data(G_OBJECT(tab->pixel_surface), "mpcecc-frame-w",
                      GINT_TO_POINTER(mpcecc_pixel_width > 0 ? MIN(mpcecc_pixel_width, 1920) : 1440));
    g_object_set_data(G_OBJECT(tab->pixel_surface), "mpcecc-frame-h",
                      GINT_TO_POINTER(mpcecc_pixel_height > 0 ? MIN(mpcecc_pixel_height + 36, 1200) : 900));
    g_object_set_data(G_OBJECT(tab->pixel_surface), "mpcecc-pos-x", GINT_TO_POINTER(40));
    g_object_set_data(G_OBJECT(tab->pixel_surface), "mpcecc-pos-y", GINT_TO_POINTER(32));
    {
        GtkWidget *maxbtn = g_object_get_data(G_OBJECT(tab->pixel_surface), "mpcecc-pw-maxbtn");
        if (maxbtn) gtk_button_set_label(GTK_BUTTON(maxbtn), "□");
    }
    mpcecc_pixel_win_apply_state(tab->pixel_surface);
    gtk_widget_set_visible(tab->pixel_surface, TRUE);
    gtk_widget_set_can_target(tab->pixel_surface, TRUE);
    gtk_widget_set_can_target(picture, TRUE);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(tab->container), tab->pixel_surface, FALSE);
    gtk_widget_queue_resize(tab->pixel_surface);
    gtk_widget_queue_draw(tab->pixel_surface);
    gtk_widget_grab_focus(picture);
    mpcecc_feed_current_tab("in-tab PixelWindow shown (FreeDOS style — click/type inside; ✕ closes)");
}


/* In-tab PixelWindow geometry modes (FreeDOS / LXQt-like chrome). */
enum { MPCECC_PW_NORMAL = 0, MPCECC_PW_MAXIMIZED = 1, MPCECC_PW_MINIMIZED = 2 };

static void
mpcecc_pixel_win_apply_state(GtkWidget *frame)
{
    if (!frame) return;
    int state = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state"));
    GtkWidget *picture = g_object_get_data(G_OBJECT(frame), "mpcecc-pixel-picture");
    GtkWidget *body = g_object_get_data(G_OBJECT(frame), "mpcecc-pixel-body");

    /* NORMAL: fixed size, top-left positioned by margins only → titlebar drag MOVES,
     * never resizes. MAXIMIZED: fill overlay. MINIMIZED: title strip only. */
    if (state == MPCECC_PW_MAXIMIZED) {
        gtk_widget_set_size_request(frame, -1, -1);
        gtk_widget_set_margin_start(frame, 0);
        gtk_widget_set_margin_end(frame, 0);
        gtk_widget_set_margin_top(frame, 0);
        gtk_widget_set_margin_bottom(frame, 0);
        gtk_widget_set_halign(frame, GTK_ALIGN_FILL);
        gtk_widget_set_valign(frame, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(frame, TRUE);
        gtk_widget_set_vexpand(frame, TRUE);
        if (body) gtk_widget_set_visible(body, TRUE);
        if (picture) gtk_widget_set_visible(picture, TRUE);
        gtk_widget_remove_css_class(frame, "mpcecc-pixel-normal");
        gtk_widget_add_css_class(frame, "mpcecc-pixel-max");
    } else if (state == MPCECC_PW_MINIMIZED) {
        int ml = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pos-x"));
        int mt = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pos-y"));
        if (ml <= 0) ml = 24;
        if (mt <= 0) mt = 8;
        gtk_widget_set_size_request(frame, 220, -1);
        gtk_widget_set_margin_start(frame, ml);
        gtk_widget_set_margin_end(frame, 0);
        gtk_widget_set_margin_top(frame, mt);
        gtk_widget_set_margin_bottom(frame, 0);
        gtk_widget_set_halign(frame, GTK_ALIGN_START);
        gtk_widget_set_valign(frame, GTK_ALIGN_START);
        gtk_widget_set_hexpand(frame, FALSE);
        gtk_widget_set_vexpand(frame, FALSE);
        if (body) gtk_widget_set_visible(body, FALSE);
        if (picture) gtk_widget_set_visible(picture, FALSE);
        gtk_widget_remove_css_class(frame, "mpcecc-pixel-max");
        gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
    } else { /* NORMAL — fixed box; position = margin-start/top only (move, not resize) */
        int ml = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pos-x"));
        int mt = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pos-y"));
        if (ml <= 0 && mt <= 0) { ml = 40; mt = 32; }
        int fw = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-frame-w"));
        int fh = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-frame-h"));
        if (fw < 320) fw = mpcecc_pixel_width > 0 ? MIN(mpcecc_pixel_width, 1920) : 1440;
        if (fh < 200) fh = mpcecc_pixel_height > 0 ? MIN(mpcecc_pixel_height + 36, 1200) : 900;
        /* titlebar ~28px included in request */
        gtk_widget_set_size_request(frame, fw, fh);
        gtk_widget_set_margin_start(frame, ml);
        gtk_widget_set_margin_end(frame, 0);
        gtk_widget_set_margin_top(frame, mt);
        gtk_widget_set_margin_bottom(frame, 0);
        gtk_widget_set_halign(frame, GTK_ALIGN_START);
        gtk_widget_set_valign(frame, GTK_ALIGN_START);
        gtk_widget_set_hexpand(frame, FALSE);
        gtk_widget_set_vexpand(frame, FALSE);
        if (body) gtk_widget_set_visible(body, TRUE);
        if (picture) gtk_widget_set_visible(picture, TRUE);
        gtk_widget_remove_css_class(frame, "mpcecc-pixel-max");
        gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
        g_object_set_data(G_OBJECT(frame), "mpcecc-pos-x", GINT_TO_POINTER(ml));
        g_object_set_data(G_OBJECT(frame), "mpcecc-pos-y", GINT_TO_POINTER(mt));
        g_object_set_data(G_OBJECT(frame), "mpcecc-frame-w", GINT_TO_POINTER(fw));
        g_object_set_data(G_OBJECT(frame), "mpcecc-frame-h", GINT_TO_POINTER(fh));
    }
    gtk_widget_queue_resize(frame);
    gtk_widget_queue_draw(frame);
}


static void
mpcecc_pixel_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
    (void)gesture; (void)x; (void)y;
    GtkWidget *frame = user_data;
    if (!frame) return;
    /* If maximized, drop to NORMAL first so the drag can move a real box */
    int state = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state"));
    if (state == MPCECC_PW_MAXIMIZED) {
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_NORMAL));
        GtkWidget *maxbtn = g_object_get_data(G_OBJECT(frame), "mpcecc-pw-maxbtn");
        if (maxbtn) gtk_button_set_label(GTK_BUTTON(maxbtn), "□");
        mpcecc_pixel_win_apply_state(frame);
    }
    g_object_set_data(G_OBJECT(frame), "mpcecc-drag-ml",
                      GINT_TO_POINTER(gtk_widget_get_margin_start(frame)));
    g_object_set_data(G_OBJECT(frame), "mpcecc-drag-mt",
                      GINT_TO_POINTER(gtk_widget_get_margin_top(frame)));
}

static void
mpcecc_pixel_drag_update(GtkGestureDrag *gesture, double offset_x, double offset_y, gpointer user_data)
{
    (void)gesture;
    GtkWidget *frame = user_data;
    if (!frame) return;
    int state = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state"));
    if (state == MPCECC_PW_MAXIMIZED) return;

    /* GTK drag offsets are cumulative from drag-begin.  Always derive from the
     *      saved origin; never accumulate deltas (that causes the window to ping-pong). */
    int x0 = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-drag-ml"));
    int y0 = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-drag-mt"));
    int x = MAX(0, x0 + (int)llround(offset_x));
    int y = MAX(0, y0 + (int)llround(offset_y));
    GtkWidget *parent = gtk_widget_get_parent(frame);
    int pw = parent ? gtk_widget_get_width(parent) : 0;
    int ph = parent ? gtk_widget_get_height(parent) : 0;
    int fw = gtk_widget_get_width(frame), fh = gtk_widget_get_height(frame);
    if (pw > 0 && fw > 0) x = MIN(x, MAX(0, pw - MIN(fw, pw)));
    if (ph > 0 && fh > 0) y = MIN(y, MAX(0, ph - MIN(fh, ph)));

    if (gtk_widget_get_margin_start(frame) != x) gtk_widget_set_margin_start(frame, x);
    if (gtk_widget_get_margin_top(frame) != y) gtk_widget_set_margin_top(frame, y);
    g_object_set_data(G_OBJECT(frame), "mpcecc-pos-x", GINT_TO_POINTER(x));
    g_object_set_data(G_OBJECT(frame), "mpcecc-pos-y", GINT_TO_POINTER(y));
}

static void
mpcecc_pixel_resize_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
    (void)gesture; (void)x; (void)y;
    GtkWidget *frame = GTK_WIDGET(user_data);
    if (!frame) return;
    g_object_set_data(G_OBJECT(frame), "mpcecc-resize-w0", GINT_TO_POINTER(gtk_widget_get_width(frame)));
    g_object_set_data(G_OBJECT(frame), "mpcecc-resize-h0", GINT_TO_POINTER(gtk_widget_get_height(frame)));
}

static void
mpcecc_pixel_resize_update(GtkGestureDrag *gesture, double dx, double dy, gpointer user_data)
{
    (void)gesture;
    GtkWidget *frame = GTK_WIDGET(user_data);
    if (!frame || GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state")) != MPCECC_PW_NORMAL)
        return;
    int w0 = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-resize-w0"));
    int h0 = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-resize-h0"));
    int w = CLAMP(w0 + (int)llround(dx), 320, 4096);
    int h = CLAMP(h0 + (int)llround(dy), 220, 2160);
    if (gtk_widget_get_width(frame) != w || gtk_widget_get_height(frame) != h) {
        gtk_widget_set_size_request(frame, w, h);
        g_object_set_data(G_OBJECT(frame), "mpcecc-frame-w", GINT_TO_POINTER(w));
        g_object_set_data(G_OBJECT(frame), "mpcecc-frame-h", GINT_TO_POINTER(h));
        gtk_widget_queue_resize(frame);
    }
}

static void
mpcecc_pixel_win_minimize(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *frame = user_data;
    if (!frame) return;
    int state = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state"));
    GtkWidget *maxbtn = g_object_get_data(G_OBJECT(frame), "mpcecc-pw-maxbtn");
    if (state == MPCECC_PW_MINIMIZED) {
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_NORMAL));
        if (maxbtn) gtk_button_set_label(GTK_BUTTON(maxbtn), "□");
    } else {
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_MINIMIZED));
    }
    mpcecc_pixel_win_apply_state(frame);
}

static void
mpcecc_pixel_win_maximize(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *frame = user_data;
    if (!frame) return;
    int state = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state"));
    GtkWidget *maxbtn = g_object_get_data(G_OBJECT(frame), "mpcecc-pw-maxbtn");
    if (state == MPCECC_PW_MAXIMIZED) {
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_NORMAL));
        if (maxbtn) gtk_button_set_label(GTK_BUTTON(maxbtn), "□");
    } else {
        g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_MAXIMIZED));
        if (maxbtn) gtk_button_set_label(GTK_BUTTON(maxbtn), "❐");
    }
    mpcecc_pixel_win_apply_state(frame);
}

static void
mpcecc_pixel_win_restore(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *frame = user_data;
    if (!frame) return;
    g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_NORMAL));
    mpcecc_pixel_win_apply_state(frame);
}

static void
mpcecc_hide_pixels_clicked(GtkButton *button, gpointer user_data)
{
    (void)button;
    GtkWidget *frame = GTK_WIDGET(user_data);
    if (frame) {
        int id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-wlid"));
        MpcEccWlid *win = mpcecc_wlid_find(id);
        if (win) {
            win->terminating = TRUE;
            if (win->motion_id) { g_source_remove(win->motion_id); win->motion_id = 0; }
            mpcecc_wlid_drop_input(win);
            if (win->capture_id) { g_source_remove(win->capture_id); win->capture_id = 0; }
            if (win->app_pid) kill((pid_t)win->app_pid, SIGTERM);
            if (win->xvfb_pid) kill((pid_t)win->xvfb_pid, SIGTERM);
            if (win->frame) gtk_widget_set_visible(win->frame, FALSE);
            if (win->frame_path) unlink(win->frame_path);
            mpcecc_wlid_list = g_list_remove(mpcecc_wlid_list, win);
            if (mpcecc_focused_wlid == id) mpcecc_focused_wlid = 0;
            /* Child-watch callbacks use only the numeric ID, so freeing here is safe. */
            mpcecc_wlid_free(win);
            return;
        }
    }
    mpcecc_hide_pixels_on_current_tab();
}

static void
mpcecc_hide_pixels_on_current_tab(void)
{
    MpcEccTab *tab = mpcecc_get_current_tab();
    if (tab && tab->pixel_surface)
        gtk_widget_set_visible(tab->pixel_surface, FALSE);
    mpcecc_active_pixel_surface = NULL;
    mpcecc_pixel_cleanup();
    mpcecc_feed_current_tab("PixelWindow closed");
}

/* Redirect capture output to the in-tab surface instead of a separate window */
static void
mpcecc_in_tab_capture_done(GPid pid, gint status, gpointer user_data)
{
    (void)user_data;
    g_spawn_close_pid(pid);
    mpcecc_pixel_capture_busy = FALSE;
    if (!mpcecc_pixel_frame_path || !mpcecc_active_pixel_surface) {
        mpcecc_feed_current_tab("capture: no surface or path");
        return;
    }
    GError *error = NULL;
    if (!g_spawn_check_wait_status(status, &error)) {
        char *m = g_strdup_printf("import failed: %s", error ? error->message : "bad status");
        mpcecc_feed_current_tab(m); g_free(m);
        g_clear_error(&error);
        return;
    }
    if (!g_file_test(mpcecc_pixel_frame_path, G_FILE_TEST_EXISTS)) {
        mpcecc_feed_current_tab("frame file missing after import");
        return;
    }
    GdkTexture *tex = gdk_texture_new_from_filename(mpcecc_pixel_frame_path, &error);
    if (!tex) {
        char *m = g_strdup_printf("texture load failed: %s", error ? error->message : "?");
        mpcecc_feed_current_tab(m); g_free(m);
        g_clear_error(&error);
        return;
    }
    gtk_picture_set_paintable(GTK_PICTURE(mpcecc_active_pixel_surface), GDK_PAINTABLE(tex));
    g_object_unref(tex);
    gtk_widget_set_visible(mpcecc_active_pixel_surface, TRUE);
    gtk_widget_queue_draw(mpcecc_active_pixel_surface);
    static int once = 0;
    if (!once) { once = 1; mpcecc_feed_current_tab("first frame painted on overlay"); }
}

static gboolean
mpcecc_in_tab_capture_tick(gpointer user_data)
{
    (void)user_data;
    if (!mpcecc_pixel_display || !mpcecc_pixel_frame_path) {
        mpcecc_pixel_capture_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (mpcecc_pixel_capture_busy) return G_SOURCE_CONTINUE;
    gchar *argv[] = {
        "import", "-silent", "-display", mpcecc_pixel_display,
        "-window", "root", mpcecc_pixel_frame_path, NULL
    };
    GError *error = NULL;
    GPid pid = 0;
    gchar **cenv = g_get_environ();
    cenv = g_environ_setenv(cenv, "DISPLAY", mpcecc_pixel_display, TRUE);
    gboolean ok = g_spawn_async(NULL, argv, cenv,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, &pid, &error);
    g_strfreev(cenv);
    if (!ok) { g_clear_error(&error); return G_SOURCE_CONTINUE; }
    mpcecc_pixel_capture_busy = TRUE;
    g_child_watch_add(pid, mpcecc_in_tab_capture_done, NULL);
    return G_SOURCE_CONTINUE;
}

static gboolean
mpcecc_embed_delayed_capture(gpointer user_data)
{
    (void)user_data;
    if (mpcecc_pixel_app_pid && !mpcecc_pixel_capture_id)
        mpcecc_pixel_capture_id = g_timeout_add(1000 / mpcecc_pixel_fps, mpcecc_in_tab_capture_tick, NULL);
    return G_SOURCE_REMOVE;
}

static void
mpcecc_embed_in_tab(char **command, int width, int height, int fps)
{
    if (!command || !command[0]) return;
    if (width >= 64 && height >= 64) {
        mpcecc_pixel_width = width;
        mpcecc_pixel_height = height;
    }
    if (fps > 0 && fps <= 60) mpcecc_pixel_fps = fps;

    /* tools check */
    const char *tools[] = { "Xvfb", "xdpyinfo", "import", "xdotool", NULL };
    for (int t = 0; tools[t]; ++t) {
        if (!g_find_program_in_path(tools[t])) {
            char *m = g_strdup_printf("missing tool: %s", tools[t]); mpcecc_feed_current_tab(m); g_free(m);
            return;
        }
    }

    int display_number = -1;
    for (int n = 100; n < 300; ++n) {
        gchar *sock = g_strdup_printf("/tmp/.X11-unix/X%d", n);
        gchar *lock = g_strdup_printf("/tmp/.X%d-lock", n);
        gboolean used = g_file_test(sock, G_FILE_TEST_EXISTS) || g_file_test(lock, G_FILE_TEST_EXISTS);
        g_free(sock); g_free(lock);
        if (!used) { display_number = n; break; }
    }
    if (display_number < 0) {
        mpcecc_feed_current_tab("no free private display");
        return;
    }

    mpcecc_pixel_cleanup(); /* stop any previous */
    mpcecc_pixel_display = g_strdup_printf(":%d", display_number);
    mpcecc_pixel_frame_path = g_strdup_printf("/tmp/mpc-pixel-%d.png", (int)getpid());

    gchar *geometry = g_strdup_printf("%dx%dx24", mpcecc_pixel_width, mpcecc_pixel_height);
    gchar *xvfb_args[] = { "Xvfb", mpcecc_pixel_display, "-screen", "0", geometry, "-ac", "-nolisten", "tcp", "+extension", "GLX", "+iglx", "-noreset", NULL };
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL, xvfb_args, NULL,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, &mpcecc_pixel_xvfb_pid, &error);
    g_free(geometry);
    if (!ok) {
        char *m = g_strdup_printf("Xvfb failed: %s", error ? error->message : "?"); mpcecc_feed_current_tab(m); g_free(m);
        g_clear_error(&error);
        mpcecc_pixel_cleanup();
        return;
    }

    /* wait for display then start app */
    for (int i = 0; i < 50; ++i) {
        g_usleep(100000);
        gchar *argv[] = { "xdpyinfo", "-display", mpcecc_pixel_display, NULL };
        gint status = 0;
        if (g_spawn_sync(NULL, argv, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, NULL, NULL, &status, NULL) && status == 0)
            break;
    }

    /* clean env + start app (reuse the hardened start logic by calling the existing function after setting globals) */
    /* We set the window command and call the existing start */
    mpcecc_window_command = command;
    /* Use a simplified start here */
    char runtime[256];
    snprintf(runtime, sizeof(runtime), "/tmp/mpc-runtime-%d", (int)getuid());
    g_mkdir_with_parents(runtime, 0700);
    const char *home = g_get_home_dir() ? g_get_home_dir() : "/tmp";
    gchar **environment = NULL;
    environment = g_environ_setenv(environment, "DISPLAY", mpcecc_pixel_display, TRUE);
    environment = g_environ_setenv(environment, "HOME", home, TRUE);
    environment = g_environ_setenv(environment, "USER", g_get_user_name() ? g_get_user_name() : "user", TRUE);
    environment = g_environ_setenv(environment, "PATH", "/usr/local/bin:/usr/bin:/bin", TRUE);
    environment = g_environ_setenv(environment, "LANG", "C.UTF-8", TRUE);
    environment = g_environ_setenv(environment, "GDK_BACKEND", "x11", TRUE);
    environment = g_environ_setenv(environment, "QT_QPA_PLATFORM", "xcb", TRUE);
    environment = g_environ_setenv(environment, "MOZ_ENABLE_WAYLAND", "0", TRUE);
    environment = g_environ_setenv(environment, "MOZ_DISABLE_WAYLAND", "1", TRUE);
    environment = g_environ_setenv(environment, "XDG_SESSION_TYPE", "x11", TRUE);
    environment = g_environ_setenv(environment, "XDG_RUNTIME_DIR", runtime, TRUE);
    environment = g_environ_setenv(environment, "MPCECC_PIXEL_WINDOW", "1", TRUE);
    environment = g_environ_setenv(environment, "LIBGL_ALWAYS_SOFTWARE", "1", TRUE);
    environment = g_environ_setenv(environment, "GALLIUM_DRIVER", "llvmpipe", TRUE);
    environment = g_environ_setenv(environment, "QT_XCB_GL_INTEGRATION", "none", TRUE);

    /* Firefox: isolated MPCECCWLPROFILE only — never the real default. */
    mpcecc_pixel_set_class(command ? command[0] : NULL);
    char **launch_argv = command;
    char **owned_argv = mpcecc_firefox_isolated_argv(command, home);
    if (owned_argv)
        launch_argv = owned_argv;

    ok = g_spawn_async(NULL, launch_argv, environment,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CHILD_INHERITS_STDIN,
                       NULL, NULL, &mpcecc_pixel_app_pid, &error);
    if (owned_argv) {
        for (int i = 0; owned_argv[i]; ++i)
            g_free(owned_argv[i]);
        g_free(owned_argv);
    }
    g_strfreev(environment);
    if (!ok) {
        char *m = g_strdup_printf("app start failed: %s", error ? error->message : "?"); mpcecc_feed_current_tab(m); g_free(m);
        g_clear_error(&error);
        mpcecc_pixel_cleanup();
        return;
    }

    {
        MpcEccTab *tab = mpcecc_get_current_tab();
        if (tab && tab->pixel_surface && command && command[0]) {
            GtkWidget *title = g_object_get_data(G_OBJECT(tab->pixel_surface), "mpcecc-pixel-title");
            if (title) {
                const char *base = strrchr(command[0], '/');
                base = base ? base + 1 : command[0];
                char buf[128];
                snprintf(buf, sizeof buf, "%s — PixelWindow", base);
                gtk_label_set_text(GTK_LABEL(title), buf);
            }
        }
    }
    mpcecc_show_pixels_on_current_tab();
    g_timeout_add_seconds(1, mpcecc_embed_delayed_capture, NULL);
    mpcecc_feed_current_tab("Xvfb+app in-tab (FreeDOS style), capture in 1s…");
}




/* Each WLID owns an independent Xvfb, application, frame file and capture timer.
 * IMPORTANT: capture is asynchronous. Never run ImageMagick `import` with
 * g_spawn_sync() from GTK's main loop; it can freeze the entire MPCECC UI. */
static void mpcecc_wlid_capture_done(GPid pid, gint status, gpointer data)
{
    guint id = GPOINTER_TO_UINT(data);
    g_spawn_close_pid(pid);
    MpcEccWlid *win = mpcecc_wlid_find((int)id);
    if (!win) return;
    win->capture_busy = FALSE;
    if (win->terminating || !win->frame_path || !win->picture) return;
    if (status != 0 || !g_file_test(win->frame_path, G_FILE_TEST_EXISTS)) return;
    GError *error = NULL;
    GdkTexture *tex = gdk_texture_new_from_filename(win->frame_path, &error);
    if (tex) {
        gtk_picture_set_paintable(GTK_PICTURE(win->picture), GDK_PAINTABLE(tex));
        g_object_unref(tex);
        if (win->frame) gtk_widget_set_visible(win->frame, TRUE);
    }
    g_clear_error(&error);
}

static gboolean mpcecc_wlid_capture_tick(gpointer data)
{
    MpcEccWlid *win = data;
    if (!win || win->terminating || !win->display || !win->frame_path || !win->picture) {
        if (win) win->capture_id = 0;
        return G_SOURCE_REMOVE;
    }
    if (win->capture_busy) return G_SOURCE_CONTINUE;
    gchar *argv[] = { "import", "-silent", "-display", win->display,
        "-window", "root", win->frame_path, NULL };
        gchar **env = g_get_environ();
        env = g_environ_setenv(env, "DISPLAY", win->display, TRUE);
        GPid pid = 0;
        GError *error = NULL;
        gboolean ok = g_spawn_async(NULL, argv, env,
                                    G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                    G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                    NULL, NULL, &pid, &error);
        g_strfreev(env);
        if (!ok) {
            g_clear_error(&error);
            return G_SOURCE_CONTINUE;
        }
        win->capture_busy = TRUE;
        g_child_watch_add(pid, mpcecc_wlid_capture_done, GUINT_TO_POINTER((guint)win->wlid));
        return G_SOURCE_CONTINUE;
}

static void mpcecc_wlid_child_exited(GPid pid, gint status, gpointer data)
{
    (void)status;
    guint encoded = GPOINTER_TO_UINT(data);
    gboolean is_xvfb = (encoded & 0x40000000u) != 0;
    int id = (int)(encoded & 0x3fffffffu);
    g_spawn_close_pid(pid);
    MpcEccWlid *win = mpcecc_wlid_find(id);
    if (!win) return;
    if (is_xvfb) {
        if (win->xvfb_pid == pid) win->xvfb_pid = 0;
        mpcecc_wlid_drop_input(win);
        return;
    }
    if (win->app_pid != pid) return;
    win->app_pid = 0;
    if (win->capture_id) { g_source_remove(win->capture_id); win->capture_id = 0; }
    mpcecc_wlid_drop_input(win);
    if (win->xvfb_pid) { kill((pid_t)win->xvfb_pid, SIGTERM); win->xvfb_pid = 0; }
    if (win->frame) gtk_widget_set_visible(win->frame, FALSE);
    if (win->frame_path) unlink(win->frame_path);
    mpcecc_wlid_list = g_list_remove(mpcecc_wlid_list, win);
    if (mpcecc_focused_wlid == id) {
        mpcecc_focused_wlid = 0;
        if (mpcecc_wlid_list) mpcecc_wlid_focus((MpcEccWlid *)mpcecc_wlid_list->data);
        else { g_clear_pointer(&mpcecc_pixel_display, g_free); mpcecc_active_pixel_surface = NULL; }
    }
    mpcecc_wlid_free(win);
}

/* Low-latency XTest input.  Load libXtst dynamically so the source still
 * compiles without XTest development headers or a pkg-config file. */
typedef Bool (*MpcXTestFakeMotionEvent)(Display *, int, int, int, unsigned long);
typedef Bool (*MpcXTestFakeButtonEvent)(Display *, unsigned int, Bool, unsigned long);
static void *mpcecc_xtst_handle = NULL;
static MpcXTestFakeMotionEvent mpcecc_xtest_motion = NULL;
static MpcXTestFakeButtonEvent mpcecc_xtest_button = NULL;
static gboolean mpcecc_xtest_loaded = FALSE;

static gboolean mpcecc_xtest_load(void)
{
    if (mpcecc_xtest_loaded) return mpcecc_xtest_motion && mpcecc_xtest_button;
    mpcecc_xtest_loaded = TRUE;
    mpcecc_xtst_handle = dlopen("libXtst.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!mpcecc_xtst_handle) return FALSE;
    mpcecc_xtest_motion = (MpcXTestFakeMotionEvent)dlsym(mpcecc_xtst_handle, "XTestFakeMotionEvent");
    mpcecc_xtest_button = (MpcXTestFakeButtonEvent)dlsym(mpcecc_xtst_handle, "XTestFakeButtonEvent");
    return mpcecc_xtest_motion && mpcecc_xtest_button;
}

static gboolean mpcecc_wlid_map_pointer(MpcEccWlid *win, GtkWidget *pic,
                                        double x, double y, int *tx, int *ty)
{
    if (!win || !pic || !tx || !ty) return FALSE;
    double aw = gtk_widget_get_width(pic), ah = gtk_widget_get_height(pic);
    if (aw < 1.0 || ah < 1.0) return FALSE;
    /* CONTAIN (letterbox-aware) mapping so clicks match the non-stretched image. */
    double scale = MIN(aw / (double)win->width, ah / (double)win->height);
    if (scale < 1e-6) return FALSE;
    double ox = (aw - win->width * scale) / 2.0;
    double oy = (ah - win->height * scale) / 2.0;
    *tx = (int)((x - ox) / scale);
    *ty = (int)((y - oy) / scale);
    *tx = CLAMP(*tx, 0, win->width - 1);
    *ty = CLAMP(*ty, 0, win->height - 1);
    return TRUE;
}

static void mpcecc_wlid_send_motion(MpcEccWlid *win, int x, int y)
{
    if (!win || win->terminating || !win->input_display || !mpcecc_xtest_load()) return;
    mpcecc_xtest_motion(win->input_display, 0, x, y, 0);
    XFlush(win->input_display);
}

static gboolean mpcecc_wlid_motion_flush(gpointer data)
{
    MpcEccWlid *win = data;
    if (!win) return G_SOURCE_REMOVE;
    win->motion_id = 0;
    int x = (int)win->pending_x, y = (int)win->pending_y;
    mpcecc_wlid_send_motion(win, x, y);
    return G_SOURCE_REMOVE;
}

static void mpcecc_wlid_picture_motion(GtkEventControllerMotion *controller,
                                       double x, double y, gpointer data)
{
    (void)data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    MpcEccWlid *win = mpcecc_wlid_for_picture(pic);
    int tx, ty;
    if (!win || !mpcecc_wlid_map_pointer(win, pic, x, y, &tx, &ty)) return;
    win->pending_x = tx; win->pending_y = ty;
    /* Drag motion must be responsive but don't spawn a process per pointer event. */
    if (win->mouse_buttons) {
        if (!win->motion_id) win->motion_id = g_timeout_add(8, mpcecc_wlid_motion_flush, win);
    }
}

static void mpcecc_wlid_picture_pressed(GtkGestureClick *gesture, int n_press,
                                        double x, double y, gpointer data)
{
    (void)n_press; (void)data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    MpcEccWlid *win = mpcecc_wlid_for_picture(pic);
    int tx, ty;
    if (!win || !mpcecc_wlid_map_pointer(win, pic, x, y, &tx, &ty) ||
        !win->input_display || !mpcecc_xtest_load()) return;
    mpcecc_wlid_focus(win);
    gtk_widget_grab_focus(pic);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (button < 1 || button > 3) button = 1;
    mpcecc_wlid_send_motion(win, tx, ty);
    mpcecc_xtest_button(win->input_display, button, True, 0);
    XFlush(win->input_display);
    win->mouse_buttons |= (1u << button);
}

static void mpcecc_wlid_picture_released(GtkGestureClick *gesture, int n_press,
                                         double x, double y, gpointer data)
{
    (void)n_press; (void)data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    MpcEccWlid *win = mpcecc_wlid_for_picture(pic);
    int tx, ty;
    if (!win || !win->input_display || !mpcecc_xtest_load()) return;
    if (mpcecc_wlid_map_pointer(win, pic, x, y, &tx, &ty))
        mpcecc_wlid_send_motion(win, tx, ty);
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
    if (button < 1 || button > 3) button = 1;
    mpcecc_xtest_button(win->input_display, button, False, 0);
    XFlush(win->input_display);
    win->mouse_buttons &= ~(1u << button);
}

/* Per-application PixelWindow overlay, inside the existing selected tab. */
static gboolean mpcecc_wlid_create_surface(MpcEccWlid *win)
{
    if (!win || !win->tab || !win->tab->container) return FALSE;
    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(frame, "mpcecc-pixel-frame");
    gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
    gtk_widget_set_halign(frame, GTK_ALIGN_START);
    gtk_widget_set_valign(frame, GTK_ALIGN_START);
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(titlebar, "mpcecc-pixel-titlebar");
    GtkWidget *icon = gtk_label_new("▣"); gtk_widget_add_css_class(icon, "mpcecc-pixel-icon");
    char *caption = g_strdup_printf("wlid %d · %s", win->wlid, win->command ? win->command : "app");
    GtkWidget *title = gtk_label_new(caption); g_free(caption);
    gtk_widget_set_hexpand(title, TRUE); gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_widget_add_css_class(title, "mpcecc-pixel-title");
    g_object_set_data(G_OBJECT(frame), "mpcecc-pixel-title", title);
    GtkWidget *mn = gtk_button_new_with_label("–"), *mx = gtk_button_new_with_label("□"), *cl = gtk_button_new_with_label("×");
    gtk_widget_add_css_class(mn, "mpcecc-pixel-btn"); gtk_widget_add_css_class(mx, "mpcecc-pixel-btn");
    gtk_widget_add_css_class(cl, "mpcecc-pixel-btn"); gtk_widget_add_css_class(cl, "mpcecc-pixel-close");
    g_object_set_data(G_OBJECT(frame), "mpcecc-pw-maxbtn", mx);
    g_signal_connect(mn, "clicked", G_CALLBACK(mpcecc_pixel_win_minimize), frame);
    g_signal_connect(mx, "clicked", G_CALLBACK(mpcecc_pixel_win_maximize), frame);
    g_signal_connect(cl, "clicked", G_CALLBACK(mpcecc_hide_pixels_clicked), frame);
    gtk_box_append(GTK_BOX(titlebar), icon); gtk_box_append(GTK_BOX(titlebar), title);
    gtk_box_append(GTK_BOX(titlebar), mn); gtk_box_append(GTK_BOX(titlebar), mx); gtk_box_append(GTK_BOX(titlebar), cl);
    GtkGestureDrag *drag = GTK_GESTURE_DRAG(gtk_gesture_drag_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(mpcecc_pixel_drag_begin), frame);
    g_signal_connect(drag, "drag-update", G_CALLBACK(mpcecc_pixel_drag_update), frame);
    gtk_widget_add_controller(titlebar, GTK_EVENT_CONTROLLER(drag));
    GtkWidget *body = gtk_overlay_new();
    gtk_widget_set_hexpand(body, TRUE); gtk_widget_set_vexpand(body, TRUE);
    g_object_set_data(G_OBJECT(frame), "mpcecc-pixel-body", body);
    GtkWidget *pic = gtk_picture_new();
    /* CONTAIN keeps the Xvfb framebuffer aspect ratio so app UIs
     * (Krita toolbars, browser chrome, etc.) are not stretched or cropped. */
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_hexpand(pic, TRUE); gtk_widget_set_vexpand(pic, TRUE);
    gtk_widget_set_can_focus(pic, TRUE); gtk_widget_set_focusable(pic, TRUE);
    g_object_set_data(G_OBJECT(frame), "mpcecc-pixel-picture", pic);
    GtkGestureClick *click = GTK_GESTURE_CLICK(gtk_gesture_click_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(mpcecc_wlid_picture_pressed), NULL);
    g_signal_connect(click, "released", G_CALLBACK(mpcecc_wlid_picture_released), NULL);
    gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(click));
    mpcecc_pixel_attach_wheel(pic);
    GtkEventControllerMotion *pointer_motion = GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());
    g_signal_connect(pointer_motion, "motion", G_CALLBACK(mpcecc_wlid_picture_motion), NULL);
    gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(pointer_motion));
    GtkEventControllerKey *keys = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(keys), GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(mpcecc_pixel_on_key), NULL);
    gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(keys));
    gtk_overlay_set_child(GTK_OVERLAY(body), pic);
    /* A real bottom-right resize grip; its gesture owns only this small area,
     *      so it doesn't steal normal painting input from the application image. */
    GtkWidget *grip = gtk_label_new("◢");
    gtk_widget_add_css_class(grip, "mpcecc-pixel-resize-grip");
    gtk_widget_set_halign(grip, GTK_ALIGN_END);
    gtk_widget_set_valign(grip, GTK_ALIGN_END);
    gtk_widget_set_size_request(grip, 20, 20);
    GtkGestureDrag *resize_drag = GTK_GESTURE_DRAG(gtk_gesture_drag_new());
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(resize_drag), 1);
    g_signal_connect(resize_drag, "drag-begin", G_CALLBACK(mpcecc_pixel_resize_begin), frame);
    g_signal_connect(resize_drag, "drag-update", G_CALLBACK(mpcecc_pixel_resize_update), frame);
    gtk_widget_add_controller(grip, GTK_EVENT_CONTROLLER(resize_drag));
    gtk_overlay_add_overlay(GTK_OVERLAY(body), grip);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(body), grip, FALSE);
    gtk_box_append(GTK_BOX(frame), titlebar); gtk_box_append(GTK_BOX(frame), body);
    win->frame = frame; win->picture = pic;
    g_object_set_data(G_OBJECT(frame), "mpcecc-wlid", GINT_TO_POINTER(win->wlid));
    g_object_set_data(G_OBJECT(frame), "mpcecc-pw-state", GINT_TO_POINTER(MPCECC_PW_NORMAL));
    g_object_set_data(G_OBJECT(frame), "mpcecc-frame-w", GINT_TO_POINTER(MIN(win->width, 1920)));
    g_object_set_data(G_OBJECT(frame), "mpcecc-frame-h", GINT_TO_POINTER(MIN((int)(MIN(win->width, 1920) * ((double)win->height / MAX(1, win->width))) + 36, 1200)));
    g_object_set_data(G_OBJECT(frame), "mpcecc-pos-x", GINT_TO_POINTER(40 + ((win->wlid - 1) % 6) * 28));
    g_object_set_data(G_OBJECT(frame), "mpcecc-pos-y", GINT_TO_POINTER(32 + ((win->wlid - 1) % 6) * 28));
    gtk_overlay_add_overlay(GTK_OVERLAY(win->tab->container), frame);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(win->tab->container), frame, FALSE);
    mpcecc_pixel_win_apply_state(frame); gtk_widget_set_visible(frame, TRUE);
    return TRUE;
}

/* Make the application's top-level X11 window occupy its entire private Xvfb
 * screen.  Without a window manager X11 applications often choose a smaller
 * default size, leaving the rest of the captured root window black.  Run this
 * helper asynchronously so it never stalls GTK's main loop. */
static gboolean mpcecc_wlid_fit_app(gpointer data)
{
    int wlid = GPOINTER_TO_INT(data);
    MpcEccWlid *win = mpcecc_wlid_find(wlid);
    if (!win || win->terminating || !win->display || !win->app_pid) return G_SOURCE_REMOVE;

    gchar *script = g_strdup_printf(
        "d=%s; w=%d; h=%d; "
        "for n in $(seq 1 40); do "
        "ids=$(xdotool search --sync --onlyvisible --name . 2>/dev/null); "
        "[ -n \"$ids\" ] && break; sleep 0.1; done; "
        "id=$(printf '%%s\n' \"$ids\" | tail -n 1); "
        "[ -n \"$id\" ] || exit 0; "
        "xdotool windowraise --sync \"$id\" 2>/dev/null; "
        "xdotool windowmove --sync \"$id\" 0 0 2>/dev/null; "
        "xdotool windowsize --sync \"$id\" \"$w\" \"$h\" 2>/dev/null; "
        "xdotool windowfocus --sync \"$id\" 2>/dev/null",
        win->display, win->width, win->height);
    gchar *argv[] = { "/bin/sh", "-c", script, NULL };
    gchar **env = g_get_environ();
    env = g_environ_setenv(env, "DISPLAY", win->display, TRUE);
    GPid helper_pid = 0;
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL, argv, env,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, &helper_pid, &error);
    if (ok) g_spawn_close_pid(helper_pid);
    g_clear_error(&error);
    g_strfreev(env);
    g_free(script);
    return G_SOURCE_REMOVE;
}

static gboolean mpcecc_wlid_start(MpcEccWlid *win, char **command)
{
    if (!win || !command || !command[0] || !win->tab || !win->tab->container) return FALSE;
    const char *tools[] = { "Xvfb", "xdpyinfo", "import", "xdotool", NULL };
    for (int i = 0; tools[i]; ++i) {
        if (!g_find_program_in_path(tools[i])) {
            char *msg = g_strdup_printf("windowlaunch: missing tool: %s", tools[i]);
            if (win->tab->terminal) { char *line = g_strdup_printf("\r\n[mpcecc] %s\r\n", msg); vte_terminal_feed(win->tab->terminal, line, -1); g_free(line); }
            g_free(msg); return FALSE;
        }
    }
    int dn = -1;
    for (int n = 100; n < 300; ++n) {
        gchar *sock = g_strdup_printf("/tmp/.X11-unix/X%d", n);
        gchar *lock = g_strdup_printf("/tmp/.X%d-lock", n);
        gboolean used = g_file_test(sock, G_FILE_TEST_EXISTS) || g_file_test(lock, G_FILE_TEST_EXISTS);
        g_free(sock); g_free(lock);
        if (!used) { dn = n; break; }
    }
    if (dn < 0) return FALSE;
    win->display = g_strdup_printf(":%d", dn);
    win->frame_path = g_strdup_printf("/tmp/mpcecc-pixel-%d-%d.ppm", (int)getpid(), win->wlid);
    gchar *geom = g_strdup_printf("%dx%dx24", win->width, win->height);
    gchar *xargv[] = { "Xvfb", win->display, "-screen", "0", geom, "-ac", "-nolisten", "tcp", "+extension", "GLX", "+iglx", "-noreset", NULL };
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL, xargv, NULL,
                                G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL, NULL, &win->xvfb_pid, &error);
    g_free(geom);
    if (!ok) { g_clear_error(&error); return FALSE; }
    g_child_watch_add(win->xvfb_pid, mpcecc_wlid_child_exited, GUINT_TO_POINTER(0x40000000u | (guint)win->wlid));
    gboolean ready = FALSE;
    for (int i = 0; i < 50; ++i) {
        g_usleep(100000);
        gchar *dargv[] = { "xdpyinfo", "-display", win->display, NULL };
        gint st = 0;
        if (g_spawn_sync(NULL, dargv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
            NULL, NULL, NULL, NULL, &st, NULL) && st == 0) { ready = TRUE; break; }
    }
    if (!ready) { kill((pid_t)win->xvfb_pid, SIGTERM); return FALSE; }
    win->input_display = XOpenDisplay(win->display);
    if (!win->input_display || !mpcecc_xtest_load()) {
        if (win->input_display) { XCloseDisplay(win->input_display); win->input_display = NULL; }
        if (win->tab && win->tab->terminal) vte_terminal_feed(win->tab->terminal, "\r\n[mpcecc] libXtst.so.6 unavailable; mouse drag forwarding disabled\r\n", -1);
    }
    char runtime[256]; snprintf(runtime, sizeof runtime, "/tmp/mpc-runtime-%d", (int)getuid());
    g_mkdir_with_parents(runtime, 0700);
    const char *home = g_get_home_dir() ? g_get_home_dir() : "/tmp";
    gchar **env = NULL;
    env = g_environ_setenv(env, "DISPLAY", win->display, TRUE);
    env = g_environ_setenv(env, "HOME", home, TRUE);
    env = g_environ_setenv(env, "USER", g_get_user_name() ? g_get_user_name() : "user", TRUE);
    env = g_environ_setenv(env, "PATH", "/usr/local/bin:/usr/bin:/bin", TRUE);
    env = g_environ_setenv(env, "LANG", "C.UTF-8", TRUE);
    env = g_environ_setenv(env, "GDK_BACKEND", "x11", TRUE);
    env = g_environ_setenv(env, "QT_QPA_PLATFORM", "xcb", TRUE);
    env = g_environ_setenv(env, "MOZ_ENABLE_WAYLAND", "0", TRUE);
    env = g_environ_setenv(env, "MOZ_DISABLE_WAYLAND", "1", TRUE);
    env = g_environ_setenv(env, "XDG_SESSION_TYPE", "x11", TRUE);
    env = g_environ_setenv(env, "XDG_RUNTIME_DIR", runtime, TRUE);
    env = g_environ_setenv(env, "MPCECC_PIXEL_WINDOW", "1", TRUE);
    /* Software GL: Krita/GIMP/etc. die hard when they hit a real GLX path
     * against a headless Xvfb. Mesa llvmpipe keeps the private X alive. */
    env = g_environ_setenv(env, "LIBGL_ALWAYS_SOFTWARE", "1", TRUE);
    env = g_environ_setenv(env, "GALLIUM_DRIVER", "llvmpipe", TRUE);
    env = g_environ_setenv(env, "QT_XCB_GL_INTEGRATION", "none", TRUE);
    mpcecc_pixel_set_class(command[0]);
    char **owned = mpcecc_firefox_isolated_argv(command, home);
    ok = g_spawn_async(NULL, owned ? owned : command, env,
                       G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CHILD_INHERITS_STDIN,
                       NULL, NULL, &win->app_pid, &error);
    if (owned) { for (int i=0; owned[i]; ++i) g_free(owned[i]); g_free(owned); }
    g_strfreev(env);
    if (!ok) { g_clear_error(&error); kill((pid_t)win->xvfb_pid, SIGTERM); win->xvfb_pid = 0; return FALSE; }
    g_child_watch_add(win->app_pid, mpcecc_wlid_child_exited, GUINT_TO_POINTER((guint)win->wlid));
    if (!mpcecc_wlid_create_surface(win)) {
        kill((pid_t)win->app_pid, SIGTERM); kill((pid_t)win->xvfb_pid, SIGTERM);
        win->app_pid = win->xvfb_pid = 0; return FALSE;
    }
    /* Wait briefly for the real app window to appear, then size it to the
     * virtual screen so the capture contains the app rather than a small
     * centered window surrounded by black pixels. Pass the ID, not a raw
     * pointer, because the app may exit before this timeout fires. */
    g_timeout_add(1200, mpcecc_wlid_fit_app, GINT_TO_POINTER(win->wlid));
    GtkWidget *frame = win->frame;
    gtk_widget_set_can_target(frame, TRUE); gtk_widget_set_can_target(win->picture, TRUE);
    gtk_widget_grab_focus(win->picture);
    mpcecc_active_pixel_surface = win->picture;
    g_free(mpcecc_pixel_display);
    mpcecc_pixel_display = g_strdup(win->display);
    mpcecc_pixel_width = win->width; mpcecc_pixel_height = win->height; mpcecc_pixel_fps = win->fps;
    mpcecc_pixel_set_class(command[0]);
    mpcecc_focused_wlid = win->wlid;
    win->capture_id = g_timeout_add(MAX(16, 1000 / MAX(1, win->fps)), mpcecc_wlid_capture_tick, win);
    return TRUE;
}

static void mpcecc_wlid_focus(MpcEccWlid *win)
{
    if (!win) return;
    mpcecc_focused_wlid = win->wlid;
    if (win->display) { g_free(mpcecc_pixel_display); mpcecc_pixel_display = g_strdup(win->display); }
    mpcecc_pixel_width = win->width; mpcecc_pixel_height = win->height; mpcecc_pixel_fps = win->fps;
    mpcecc_pixel_set_class(win->command);
    mpcecc_active_pixel_surface = win->picture;
    if (win->frame) { gtk_widget_set_visible(win->frame, TRUE); if (win->picture) gtk_widget_grab_focus(win->picture); }
}

/* 1.8: find the tab a control request came from (falls back to the
 * selected tab when the request has no "@ID " prefix or the tab is gone). */
static MpcEccTab *
mpcecc_tab_by_id(int id)
{
    if (id > 0 && mpcecc_main_ecc && mpcecc_main_ecc->tabs) {
        int n = gtk_notebook_get_n_pages(mpcecc_main_ecc->tabs);
        for (int i = 0; i < n; ++i) {
            GtkWidget *page = gtk_notebook_get_nth_page(mpcecc_main_ecc->tabs, i);
            MpcEccTab *t = g_object_get_data(G_OBJECT(page), "mpcecc-tab");
            if (t && t->id == id)
                return t;
        }
    }
    return mpcecc_get_current_tab();
}

/* 1.8: MPSP images are temp files made by mpcimgcat. Delete only those. */
static void
mpcecc_unlink_mpsp_temp(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid())
        return;
    char *dir  = g_path_get_dirname(path);
    char *base = g_path_get_basename(path);
    char *cdir = g_canonicalize_filename(dir, NULL);
    char *tmp  = g_canonicalize_filename(g_get_tmp_dir(), NULL);
    if (strcmp(cdir, tmp) == 0 &&
        g_str_has_prefix(base, "mpcimgcat-") && g_str_has_suffix(base, ".png"))
        unlink(path);
    g_free(dir); g_free(base); g_free(cdir); g_free(tmp);
}

static void
mpcecc_mpsp_close_clicked(GtkButton *button, gpointer frame_ptr)
{
    (void)button;
    GtkWidget *frame = GTK_WIDGET(frame_ptr);
    GtkWidget *parent = gtk_widget_get_parent(frame);
    if (parent && GTK_IS_OVERLAY(parent))
        gtk_overlay_remove_overlay(GTK_OVERLAY(parent), frame);
    else
        gtk_widget_unparent(frame);
}

static gboolean
mpcecc_control_incoming(GSocketService *service, GSocketConnection *conn,
                        GObject *source, gpointer user_data)
{
    (void)service; (void)source; (void)user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gchar buf[65536];
    gssize n = 0;
    g_socket_set_timeout(g_socket_connection_get_socket(conn), 2);
    while ((gsize)n < sizeof(buf) - 1) {
        gssize r = g_input_stream_read(in, buf + n, sizeof(buf) - 1 - (gsize)n, NULL, NULL);
        if (r <= 0) break;
        n += r;
        if (memchr(buf, '\n', (size_t)n)) break;   /* requests end with a newline */
    }
    if (n <= 0) return TRUE;
    buf[n] = 0;
    int req_tab = 0;   /* optional "@ID " prefix: the tab that sent this request */
    if (buf[0] == '@') {
        char *endp = NULL;
        req_tab = (int)strtol(buf + 1, &endp, 10);
        while (endp && *endp == ' ') ++endp;
        if (endp) memmove(buf, endp, strlen(endp) + 1);
    }

    char *reply = NULL;
    if (g_str_has_prefix(buf, "LAUNCH ")) {
        /* LAUNCH W H FPS -- command args... ; one embedded app per MPCECC tab. */
        int w = 1440, h = 900, fps = 15;
        char *p = buf + 7;
        sscanf(p, "%d %d %d", &w, &h, &fps);
        char *sep = strstr(p, " -- ");
        if (!sep) {
            reply = g_strdup("ERR syntax\n");
        } else {
            sep += 4;
            while (*sep == ' ') ++sep;
            g_strstrip(sep);
            for (char *q = sep; *q; ++q)
                if (*q == '\n' || *q == '\r') *q = ' ';
                int parsed_argc = 0;
            char **parsed_argv = NULL;
            GError *parse_error = NULL;
            gboolean parsed = g_shell_parse_argv(sep, &parsed_argc, &parsed_argv, &parse_error);
            GPtrArray *clean = g_ptr_array_new_with_free_func(g_free);
            if (parsed) {
                for (int i = 0; i < parsed_argc; ++i)
                    g_ptr_array_add(clean, g_strdup(parsed_argv[i]));
            }
            g_ptr_array_add(clean, NULL);
            g_strfreev(parsed_argv);
            g_clear_error(&parse_error);
            if (!parsed || clean->len < 2) {
                reply = g_strdup("ERR no program\n");
            } else if (!mpcecc_main_ecc) {
                reply = g_strdup("ERR MPCECC window unavailable\n");
            } else {
                /* Reuse the selected tab. Every WLID gets its own in-tab overlay;
                 * launching an application must not create another notebook tab. */
                MpcEccTab *tab = mpcecc_tab_by_id(req_tab);
                MpcEccWlid *win = g_new0(MpcEccWlid, 1);
                win->wlid = mpcecc_next_wlid++;
                const char *base = strrchr((char*)clean->pdata[0], '/');
                base = base ? base + 1 : (char*)clean->pdata[0];
                win->command = g_strdup(base);
                win->title = g_strdup_printf("%s — PixelWindow", base);
                win->width = CLAMP(w, 64, 8192);
                win->height = CLAMP(h, 64, 8192);
                win->fps = CLAMP(fps, 1, 60);
                win->tab = tab;
                guint hash = g_str_hash(base);
                win->color = 0xFF000000u | ((hash & 0xFF) << 16) | (((hash >> 8) & 0xFF) << 8) | ((hash >> 16) & 0xFF);
                mpcecc_wlid_list = g_list_append(mpcecc_wlid_list, win);
                if (!mpcecc_wlid_start(win, (char**)clean->pdata)) {
                    mpcecc_wlid_list = g_list_remove(mpcecc_wlid_list, win);
                    /* Keep the newly created tab intact; report failure in its terminal. */
                    if (tab && tab->terminal) vte_terminal_feed(tab->terminal, "\r\n[mpcecc] Failed to start embedded application.\r\n", -1);
                    mpcecc_wlid_free(win);
                    reply = g_strdup("ERR could not start in-tab application\n");
                } else {
                    reply = g_strdup_printf("OK %d\n", win->wlid);
                    if (tab && tab->terminal) {
                        char *line = g_strdup_printf("\r\n[mpcecc] launched wlid %d (%s) inside the current MPCECC tab\r\n", win->wlid, base);
                        vte_terminal_feed(tab->terminal, line, -1);
                        g_free(line);
                    }
                }
            }
            g_ptr_array_free(clean, TRUE);
        }
    } else if (g_str_has_prefix(buf, "TERMINATE ") || g_str_has_prefix(buf, "KILL ")) {
        int id = atoi(buf + (buf[0] == 'T' ? 10 : 5));
        MpcEccWlid *win = mpcecc_wlid_find(id);
        if (!win) {
            reply = g_strdup("ERR no such wlid\n");
        } else {
            win->terminating = TRUE;
            if (win->capture_id) { g_source_remove(win->capture_id); win->capture_id = 0; }
            if (win->app_pid) kill((pid_t)win->app_pid, SIGTERM);
            if (win->xvfb_pid) kill((pid_t)win->xvfb_pid, SIGTERM);
            if (win->frame) gtk_widget_set_visible(win->frame, FALSE);
            if (win->frame_path) unlink(win->frame_path);
            mpcecc_wlid_list = g_list_remove(mpcecc_wlid_list, win);
            if (mpcecc_focused_wlid == id) mpcecc_focused_wlid = 0;
            mpcecc_wlid_free(win);
            reply = g_strdup("OK\n");
            {
                char msg[64];
                snprintf(msg, sizeof msg, "terminated wlid %d", id);
                mpcecc_feed_current_tab(msg);
            }
        }
    } else if (g_str_has_prefix(buf, "LIST") || g_str_has_prefix(buf, "WLIDLIST")) {
        GString *s = g_string_new(NULL);
        for (GList *l = mpcecc_wlid_list; l; l = l->next) {
            MpcEccWlid *w = l->data;
            g_string_append_printf(s, "%d\t%s\t%s\t%d\n",
                                   w->wlid, w->command ? w->command : "?",
                                   w->title ? w->title : "",
                                   (w->wlid == mpcecc_focused_wlid) ? 1 : 0);
        }
        if (s->len == 0) g_string_append(s, "\n");
        reply = g_string_free(s, FALSE);
    } else if (g_str_has_prefix(buf, "FOCUS ")) {
        int id = atoi(buf + 6);
        MpcEccWlid *win = mpcecc_wlid_find(id);
        if (!win) {
            reply = g_strdup("ERR no such wlid\n");
        } else {
            mpcecc_wlid_focus(win);
            reply = g_strdup("OK\n");
        }
    } else if (g_str_has_prefix(buf, "HIDE")) {
        MpcEccWlid *win = mpcecc_wlid_find(mpcecc_focused_wlid);
        if (win && win->frame) gtk_widget_set_visible(win->frame, FALSE);
        mpcecc_focused_wlid = 0;
        reply = g_strdup("OK\n");
    } else if (g_str_has_prefix(buf, "MPSP ")) {
        /* MPCECC Pixel Sending Protocol – display a real image overlay */
        char *imgpath = g_strstrip(buf + 5);
        size_t len = strlen(imgpath);
        while (len && (imgpath[len-1] == '\n' || imgpath[len-1] == '\r'))
            imgpath[--len] = 0;

        if (!*imgpath || !g_file_test(imgpath, G_FILE_TEST_EXISTS)) {
            reply = g_strdup("ERR file not found\n");
        } else {
            MpcEccTab *tab = mpcecc_tab_by_id(req_tab);
            if (!tab || !tab->container) {
                reply = g_strdup("ERR no tab\n");
            } else {
                GError *load_err = NULL;
                GdkPixbuf *pb = gdk_pixbuf_new_from_file(imgpath, &load_err);
                if (!pb) {
                    reply = g_strdup_printf("ERR load: %s\n",
                        load_err ? load_err->message : "unknown");
                    g_clear_error(&load_err);
                } else {
                    GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                    gtk_widget_add_css_class(frame, "mpcecc-pixel-frame");
                    gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
                    gtk_widget_set_halign(frame, GTK_ALIGN_CENTER);
                    gtk_widget_set_valign(frame, GTK_ALIGN_CENTER);

                    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
                    gtk_widget_add_css_class(titlebar, "mpcecc-pixel-titlebar");
                    GtkWidget *icon = gtk_label_new("🖼");
                    gtk_widget_add_css_class(icon, "mpcecc-pixel-icon");
                    GtkWidget *title = gtk_label_new("mpcimgcat — MPSP");
                    gtk_widget_set_hexpand(title, TRUE);
                    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
                    gtk_widget_add_css_class(title, "mpcecc-pixel-title");
                    GtkWidget *btn_close = gtk_button_new_with_label("×");
                    gtk_widget_add_css_class(btn_close, "mpcecc-pixel-btn");
                    gtk_widget_add_css_class(btn_close, "mpcecc-pixel-close");
                    g_signal_connect(btn_close, "clicked",
                                     G_CALLBACK(mpcecc_mpsp_close_clicked), frame);
                    gtk_box_append(GTK_BOX(titlebar), icon);
                    gtk_box_append(GTK_BOX(titlebar), title);
                    gtk_box_append(GTK_BOX(titlebar), btn_close);

                    GtkGestureDrag *drag = GTK_GESTURE_DRAG(gtk_gesture_drag_new());
                    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 1);
                    g_signal_connect(drag, "drag-begin", G_CALLBACK(mpcecc_pixel_drag_begin), frame);
                    g_signal_connect(drag, "drag-update", G_CALLBACK(mpcecc_pixel_drag_update), frame);
                    gtk_widget_add_controller(titlebar, GTK_EVENT_CONTROLLER(drag));

                    GtkWidget *picture = gtk_picture_new();
                    gtk_picture_set_pixbuf(GTK_PICTURE(picture), pb);
                    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
                    gtk_widget_set_size_request(picture,
                        MIN(gdk_pixbuf_get_width(pb), 1200),
                        MIN(gdk_pixbuf_get_height(pb), 800));
                    gtk_widget_set_hexpand(picture, TRUE);
                    gtk_widget_set_vexpand(picture, TRUE);

                    gtk_box_append(GTK_BOX(frame), titlebar);
                    gtk_box_append(GTK_BOX(frame), picture);

                    gtk_overlay_add_overlay(GTK_OVERLAY(tab->container), frame);
                    gtk_overlay_set_measure_overlay(GTK_OVERLAY(tab->container), frame, FALSE);
                    gtk_widget_set_visible(frame, TRUE);

                    g_object_unref(pb);
                    reply = g_strdup("OK\n");
                    if (tab->terminal) {
                        vte_terminal_feed(tab->terminal,
                            "\r\n[mpcecc] MPSP image displayed (close with ×)\r\n", -1);
                    }
                }
            }
            mpcecc_unlink_mpsp_temp(imgpath);
        }
    } else {
        reply = g_strdup("ERR unknown\n");
    }
    if (reply) {
        g_output_stream_write(out, reply, strlen(reply), NULL, NULL);
        g_free(reply);
    }
    return TRUE;
}

static void
mpcecc_start_control_socket(void)
{
    const char *path = mpcecc_control_sock_path();
    unlink(path);
    GError *error = NULL;
    GSocketAddress *addr = g_unix_socket_address_new(path);
    mpcecc_control_service = g_socket_service_new();
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(mpcecc_control_service),
        addr, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL, &error)) {
        fprintf(stderr, "mpcecc: control socket: %s\n", error->message);
    g_clear_error(&error);
    g_object_unref(addr);
    return;
        }
        g_object_unref(addr);
        g_signal_connect(mpcecc_control_service, "incoming", G_CALLBACK(mpcecc_control_incoming), NULL);
        g_socket_service_start(mpcecc_control_service);
        chmod(path, 0600);
        fprintf(stderr, "mpcecc: control socket ready (%s)\n", path);
}

static void
mpcecc_activate(GtkApplication *application, gpointer user_data)
{
    g_setenv("MPCECC_VERSION", "1.8", TRUE);
    g_setenv("MPCECC_NAME", "MarekPC Extended Capabilities Console", TRUE);
    g_setenv("MPCECC", "1", TRUE);

    MPCECC *ecc = user_data;

    ecc->app = application;
    mpcecc_main_ecc = ecc;
    if (!(mpcecc_window_command && mpcecc_window_command[0]))
        mpcecc_start_control_socket();
    if (mpcecc_window_command && mpcecc_window_command[0]) {
        mpcecc_open_command_window(ecc, mpcecc_window_command);
        return;
    }
    ecc->profiles = mpcecc_load_profiles();
    ecc->active_profile = g_strdup("MarekPC Purple");

    ecc->window = GTK_WINDOW(gtk_application_window_new(application));

    gtk_window_set_title(ecc->window,
                         "MarekPC Extended Capabilities Console 1.8");
    gtk_window_set_default_size(ecc->window, 1150, 720);
    /* Launch maximized so the wallpaper has room to breathe;
     * the default size above still applies if unmaximized. */
    gtk_window_maximize(ecc->window);

    mpcecc_add_action(ecc, "copy", mpcecc_copy_action);
    mpcecc_add_action(ecc, "paste", mpcecc_paste_action);
    mpcecc_add_action(ecc, "select-all", mpcecc_select_all_action);
    mpcecc_add_action(ecc, "clear", mpcecc_clear_action);
    mpcecc_add_action(ecc, "new-tab", mpcecc_new_tab_action);
    mpcecc_add_action(ecc, "close-tab", mpcecc_close_tab_action);
    mpcecc_add_action(ecc, "settings", mpcecc_settings_action);
    mpcecc_add_action(ecc, "help", mpcecc_help_action);

    mpcecc_install_shortcuts(ecc);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(ecc->window, root);

    GtkWidget *header = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(header), TRUE);
    gtk_window_set_titlebar(ecc->window, header);

    GtkWidget *new_tab_button = gtk_button_new_with_label("＋ Tab");

    g_signal_connect(new_tab_button, "clicked",
                     G_CALLBACK(mpcecc_new_tab_button_clicked), ecc);

    gtk_header_bar_pack_start(GTK_HEADER_BAR(header), new_tab_button);

    GtkWidget *settings_button = gtk_button_new_with_label("Settings");

    g_signal_connect(settings_button, "clicked",
                     G_CALLBACK(mpcecc_show_settings), ecc);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), settings_button);

    GtkWidget *help_button = gtk_button_new_with_label("Help");

    g_signal_connect(help_button, "clicked",
                     G_CALLBACK(mpcecc_show_help), ecc);

    gtk_header_bar_pack_end(GTK_HEADER_BAR(header), help_button);

    GtkStringList *profile_model = mpcecc_make_profile_model(ecc);

    ecc->profile_selector = GTK_DROP_DOWN(
        gtk_drop_down_new(G_LIST_MODEL(profile_model), NULL));

    gtk_drop_down_set_selected(ecc->profile_selector, 0);

    gtk_header_bar_pack_start(GTK_HEADER_BAR(header),
                              GTK_WIDGET(ecc->profile_selector));

    GtkWidget *interpreter_bar =
    gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    gtk_widget_set_margin_top(interpreter_bar, 6);
    gtk_widget_set_margin_bottom(interpreter_bar, 6);
    gtk_widget_set_margin_start(interpreter_bar, 8);
    gtk_widget_set_margin_end(interpreter_bar, 8);

    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "🐍 Python", "python3", "tab");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "🟨 Node", "node", "tab");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "🦀 Rust", "rustc --version", "current");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "🐹 Go", "go version", "current");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "☕ Java", "java --version", "current");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "⚡ Zig", "zig version", "current");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "🐚 mpcsh", "mpcsh", "tab");
    mpcecc_add_interpreter_button(interpreter_bar, ecc,
                                  "🐟 Fish", "fish", "tab");

    GtkScrolledWindow *tab_scroll =
    GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());

    gtk_widget_set_vexpand(GTK_WIDGET(tab_scroll), TRUE);
    gtk_scrolled_window_set_policy(tab_scroll,
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);

    ecc->tabs = GTK_NOTEBOOK(gtk_notebook_new());
    gtk_notebook_set_tab_pos(ecc->tabs, GTK_POS_TOP);

    gtk_scrolled_window_set_child(tab_scroll, GTK_WIDGET(ecc->tabs));

    gtk_box_append(GTK_BOX(root), interpreter_bar);
    gtk_box_append(GTK_BOX(root), GTK_WIDGET(tab_scroll));

    mpcecc_new_tab(ecc, "MarekPC Purple", NULL);

    /* Global hotkeys: F3 = star-map picker, Ctrl+Tab = cycle PixelWindows.
     * Capture phase so they work even when a VTE or PixelWindow has focus. */
    {
        GtkEventControllerKey *gkeys =
        GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
        gtk_event_controller_set_propagation_phase(
            GTK_EVENT_CONTROLLER(gkeys), GTK_PHASE_CAPTURE);
        g_signal_connect(gkeys, "key-pressed",
                         G_CALLBACK(mpcecc_global_key), ecc);
        gtk_widget_add_controller(GTK_WIDGET(ecc->window),
                                  GTK_EVENT_CONTROLLER(gkeys));
    }

    gtk_window_present(ecc->window);
}

/* ============================================================
 * F3 Star-map Window Picker
 * Dark-purple sky; each PixelWindow is a star with title.
 * Same-command windows (e.g. two firefox) are linked by a line.
 * ============================================================ */

static void
mpcecc_picker_focus_clicked(GtkButton *button, gpointer user_data)
{
    int id = GPOINTER_TO_INT(user_data);
    (void)button;
    MpcEccWlid *win = mpcecc_wlid_find(id);
    if (win) {
        mpcecc_wlid_focus(win);
        char msg[80];
        snprintf(msg, sizeof msg, "focused wlid %d", id);
        mpcecc_feed_current_tab(msg);
    }
}

static void
mpcecc_show_aero_picker(MPCECC *ecc)
{
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Star Map — PixelWindows");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), ecc->window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 420);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
                                      "window.mpcecc-starmap { background-color: #0c0614; border-radius: 4px; }"
                                      ".mpcecc-starmap label { color: #d8c8f0; }"
                                      ".mpcecc-starmap button.star {"
                                      "  background-color: transparent; border: 1px solid #5a3d82;"
                                      "  border-radius: 999px; min-width: 36px; min-height: 36px;"
                                      "  color: #f0e6ff; font-size: 16px; padding: 4px;"
                                      "}"
                                      ".mpcecc-starmap button.star:hover { background-color: #2a1840; border-color: #a890d0; }"
                                      ".mpcecc-starmap button.close-btn {"
                                      "  background-color: #1a1028; color: #c8b8e0; border-radius: 3px; padding: 6px 14px;"
                                      "}"
                                      ".mpcecc-starmap button.close-btn:hover { background-color: #3d2a58; }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    gtk_widget_add_css_class(dialog, "mpcecc-starmap");

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top(root, 16);
    gtk_widget_set_margin_bottom(root, 16);
    gtk_widget_set_margin_start(root, 16);
    gtk_widget_set_margin_end(root, 16);
    gtk_window_set_child(GTK_WINDOW(dialog), root);

    GtkWidget *heading = gtk_label_new("✦  Star Map");
    gtk_widget_add_css_class(heading, "title-3");
    gtk_box_append(GTK_BOX(root), heading);

    GtkWidget *sub = gtk_label_new("Programs are stars on a dark-purple sky. Same apps are linked.");
    gtk_label_set_wrap(GTK_LABEL(sub), TRUE);
    gtk_box_append(GTK_BOX(root), sub);

    if (!mpcecc_wlid_list) {
        GtkWidget *empty = gtk_label_new("No PixelWindows open.\nLaunch one with: windowlaunch <program>");
        gtk_label_set_justify(GTK_LABEL(empty), GTK_JUSTIFY_CENTER);
        gtk_box_append(GTK_BOX(root), empty);
    } else {
        /* Group by command for neural-style links */
        GtkWidget *flow = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 4);
        gtk_box_append(GTK_BOX(root), flow);

        for (GList *l = mpcecc_wlid_list; l; l = l->next) {
            MpcEccWlid *w = l->data;
            GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_widget_set_halign(cell, GTK_ALIGN_CENTER);

            char star_label[64];
            snprintf(star_label, sizeof star_label, "★\n%d", w->wlid);
            GtkWidget *star = gtk_button_new_with_label(star_label);
            gtk_widget_add_css_class(star, "star");
            g_signal_connect(star, "clicked", G_CALLBACK(mpcecc_picker_focus_clicked),
                             GINT_TO_POINTER(w->wlid));
            gtk_box_append(GTK_BOX(cell), star);

            char cap[128];
            snprintf(cap, sizeof cap, "%s", w->command ? w->command : "?");
            GtkWidget *capl = gtk_label_new(cap);
            gtk_label_set_ellipsize(GTK_LABEL(capl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_size_request(capl, 100, -1);
            gtk_box_append(GTK_BOX(cell), capl);

            /* related: count siblings with same command */
            int siblings = 0;
            for (GList *m = mpcecc_wlid_list; m; m = m->next) {
                MpcEccWlid *o = m->data;
                if (o != w && o->command && w->command &&
                    !strcmp(o->command, w->command))
                    ++siblings;
            }
            if (siblings > 0) {
                char link[48];
                snprintf(link, sizeof link, "⟷ %d linked", siblings);
                GtkWidget *ll = gtk_label_new(link);
                gtk_widget_add_css_class(ll, "dim-label");
                gtk_box_append(GTK_BOX(cell), ll);
            }

            gtk_flow_box_append(GTK_FLOW_BOX(flow), cell);
        }
    }

    GtkWidget *btn = gtk_button_new_with_label("Close");
    gtk_widget_add_css_class(btn, "close-btn");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_box_append(GTK_BOX(root), btn);

    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean
mpcecc_global_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                  GdkModifierType state, gpointer user_data)
{
    (void)controller; (void)keycode;
    if (keyval == GDK_KEY_F3) {
        mpcecc_show_aero_picker((MPCECC *)user_data);
        return TRUE;
    }
    /* Ctrl+Shift+Tab / Ctrl+Shift+` : cycle focused PixelWindows.
     * Requires BOTH Control and Shift so the WM/DE never steals Alt+Tab
     * and plain Ctrl+Tab stays free for the terminal / other apps. */
    if ((state & GDK_CONTROL_MASK) && (state & GDK_SHIFT_MASK) &&
        (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab ||
        keyval == GDK_KEY_grave || keyval == GDK_KEY_asciitilde)) {
        if (!mpcecc_wlid_list) return FALSE;
        GList *cur = NULL;
        for (GList *l = mpcecc_wlid_list; l; l = l->next) {
            if (((MpcEccWlid *)l->data)->wlid == mpcecc_focused_wlid) { cur = l; break; }
        }
        /* ISO_Left_Tab or `~` reverse; Tab forward */
        gboolean reverse = (keyval == GDK_KEY_ISO_Left_Tab ||
        keyval == GDK_KEY_grave || keyval == GDK_KEY_asciitilde);
    GList *next = NULL;
    if (reverse)
        next = cur ? (cur->prev ? cur->prev : g_list_last(mpcecc_wlid_list))
        : g_list_last(mpcecc_wlid_list);
    else
        next = cur ? (cur->next ? cur->next : mpcecc_wlid_list) : mpcecc_wlid_list;
        if (next) {
            MpcEccWlid *w = next->data;
            mpcecc_wlid_focus(w);
            char msg[96];
            snprintf(msg, sizeof msg, "Ctrl+Shift+Tab → wlid %d (%s)", w->wlid,
                     w->command ? w->command : "?");
            mpcecc_feed_current_tab(msg);
        }
        return TRUE;
        }
        return FALSE;
}


/* CLI launchers must talk to the already-running MPCECC instance. They must
 * never start a second GTK application window. The control service embeds the
 * app as a PixelWindow overlay in the currently selected notebook tab. */
static int
mpcecc_forward_windowlaunch(int argc, char **argv, int first_command)
{
    unsigned width = 1440, height = 900;
    int fps = 15;
    if (mpcecc_window_size)
        (void)sscanf(mpcecc_window_size, "%ux%u", &width, &height);
    if (mpcecc_window_fps) fps = atoi(mpcecc_window_fps);
    width = CLAMP(width, 64u, 8192u);
    height = CLAMP(height, 64u, 8192u);
    fps = CLAMP(fps, 1, 60);

    GString *request = g_string_new(NULL);
    g_string_append_printf(request, "LAUNCH %u %u %d --", width, height, fps);
    for (int i = first_command; i < argc; ++i) {
        char *quoted = g_shell_quote(argv[i]);
        g_string_append_c(request, ' ');
        g_string_append(request, quoted);
        g_free(quoted);
    }
    g_string_append_c(request, '\n');

    GError *error = NULL;
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *address = g_unix_socket_address_new(mpcecc_control_sock_path());
    GSocketConnection *connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error);
    g_object_unref(address);
    g_object_unref(client);
    if (!connection) {
        fprintf(stderr, "mpcecc: cannot reach the running MPCECC window (%s). Start MPCECC first; --windowlaunch will not open a separate window.\n",
                error ? error->message : "control socket unavailable");
        g_clear_error(&error);
        g_string_free(request, TRUE);
        return 1;
    }
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gsize written = 0;
    gboolean ok = g_output_stream_write_all(out, request->str, request->len, &written, NULL, &error);
    g_string_free(request, TRUE);
    if (!ok) {
        fprintf(stderr, "mpcecc: sending launch request failed: %s\n", error ? error->message : "write failed");
        g_clear_error(&error);
        g_object_unref(connection);
        return 1;
    }
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    char response[512] = {0};
    gssize n = g_input_stream_read(in, response, sizeof response - 1, NULL, &error);
    if (n > 0) {
        response[n] = '\0';
        g_print("%s", response);
    } else {
        fprintf(stderr, "mpcecc: no response from MPCECC control service: %s\n", error ? error->message : "connection closed");
    }
    g_clear_error(&error);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
    return (n > 0 && g_str_has_prefix(response, "OK ")) ? 0 : 1;
}

int
main(int argc, char **argv)
{
    MPCECC ecc = {0};
    int app_argc = argc;
    char **app_argv = argv;
    char *application_argv[] = { argv[0], NULL };
    if (argc == 2 && !strcmp(argv[1], "--windowlaunch-protocol")) { puts("MPCECC-WINDOWLAUNCH-4"); return 0; }
    if (argc >= 2 && !strcmp(argv[1], "--windowlaunch")) {
        int i = 2;
        for (; i < argc; ++i) {
            if (!strcmp(argv[i], "--")) { ++i; break; }
            if (argv[i][0] != '-') break;
            if (!strcmp(argv[i], "--pixels") || !strcmp(argv[i], "--embed") || !strcmp(argv[i], "--auto")) { mpcecc_window_pixels = TRUE; mpcecc_window_terminal = FALSE; }
            else if (!strcmp(argv[i], "--terminal")) { mpcecc_window_terminal = TRUE; mpcecc_window_pixels = FALSE; }
            else if (!strcmp(argv[i], "--size")) { if (++i >= argc) { fprintf(stderr, "mpcecc: option requires a value\n"); return 2; } mpcecc_window_size = argv[i]; }
            else if (!strcmp(argv[i], "--fps")) { if (++i >= argc) { fprintf(stderr, "mpcecc: option requires a value\n"); return 2; } mpcecc_window_fps = argv[i]; }
            else if (!strcmp(argv[i], "--cwd") || !strcmp(argv[i], "--title")) {
                gboolean is_cwd = !strcmp(argv[i], "--cwd");
                if (++i >= argc) { fprintf(stderr, "mpcecc: option requires a value\n"); return 2; }
                if (is_cwd) mpcecc_window_cwd = argv[i]; else mpcecc_window_title = argv[i];
            } else { fprintf(stderr, "mpcecc: unknown windowlaunch option: %s\n", argv[i]); return 2; }
        }
        if (i >= argc || !*argv[i]) { fprintf(stderr, "mpcecc: --windowlaunch requires a program name\n"); return 2; }
        if (mpcecc_window_cwd && chdir(mpcecc_window_cwd) != 0) { fprintf(stderr, "mpcecc: --cwd: %s: %s\n", mpcecc_window_cwd, strerror(errno)); return 2; }
        /* Forward to the existing MPCECC process. Do not create a second
         * GtkApplication or a separate top-level PixelWindow. */
        return mpcecc_forward_windowlaunch(argc, argv, i);
    }
    GtkApplication *application = gtk_application_new("com.marekpc.mpcecc", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(mpcecc_activate), &ecc);
    int status = g_application_run(G_APPLICATION(application), app_argc, app_argv);
    g_object_unref(application);
    return status;
}
