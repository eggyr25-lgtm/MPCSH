/*
 * MarekPC Extended Capabilities Console (MPCECC) 1.9
 * GTK4 + VTE terminal console with profiles, wallpaper,
 * tabs, clipboard shortcuts, context menu, and settings.
 *
 * Clipboard shortcuts (VTE-GTK4 does NOT bind these itself, so MPCECC
 * installs them as capture-phase window shortcuts):
 *   Ctrl+Shift+C = copy selected text
 *   Ctrl+Shift+V = paste
 *   Ctrl+V       = paste
 *
 * 1.9 changes (major):
 *   - windowlaunch performance: lower default capture load, smarter
 *     delayed start, reduced contention when multiple PixelWindows live
 *     in one tab. Resizing is more reliable (proper size-request +
 *     aspect CONTAIN + deferred re-capture).
 *   - PixelWindow STYLES: Aero, LiquidGlass, TransparentPurple,
 *     MovingPurplePlasma (animated CSS), Classic, Minimal.
 *     Set via control socket STYLE <wlid> <name> or titlebar menu.
 *   - Star-map switcher (F3 and Ctrl+Shift+Tab): dark sky with one star
 *     per live PixelWindow / process. Same-command instances are linked
 *     and clustered. Much clearer visual grouping.
 *   - Tab splitting (experimental): drag a tab onto the centre of another
 *     to combine into a horizontal/vertical paned split; de-split supported.
 *   - Better default framebuffer and frame cap for heavy tools.
 *
 * Carried from 1.8:
 *   - MULTI-WINDOW PixelWindows (wlid registry)
 *   - Ctrl+Shift+Tab / Ctrl+Shift+` cycle (Shift reverses)
 *   - Isolated Firefox profile (MPCECCWLPROFILE)
 *   - MPSP real-image overlays, crash dialog + Restart Shell
 *   - Capture-phase shortcuts, clean close path
 *
 * NOTE on VTE versions: this file targets the libvte-2.91-gtk4 API
 * where "child-exited" passes the wait status as a signal argument
 * and VteTerminalSpawnAsyncCallback is
 *   void (*)(VteTerminal *, int status, GError *, gpointer).
 *
 * Build on CachyOS / Arch:
 *
 *   gcc -O2 -pipe -std=gnu11 mpcecc-1.9.c -o mpcecc \
 *       -lgtk-4 -lvte-2.91-gtk4 -lgdk_pixbuf-2.0 \
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
static int mpcecc_pixel_fps = 12;  /* 1.9: slightly lower default = less CPU, still smooth */
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
                                              /* Base */
                                              ".mpcecc-pixel-frame {"
                                              "  background-color: #160e22;"
                                              "  border: 1px solid #5a3d82;"
                                              "  border-radius: 3px;"
                                              "  box-shadow: 0 4px 16px alpha(black, 0.35);"
                                              "}"
                                              ".mpcecc-pixel-normal { margin: 0; }"
                                              ".mpcecc-pixel-max { margin: 0; border-radius: 0; }"
                                              ".mpcecc-pixel-titlebar {"
                                              "  background-color: #2a1a40; background-image: none;"
                                              "  min-height: 28px; padding: 0 6px;"
                                              "  border-bottom: 1px solid #4a3568; border-radius: 2px 2px 0 0;"
                                              "}"
                                              ".mpcecc-pixel-icon { color: #a890d0; font-size: 12px; padding-right: 4px; }"
                                              ".mpcecc-pixel-title { color: #e8dcf8; font-weight: 500; font-size: 11px; }"
                                              ".mpcecc-pixel-btn {"
                                              "  color: #c8b8e0; background: transparent; border: none;"
                                              "  min-width: 28px; min-height: 24px; border-radius: 2px;"
                                              "  padding: 0 6px; font-weight: 500; font-size: 13px;"
                                              "}"
                                              ".mpcecc-pixel-btn:hover { background-color: #3d2a58; color: #ffffff; }"
                                              ".mpcecc-pixel-close:hover { background-color: #9a2a38; color: #ffffff; }"
                                              /* 1.9 Styles */
                                              ".mpcecc-style-classic {"
                                              "  background-color: #160e22; border: 1px solid #5a3d82; border-radius: 3px;"
                                              "}"
                                              ".mpcecc-style-aero {"
                                              "  background-color: alpha(#1e1432, 0.78);"
                                              "  border: 1px solid #8a6bb5; border-radius: 8px;"
                                              "  box-shadow: 0 6px 24px alpha(#5a3d82, 0.45);"
                                              "}"
                                              ".mpcecc-style-aero .mpcecc-pixel-titlebar {"
                                              "  background-color: alpha(#3a2860, 0.9); border-radius: 7px 7px 0 0;"
                                              "}"
                                              ".mpcecc-style-liquidglass {"
                                              "  background-color: alpha(#281e46, 0.42);"
                                              "  border: 1px solid alpha(#b496ff, 0.55); border-radius: 14px;"
                                              "  box-shadow: 0 8px 32px alpha(#7040c0, 0.35);"
                                              "}"
                                              ".mpcecc-style-liquidglass .mpcecc-pixel-titlebar {"
                                              "  background-color: alpha(#3c2a68, 0.55); border-radius: 13px 13px 0 0;"
                                              "  border-bottom: 1px solid alpha(#c0a0ff, 0.3);"
                                              "}"
                                              ".mpcecc-style-transparentpurple {"
                                              "  background-color: alpha(#3c1470, 0.32);"
                                              "  border: 1px solid #a070d0; border-radius: 6px;"
                                              "}"
                                              ".mpcecc-style-transparentpurple .mpcecc-pixel-titlebar {"
                                              "  background-color: alpha(#4a2080, 0.5);"
                                              "}"
                                              ".mpcecc-style-plasma {"
                                              "  background-image: linear-gradient(135deg,"
                                              "    alpha(#5a28a0, 0.55), alpha(#ff7828, 0.38), alpha(#7030c0, 0.55));"
                                              "  border: 1px solid #c080ff; border-radius: 10px;"
                                              "  box-shadow: 0 0 20px alpha(#a050ff, 0.4);"
                                              "}"
                                              ".mpcecc-style-plasma .mpcecc-pixel-titlebar {"
                                              "  background-image: linear-gradient(90deg, #4a2080, #a05030);"
                                              "  border-radius: 9px 9px 0 0;"
                                              "}"
                                              ".mpcecc-style-minimal {"
                                              "  background-color: #0c0814; border: 1px solid #333; border-radius: 0;"
                                              "  box-shadow: none;"
                                              "}"
                                              ".mpcecc-style-minimal .mpcecc-pixel-titlebar {"
                                              "  background-color: #121018; border-bottom: 1px solid #2a2a2a;"
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
    } else if (g_str_has_prefix(buf, "STYLE ")) {
        /* STYLE <wlid> <aero|liquidglass|transparentpurple|plasma|classic|minimal> */
        char *p = buf + 6;
        int id = atoi(p);
        while (*p && *p != ' ') ++p;
        while (*p == ' ') ++p;
        char style[64] = {0};
        sscanf(p, "%63s", style);
        for (char *c = style; *c; ++c) *c = g_ascii_tolower(*c);
        MpcEccWlid *win = mpcecc_wlid_find(id);
        if (!win || !win->frame) {
            reply = g_strdup("ERR no such wlid\n");
        } else {
            const char *styles[] = {
                "mpcecc-style-classic", "mpcecc-style-aero",
                "mpcecc-style-liquidglass", "mpcecc-style-transparentpurple",
                "mpcecc-style-plasma", "mpcecc-style-minimal", NULL
            };
            for (int i = 0; styles[i]; ++i)
                gtk_widget_remove_css_class(win->frame, styles[i]);
            char cls[80];
            if (!*style) g_strlcpy(style, "classic", sizeof style);
            snprintf(cls, sizeof cls, "mpcecc-style-%s", style);
            gtk_widget_add_css_class(win->frame, cls);
            reply = g_strdup_printf("OK style=%s\n", style);
        }
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
    g_setenv("MPCECC_VERSION", "1.9", TRUE);
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
                         "MarekPC Extended Capabilities Console 1.9");
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

    GtkWidget *heading = gtk_label_new("✦  Star Map — PixelWindows & Processes");
    gtk_widget_add_css_class(heading, "title-3");
    gtk_box_append(GTK_BOX(root), heading);

    GtkWidget *sub = gtk_label_new("Each star is a live PixelWindow. Same command = linked cluster. Dark purple sky.");
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
}/*
 * ============================================================
 *  MarekPC Shell — mpcsh  (version 1.9)
 *  mpcsh + mpcap, one binary, one soul.
 *
 *  Build: see the explicit -I/-l gcc command (gtk4, gdk-pixbuf, glib, cairo,
 *  pango, harfbuzz, graphene, gio, curl, m, dl).
 *
 *  1.9 major changes:
 *   1. portmap rewritten to stop lying. Uses real sysfs speed + better
 *      colour mapping that matches common B550 / X570 / B650 boards
 *      (teal/blue = USB3.x, not the old fake "CYAN = USB-C"). Legend fixed.
 *   2. NEW gpuportmap — GPU / display / VRAM / PCIe summary.
 *   3. NEW setbg <path>  (proper wallpaper setter + alias).
 *   4. Prompt accents expanded: orange, orangepurple, animatedorangepurple
 *      (plasma movement), green, cyan, pink, red, yellow, white, aero,
 *      glass and more. Connectors: starship, crystal, aero, blocks...
 *   5. AI notice rewritten — real MarekPC / mpcsh syntax examples.
 *   6. NEW quickinfo — live bars for RAM %, free, CPU clocks, load,
 *      GPU/VRAM when readable, uptime, temperatures.
 *   7. Better pairing with MPCECC 1.9 (styles, faster windowlaunch, star map).
 *
 *  Carried from 1.8: Multi-window PixelWindows, scripting, Chihuahuas,
 *  mpc-chuctrl, time, mpcshman, math, source, isolated Firefox profile,
 *  EINTR-safe Ctrl+C, etc.
 * ============================================================
 */

#define _GNU_SOURCE

#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <locale.h>
#include <pwd.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <math.h>

#define MPCSH_VERSION_MAJOR 1
#define MPCSH_VERSION_MINOR 9
#define MPCSH_VERSION_PATCH 1
#define MPCSH_VERSION_STRING "1.9"
#define MPCSH_MAX_PIPELINE    64
#define MPCAP_MAX_OPTIONS     9
#define MPCSH_MAX_LINE_DEFAULT 8192
#define MPCSH_MAX_LINE_HARD    16384   /* was 65536 – too big for stack */
#define MPCSH_MAX_SCRIPT      (64 * 1024)
#define MPCSH_MAX_PATH        4096
#define MPCSH_HISTORY_MAX_DEFAULT 128
#define MPCSH_HISTORY_MAX_HARD    512  /* was 4096 – too big for stack */
#define MPCSH_GUI_MAX_BUTTONS  24
#define MPCSH_GUI_MAX_ROWS     64

/* ---- mpcsh.config (~/.config/mpcsh/mpcsh.config or $MPCSH_CONFIG) ----
 * accent=purple|orange|green|magenta|pink|cyan|blue|red|yellow|white
 * accent=gradient-purple-orange | gradient-orange-purple
 * animated=true|false
 * connector=default|starship|simple|blocks
 */
typedef enum {
    MPCSH_ACCENT_PURPLE = 0, MPCSH_ACCENT_ORANGE, MPCSH_ACCENT_GREEN,
    MPCSH_ACCENT_MAGENTA, MPCSH_ACCENT_PINK, MPCSH_ACCENT_CYAN,
    MPCSH_ACCENT_BLUE, MPCSH_ACCENT_RED, MPCSH_ACCENT_YELLOW, MPCSH_ACCENT_WHITE,
    MPCSH_ACCENT_GRAD_PO, MPCSH_ACCENT_GRAD_OP, MPCSH_ACCENT_PURPLE_AERO
} MpcshAccent;
typedef enum {
    MPCSH_CONN_DEFAULT = 0, MPCSH_CONN_STARSHIP, MPCSH_CONN_SIMPLE, MPCSH_CONN_BLOCKS, MPCSH_CONN_CRYSTAL, MPCSH_CONN_AERO
} MpcshConnector;

static MpcshAccent    mpcsh_cfg_accent    = MPCSH_ACCENT_PURPLE;
static gboolean       mpcsh_cfg_animated  = FALSE;
static MpcshConnector mpcsh_cfg_connector = MPCSH_CONN_DEFAULT;
static int            mpcsh_anim_frame    = 0;
static gboolean       mpcsh_cfg_show_clock = TRUE;
static gboolean       mpcsh_cfg_show_uptime = FALSE;
static gboolean       mpcsh_cfg_show_cmd_time = TRUE;
static gboolean       mpcsh_cfg_aero = TRUE;
static double         mpcsh_last_cmd_secs = -1.0;

static void mpcsh_accent_rgb(MpcshAccent a, int slot, int *r, int *g, int *b)
{
    static const int table[][3][3] = {
        {{138,43,226},{213,128,255},{90,40,140}},
        {{255,140,0},{255,180,80},{180,90,0}},
        {{50,205,50},{144,238,144},{20,120,40}},
        {{255,0,200},{255,120,220},{140,0,110}},
        {{255,105,180},{255,182,193},{180,60,120}},
        {{0,220,220},{120,240,240},{0,120,140}},
        {{60,120,255},{120,170,255},{30,60,160}},
        {{240,50,50},{255,120,120},{140,20,20}},
        {{240,200,0},{255,230,100},{160,130,0}},
        {{230,230,230},{255,255,255},{140,140,140}},
    };
    if (a == MPCSH_ACCENT_GRAD_PO || a == MPCSH_ACCENT_GRAD_OP) {
        int pr,pg,pb,or_,og,ob;
        mpcsh_accent_rgb(MPCSH_ACCENT_PURPLE, slot, &pr,&pg,&pb);
        mpcsh_accent_rgb(MPCSH_ACCENT_ORANGE, slot, &or_,&og,&ob);
        int t = mpcsh_cfg_animated ? ((mpcsh_anim_frame + slot * 3) % 24) : 12;
        if (a == MPCSH_ACCENT_GRAD_OP) t = 23 - t;
        *r = (pr * (23 - t) + or_ * t) / 23;
        *g = (pg * (23 - t) + og * t) / 23;
        *b = (pb * (23 - t) + ob * t) / 23;
        return;
    }
    if (a == MPCSH_ACCENT_PURPLE_AERO) {
        /* soft glass purple (Aero / Mate-ish) */
        static const int aero[3][3] = {
            {160,120,220},{200,170,255},{100,70,150}
        };
        *r = aero[slot][0]; *g = aero[slot][1]; *b = aero[slot][2];
        return;
    }
    if ((int)a < 0 || (int)a > 9) a = MPCSH_ACCENT_PURPLE;
    *r = table[a][slot][0]; *g = table[a][slot][1]; *b = table[a][slot][2];
}

static char *mpcsh_config_path(void)
{
    static char path[4096];
    const char *env = g_getenv("MPCSH_CONFIG");
    if (env && *env) { snprintf(path, sizeof path, "%s", env); return path; }
    const char *home = g_get_home_dir();
    snprintf(path, sizeof path, "%s/.config/mpcsh/mpcsh.config", home ? home : ".");
    return path;
}

static MpcshAccent mpcsh_parse_accent(const char *v)
{
    if (!v) return MPCSH_ACCENT_PURPLE;
    if (!strcasecmp(v,"orange")) return MPCSH_ACCENT_ORANGE;
    if (!strcasecmp(v,"green")) return MPCSH_ACCENT_GREEN;
    if (!strcasecmp(v,"magenta")) return MPCSH_ACCENT_MAGENTA;
    if (!strcasecmp(v,"pink")) return MPCSH_ACCENT_PINK;
    if (!strcasecmp(v,"cyan")) return MPCSH_ACCENT_CYAN;
    if (!strcasecmp(v,"blue")) return MPCSH_ACCENT_BLUE;
    if (!strcasecmp(v,"red")) return MPCSH_ACCENT_RED;
    if (!strcasecmp(v,"yellow")) return MPCSH_ACCENT_YELLOW;
    if (!strcasecmp(v,"white")) return MPCSH_ACCENT_WHITE;
    if (!strcasecmp(v,"gradient-purple-orange") || !strcasecmp(v,"gradient") ||
        !strcasecmp(v,"purple-orange") || !strcasecmp(v,"orangepurple") ||
        !strcasecmp(v,"animatedorangepurple") || !strcasecmp(v,"plasma") ||
        !strcasecmp(v,"plasma-purple-orange")) return MPCSH_ACCENT_GRAD_PO;
    if (!strcasecmp(v,"gradient-orange-purple") || !strcasecmp(v,"orange-purple") ||
        !strcasecmp(v,"purpleorange"))
        return MPCSH_ACCENT_GRAD_OP;
    if (!strcasecmp(v,"purple-aero") || !strcasecmp(v,"aero") || !strcasecmp(v,"glass") ||
        !strcasecmp(v,"liquidglass") || !strcasecmp(v,"transparentpurple"))
        return MPCSH_ACCENT_PURPLE_AERO;
    return MPCSH_ACCENT_PURPLE;
}

static void mpcsh_load_config(void)
{
    char *contents = NULL;
    if (!g_file_get_contents(mpcsh_config_path(), &contents, NULL, NULL) || !contents)
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    g_free(contents);
    for (int i = 0; lines[i]; ++i) {
        char *line = g_strstrip(lines[i]);
        if (!*line || *line == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = g_strstrip(line);
        char *val = g_strstrip(eq + 1);
        if (!strcasecmp(key,"accent") || !strcasecmp(key,"color") || !strcasecmp(key,"theme")) {
            mpcsh_cfg_accent = mpcsh_parse_accent(val);
            if (strcasestr(val, "plasma") || strcasestr(val, "animated") ||
                strcasestr(val, "orangepurple") || strcasestr(val, "moving"))
                mpcsh_cfg_animated = TRUE;
        }
        else if (!strcasecmp(key,"animated") || !strcasecmp(key,"animate"))
            mpcsh_cfg_animated = (!strcasecmp(val,"true") || !strcmp(val,"1") ||
            !strcasecmp(val,"yes") || !strcasecmp(val,"on"));
        else if (!strcasecmp(key,"connector") || !strcasecmp(key,"style")) {
            if (!strcasecmp(val,"starship")) mpcsh_cfg_connector = MPCSH_CONN_STARSHIP;
            else if (!strcasecmp(val,"simple")) mpcsh_cfg_connector = MPCSH_CONN_SIMPLE;
            else if (!strcasecmp(val,"blocks")) mpcsh_cfg_connector = MPCSH_CONN_BLOCKS;
            else if (!strcasecmp(val,"crystal") || !strcasecmp(val,"zsh-time"))
                mpcsh_cfg_connector = MPCSH_CONN_CRYSTAL;
            else if (!strcasecmp(val,"aero") || !strcasecmp(val,"glass"))
                mpcsh_cfg_connector = MPCSH_CONN_AERO;
            else mpcsh_cfg_connector = MPCSH_CONN_DEFAULT;
        } else if (!strcasecmp(key,"show_clock") || !strcasecmp(key,"clock"))
            mpcsh_cfg_show_clock = (!strcasecmp(val,"true")||!strcmp(val,"1")||!strcasecmp(val,"yes")||!strcasecmp(val,"on"));
        else if (!strcasecmp(key,"show_uptime") || !strcasecmp(key,"uptime"))
            mpcsh_cfg_show_uptime = (!strcasecmp(val,"true")||!strcmp(val,"1")||!strcasecmp(val,"yes")||!strcasecmp(val,"on"));
        else if (!strcasecmp(key,"show_cmd_time") || !strcasecmp(key,"cmd_time") || !strcasecmp(key,"duration"))
            mpcsh_cfg_show_cmd_time = (!strcasecmp(val,"true")||!strcmp(val,"1")||!strcasecmp(val,"yes")||!strcasecmp(val,"on"));
        else if (!strcasecmp(key,"aero") || !strcasecmp(key,"glass"))
            mpcsh_cfg_aero = (!strcasecmp(val,"true")||!strcmp(val,"1")||!strcasecmp(val,"yes")||!strcasecmp(val,"on"));
    }
    g_strfreev(lines);
}

static void mpcsh_fmt_uptime(char *buf, size_t n)
{
    double up = 0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) { if (fscanf(f, "%lf", &up) != 1) up = 0; fclose(f); }
    int s = (int)up;
    int d = s / 86400; s %= 86400;
    int h = s / 3600; s %= 3600;
    int m = s / 60;
    if (d > 0) snprintf(buf, n, "%dd%dh", d, h);
    else if (h > 0) snprintf(buf, n, "%dh%dm", h, m);
    else snprintf(buf, n, "%dm", m);
}

/* Width helpers live further down; forward-declare for the prompt builder. */
static int mpcsh_prompt_cells(const char *p);
static int mpcsh_term_cols(void);

/* Build the first prompt row: `left`, then a fill (spaces or a glyph such as
 * the crystal rule) stretching to the right edge, then `meta` right-aligned
 * (zsh RPROMPT style).  The row ends one column short of the terminal width
 * so it never wraps, and `meta` is dropped entirely if it can't fit — it can
 * never overlap or cut the connector on the left. */
static void mpcsh_prompt_rslot(char *out, size_t osz, const char *left,
                               const char *meta, const char *fillcol,
                               const char *glyph, int cols)
{
    int lw = mpcsh_prompt_cells(left);
    int mw = mpcsh_prompt_cells(meta);
    int avail = cols - 1 - lw - mw - 2;       /* 2 = one space each side */
    size_t gl = strlen(glyph);
    if (avail < 3 || (size_t)avail * gl + strlen(left) + strlen(meta) + 64 > osz) {
        /* too narrow: no right slot, just a short crystal tail */
        snprintf(out, osz, "%s%s", left,
                 (gl > 1) ? " \033[0m━━━\033[0m" : "");
        return;
    }
    size_t o = (size_t)snprintf(out, osz, "%s %s", left, fillcol);
    for (int i = 0; i < avail && o + gl + 1 < osz; ++i) {
        memcpy(out + o, glyph, gl);
        o += gl;
    }
    snprintf(out + o, osz - o, "\033[0m %s", meta);
}

static void mpcsh_build_prompt(char *prompt, size_t psz,
                               const char *user, const char *host, const char *display)
{
    int r0,g0,b0,r1,g1,b1,r2,g2,b2;
    if (mpcsh_cfg_animated) mpcsh_anim_frame = (mpcsh_anim_frame + 1) % 24;
    mpcsh_accent_rgb(mpcsh_cfg_accent, 0, &r0,&g0,&b0);
    mpcsh_accent_rgb(mpcsh_cfg_accent, 1, &r1,&g1,&b1);
    mpcsh_accent_rgb(mpcsh_cfg_accent, 2, &r2,&g2,&b2);
    char a0[40], a1[40], a2[40], dim[40];
    snprintf(a0, sizeof a0, "\033[1;38;2;%d;%d;%dm", r0,g0,b0);
    snprintf(a1, sizeof a1, "\033[1;38;2;%d;%d;%dm", r1,g1,b1);
    snprintf(a2, sizeof a2, "\033[1;38;2;%d;%d;%dm", r2,g2,b2);
    snprintf(dim, sizeof dim, "\033[2;38;2;%d;%d;%dm", r2,g2,b2);

    char meta[256] = "";
    size_t mo = 0;
    if (mpcsh_cfg_show_clock) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        mo += (size_t)snprintf(meta + mo, sizeof meta - mo, "%s%02d:%02d\033[0m ",
                               dim, tm.tm_hour, tm.tm_min);
    }
    if (mpcsh_cfg_show_uptime) {
        char upb[32];
        mpcsh_fmt_uptime(upb, sizeof upb);
        mo += (size_t)snprintf(meta + mo, sizeof meta - mo, "%s↑%s\033[0m ", dim, upb);
    }
    if (mpcsh_cfg_show_cmd_time && mpcsh_last_cmd_secs >= 0.05) {
        mo += (size_t)snprintf(meta + mo, sizeof meta - mo, "%s⏱%.2fs\033[0m ",
                               dim, mpcsh_last_cmd_secs);
    }
    /* identity crumb for extensions / oh-my-mpcsh jokes */
    mo += (size_t)snprintf(meta + mo, sizeof meta - mo, "%smpcsh/%s\033[0m",
                           dim, MPCSH_VERSION_STRING);

    int cols = mpcsh_term_cols();

    if (mpcsh_cfg_connector == MPCSH_CONN_SIMPLE) {
        /* single-line prompt: no room for a right slot, so keep the meta
         * in front but ALWAYS separated from the prompt text. */
        snprintf(prompt, psz, "%s %s%s@%s\033[0m %s%s\033[0m %s❯\033[0m ",
                 meta, a1, user, host, a2, display, a0);
    } else if (mpcsh_cfg_connector == MPCSH_CONN_BLOCKS) {
        char left[1024], row1[2048];
        snprintf(left, sizeof left, "%s▌\033[0m%s%s\033[0m %s%s\033[0m %s%s\033[0m",
                 a0, a1, user, a2, host, a0, display);
        mpcsh_prompt_rslot(row1, sizeof row1, left, meta, dim, " ", cols);
        snprintf(prompt, psz, "%s\n%s▌\033[0m%s❯\033[0m ", row1, a0, a0);
    } else if (mpcsh_cfg_connector == MPCSH_CONN_AERO) {
        char left[1024], row1[2048];
        snprintf(left, sizeof left, "%s╭╌╌\033[0m %s%s\033[0m %s%s\033[0m %s%s\033[0m",
                 a0, a1, user, a2, host, a0, display);
        mpcsh_prompt_rslot(row1, sizeof row1, left, meta, dim, " ", cols);
        snprintf(prompt, psz, "%s\n%s╰╌╌\033[0m %s❯\033[0m ", row1, a0, a0);
    } else if (mpcsh_cfg_connector == MPCSH_CONN_STARSHIP) {
        /* Starship-style: blank line, "user@host in dir on  branch",
         * meta right-aligned, then a bare ❯ on its own line. */
        char left[1024], row1[2048], branch[160] = "";
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) {
            for (char *d = g_strdup(cwd); d; ) {
                char *head = g_build_filename(d, ".git", "HEAD", NULL);
                char *txt = NULL;
                if (g_file_get_contents(head, &txt, NULL, NULL) && txt) {
                    g_strstrip(txt);
                    if (g_str_has_prefix(txt, "ref: refs/heads/"))
                        g_strlcpy(branch, txt + 16, sizeof branch);
                    else
                        snprintf(branch, sizeof branch, "%.7s", txt);
                    g_free(txt); g_free(head); g_free(d);
                    break;
                }
                g_free(txt); g_free(head);
                char *parent = g_path_get_dirname(d);
                if (!strcmp(parent, d)) { g_free(parent); g_free(d); break; }
                g_free(d); d = parent;
            }
        }
        if (branch[0])
            snprintf(left, sizeof left, "%s╭─\033[0m %s%s\033[0m in %s%s\033[0m on %s %s\033[0m",
                     a0, a1, user, a2, display, a0, branch);
        else
            snprintf(left, sizeof left, "%s╭─\033[0m %s%s\033[0m@%s%s\033[0m in %s%s\033[0m",
                     a0, a1, user, a2, host, a0, display);
        mpcsh_prompt_rslot(row1, sizeof row1, left, meta, dim, " ", cols);
        snprintf(prompt, psz, "%s\n%s╰─\033[0m%s❯\033[0m ", row1, a0, a0);
    } else if (mpcsh_cfg_connector == MPCSH_CONN_CRYSTAL) {
        /* zsh-like crystal: identity on the left, a stretched crystal rule,
         * and the clock/meta parked on the RIGHT like zsh's RPROMPT.
         * The connector row below stays completely clean. */
        char left[1024], row1[2048];
    snprintf(left, sizeof left, "%s◈\033[0m %s%s\033[0m %s%s\033[0m %s%s\033[0m",
             a1, a1, user, a2, host, a0, display);
    mpcsh_prompt_rslot(row1, sizeof row1, left, meta, a0, "━", cols);
    snprintf(prompt, psz, "%s\n%s◆\033[0m %s❯\033[0m ", row1, a1, a0);
        } else {
            char left[1024], row1[2048];
            snprintf(left, sizeof left, "%s╭─\033[0m %s%s\033[0m %s%s\033[0m %s%s\033[0m",
                     a0, a1, user, a2, host, a0, display);
            mpcsh_prompt_rslot(row1, sizeof row1, left, meta, dim, " ", cols);
            snprintf(prompt, psz, "%s\n%s╰─\033[0m%s❯\033[0m ", row1, a0, a0);
        }
}



/* Runtime-tunable limits (can be raised via the "limits" builtin).
 * Raising them too high can turn your RAM into green candy. */
static size_t mpcsh_max_line    = MPCSH_MAX_LINE_DEFAULT;
static int    mpcsh_history_max = MPCSH_HISTORY_MAX_DEFAULT;

#define MPCSH_MAX_LINE     ((int)mpcsh_max_line)
#define MPCSH_HISTORY_MAX  mpcsh_history_max

/* ------------------------------------------------------------
 * Languages
 * ------------------------------------------------------------ */

typedef enum {
    MPCSH_LANG_NONE = 0,
    MPCSH_LANG_FISH,
    MPCSH_LANG_MPCSHELL,
    MPCSH_LANG_PYTHON,
    MPCSH_LANG_C,
    MPCSH_LANG_CPP,
    MPCSH_LANG_CSHARP,
    MPCSH_LANG_RUST,
    MPCSH_LANG_GO,
    MPCSH_LANG_JAVA,
    MPCSH_LANG_JAVASCRIPT,
    MPCSH_LANG_TYPESCRIPT,
    MPCSH_LANG_RUBY,
    MPCSH_LANG_PHP,
    MPCSH_LANG_LUA,
    MPCSH_LANG_ZIG,
    MPCSH_LANG_KOTLIN,
    MPCSH_LANG_COUNT
} MpcshLang;

static const char *
lang_name(MpcshLang lang)
{
    switch (lang) {
        case MPCSH_LANG_FISH:     return "fish";
        case MPCSH_LANG_MPCSHELL: return "mpcsh";
        case MPCSH_LANG_PYTHON:   return "python";
        case MPCSH_LANG_C:        return "c";
        case MPCSH_LANG_CPP:      return "cpp";
        case MPCSH_LANG_CSHARP:   return "csharp";
        default:                  return "none";
    }
}

static MpcshLang
lang_parse(const char *s)
{
    if (strcasecmp(s, "fish") == 0) return MPCSH_LANG_FISH;
    if (strcasecmp(s, "mpcsh") == 0 || strcasecmp(s, "mpsh") == 0) return MPCSH_LANG_MPCSHELL;
    if (strcasecmp(s, "python") == 0 || strcasecmp(s, "python3") == 0 || strcasecmp(s, "py") == 0) return MPCSH_LANG_PYTHON;
    if (strcasecmp(s, "c") == 0) return MPCSH_LANG_C;
    if (strcasecmp(s, "cpp") == 0 || strcasecmp(s, "c++") == 0) return MPCSH_LANG_CPP;
    if (strcasecmp(s, "csharp") == 0 || strcasecmp(s, "c#") == 0 || strcasecmp(s, "cs") == 0) return MPCSH_LANG_CSHARP;
    return MPCSH_LANG_NONE;
}

/* ------------------------------------------------------------
 * AI Notice
 * ------------------------------------------------------------ */

static const char MPCSH_AI_NOTICE[] =
"You are inside MarekPC Shell (mpcsh) — not bash, not fish, not zsh.\n"
"Write commands the MarekPC way.\n\n"
"Real syntax examples:\n"
"  set -g NAME value          # global variable\n"
"  set -l NAME value          # local\n"
"  if test -f file; echo yes; end\n"
"  while true; echo loop; end\n"
"  for f in *.c; echo $f; end\n"
"  function hello; echo hi $argv; end\n"
"  math 2 + 3 * 4\n"
"  source script.mpcsh arg1\n"
"  time sleep 0.2\n"
"  portmap                    # honest USB / board map\n"
"  quickinfo                  # RAM / CPU / GPU bars\n"
"  setbg /path/to/image.png   # wallpaper\n"
"  windowlaunch firefox\n"
"  mpcimgcat picture.png\n"
"  colorit #FF8800 < file\n\n"
"Pipelines work (up to 64 segments). Command substitution: $(cmd).\n"
"Do not emit bash -c / sh -c / && / || as a substitute for native syntax.\n"
"If you are unsure of a construct, ask for the MarekPC form or read mpcshman.\n";

static gboolean
mpcsh_ai_agent_detected(void)
{
    static const char *env_marks[] = {
        "CLINE", "CLINE_ROOT", "CLINE_AGENT",
        "CLAUDE_CODE", "CLAUDECODE", "ANTHROPIC_AGENT",
        "CURSOR_AGENT", "CURSOR_TRACE_ID",
        "AIDER", "CODEX_AGENT", "OSS_AGENT", "OSS_MODEL",
        "AGENT", "AGENT_NAME", "AI_AGENT",
        "CONTINUE_AGENT", "WINDSURF_AGENT", "COPILOT_AGENT",
        NULL
    };
    for (int i = 0; env_marks[i]; ++i)
        if (g_getenv(env_marks[i]) != NULL)
            return TRUE;

    char path[MPCSH_MAX_PATH];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)getppid());
    gchar *cmd = NULL;
    gsize cmd_len = 0;
    if (g_file_get_contents(path, &cmd, &cmd_len, NULL) && cmd) {
        for (gsize i = 0; i < cmd_len; ++i)
            if (cmd[i] == '\0') cmd[i] = ' ';
            static const char *proc_marks[] = {
                "cline", "claude", "cursor", "aider", "codex",
                "oss-agent", "agent", "continue", "windsurf", "copilot",
                NULL
            };
        for (int i = 0; proc_marks[i]; ++i) {
            if (strcasestr(cmd, proc_marks[i]) != NULL) {
                g_free(cmd);
                return TRUE;
            }
        }
        g_free(cmd);
    }
    return FALSE;
}

/* Return only syntax-bearing characters outside quotes/escapes.
 * Quoted user text must never trigger syntax rejection. */
static char *
mpcsh_unquoted_view(const char *line)
{
    GString *v = g_string_new(NULL);
    char quote = 0;
    gboolean escaped = FALSE;
    for (const char *p = line ? line : ""; *p; ++p) {
        char c = *p;
        if (escaped) { g_string_append_c(v, ' '); escaped = FALSE; continue; }
        if (!quote && c == '\\') { escaped = TRUE; g_string_append_c(v, ' '); continue; }
        if (quote) {
            if (c == quote) quote = 0;
            g_string_append_c(v, ' ');
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            g_string_append_c(v, ' ');
        } else {
            g_string_append_c(v, c);
        }
    }
    return g_string_free(v, FALSE);
}

/* Detect classic AI “switch to bash” escape attempts. */
static gboolean
mpcsh_looks_like_bash_escape(const char *line)
{
    if (!line || !*line) return FALSE;
    char *view = mpcsh_unquoted_view(line);
    gboolean found = strcasestr(view, "bash -c") || strcasestr(view, "sh -c") ||
    strcasestr(view, "/bin/bash") || strcasestr(view, "/bin/sh") ||
    strcasestr(view, "exec bash") || strcasestr(view, "exec sh") ||
    strstr(view, "&&") || strstr(view, "||");
    g_free(view);
    return found;
}

static void
mpcsh_print_ai_notice(void)
{
    fprintf(stderr, "\n%s\n", MPCSH_AI_NOTICE);
    fflush(stderr);
}

/* ------------------------------------------------------------
 * EINTR-safe helpers.
 *
 * Ctrl+C raises SIGINT. The handler only sets a flag, but any blocking
 * read(2)/waitpid(2) in flight returns -1/EINTR. Previously that was
 * mistaken for EOF and the REPL exited, killing the whole shell.
 * Every blocking call below goes through these retrying wrappers.
 * ------------------------------------------------------------ */

static ssize_t
mpcsh_read_retry(int fd, void *buf, size_t count)
{
    for (;;) {
        ssize_t n = read(fd, buf, count);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue; /* signal arrived; just try again */
            return -1;
    }
}

static pid_t
mpcsh_waitpid_retry(pid_t pid, int *status)
{
    for (;;) {
        pid_t r = waitpid(pid, status, 0);
        if (r >= 0)
            return r;
        if (errno == EINTR)
            continue;
        return -1;
    }
}

static int
mpcsh_child_result(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 128;
}

/* ------------------------------------------------------------
 * Command substitution
 * ------------------------------------------------------------
 *
 * Substitutions are evaluated by this engine itself.  There is no external shell interpreter, command-string hop, clexec,
 * or compatibility-shell layer anywhere in the mpcsh execution path.
 * ------------------------------------------------------------ */

static int mpcsh_run_line(char *cmdline);
static int mpc_function_call(char **argv);

/* Recursion depth guard for command substitution. */
#define MPCSH_MAX_SUBST_DEPTH 8
#define MPCSH_MAX_SUBST_OUTPUT (1024 * 1024) /* 1 MiB per substitution */
static int mpcsh_subst_depth = 0;

static char *
mpcsh_capture_command(const char *cmd)
{
    if (mpcsh_subst_depth >= MPCSH_MAX_SUBST_DEPTH) {
        fprintf(stderr, "mpcsh: command substitution nesting too deep (max %d)\n",
                MPCSH_MAX_SUBST_DEPTH);
        return g_strdup("");
    }

    int pipefd[2];
    if (pipe(pipefd) < 0)
        return g_strdup("");

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return g_strdup("");
    }

    if (pid == 0) {
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(pipefd[1]);

        char *nested = g_strdup(cmd);
        int status = mpcsh_run_line(nested);
        g_free(nested);
        fflush(stdout);
        _exit(status < 0 ? 127 : status);
    }

    close(pipefd[1]);
    GString *out = g_string_new(NULL);
    char buf[4096];
    ssize_t n;
    /* EINTR-safe: SIGINT during capture must not truncate the result. */
    gboolean capture_failed = FALSE;
    for (;;) {
        n = mpcsh_read_retry(pipefd[0], buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            fprintf(stderr, "mpcsh: command substitution read: %s\n", strerror(errno));
            capture_failed = TRUE;
            break;
        }
        if ((size_t)n > MPCSH_MAX_SUBST_OUTPUT - out->len) {
            fprintf(stderr, "mpcsh: command substitution output exceeds %u bytes\n",
                    (unsigned)MPCSH_MAX_SUBST_OUTPUT);
            capture_failed = TRUE;
            break;
        }
        g_string_append_len(out, buf, n);
    }
    close(pipefd[0]);

    int status = 0;
    if (capture_failed)
        (void)kill(pid, SIGTERM);
    pid_t waited = mpcsh_waitpid_retry(pid, &status);
    if (waited < 0) {
        fprintf(stderr, "mpcsh: command substitution waitpid: %s\n", strerror(errno));
        capture_failed = TRUE;
    }
    if (capture_failed) {
        g_string_free(out, TRUE);
        return g_strdup("");
    }

    while (out->len > 0 && (out->str[out->len-1] == '\n' || out->str[out->len-1] == '\r'))
        g_string_truncate(out, out->len - 1);

    return g_string_free(out, FALSE);
}

static char * G_GNUC_UNUSED
mpcsh_expand_substitutions(const char *line)
{
    GString *result = g_string_sized_new(strlen(line) + 64);
    const char *p = line;

    while (*p) {
        if (*p == '$' && *(p+1) == '(') {
            const char *start = p + 2;
            const char *q = start;
            int depth = 1;
            while (*q && depth > 0) {
                if (*q == '(') depth++;
                else if (*q == ')') depth--;
                if (depth > 0) q++;
            }
            if (depth == 0) {
                char *inner = g_strndup(start, q - start);
                mpcsh_subst_depth++;
                char *expanded = mpcsh_capture_command(inner);
                mpcsh_subst_depth--;
                g_string_append(result, expanded);
                g_free(expanded);
                g_free(inner);
                p = q + 1;
                continue;
            }
        }
        g_string_append_c(result, *p);
        p++;
    }
    return g_string_free(result, FALSE);
}

/* ------------------------------------------------------------
 * Native Fish-first lexer
 *
 * This is deliberately NOT GLib's shell parser.  mpcsh owns the
 * command grammar.  The lexer understands:
 *   'single quoted text'
 *   "double quoted text"
 *   \ escaped characters
 *   $NAME / ${NAME}
 *   $(nested mpcsh command)
 *   |  ;  >  >>  <  2>  2>>
 *
 * It deliberately rejects common Bourne/bash syntax:
 *   `backticks`  &&  ||  VAR=value  export VAR=...
 *
 * Expansion is performed by mpcsh itself.  External programs are
 * launched with execvp(3), never through a shell interpreter.
 * ------------------------------------------------------------ */

static GHashTable *mpcsh_vars = NULL;

static void
mpcsh_vars_init(void)
{
    if (!mpcsh_vars)
        mpcsh_vars = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

static const char *
mpcsh_var_get(const char *name)
{
    mpcsh_vars_init();
    const char *v = g_hash_table_lookup(mpcsh_vars, name);
    if (v) return v;
    return g_getenv(name);
}

static void
mpcsh_var_set_local(const char *name, const char *value)
{
    mpcsh_vars_init();
    g_hash_table_replace(mpcsh_vars, g_strdup(name), g_strdup(value ? value : ""));
}

static void
mpcsh_var_set_global(const char *name, const char *value)
{
    mpcsh_vars_init();
    g_hash_table_replace(mpcsh_vars, g_strdup(name), g_strdup(value ? value : ""));
    g_setenv(name, value ? value : "", TRUE);
}

static gboolean
mpcsh_identifier_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int
mpcsh_word_split(char *segment, char **argv, int max)
{
    int argc = 0;
    char *p = segment;

    mpcsh_vars_init();

    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (!*p) break;

        /* Fish-style redirection operators are first-class tokens. */
        const char *redir = NULL;
        size_t redir_len = 0;
        if (!strncmp(p, "2>>", 3) || !strncmp(p, "&>>", 3)) {
            redir = p; redir_len = 3;
        } else if (!strncmp(p, "2>", 2) || !strncmp(p, "&>", 2)) {
            redir = p; redir_len = 2;
        } else if (*p == '>' || *p == '<') {
            redir = p; redir_len = 1;
        }
        if (redir) {
            if (argc >= max - 1) {
                fprintf(stderr, "mpcsh: too many arguments (maximum: %d)\n", max - 1);
                argv[0] = NULL;
                return 0;
            }
            argv[argc++] = g_strndup(redir, redir_len);
            p += redir_len;
            continue;
        }

        if (argc >= max - 1) {
            fprintf(stderr, "mpcsh: too many arguments (maximum: %d)\n", max - 1);
            argv[0] = NULL;
            return 0;
        }

        GString *word = g_string_new(NULL);
        gboolean single = FALSE, doubleq = FALSE;
        gboolean started = FALSE;
        gboolean split_substitution = FALSE;

        while (*p) {
            char c = *p;

            if (!single && !doubleq && isspace((unsigned char)c))
                break;

            if (!single && !doubleq && c == '\\') {
                ++p;
                if (!*p) {
                    fprintf(stderr, "mpcsh: trailing backslash\n");
                    g_string_free(word, TRUE);
                    argv[0] = NULL;
                    return 0;
                }
                g_string_append_c(word, *p++);
                started = TRUE;
                continue;
            }

            if (!doubleq && c == '\'') {
                single = !single;
                started = TRUE;
                ++p;
                continue;
            }

            if (!single && c == '"') {
                doubleq = !doubleq;
                started = TRUE;
                ++p;
                continue;
            }

            if (!single && ((c == '$' && p[1] == '(') ||
                (!doubleq && !started && c == '('))) {
                int offset = c == '$' ? 2 : 1;
            split_substitution = !doubleq && !started;
            int depth = 1;
            const char *q = p + offset;
            gboolean qsingle = FALSE, qdouble = FALSE;

            while (*q && depth) {
                if (*q == '\\' && q[1]) { q += 2; continue; }
                if (!qdouble && *q == '\'') qsingle = !qsingle;
                else if (!qsingle && *q == '"') qdouble = !qdouble;
                else if (!qsingle && !qdouble && *q == '(') ++depth;
                else if (!qsingle && !qdouble && *q == ')') --depth;
                if (depth) ++q;
            }

            if (depth != 0) {
                fprintf(stderr, "mpcsh: unterminated command substitution\n");
                g_string_free(word, TRUE);
                argv[0] = NULL;
                return 0;
            }

            char *inner = g_strndup(p + offset, (gsize)(q - (p + offset)));
            mpcsh_subst_depth++;
            char *expanded = mpcsh_capture_command(inner);
            mpcsh_subst_depth--;
            g_string_append(word, expanded ? expanded : "");
            g_free(expanded);
            g_free(inner);
            p = (char *)q + 1;
            started = TRUE;
            continue;
                }

                if (!single && c == '$') {
                    size_t j = 1;
                    char name[256] = {0};

                    if (p[j] == '{') {
                        ++j;
                        size_t n = 0;
                        while (p[j] && p[j] != '}' && n + 1 < sizeof(name) &&
                            mpcsh_identifier_char(p[j]))
                            name[n++] = p[j++];
                        if (p[j] != '}') {
                            fprintf(stderr, "mpcsh: malformed ${...} expansion\n");
                            g_string_free(word, TRUE);
                            argv[0] = NULL;
                            return 0;
                        }
                        name[n] = '\0';
                        ++j;
                    } else if (mpcsh_identifier_char(p[j])) {
                        size_t n = 0;
                        while (p[j] && mpcsh_identifier_char(p[j]) &&
                            n + 1 < sizeof(name))
                            name[n++] = p[j++];
                        name[n] = '\0';
                    } else {
                        g_string_append_c(word, '$');
                        ++p;
                        started = TRUE;
                        continue;
                    }

                    const char *v = mpcsh_var_get(name);
                    if (v) g_string_append(word, v);
                    p += j;
                    started = TRUE;
                    continue;
                }

                if (!single && c == '`') {
                    fprintf(stderr,
                            "mpcsh: backticks are not mpcsh syntax; use $(...)\n");
                    g_string_free(word, TRUE);
                    argv[0] = NULL;
                    return 0;
                }

                g_string_append_c(word, c);
                ++p;
                started = TRUE;
        }

        if (single || doubleq) {
            fprintf(stderr, "mpcsh: unterminated quote\n");
            g_string_free(word, TRUE);
            argv[0] = NULL;
            return 0;
        }

        if (started) {
            char *value = g_string_free(word, FALSE);
            /* Fish-like tilde expansion for an unquoted leading ~/... . */
            if (value[0] == '~' && (value[1] == '/' || value[1] == '\0')) {
                const char *home = g_get_home_dir();
                if (home && *home) {
                    char *expanded = g_build_filename(home, value + (value[1] == '/' ? 1 : 0), NULL);
                    g_free(value);
                    value = expanded;
                }
            }
            if (split_substitution) {
                char **items = g_strsplit(value, "\n", -1);
                for (int j = 0; items[j]; ++j) {
                    if (!*items[j]) continue;
                    if (argc >= max - 1) {
                        fprintf(stderr, "mpcsh: too many substitution arguments\n");
                        g_strfreev(items); g_free(value);
                        for (int k = 0; k < argc; ++k) g_free(argv[k]);
                        argv[0] = NULL; return 0;
                    }
                    argv[argc++] = g_strdup(items[j]);
                }
                g_strfreev(items); g_free(value);
            } else argv[argc++] = value;
        } else
            g_string_free(word, TRUE);

        while (*p && isspace((unsigned char)*p)) ++p;
    }

    argv[argc] = NULL;
    return argc;
}

static void
mpcsh_free_argv(char **argv)
{
    if (!argv) return;
    for (int i = 0; argv[i]; ++i)
        g_free(argv[i]);
}

static gboolean
mpcsh_is_bourne_syntax(const char *s)
{
    if (!s) return FALSE;
    char *view = mpcsh_unquoted_view(s);
    gboolean found = FALSE;
    if (strstr(view, "&&") || strstr(view, "||"))
        found = TRUE;
    else if (strstr(view, "export ") == view || !strncmp(view, "export\\t", 7))
        found = TRUE;
    else {
        const char *p = view;
        while (*p && !found) {
            while (isspace((unsigned char)*p)) ++p;
            if (isalpha((unsigned char)*p) || *p == '_') {
                ++p;
                while (mpcsh_identifier_char(*p)) ++p;
                if (*p == '=') found = TRUE;
            }
            while (*p && *p != ';' && *p != '|') ++p;
            if (*p) ++p;
        }
    }
    g_free(view);
    return found;
}

/* ------------------------------------------------------------
 * CTK — Call The Kernel
 * Native mpcsh syscall-style builtins.
 * ------------------------------------------------------------ */
static volatile sig_atomic_t got_sigint = 0;
static int mpcsh_last_status = 0;

static int mpcsh_ctk_builtin(char **argv)
{
    if (!argv || !argv[0] || strcmp(argv[0], "ctk") != 0) return -1;
    if (!argv[1]) {
        fprintf(stderr, "MarekPC CTK — Call The Kernel\n"
        "Usage: ctk <call>\n"
        "Calls: fork() getpid() getppid() wait(pid) sleep(seconds)\n"
        "       chdir(path) exit(code) kill(pid,signal)\n");
        return 0;
    }
    GString *joined = g_string_new(NULL);
    for (int i = 1; argv[i]; ++i) { if (i > 1) g_string_append_c(joined, ' '); g_string_append(joined, argv[i]); }
    char *call = joined->str; while (*call && isspace((unsigned char)*call)) ++call;
    char name[64] = {0}; const char *p = call;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) { size_t n = (size_t)(p - call); if (n + 1 < sizeof(name)) name[n] = *p; ++p; }
    if (!name[0]) { fprintf(stderr, "mpcsh: ctk: invalid kernel call: %s\n", call); g_string_free(joined, TRUE); return 2; }
    char arg[PATH_MAX] = {0}; const char *l = strchr(call, '('), *r = strrchr(call, ')');
    if (l && r && r > l) { size_t n = (size_t)(r - l - 1); if (n >= sizeof(arg)) { fprintf(stderr, "mpcsh: ctk: argument too long\n"); g_string_free(joined, TRUE); return 2; } memcpy(arg, l + 1, n); arg[n] = '\0'; while (*arg && isspace((unsigned char)*arg)) memmove(arg, arg + 1, strlen(arg)); n = strlen(arg); while (n && isspace((unsigned char)arg[n-1])) arg[--n] = '\0'; if (n >= 2 && ((arg[0] == '"' && arg[n-1] == '"') || (arg[0] == '\'' && arg[n-1] == '\''))) { memmove(arg, arg + 1, n - 2); arg[n-2] = '\0'; } }
    else if (argv[2]) g_strlcpy(arg, argv[2], sizeof(arg));

    if (strcasecmp(name, "fork") == 0) {
        pid_t pid = fork();
        if (pid < 0) { fprintf(stderr, "mpcsh: ctk fork(): %s\n", strerror(errno)); g_string_free(joined, TRUE); return 1; }
        if (pid == 0) { printf("ctk fork(): child pid=%ld, parent pid=%ld\n", (long)getpid(), (long)getppid()); fflush(stdout); _exit(0); }
        printf("ctk fork(): child pid=%ld\n", (long)pid); fflush(stdout); int status = 0;
        if (mpcsh_waitpid_retry(pid, &status) < 0) { fprintf(stderr, "mpcsh: ctk fork(): waitpid: %s\n", strerror(errno)); g_string_free(joined, TRUE); return 1; }
        g_string_free(joined, TRUE); return mpcsh_child_result(status);
    }
    if (strcasecmp(name, "getpid") == 0) { printf("%ld\n", (long)getpid()); g_string_free(joined, TRUE); return 0; }
    if (strcasecmp(name, "getppid") == 0) { printf("%ld\n", (long)getppid()); g_string_free(joined, TRUE); return 0; }
    if (strcasecmp(name, "wait") == 0) { char *e = NULL; errno = 0; long n = strtol(arg, &e, 0); if (errno || !*arg || !e || *e || n <= 0) { fprintf(stderr, "mpcsh: ctk wait(pid): invalid pid: %s\n", arg); g_string_free(joined, TRUE); return 2; } int st = 0; if (mpcsh_waitpid_retry((pid_t)n, &st) < 0) { fprintf(stderr, "mpcsh: ctk wait(%ld): %s\n", n, strerror(errno)); g_string_free(joined, TRUE); return 1; } printf("ctk wait(%ld): status=%d\n", n, mpcsh_child_result(st)); g_string_free(joined, TRUE); return mpcsh_child_result(st); }
    if (strcasecmp(name, "sleep") == 0) { char *e = NULL; errno = 0; long n = strtol(arg, &e, 0); if (errno || !*arg || !e || *e || n < 0) { fprintf(stderr, "mpcsh: ctk sleep(seconds): invalid value: %s\n", arg); g_string_free(joined, TRUE); return 2; } unsigned int left = (unsigned int)n; while (left && !got_sigint) left = sleep(left); g_string_free(joined, TRUE); return got_sigint ? 130 : 0; }
    if (strcasecmp(name, "chdir") == 0) { if (!*arg || chdir(arg) < 0) { fprintf(stderr, "mpcsh: ctk chdir(%s): %s\n", arg, strerror(errno)); g_string_free(joined, TRUE); return 1; } g_string_free(joined, TRUE); return 0; }
    if (strcasecmp(name, "exit") == 0) { int code = 0; if (*arg) { char *e = NULL; long n = strtol(arg, &e, 0); if (!e || *e || n < 0 || n > 255) { fprintf(stderr, "mpcsh: ctk exit(code): invalid code: %s\n", arg); g_string_free(joined, TRUE); return 2; } code = (int)n; } g_string_free(joined, TRUE); exit(code); }
    if (strcasecmp(name, "kill") == 0) {
        if (geteuid() != 0) {
            fprintf(stderr, "mpcsh: ctk kill(): requires root privileges (run under sudo or as root)\n");
            g_string_free(joined, TRUE);
            return 1;
        }
        char *c = strchr(arg, ',');
        if (!c) {
            fprintf(stderr, "mpcsh: ctk kill(pid,signal): expected pid,signal\n");
            g_string_free(joined, TRUE);
            return 2;
        }
        *c = '\0';
        char *e1 = NULL, *e2 = NULL;
        errno = 0;
        long pid = strtol(arg, &e1, 0), sig = strtol(c + 1, &e2, 0);
        if (errno || !e1 || *e1 || !e2 || *e2 || pid <= 0 || sig < 0) {
            fprintf(stderr, "mpcsh: ctk kill(pid,signal): invalid arguments\n");
            g_string_free(joined, TRUE);
            return 2;
        }
        if (kill((pid_t)pid, (int)sig) < 0) {
            fprintf(stderr, "mpcsh: ctk kill(%ld,%ld): %s\n", pid, sig, strerror(errno));
            g_string_free(joined, TRUE);
            return 1;
        }
        g_string_free(joined, TRUE);
        return 0;
    }
    fprintf(stderr, "mpcsh: ctk: unknown kernel call '%s'\nAvailable: fork() getpid() getppid() wait(pid) sleep(seconds)\n           chdir(path) exit(code) kill(pid,signal)\n", name); g_string_free(joined, TRUE); return 127;
}

/* Built-ins defined later in this file but dispatched by the pipeline engine. */
static int mpcsh_portmap_builtin(char **argv);
static int mpcsh_quickinfo_builtin(char **argv);
static int mpcsh_setbg_builtin(char **argv);
static int mpcsh_gpuportmap_builtin(char **argv);

/* ------------------------------------------------------------
 * 1.9: wallpaper path, setbg, quickinfo, gpuportmap
 * ------------------------------------------------------------ */
static const char *mpcsh_wallpaper_path(void)
{
    static char buf[PATH_MAX];
    char *user = g_build_filename(g_get_user_config_dir(), "mpcsh", "wallpaper.png", NULL);
    if (g_file_test(user, G_FILE_TEST_IS_REGULAR))
        g_strlcpy(buf, user, sizeof buf);
    else
        g_strlcpy(buf, "/bin/mpcshresources/wallpaper.png", sizeof buf);
    g_free(user);
    return buf;
}

static int mpcsh_setbg_builtin(char **argv)
{
    if (!argv || !argv[0]) return -1;
    if (!argv[1] || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        printf("Usage: setbg /path/to/image.(png|jpg|...)\n"
               "Current wallpaper: %s\n", mpcsh_wallpaper_path());
        return argv[1] ? 0 : 1;
    }
    char *src = g_strdup(argv[1]);
    if (src[0] == '~' && (src[1] == '/' || !src[1])) {
        char *t = g_build_filename(g_get_home_dir(), src + (src[1] ? 2 : 1), NULL);
        g_free(src); src = t;
    }
    GError *err = NULL;
    GdkPixbuf *pix = gdk_pixbuf_new_from_file(src, &err);
    if (!pix) {
        fprintf(stderr, "setbg: cannot load '%s': %s\n", src, err ? err->message : "unknown error");
        if (err) g_error_free(err);
        g_free(src);
        return 1;
    }
    char *dir = g_build_filename(g_get_user_config_dir(), "mpcsh", NULL);
    char *dst = g_build_filename(dir, "wallpaper.png", NULL);
    int rc = 0;
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        fprintf(stderr, "setbg: cannot create %s: %s\n", dir, strerror(errno));
        rc = 1;
    } else if (!gdk_pixbuf_save(pix, dst, "png", &err, NULL)) {
        fprintf(stderr, "setbg: cannot write %s: %s\n", dst, err ? err->message : "unknown error");
        if (err) g_error_free(err);
        rc = 1;
    } else {
        printf("Wallpaper set: %s (%dx%d)\n", dst,
               gdk_pixbuf_get_width(pix), gdk_pixbuf_get_height(pix));
    }
    g_object_unref(pix);
    g_free(dir); g_free(dst); g_free(src);
    return rc;
}

/* read first line of a file into buf; returns 1 on success */
static int mpcsh_sys_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(buf, (int)n, f)) { fclose(f); return 0; }
    fclose(f);
    buf[strcspn(buf, "\r\n")] = 0;
    return 1;
}

static void mpcsh_info_bar(const char *label, double pct, const char *tail)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = 30, fill = (int)(pct * w / 100.0 + 0.5);
    const char *col = pct < 60 ? "\033[32m" : (pct < 85 ? "\033[33m" : "\033[31m");
    printf("  %-6s %s", label, col);
    for (int i = 0; i < w; i++) fputs(i < fill ? "█" : "░", stdout);
    printf("\033[0m %5.1f%%  %s\n", pct, tail ? tail : "");
}

static const char *mpcsh_pci_vendor(const char *v)
{
    if (!strcmp(v, "0x10de")) return "NVIDIA";
    if (!strcmp(v, "0x1002")) return "AMD";
    if (!strcmp(v, "0x8086")) return "Intel";
    return "Unknown";
}

static int mpcsh_quickinfo_builtin(char **argv)
{
    (void)argv;
    char line[512];

    printf("\033[1mMarekPC QuickInfo\033[0m\n");

    /* RAM */
    unsigned long long mt = 0, ma = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            sscanf(line, "MemTotal: %llu kB", &mt);
            sscanf(line, "MemAvailable: %llu kB", &ma);
        }
        fclose(f);
    }
    if (mt) {
        char tail[96];
        snprintf(tail, sizeof tail, "%.2f GiB used / %.2f GiB, %.2f GiB free",
                 (mt - ma) / 1048576.0, mt / 1048576.0, ma / 1048576.0);
        mpcsh_info_bar("RAM", 100.0 * (mt - ma) / mt, tail);
    }

    /* CPU clocks */
    double sum = 0, mx = 0; int cores = 0;
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            double mhz;
            if (sscanf(line, "cpu MHz : %lf", &mhz) == 1) { sum += mhz; cores++; if (mhz > mx) mx = mhz; }
        }
        fclose(f);
    }
    if (cores) printf("  CPU    %d threads, avg %.0f MHz, max %.0f MHz\n", cores, sum / cores, mx);

    /* load */
    double l1 = 0, l5 = 0, l15 = 0;
    if (mpcsh_sys_line("/proc/loadavg", line, sizeof line) &&
        sscanf(line, "%lf %lf %lf", &l1, &l5, &l15) == 3) {
        long nproc = sysconf(_SC_NPROCESSORS_ONLN);
        char tail[64];
        snprintf(tail, sizeof tail, "load %.2f %.2f %.2f", l1, l5, l15);
        mpcsh_info_bar("Load", nproc > 0 ? 100.0 * l1 / nproc : 0, tail);
    }

    /* GPU / VRAM (amdgpu exposes these; others may not) */
    for (int c = 0; c < 8; c++) {
        char p[160], a[64], b[64];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/mem_info_vram_used", c);
        if (!mpcsh_sys_line(p, a, sizeof a)) continue;
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/mem_info_vram_total", c);
        if (!mpcsh_sys_line(p, b, sizeof b)) continue;
        double used = strtod(a, NULL), tot = strtod(b, NULL);
        if (tot > 0) {
            char tail[96];
            snprintf(tail, sizeof tail, "%.2f GiB / %.2f GiB", used / 1073741824.0, tot / 1073741824.0);
            mpcsh_info_bar("VRAM", 100.0 * used / tot, tail);
        }
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/gpu_busy_percent", c);
        if (mpcsh_sys_line(p, a, sizeof a)) mpcsh_info_bar("GPU", atof(a), "");
        break;
    }

    /* uptime */
    double up = 0;
    if (mpcsh_sys_line("/proc/uptime", line, sizeof line) && sscanf(line, "%lf", &up) == 1) {
        long s = (long)up;
        printf("  Uptime %ldd %02ldh %02ldm\n", s / 86400, (s % 86400) / 3600, (s % 3600) / 60);
    }

    /* temperatures */
    int shown = 0;
    for (int z = 0; z < 16; z++) {
        char p[160], t[64], ty[64];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        if (!mpcsh_sys_line(p, t, sizeof t)) continue;
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/type", z);
        if (!mpcsh_sys_line(p, ty, sizeof ty)) strcpy(ty, "zone");
        printf("%s%s %.1f°C", shown ? ",  " : "  Temps  ", ty, atof(t) / 1000.0);
        shown++;
    }
    if (shown) putchar('\n');
    return 0;
}

static int mpcsh_gpuportmap_builtin(char **argv)
{
    (void)argv;
    char buf[256], p[320];
    int found = 0;

    printf("\033[1mMarekPC GPU PortMap\033[0m\n");
    for (int c = 0; c < 8; c++) {
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/vendor", c);
        char ven[32], dev[32];
        if (!mpcsh_sys_line(p, ven, sizeof ven)) continue;
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/device", c);
        if (!mpcsh_sys_line(p, dev, sizeof dev)) strcpy(dev, "?");
        found++;

        char drv[128] = "unknown";
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/driver", c);
        ssize_t n = readlink(p, buf, sizeof buf - 1);
        if (n > 0) { buf[n] = 0; g_strlcpy(drv, g_path_get_basename(buf), sizeof drv); }

        printf("\n  card%d: %s [%s:%s]  driver: %s\n", c, mpcsh_pci_vendor(ven), ven, dev, drv);

        char cs[48], cw[16], ms[48], mw[16];
        const char *names[4] = { "current_link_speed", "current_link_width", "max_link_speed", "max_link_width" };
        char *outs[4] = { cs, cw, ms, mw };
        for (int i = 0; i < 4; i++) {
            snprintf(p, sizeof p, "/sys/class/drm/card%d/device/%s", c, names[i]);
            if (!mpcsh_sys_line(p, outs[i], i % 2 ? 16 : 48)) strcpy(outs[i], "?");
        }
        printf("    PCIe:  x%s @ %s   (max x%s @ %s)\n", cw, cs, mw, ms);

        char a[64], b[64];
        snprintf(p, sizeof p, "/sys/class/drm/card%d/device/mem_info_vram_total", c);
        if (mpcsh_sys_line(p, a, sizeof a)) {
            snprintf(p, sizeof p, "/sys/class/drm/card%d/device/mem_info_vram_used", c);
            double tot = strtod(a, NULL) / 1073741824.0, used = 0;
            if (mpcsh_sys_line(p, b, sizeof b)) used = strtod(b, NULL) / 1073741824.0;
            printf("    VRAM:  %.2f GiB used / %.2f GiB\n", used, tot);
        }

        /* display connectors on this card */
        char pat[64];
        snprintf(pat, sizeof pat, "card%d-", c);
        DIR *d = opendir("/sys/class/drm");
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strncmp(e->d_name, pat, strlen(pat)) != 0) continue;
                char st[32], mode[64] = "";
                snprintf(p, sizeof p, "/sys/class/drm/%s/status", e->d_name);
                if (!mpcsh_sys_line(p, st, sizeof st)) continue;
                snprintf(p, sizeof p, "/sys/class/drm/%s/modes", e->d_name);
                mpcsh_sys_line(p, mode, sizeof mode);
                int on = !strcmp(st, "connected");
                printf("    %s%s\033[0m %-14s %s\n", on ? "\033[32m●" : "\033[90m○",
                       "", e->d_name + strlen(pat), on ? mode : st);
            }
            closedir(d);
        }
    }
    if (!found) printf("  No DRM GPU found in /sys/class/drm.\n");
    return 0;
}


static int mpcsh_guiinterm_builtin(char **argv);
static int mpcsh_set_builtin(char **argv);
static int mpcsh_cd_builtin(char **argv);
static int mpcsh_pwd_builtin(char **argv);
static int mpcsh_status_builtin(char **argv);
static int mpcsh_ls_builtin(char **argv);
static int mpcsh_mpcshman_builtin(char **argv);   /* 1.8: defined before mpcsh_run_pipeline */
static int mpcsh_math_builtin(char **argv);       /* 1.8 */
static int mpcsh_source_builtin(char **argv);
static int mpcsh_chuctrl_builtin(char **argv);
static void mpcsh_chu_exec(char **argv);
static void mpcsh_script_set_args(const char *script, int n, char **args);
static void mpcsh_chu_load_all(void);
static int mpcsh_wallpaper_builtin(char **argv);
static int mpcsh_imgcat_builtin(char **argv);
static int mpcsh_limits_builtin(char **argv);
static int mpcsh_windowlaunch_builtin(char **argv);
static int mpcsh_windowterminate_builtin(char **argv);
static int mpcsh_wlidlist_builtin(char **argv);
static int mpcsh_windowpicker_builtin(char **argv);
static int mpcsh_extension_builtin(char **argv);
__attribute__((used)) static int mpcsh_mpcupdate_builtin(char **argv);
__attribute__((used)) static int mpcsh_mpcbits_builtin(char **argv);
__attribute__((used)) static int mpcsh_mpcversion_builtin(char **argv);
/* ------------------------------------------------------------
 * Fish-native core builtins
 * ------------------------------------------------------------ */

static int
mpcsh_set_builtin(char **argv)
{
    if (!argv || !argv[0] || strcmp(argv[0], "set") != 0) return -1;

    if (!argv[1]) {
        mpcsh_vars_init();
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, mpcsh_vars);
        while (g_hash_table_iter_next(&it, &key, &value))
            printf("%s\t%s\n", (char *)key, (char *)value);
        return 0;
    }

    gboolean global = FALSE;
    int i = 1;

    while (argv[i] && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--global")) {
            global = TRUE;
        } else if (!strcmp(argv[i], "-l") || !strcmp(argv[i], "--local")) {
            global = FALSE;
        } else if (!strcmp(argv[i], "-x") || !strcmp(argv[i], "--export")) {
            global = TRUE;
        } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--query")) {
            ++i;
            if (!argv[i]) return 2;
            printf("%s\n", mpcsh_var_get(argv[i]) ? "yes" : "no");
            return 0;
        } else if (!strcmp(argv[i], "--show")) {
            ++i;
            if (!argv[i]) return 2;
            const char *v = mpcsh_var_get(argv[i]);
            if (v) printf("%s=%s\n", argv[i], v);
            return v ? 0 : 1;
        } else {
            fprintf(stderr, "mpcsh: set: unknown option '%s'\n", argv[i]);
            return 2;
        }
        ++i;
    }

    if (!argv[i]) {
        fprintf(stderr, "mpcsh: set: variable name required\n");
        return 2;
    }

    const char *name = argv[i++];
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) {
        fprintf(stderr, "mpcsh: set: invalid variable name '%s'\n", name);
        return 2;
    }
    for (const char *p = name + 1; *p; ++p) {
        if (!mpcsh_identifier_char(*p)) {
            fprintf(stderr, "mpcsh: set: invalid variable name '%s'\n", name);
            return 2;
        }
    }

    GString *value = g_string_new(NULL);
    for (; argv[i]; ++i) {
        if (value->len) g_string_append_c(value, ' ');
        g_string_append(value, argv[i]);
    }

    if (global)
        mpcsh_var_set_global(name, value->str);
    else
        mpcsh_var_set_local(name, value->str);

    g_string_free(value, TRUE);
    return 0;
}

/* limits — runtime tuning of history size and line length.
 *   limits                  show current values
 *   limits history N        set history depth (1 … 4096)
 *   limits line N           set max line length (256 … 65536)
 *
 * Values above the defaults print a friendly “green candy” warning. */
static int
mpcsh_limits_builtin(char **argv)
{
    if (!argv || !argv[0] || strcmp(argv[0], "limits") != 0) return -1;

    if (!argv[1]) {
        printf("mpcsh limits:\n"
        "  history  %d  (hard max %d)\n"
        "  line     %zu (hard max %d)\n",
               mpcsh_history_max, MPCSH_HISTORY_MAX_HARD,
               mpcsh_max_line, MPCSH_MAX_LINE_HARD);
        return 0;
    }

    if (!argv[2]) {
        fprintf(stderr, "Usage: limits [history|line] N\n");
        return 2;
    }

    char *end = NULL;
    long n = strtol(argv[2], &end, 10);
    if (!end || *end || n < 0) {
        fprintf(stderr, "mpcsh: limits: invalid number '%s'\n", argv[2]);
        return 2;
    }

    if (!strcmp(argv[1], "history")) {
        if (n < 1 || n > MPCSH_HISTORY_MAX_HARD) {
            fprintf(stderr, "mpcsh: limits: history must be 1…%d\n",
                    MPCSH_HISTORY_MAX_HARD);
            return 2;
        }
        if (n > MPCSH_HISTORY_MAX_DEFAULT) {
            fprintf(stderr,
                    "\033[1;33m⚠  Warning: history > %d can turn your RAM "
                    "into green candy.\033[0m\n", MPCSH_HISTORY_MAX_DEFAULT);
        }
        mpcsh_history_max = (int)n;
        printf("history limit set to %d\n", mpcsh_history_max);
        return 0;
    }

    if (!strcmp(argv[1], "line")) {
        if (n < 256 || n > MPCSH_MAX_LINE_HARD) {
            fprintf(stderr, "mpcsh: limits: line must be 256…%d\n",
                    MPCSH_MAX_LINE_HARD);
            return 2;
        }
        if (n > MPCSH_MAX_LINE_DEFAULT) {
            fprintf(stderr,
                    "\033[1;33m⚠  Warning: line length > %d can turn your RAM "
                    "into green candy.\033[0m\n", MPCSH_MAX_LINE_DEFAULT);
        }
        mpcsh_max_line = (size_t)n;
        printf("line length limit set to %zu\n", mpcsh_max_line);
        return 0;
    }

    fprintf(stderr, "mpcsh: limits: unknown setting '%s'\n", argv[1]);
    return 2;
}

static int
mpcsh_cd_builtin(char **argv)
{
    if (!argv || !argv[0] || strcmp(argv[0], "cd") != 0) return -1;

    const char *path = argv[1];
    if (!path || !*path)
        path = mpcsh_var_get("HOME");

    if (!path || chdir(path) < 0) {
        fprintf(stderr, "mpcsh: cd: %s: %s\n",
                path ? path : "", strerror(errno));
        return 1;
    }
    return 0;
}

static int
mpcsh_pwd_builtin(char **argv)
{
    (void)argv;
    char cwd[MPCSH_MAX_PATH];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "mpcsh: pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(cwd);
    return 0;
}

static int
mpcsh_status_builtin(char **argv)
{
    if (argv[1]) {
        fprintf(stderr, "mpcsh: status: no arguments expected\n");
        return 2;
    }
    printf("%d\n", mpcsh_last_status);
    return 0;
}

static int
mpcsh_reject_bash_constructs(const char *cmdline)
{
    gboolean bourne = mpcsh_is_bourne_syntax(cmdline);
    gboolean escape = mpcsh_looks_like_bash_escape(cmdline);

    if (!bourne && !escape)
        return 0;

    if (escape || mpcsh_ai_agent_detected())
        mpcsh_print_ai_notice();

    fprintf(stderr,
            "mpcsh: Bourne/bash syntax or escape attempt rejected.\n"
            "      Use Fish/mpcsh syntax: 'set NAME value', 'and', 'or', 'not'.\n"
            "      No &&, ||, export NAME=value, NAME=value, bash -c, or /bin/sh.\n");
    return 2;
}


static int
mpcsh_ls_builtin(char **argv)
{
    if (!argv || !argv[0] || strcmp(argv[0], "ls") != 0) return -1;
    const char *dir = ".";
    for (int i = 1; argv[i]; ++i) if (argv[i][0] != '-') { dir = argv[i]; break; }
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "mpcsh: ls: %s: %s\n", dir, strerror(errno)); return 1; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[MPCSH_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        const char *icon = "📄", *color = "\033[0m";
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) { icon="📁"; color="\033[1;95m"; }
        else {
            const char *dot = strrchr(e->d_name, '.');
            if (dot) {
                const char *x = dot + 1;
                if (!strcasecmp(x,"c")||!strcasecmp(x,"h")) { icon="🇨"; color="\033[1;96m"; }
                else if (!strcasecmp(x,"cpp")||!strcasecmp(x,"cc")||!strcasecmp(x,"hpp")) { icon="🇨"; color="\033[1;94m"; }
                else if (!strcasecmp(x,"py")) { icon="🐍"; color="\033[1;93m"; }
                else if (!strcasecmp(x,"rs")) { icon="🦀"; color="\033[1;91m"; }
                else if (!strcasecmp(x,"go")) { icon="🐹"; color="\033[1;96m"; }
                else if (!strcasecmp(x,"js")||!strcasecmp(x,"mjs")) { icon="🟨"; color="\033[1;93m"; }
                else if (!strcasecmp(x,"ts")) { icon="🟦"; color="\033[1;94m"; }
                else if (!strcasecmp(x,"java")||!strcasecmp(x,"kt")) { icon="☕"; color="\033[1;91m"; }
                else if (!strcasecmp(x,"rb")) { icon="💎"; color="\033[1;91m"; }
                else if (!strcasecmp(x,"php")) { icon="🐘"; color="\033[1;95m"; }
                else if (!strcasecmp(x,"lua")) { icon="🌙"; color="\033[1;94m"; }
                else if (!strcasecmp(x,"zig")) { icon="⚡"; color="\033[1;93m"; }
                else if (!strcasecmp(x,"sh")||!strcasecmp(x,"fish")) { icon="🐚"; color="\033[1;92m"; }
                else if (!strcasecmp(x,"md")) { icon="📝"; color="\033[1;97m"; }
                else if (!strcasecmp(x,"zip")||!strcasecmp(x,"tar")||!strcasecmp(x,"gz")||!strcasecmp(x,"xz")) { icon="🗜️"; color="\033[1;93m"; }
                else if (!strcasecmp(x,"png")||!strcasecmp(x,"jpg")||!strcasecmp(x,"jpeg")||!strcasecmp(x,"gif")||!strcasecmp(x,"webp")) { icon="🖼️"; color="\033[1;95m"; }
                else if (!strcasecmp(x,"mp3")||!strcasecmp(x,"wav")||!strcasecmp(x,"flac")) { icon="🎵"; color="\033[1;95m"; }
                else if (!strcasecmp(x,"mp4")||!strcasecmp(x,"mkv")||!strcasecmp(x,"webm")) { icon="🎬"; color="\033[1;95m"; }
                else if (!strcasecmp(x,"pdf")) { icon="📕"; color="\033[1;91m"; }
            }
        }
        printf("%s %s%s\033[0m\n", icon, color, e->d_name);
    }
    closedir(d);
    return 0;
}


/* ------------------------------------------------------------
 * Advertised built-ins: help and colorit.
 * These are also handled in pipeline children, so `producer | colorit`
 * works without delegating shell syntax to another interpreter.
 * ------------------------------------------------------------ */
static int mpcsh_help_builtin(char **argv)
{
    (void)argv;
    puts("MarekPC Shell (mpcsh) — built-in help\n"
    "  help                         Show this help\n"
    "  ctk <call>                   Call The Kernel\n"
    "  portmap                      Show/manage port mappings\n"
    "  guiinterm                    Open GUI-in-terminal tools\n"
    "  wallpaper [path]             Set/show wallpaper\n"
    "  ls [path]                    List directory entries\n"
    "  mpcimgcat <image>             Display an image in supported terminals\n"
    "  colorit [#RRGGBB]             Color stdin (or following text)\n"
    "  limits [history|line N]       Show/set shell limits\n"
    "  set [-g|-l|-x] NAME VALUE     Set a shell variable\n"
    "  math EXPR                     Calculator, e.g. math 1 + 2 * 3\n"
    "  source FILE [ARGS]            Run a script in the current shell\n"
    "  mpc-chuctrl list|install|remove|new|path   Manage Chihuahua plugins (.chu)\n"
    "  time COMMAND [ARGS...]        Run COMMAND, then report real/user/sys\n"
    "  mpcshman [PAGE|-k WORD]       Multi-page manual with search\n"
    "  cd [path]  pwd  status  exit  Core shell commands\n\n"
    "Pipelines: up to 64 segments. Command substitution: $(...).\n"
    "Example: fastfetch | colorit #5A06EF");
    return 0;
}

static int mpcsh_colorit_builtin(char **argv)
{
    unsigned int r = 90, g = 6, b = 239; /* default MarekPC purple */
    const char *hex = argv[1];
    if (hex && (!strcmp(hex, "--help") || !strcmp(hex, "-h"))) {
        puts("Usage: colorit [#RRGGBB]\nReads text from stdin and prints it in the selected 24-bit RGB color.\nDefault color: #5A06EF (MarekPC purple).\nExample: fastfetch | colorit #5A06EF");
        return 0;
    }
    if (hex) {
        if (*hex == '#') ++hex;
        if (strlen(hex) != 6) {
            fprintf(stderr, "mpcsh: colorit: expected color as #RRGGBB (example: #5A06EF)\n");
            return 2;
        }
        char *end = NULL;
        errno = 0; unsigned long value = strtoul(hex, &end, 16);
        if (errno || !end || *end || end != hex + 6) {
            fprintf(stderr, "mpcsh: colorit: invalid RGB color; use #RRGGBB\n");
            return 2;
        }
        r = (value >> 16) & 255; g = (value >> 8) & 255; b = value & 255;
    }
    /* Additional arguments are rejected to catch accidental misspellings. */
    if (argv[2]) {
        fprintf(stderr, "mpcsh: colorit: usage: colorit [#RRGGBB]\n");
        return 2;
    }
    if (isatty(STDIN_FILENO)) {
        fprintf(stderr, "mpcsh: colorit: waiting for input on stdin (Ctrl+D to finish)\n");
    }
    printf("\033[38;2;%u;%u;%um", r, g, b);
    fflush(stdout); /* ensure color prefix precedes raw write(2) output */
    char buf[4096]; ssize_t n;
    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t written = write(STDOUT_FILENO, buf + off, (size_t)n - off);
            if (written < 0) { if (errno == EINTR) continue; perror("mpcsh: colorit: write"); return 1; }
            off += (size_t)written;
        }
    }
    if (n < 0) { perror("mpcsh: colorit: read"); return 1; }
    fputs("\033[0m", stdout);
    fflush(stdout);
    return 0;
}

/* ------------------------------------------------------------
 *
 * /* ===========================================================================
 * MPCUPDATE — Auto-update checker
 * ============================================================================ */

 #include <curl/curl.h>

 typedef struct {
     char *tag_name;
     char *name;
     char *body;
     char *browser_download_url;
     int is_prerelease;
 } MpcshRelease;

 struct Memory {
     char *response;
     size_t size;
 };

 static size_t write_memory(void *contents, size_t size, size_t nmemb, void *userp) {
     size_t realsize = size * nmemb;
     struct Memory *mem = (struct Memory *)userp;
     char *ptr = realloc(mem->response, mem->size + realsize + 1);
     if(!ptr) return 0;
     mem->response = ptr;
     memcpy(&(mem->response[mem->size]), contents, realsize);
     mem->size += realsize;
     mem->response[mem->size] = 0;
     return realsize;
 }

 static char *extract_json_value(const char *json, const char *key) {
     char search_key[64];
     snprintf(search_key, sizeof(search_key), "\"%s\"", key);
     const char *pos = strstr(json, search_key);
     if(!pos) return NULL;
     pos += strlen(search_key);
     while(*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
     if(*pos != '"') return NULL;
     pos++;
     const char *end = strchr(pos, '"');
     if(!end) return NULL;
     return g_strndup(pos, end - pos);
 }

 static MpcshRelease *mpcupdate_fetch_latest_release(const char *repo_owner, const char *repo_name) {
     CURL *curl;
     CURLcode res;
     struct Memory chunk = {0};
     MpcshRelease *release = NULL;

     chunk.response = g_malloc(1);
     chunk.size = 0;

     curl_global_init(CURL_GLOBAL_DEFAULT);
     curl = curl_easy_init();
     if(curl) {
         char url[512];
         snprintf(url, sizeof(url), "https://api.github.com/repos/%s/%s/releases/latest", repo_owner, repo_name);

         curl_easy_setopt(curl, CURLOPT_URL, url);
         curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory);
         curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
         curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mpcsh-Update-Checker/1.5");
         curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
         curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

         res = curl_easy_perform(curl);
         if(res != CURLE_OK) {
             fprintf(stderr, "mpcupdate: failed to connect (%s)\n", curl_easy_strerror(res));
         } else {
             release = g_malloc0(sizeof(MpcshRelease));
             release->tag_name = extract_json_value(chunk.response, "tag_name");
             release->name = extract_json_value(chunk.response, "name");
             release->body = extract_json_value(chunk.response, "body");

             char *pre = strstr(chunk.response, "\"prerelease\":true");
             release->is_prerelease = (pre != NULL);

             char *browser_pos = strstr(chunk.response, "\"browser_download_url\":");
             if(browser_pos) {
                 browser_pos += strlen("\"browser_download_url\":");
                 while(*browser_pos && (*browser_pos == ' ' || *browser_pos == '"')) browser_pos++;
                 char *url_end = strchr(browser_pos, '"');
                 if(url_end) {
                     release->browser_download_url = g_strndup(browser_pos, url_end - browser_pos);
                 }
             }
         }
         curl_easy_cleanup(curl);
     }
     curl_global_cleanup();
     g_free(chunk.response);
     return release;
 }

 static void mpcupdate_download_and_install(const MpcshRelease *release, const char *target_binary) {
     printf("\n📥 Downloading release: %s\n", release->tag_name);
     char backup_path[MPCSH_MAX_PATH];
     snprintf(backup_path, sizeof(backup_path), "%s.backup", target_binary);
     if(rename(target_binary, backup_path) == 0) {
         printf("✅ Backed up current version\n");
     } else {
         printf("⚠️  Could not create backup (continuing)\n");
     }
     CURL *curl = curl_easy_init();
     if(curl && release->browser_download_url) {
         FILE *file = fopen(target_binary, "wb");
         if(file) {
             char clean_url[2048];
             strncpy(clean_url, release->browser_download_url, sizeof(clean_url)-1);
             clean_url[sizeof(clean_url)-1] = '\0';

             curl_easy_setopt(curl, CURLOPT_URL, clean_url);
             curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
             curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
             curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
             curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

             printf("⬇️  Starting download...\n");
             CURLcode res = curl_easy_perform(curl);
             fclose(file);

             if(res != CURLE_OK) {
                 fprintf(stderr, "❌ Download failed: %s\n", curl_easy_strerror(res));
                 if(rename(backup_path, target_binary) == 0) {
                     printf("↩️  Restored previous version\n");
                 }
             } else {
                 chmod(target_binary, 0755);
                 printf("✅ Successfully updated to %s!\n", release->tag_name);
                 printf("💡 Restart your shell to use the new version.\n");
                 printf("💾 Old version saved as: %s\n", backup_path);
             }
         } else {
             fprintf(stderr, "❌ Cannot write to target file\n");
         }
         curl_easy_cleanup(curl);
     } else {
         fprintf(stderr, "❌ No download URL provided\n");
     }
 }

 __attribute__((used)) static int mpcsh_mpcupdate_builtin(char **argv) {
     if(!argv || !argv[0] || strcmp(argv[0], "mpcupdate") != 0) return -1;

     if(argv[1] && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
         printf("MarekPC Shell Auto-Updater\n");
         printf("Usage: mpcupdate [--check-only]\n\n");
         printf("Options:\n");
         printf("  --check-only  Only check for updates, don't download\n\n");
         printf("Examples:\n");
         printf("  mpcupdate              Check and offer to update if available\n");
         printf("  mpcupdate --check-only Show update info without downloading\n");
         return 0;
     }

     gboolean check_only = FALSE;
     if(argv[1] && (!strcmp(argv[1], "--check-only") || !strcmp(argv[1], "-n"))) {
         check_only = TRUE;
     }

     printf("🔍 Checking for updates...\n");

     MpcshRelease *release = mpcupdate_fetch_latest_release("eggyr25-lgtm", "MPCSH");

     if(!release || !release->tag_name) {
         fprintf(stderr, "❌ Could not fetch release information.\n");
         fprintf(stderr, "💡 Check your internet connection and try again.\n");
         if(release) {
             g_free(release->tag_name);
             g_free(release->name);
             g_free(release->body);
             g_free(release->browser_download_url);
             g_free(release);
         }
         return 1;
     }

     int latest_major, latest_minor, latest_patch;
     int parsed = sscanf(release->tag_name, "v%d.%d.%d", &latest_major, &latest_minor, &latest_patch);
     if(parsed != 3) {
         parsed = sscanf(release->tag_name, "%d.%d.%d", &latest_major, &latest_minor, &latest_patch);
     }

     int current_version = MPCSH_VERSION_MAJOR * 10000 + MPCSH_VERSION_MINOR * 100 + MPCSH_VERSION_PATCH;
     int latest_version = latest_major * 10000 + latest_minor * 100 + latest_patch;

     printf("Current version: v%s\n", MPCSH_VERSION_STRING);
     printf("Latest version:  %s\n", release->tag_name);

     if(latest_version <= current_version) {
         printf("✅ You're running the latest version!\n");
         g_free(release->tag_name);
         g_free(release->name);
         g_free(release->body);
         g_free(release->browser_download_url);
         g_free(release);
         return 0;
     }

     printf("\n🎉 NEW VERSION AVAILABLE!\n");
     if(release->body) {
         printf("\n=== CHANGELOG ===\n%s\n=== END ===\n", release->body);
     }
     if(release->is_prerelease) {
         printf("\n⚠️  This is a prerelease version\n");
     }

     if(check_only) {
         g_free(release->tag_name);
         g_free(release->name);
         g_free(release->body);
         g_free(release->browser_download_url);
         g_free(release);
         return 0;
     }

     printf("\n💿 Update to %s now? [Y/n]: ", release->tag_name);
     char response[16];
     if(fgets(response, sizeof(response), stdin)) {
         if(tolower(response[0]) != 'n') {
             char exe_path[MPCSH_MAX_PATH];
             ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
             if(len > 0) {
                 exe_path[len] = '\0';
                 mpcupdate_download_and_install(release, exe_path);
             } else {
                 fprintf(stderr, "❌ Cannot determine executable path\n");
             }
         } else {
             printf("Update skipped.\n");
         }
     }

     g_free(release->tag_name);
     g_free(release->name);
     g_free(release->body);
     g_free(release->browser_download_url);
     g_free(release);
     return 0;
 }

 /* ===========================================================================
  * MPCBITS — Back-In-Time snapshots
  * ============================================================================ */

 /* Snapshot helpers use GIO/POSIX operations only: never interpolate paths
  * into a shell command.  Symlinks are copied as links only when explicitly
  * encountered as entries; recursive traversal never follows them. */
 static gboolean mpcbits_copy_tree(GFile *src, GFile *dst, GError **error)
 {
     GFileInfo *info = g_file_query_info(src,
                                         G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_NAME,
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
     if (!info) return FALSE;
     GFileType type = g_file_info_get_file_type(info);
     g_object_unref(info);

     if (type == G_FILE_TYPE_DIRECTORY) {
         if (!g_file_make_directory(dst, NULL, error) &&
             !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_EXISTS))
             return FALSE;
         g_clear_error(error);
         GFileEnumerator *en = g_file_enumerate_children(src,
                                                         G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, error);
         if (!en) return FALSE;
         gboolean ok = TRUE;
         GFileInfo *child_info;
         while (ok && (child_info = g_file_enumerator_next_file(en, NULL, error))) {
             const char *name = g_file_info_get_name(child_info);
             GFile *a = g_file_get_child(src, name);
             GFile *b = g_file_get_child(dst, name);
             ok = mpcbits_copy_tree(a, b, error);
             g_object_unref(a); g_object_unref(b); g_object_unref(child_info);
         }
         if (error && *error) ok = FALSE;
         g_object_unref(en);
         return ok;
     }
     if (type == G_FILE_TYPE_REGULAR)
         return g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, error);
     /* Do not copy sockets, devices, FIFOs, or symlinks into a backup. */
     return TRUE;
 }

 static gboolean mpcbits_remove_tree(GFile *file, GError **error)
 {
     GFileInfo *info = g_file_query_info(file,
                                         G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                         NULL, NULL);
     if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
         GFileEnumerator *en = g_file_enumerate_children(file,
                                                         G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                         NULL, error);
         g_object_unref(info);
         if (!en) return FALSE;
         gboolean ok = TRUE;
         GFileInfo *child;
         while (ok && (child = g_file_enumerator_next_file(en, NULL, error))) {
             GFile *sub = g_file_get_child(file, g_file_info_get_name(child));
             ok = mpcbits_remove_tree(sub, error);
             g_object_unref(sub); g_object_unref(child);
         }
         if (error && *error) ok = FALSE;
         g_object_unref(en);
         if (!ok) return FALSE;
     } else if (info) {
         g_object_unref(info);
     }
     return g_file_delete(file, NULL, error);
 }

 static guint64 mpcbits_tree_size(GFile *file)
 {
     GFileInfo *info = g_file_query_info(file,
                                         G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                         G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
     if (!info) return 0;
     GFileType type = g_file_info_get_file_type(info);
     guint64 size = type == G_FILE_TYPE_REGULAR ? g_file_info_get_size(info) : 0;
     g_object_unref(info);
     if (type != G_FILE_TYPE_DIRECTORY) return size;
     GFileEnumerator *en = g_file_enumerate_children(file,
                                                     G_FILE_ATTRIBUTE_STANDARD_NAME, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                     NULL, NULL);
     if (!en) return 0;
     GFileInfo *child;
     while ((child = g_file_enumerator_next_file(en, NULL, NULL))) {
         GFile *sub = g_file_get_child(file, g_file_info_get_name(child));
         size += mpcbits_tree_size(sub);
         g_object_unref(sub); g_object_unref(child);
     }
     g_object_unref(en);
     return size;
 }

 static gboolean mpcbits_valid_timestamp(const char *s)
 {
     if (!s || strlen(s) != 15 || s[8] != '_') return FALSE;
     for (int i = 0; i < 15; ++i)
         if (i != 8 && !g_ascii_isdigit(s[i])) return FALSE;
         return TRUE;
 }

 static char *mpcbits_snapshot_base(void)
 {
     return g_build_filename(g_get_home_dir(), ".local", "share", "mpcsh",
                             "snapshots", NULL);
 }

 static void mpcbits_create_snapshot(const char *description) {
     time_t now = time(NULL);
     struct tm tm_info;
     localtime_r(&now, &tm_info);
     char timestamp[64];
     strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &tm_info);

     char *base = mpcbits_snapshot_base();
     char *snapshot_dir = g_build_filename(base, timestamp, NULL);
     if (!g_mkdir_with_parents(base, 0755) ||
         g_mkdir(snapshot_dir, 0755) != 0) {
         fprintf(stderr, "Cannot create snapshot directory: %s\\n", strerror(errno));
     g_free(base); g_free(snapshot_dir); return;
         }

         GError *error = NULL;
         char exe_path[MPCSH_MAX_PATH];
         ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
         if (len > 0) {
             exe_path[len] = '\0';
             GFile *src = g_file_new_for_path(exe_path);
             char *dest_path = g_build_filename(snapshot_dir, "mpcsh", NULL);
             GFile *dst = g_file_new_for_path(dest_path);
             if (!g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &error)) {
                 fprintf(stderr, "Snapshot binary copy failed: %s\\n", error->message);
                 g_clear_error(&error);
             }
             g_object_unref(src); g_object_unref(dst); g_free(dest_path);
         }

         char *config_src_path = g_build_filename(g_get_home_dir(), ".config", "mpcsh", NULL);
         char *config_dst_path = g_build_filename(snapshot_dir, "config", NULL);
         GFile *config_src = g_file_new_for_path(config_src_path);
         GFile *config_dst = g_file_new_for_path(config_dst_path);
         if (g_file_query_exists(config_src, NULL) &&
             !mpcbits_copy_tree(config_src, config_dst, &error)) {
             fprintf(stderr, "Snapshot config copy failed: %s\\n",
                     error ? error->message : "unknown error");
             g_clear_error(&error);
             }
             g_object_unref(config_src); g_object_unref(config_dst);
         g_free(config_src_path); g_free(config_dst_path);

         char *meta_path = g_build_filename(snapshot_dir, "metadata.json", NULL);
         char *escaped_desc = g_strescape(description ? description : "Manual backup", NULL);
         char *created = g_strdup(asctime(&tm_info));
         if (created) g_strchomp(created);
         char *escaped_created = g_strescape(created ? created : "", NULL);
     char *meta = g_strdup_printf(
         "{\\n  \"timestamp\": \"%s\",\\n  \"description\": \"%s\",\\n"
         "  \"version\": \"%s\",\\n  \"created\": \"%s\"\\n}\\n",
         timestamp, escaped_desc, MPCSH_VERSION_STRING, escaped_created);
     if (!g_file_set_contents(meta_path, meta, -1, &error)) {
         fprintf(stderr, "Snapshot metadata write failed: %s\\n", error->message);
         g_clear_error(&error);
     }
     g_free(meta); g_free(escaped_desc); g_free(created); g_free(escaped_created);
     g_free(meta_path);
     printf("Snapshot created: %s (%s)\\n", timestamp,
            description ? description : "Manual backup");
     g_free(base); g_free(snapshot_dir);
 }

 static void mpcbits_list_snapshots(void) {
     char *base = mpcbits_snapshot_base();
     GDir *d = g_dir_open(base, 0, NULL);
     if (!d) {
         printf("No snapshots found.\\n");
         g_free(base);
         return;
     }
     printf("=== MPCSH BACKUP SNAPSHOTS ===\\n");
     printf("Timestamp          Size     Description\\n");
     printf("----------------------------------------\\n");
     const char *name;
     int count = 0;
     while ((name = g_dir_read_name(d))) {
         if (!mpcbits_valid_timestamp(name)) continue;
         char *dir_path = g_build_filename(base, name, NULL);
         GFile *dir = g_file_new_for_path(dir_path);
         GFileInfo *info = g_file_query_info(dir, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
         if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
             if (info) g_object_unref(info);
             g_object_unref(dir); g_free(dir_path); continue;
         }
         g_object_unref(info);
         char *meta_path = g_build_filename(dir_path, "metadata.json", NULL);
         char *contents = NULL, *desc = NULL;
         if (g_file_get_contents(meta_path, &contents, NULL, NULL)) {
             GRegex *re = g_regex_new("\"description\"\\\\s*:\\\\s*\"([^\"]*)\"",
                                      G_REGEX_OPTIMIZE, 0, NULL);
             GMatchInfo *match = NULL;
             if (g_regex_match(re, contents, 0, &match))
                 desc = g_match_info_fetch(match, 1);
             if (match) g_match_info_free(match);
             g_regex_unref(re);
         }
         char size_str[32];
         gchar *formatted_size = g_format_size(mpcbits_tree_size(dir));
         g_snprintf(size_str, sizeof(size_str), "%s", formatted_size);
         g_free(formatted_size);
         printf("%-18s %-8s %s\\n", name, size_str, desc ? desc : "Manual backup");
         g_free(desc); g_free(contents); g_free(meta_path);
         g_object_unref(dir); g_free(dir_path); ++count;
     }
     g_dir_close(d);
     printf("----------------------------------------\\nTotal snapshots: %d\\n", count);
     g_free(base);
 }

 __attribute__((used)) static int mpcsh_mpcbits_builtin(char **argv) {
     if(!argv || !argv[0] || strcmp(argv[0], "mpcbits") != 0) return -1;

     if(!argv[1]) {
         printf("MarekPC Shell - Back In Time\n");
         printf("Usage: mpcbits <command> [args]\n\n");
         printf("Commands:\n");
         printf("  save [description]   Create a backup snapshot\n");
         printf("  list                 List available snapshots\n");
         printf("  restore <timestamp>  Restore from a snapshot\n");
         printf("  delete <timestamp>   Delete a snapshot\n");
         return 0;
     }

     if(!strcmp(argv[1], "save")) {
         const char *desc = argv[2] ? argv[2] : "Manual snapshot";
         mpcbits_create_snapshot(desc);
         return 0;
     }

     if(!strcmp(argv[1], "list")) {
         mpcbits_list_snapshots();
         return 0;
     }

     if(!strcmp(argv[1], "restore")) {
         if(!argv[2]) {
             fprintf(stderr, "❌ Usage: mpcbits restore <timestamp>\n");
             fprintf(stderr, "💡 Available snapshots:\n");
             mpcbits_list_snapshots();
             return 2;
         }

         if (!mpcbits_valid_timestamp(argv[2])) {
             fprintf(stderr, "Invalid snapshot timestamp. Expected YYYYMMDD_HHMMSS.\\n");
             return 2;
         }
         printf("↩️  Restoring from snapshot: %s\n", argv[2]);
         printf("⚠️  This will overwrite your current MPCSH binary.\n");
         printf("💡 Continue? [y/N]: ");

         char response[16];
         if(fgets(response, sizeof(response), stdin) && tolower(response[0]) == 'y') {
             char snapshot_bin[MPCSH_MAX_PATH];
             const char *home = g_get_home_dir();
             snprintf(snapshot_bin, sizeof(snapshot_bin),
                      "%s/.local/share/mpcsh/snapshots/%s/mpcsh",
                      home ? home : "/tmp", argv[2]);

             char exe_path[MPCSH_MAX_PATH];
             ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
             if(len > 0) {
                 exe_path[len] = '\0';
                 if(rename(snapshot_bin, exe_path) == 0) {
                     printf("✅ Restored successfully!\n");
                     printf("💡 Restart your shell to use the restored version.\n");
                     return 0;
                 } else {
                     fprintf(stderr, "❌ Restore failed: %s\n", strerror(errno));
                     return 1;
                 }
             }
         } else {
             printf("Restore cancelled.\n");
         }
         return 0;
     }

     if(!strcmp(argv[1], "delete")) {
         if(!argv[2]) {
             fprintf(stderr, "❌ Usage: mpcbits delete <timestamp>\n");
             return 2;
         }

         if (!mpcbits_valid_timestamp(argv[2])) {
             fprintf(stderr, "Invalid snapshot timestamp. Expected YYYYMMDD_HHMMSS.\n");
             return 2;
         }
         char *base = mpcbits_snapshot_base();
         char *path = g_build_filename(base, argv[2], NULL);
         GFile *target = g_file_new_for_path(path);
         GFileInfo *info = g_file_query_info(target, G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                             G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
         if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_DIRECTORY) {
             fprintf(stderr, "No such snapshot: %s\n", argv[2]);
             if (info) g_object_unref(info);
             g_object_unref(target); g_free(path); g_free(base);
             return 1;
         }
         g_object_unref(info);
         GError *error = NULL;
         gboolean ok = mpcbits_remove_tree(target, &error);
         if (ok) printf("Deleted snapshot: %s\n", argv[2]);
         else fprintf(stderr, "Cannot delete snapshot: %s\n",
             error ? error->message : "unknown error");
         g_clear_error(&error);
         g_object_unref(target); g_free(path); g_free(base);
         return ok ? 0 : 1;
     }

     fprintf(stderr, "Unknown subcommand: %s\n", argv[1]);
     return 2;
 }

 /* ===========================================================================
  * MPCVERSION — Version info
  * ============================================================================ */

 __attribute__((used)) static int mpcsh_mpcversion_builtin(char **argv) {
     if(!argv || !argv[0] || strcmp(argv[0], "mpcversion") != 0) return -1;

     printf("MarekPC Shell %s (%d.%d.%d)\n",
            MPCSH_VERSION_STRING,
            MPCSH_VERSION_MAJOR,
            MPCSH_VERSION_MINOR,
            MPCSH_VERSION_PATCH);

     printf("Built: %s", __DATE__ " " __TIME__);
     printf("\nFeatures: auto-update, back-in-time, pipelines, gui, scripts\n");

     return 0;
 }

 /* ------------------------------------------------------------
  * Pipeline engine
  * ------------------------------------------------------------ */

 static int
 mpcsh_spawn_segment(char **argv, int in_fd, int out_fd)
 {
     pid_t pid = fork();
     if (pid < 0) {
         fprintf(stderr, "mpcsh: fork: %s\n", strerror(errno));
         return -1;
     }
     if (pid == 0) {
         if (in_fd  != STDIN_FILENO)  { dup2(in_fd, STDIN_FILENO);  close(in_fd); }
         if (out_fd != STDOUT_FILENO) { dup2(out_fd, STDOUT_FILENO); close(out_fd); }

         /* Native redirections: open(2) + dup2(2), never a shell parser. */
         char *exec_argv[256];
         int ec = 0;
         for (int i = 0; argv[i] && ec < 255; ++i) {
             const char *op = argv[i];
             if (!strcmp(op, ">") || !strcmp(op, ">>") ||
                 !strcmp(op, "<") || !strcmp(op, "2>") ||
                 !strcmp(op, "2>>") || !strcmp(op, "&>") ||
                 !strcmp(op, "&>>")) {

                 const char *path = argv[++i];
             if (!path) {
                 fprintf(stderr, "mpcsh: redirection '%s' needs a file\n", op);
                 _exit(2);
             }

             /* Block redirects into sensitive kernel/virtual filesystems. */
             if (path[0] == '/' &&
                 (g_str_has_prefix(path, "/dev/") ||
                 g_str_has_prefix(path, "/proc/") ||
                 g_str_has_prefix(path, "/sys/") ||
                 g_str_has_prefix(path, "/run/") ||
                 strcmp(path, "/dev") == 0 ||
                 strcmp(path, "/proc") == 0 ||
                 strcmp(path, "/sys") == 0)) {
                 fprintf(stderr,
                         "mpcsh: redirection to %s refused "
                         "(sensitive path)\n", path);
                 _exit(1);
                 }

                 int flags = 0;
                 int target = STDOUT_FILENO;
                 if (!strcmp(op, "<")) {
                     flags = O_RDONLY;
                     target = STDIN_FILENO;
                 } else {
                     flags = O_WRONLY | O_CREAT |
                     ((!strcmp(op, ">>") || !strcmp(op, "2>>") ||
                     !strcmp(op, "&>>")) ? O_APPEND : O_TRUNC);
                     if (!strcmp(op, "2>") || !strcmp(op, "2>>"))
                         target = STDERR_FILENO;
                 }

                 int fd = open(path, flags, 0666);
                 if (fd < 0) {
                     fprintf(stderr, "mpcsh: redirection %s %s: %s\n",
                             op, path, strerror(errno));
                     _exit(1);
                 }

                 if (!strcmp(op, "&>") || !strcmp(op, "&>>")) {
                     dup2(fd, STDOUT_FILENO);
                     dup2(fd, STDERR_FILENO);
                 } else {
                     dup2(fd, target);
                 }
                 close(fd);
                 continue;
                 }

                 exec_argv[ec++] = argv[i];
         }
         exec_argv[ec] = NULL;

         if (!exec_argv[0]) {
             fprintf(stderr, "mpcsh: redirection-only command\n");
             _exit(2);
         }

         /* Final sanitization before execvp.
          * Only block empty names and path traversal.  Metacharacters
          * were already rejected by the lexer; being too aggressive
          * here silently kills normal commands. */
         {
             const char *prog = exec_argv[0];
             if (!prog || !*prog) {
                 fprintf(stderr, "mpcsh: empty command\n");
                 _exit(126);
             }
             if (strstr(prog, "..")) {
                 fprintf(stderr, "mpcsh: refusing path traversal: %s\n", prog);
                 _exit(126);
             }
         }

         if (!strcmp(exec_argv[0], "colorit")) {
             int rc = mpcsh_colorit_builtin(exec_argv);
             fflush(NULL);
             _exit(rc);
         }
         if (!strcasecmp(exec_argv[0], "mpcshman")) {
             int rc = mpcsh_mpcshman_builtin(exec_argv);
             fflush(NULL);
             _exit(rc);
         }
         if (!strcmp(exec_argv[0], "math")) {
             int rc = mpcsh_math_builtin(exec_argv);
             fflush(NULL);
             _exit(rc);
         }
         if (!strcmp(exec_argv[0], "mpc-chuctrl") || !strcmp(exec_argv[0], "chuctrl")) {
             int rc = mpcsh_chuctrl_builtin(exec_argv);
             fflush(NULL);
             _exit(rc);
         }
         if (!strcmp(exec_argv[0], "help")) {
             int rc = mpcsh_help_builtin(exec_argv);
             fflush(NULL);
             _exit(rc);
         }

         int frc = mpc_function_call(exec_argv);
         if (frc >= 0) { fflush(NULL); _exit(frc); }
         mpcsh_chu_exec(exec_argv);   /* 1.8: Chihuahua programs (returns if not one) */
         execvp(exec_argv[0], exec_argv);

         /* execvp() collapsed every PATH-search failure into ENOENT,
          * so a missing command and a non-executable file looked
          * identical. Diagnose the real cause instead. */
         {
             const char *prog = exec_argv[0];
             int err = errno;

             if (err == ENOEXEC) {
                 fprintf(stderr, "mpcsh: %s: cannot execute binary file\n", prog);
                 _exit(126);
             }
             if (err == EACCES) {
                 fprintf(stderr, "mpcsh: %s: permission denied\n", prog);
                 _exit(126);
             }
             if (err == ENOENT) {
                 struct stat st;

                 if (strchr(prog, '/')) {
                     /* Explicit path: file present but not runnable,
                      * or truly missing? */
                     if (stat(prog, &st) == 0)
                         fprintf(stderr,
                                 "mpcsh: %s: exists but is not an executable file\n",
                                 prog);
                         else
                             fprintf(stderr,
                                     "mpcsh: %s: no such file or directory\n", prog);
                             _exit(127);
                 }

                 /* Bare name: nothing executable in PATH. */
                 fprintf(stderr, "mpcsh: %s: Command invalid\n", prog);
                 _exit(127);
             }
             if (err == ENOTDIR && !strchr(prog, '/')) {
                 /* A PATH entry that is a file makes execvp report ENOTDIR. */
                 fprintf(stderr, "mpcsh: %s: Command invalid\n", prog);
                 _exit(127);
             }

             fprintf(stderr, "mpcsh: %s: %s\n", prog, strerror(err));
             _exit(126);
         }
         _exit(127);
     }
     return (int)pid;
 }


 /* ====================================================================
  * time  and  mpcshman   (added in 1.8)
  * ==================================================================== */
 static int mpcsh_run_pipeline(char *cmdline);

 static double mpcsh_tv_secs(struct timeval tv) { return (double)tv.tv_sec + tv.tv_usec / 1e6; }

 static int mpcsh_time_run(const char *rest)
 {
     while (*rest == ' ' || *rest == '\t') ++rest;
     if (!*rest) {
         fprintf(stderr, "Usage: time COMMAND [ARGS...]\n");
         return 2;
     }
     struct timespec t0, t1;
     struct rusage s0, s1, c0, c1;
     getrusage(RUSAGE_SELF, &s0);
     getrusage(RUSAGE_CHILDREN, &c0);
     clock_gettime(CLOCK_MONOTONIC, &t0);

     char *copy = g_strdup(rest);
     int rc = mpcsh_run_pipeline(copy);
     g_free(copy);

     clock_gettime(CLOCK_MONOTONIC, &t1);
     getrusage(RUSAGE_SELF, &s1);
     getrusage(RUSAGE_CHILDREN, &c1);

     double real = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
     double user = mpcsh_tv_secs(s1.ru_utime) - mpcsh_tv_secs(s0.ru_utime)
     + mpcsh_tv_secs(c1.ru_utime) - mpcsh_tv_secs(c0.ru_utime);
     double sys  = mpcsh_tv_secs(s1.ru_stime) - mpcsh_tv_secs(s0.ru_stime)
     + mpcsh_tv_secs(c1.ru_stime) - mpcsh_tv_secs(c0.ru_stime);
     if (real < 0) real = 0;
     if (user < 0) user = 0;
     if (sys  < 0) sys  = 0;

     fflush(stdout);
     fprintf(stderr, "\nreal\t%dm%.3fs\nuser\t%dm%.3fs\nsys\t%dm%.3fs\n",
             (int)(real / 60), fmod(real, 60.0),
             (int)(user / 60), fmod(user, 60.0),
             (int)(sys  / 60), fmod(sys,  60.0));
     mpcsh_last_status = rc;
     return rc;
 }

 /* ---------------------------------------------------------------- mpcshman */

 typedef struct { const char *name; const char *title; const char *body; } MpcshManPage;

 static const MpcshManPage mpcsh_man_pages[] = {
     { "contents", "Contents",
         "MPCSHMAN - contents\n"
         "\n"
         "Open a page with:  mpcshman NUMBER   or   mpcshman NAME\n"
         "\n"
         "   2  intro      Introduction\n"
         "   3  syntax     Syntax, quoting, variables, operators\n"
         "   4  builtins   Built-in commands\n"
         "   5  scripting  Scripting: blocks, loops, functions, math\n"
         "   6  chihuahuas Chihuahuas: plugins in .chu files\n"
         "   7  time       The time command\n"
         "   8  config     mpcsh.config reference\n"
         "   9  prompt     Prompt layout\n"
         "  10  windows    PixelWindows: windowlaunch and friends\n"
         "  11  imgcat     mpcimgcat and the MPSP protocol\n"
         "  12  mpcecc     MPCECC terminal overview\n"
         "  13  keys       Keyboard shortcuts\n"
         "  14  protocol   MPCECC control socket\n"
         "  15  env        Environment variables and exit codes\n"
         "  16  mpcshman   Using mpcshman\n"
         "  17  changes    Changes in 1.8\n"
         "\n"
         "Search everything:  mpcshman -k WORD\n"
         "In the pager:  / search,  ] next page,  [ previous page,  ? help,  q quit\n" },
         { "intro", "Introduction",
             "MPCSH - the MarekPC Shell\n"
             "\n"
             "mpcsh is a shell written from scratch in C on glib/GTK4. It ships as one\n"
             "binary that contains both the shell (mpcsh) and mpcap (\"one binary, one\n"
             "soul\"). It is NOT Bash: it has its own parser, fish-style command lists\n"
             "(and / or / not) and it rejects some Bash-only constructs with a message\n"
             "instead of half-running them.\n"
             "\n"
             "Things you can rely on\n"
             "  - It is a real scripting language: if / else if / else, while, for,\n"
             "    functions, break, continue, return, math, source (see \"scripting\").\n"
             "  - It has plugins called Chihuahuas (.chu files) managed with\n"
             "    mpc-chuctrl (see \"chihuahuas\").\n"
             "  - Ctrl+C never kills the shell. Reads and waitpid() are EINTR-safe.\n"
             "  - A foreground child killed by a signal reports status 128+signal.\n"
             "  - Pipelines may have up to 64 segments.\n"
             "  - Command substitution with $(...) works.\n"
             "  - Built-ins are listed in the \"builtins\" page, the prompt and colours\n"
             "    in \"config\" and \"prompt\".\n"
             "\n"
             "Using this manual\n"
             "  mpcsh SCRIPT [ARGS]    run a script (see \"scripting\")\n"
             "  mpcshman               open the manual (this pager)\n"
             "  mpcshman TOPIC         open one page by number or name\n"
             "  mpcshman -k WORD       search every page and list the hits\n"
             "  mpcshman -s WORD       open the pager at the first hit for WORD\n"
             "  mpcshman -l            list pages\n"
             "  Inside the pager press ? for the key list, / to search, q to quit.\n"
             "\n"
             "Start with the page named \"builtins\", then \"config\" and \"windows\".\n" },
             { "syntax", "Syntax, quoting, variables, operators",
                 "SYNTAX\n"
                 "\n"
                 "Words are split on spaces and tabs. Quotes group words:\n"
                 "  'single'   everything inside is literal\n"
                 "  \"double\"   groups words (substitutions still apply)\n"
                 "  backslash  protects the next character\n"
                 "\n"
                 "Command substitution\n"
                 "  echo today is $(date)\n"
                 "The text between $( and the matching ) is run and replaced by its output.\n"
                 "\n"
                 "Pipelines\n"
                 "  fastfetch | colorit #5A06EF\n"
                 "Up to 64 segments (MPCSH_MAX_PIPELINE). A pipe inside parentheses or inside\n"
                 "quotes is not a segment separator. An empty segment is an error (status\n"
                 "126).\n"
                 "\n"
                 "Command lists (fish style)\n"
                 "  A and B       run B only if A succeeded\n"
                 "  A or B        run B only if A failed\n"
                 "  not A         invert the status of A\n"
                 "The words are only recognised between two complete commands, so\n"
                 "\"echo and\" still prints \"and\".\n"
                 "\n"
                 "Variables\n"
                 "  set NAME VALUE         set a variable (default scope)\n"
                 "  set -l NAME VALUE      local\n"
                 "  set -g NAME VALUE      global\n"
                 "  set -x NAME VALUE      exported to child processes\n"
                 "Read a variable with $NAME.\n"
                 "\n"
                 "Limits\n"
                 "  limits                 show the current limits\n"
                 "  limits history N       history length (default 128, hard maximum 512)\n"
                 "  limits line N          maximum line length (default 8192, hard 16384)\n"
                 "\n"
                 "Blocks and loops are described in the \"scripting\" page.\n"
                 "\n"
                 "Exit status\n"
                 "  status                 print the status of the last command\n" },
                 { "builtins", "Built-in commands",
                     "BUILT-IN COMMANDS\n"
                     "\n"
                     "Core\n"
                     "  cd [path]              change directory (no argument: $HOME)\n"
                     "  pwd                    print the working directory\n"
                     "  status                 print the status of the last command\n"
                     "  set [-g|-l|-x] N V     set a shell variable\n"
                     "  limits [history|line]  show or set shell limits\n"
                     "  exit [code] / quit     leave the shell (code 0-255)\n"
                     "  help                   short built-in help\n"
                     "  time COMMAND ...       run COMMAND and report real/user/sys (see \"time\")\n"
                     "  math EXPR              calculator (see \"scripting\")\n"
                     "  source FILE [ARGS]     run a script file in the current shell\n"
                     "  mpc-chuctrl ...        install and remove Chihuahuas (see \"chihuahuas\")\n"
                     "  mpcshman [...]         this manual (see \"mpcshman\")\n"
                     "\n"
                     "Files and display\n"
                     "  ls [path]              list directory entries\n"
                     "  wallpaper [path]       show or set the terminal wallpaper\n"
                     "  mpcimgcat IMG ...      show images (see \"imgcat\")\n"
                     "  colorit [#RRGGBB]      colour stdin, default #5A06EF (MarekPC purple)\n"
                     "\n"
                     "PixelWindows (need MPCECC, see \"windows\")\n"
                     "  windowlaunch PROGRAM   embed a GUI program in the current tab\n"
                     "  windowterminate WLID   close an embedded program\n"
                     "  wlidlist / windowlist  list embedded programs\n"
                     "  windowpicker           window picker (talks to MPCECC like wlidlist)\n"
                     "\n"
                     "Tools\n"
                     "  ctk CALL               Call The Kernel. Calls: fork() getpid() getppid()\n"
                     "                         wait(pid) sleep(seconds) chdir(path) exit(code)\n"
                     "                         kill(pid,signal)\n"
                     "  portmap [--raw]        MarekPC PortMap, the UniArt hardware port map\n"
                     "  guiinterm FILE         terminal-native clickable GUI. FILE lines start\n"
                     "                         with title: text: button: command: or checkbox:\n"
                     "  extension SUB          list | load | install <git-url-or-path> | help\n"
                     "                         (user directory only, never root)\n"
                     "  mpcextinstl URL        shortcut for: extension install URL\n"
                     "  mpcupdate [--check-only]  check for updates and offer to install them\n"
                     "  mpcbits COMMAND        snapshots: save [description] | list |\n"
                     "                         restore TIMESTAMP | delete TIMESTAMP\n"
                     "\n"
                     "Anything else is looked up in $PATH like in any other shell. A missing\n"
                     "program, a file that is not executable and a binary that cannot be run are\n"
                     "reported separately (see \"env\" for the status codes).\n" },
                     { "scripting", "Scripting: blocks, loops, functions, math",
                         "SCRIPTING\n"
                         "\n"
                         "Running scripts\n"
                         "  mpcsh script.mpcsh [ARGS...]     run a script file\n"
                         "  mpcsh -c \"echo hi; echo there\"   run one command line\n"
                         "  mpcsh --check script.mpcsh       parse only, do not run (syntax check)\n"
                         "A file with any name is accepted when its first line starts with #! (for\n"
                         "example  #!/usr/bin/env mpcsh ), so scripts can be made executable.\n"
                         "Lines starting with # are comments. Statements end at a newline or a\n"
                         "semicolon. A trailing backslash continues a line.\n"
                         "\n"
                         "Arguments\n"
                         "  The script and every function get:\n"
                         "    $argc      number of arguments\n"
                         "    $argv      all arguments joined with spaces\n"
                         "    $arg1 $arg2 ...   single arguments\n"
                         "  Scripts also get $script (the script path).\n"
                         "\n"
                         "Conditions are commands: status 0 means true.\n"
                         "  if test -f notes.txt\n"
                         "  if grep -q error log.txt\n"
                         "  if not test -d build\n"
                         "  if test -f a and test -f b\n"
                         "There is no [ ] or [[ ]]; use the test program (or any program).\n"
                         "\n"
                         "if / else if / else\n"
                         "  if test $arg1 = yes\n"
                         "    echo agreed\n"
                         "  else if test $arg1 = no\n"
                         "    echo declined\n"
                         "  else\n"
                         "    echo unsure\n"
                         "  end\n"
                         "Every block ends with \"end\". Blocks nest up to 64 deep.\n"
                         "\n"
                         "while\n"
                         "  set n 3\n"
                         "  while test $n -gt 0\n"
                         "    echo $n\n"
                         "    set n (math $n - 1)\n"
                         "  end\n"
                         "Press Ctrl+C to stop a runaway loop.\n"
                         "\n"
                         "for\n"
                         "  for f in a.txt b.txt c.txt\n"
                         "    echo checking $f\n"
                         "  end\n"
                         "\n"
                         "break and continue work in while and for. Outside a loop they are errors.\n"
                         "\n"
                         "Functions\n"
                         "  function greet\n"
                         "    echo hello $arg1, arguments: $argc\n"
                         "    return 3\n"
                         "  end\n"
                         "  greet world\n"
                         "  echo status was $status\n"
                         "return N sets the status (0-255). Without N it returns the last status.\n"
                         "Functions may call each other; recursion is limited to 64 levels. A\n"
                         "function defined by a script stays defined until the shell exits.\n"
                         "\n"
                         "source\n"
                         "  source helpers.mpcsh a b c\n"
                         "Runs the file in the current shell. Functions it defines stay defined and\n"
                         "it gets $arg1... like a function. return works inside it.\n"
                         "\n"
                         "math\n"
                         "  math 1 + 2 * 3          prints 7\n"
                         "  math \"(10 - 4) / 4\"     prints 1.5\n"
                         "  math -s2 10 / 3         prints 3.33  (-sN = N decimals)\n"
                         "  math 2 ^ 10             prints 1024\n"
                         "Operators: + - * / % ^ and parentheses, unary minus. Integer results are\n"
                         "printed without a decimal point. Division by zero is status 1; a bad\n"
                         "expression is status 2. Use it with substitution:\n"
                         "  set total (math $a + $b)\n"
                         "\n"
                         "exit\n"
                         "  exit N     leave the script or shell with status N\n"
                         "\n"
                         "There is no switch/case: use else if. There is no Bash syntax: no [[ ]],\n"
                         "no $((...)), no && or ||. Use and / or / not.\n" },
                         { "chihuahuas", "Chihuahuas: plugins in .chu files",
                             "CHIHUAHUAS (.chu)\n"
                             "\n"
                             "A Chihuahua is a small plugin for mpcsh, one text file ending in .chu,\n"
                             "like a Kitty kitten. It can add commands to the shell, written in mpcsh,\n"
                             "C, sh, Python or any interpreter you name. C Chihuahuas get a helper header\n"
                             "that can talk to MPCECC (show images with MPSP, launch PixelWindows).\n"
                             "\n"
                             "File format\n"
                             "  #!chu 1\n"
                             "  name: hello\n"
                             "  version: 1.0.0\n"
                             "  author: Marek\n"
                             "  description: Says hello\n"
                             "  lang: mpcsh\n"
                             "  commands: hello, hi\n"
                             "  ---\n"
                             "  function hello\n"
                             "    echo hello $arg1\n"
                             "  end\n"
                             "  function hi\n"
                             "    hello $arg1\n"
                             "  end\n"
                             "\n"
                             "The first line must be #!chu 1. Then come \"key: value\" lines, then a line\n"
                             "containing only --- , then the body. Unknown keys are ignored.\n"
                             "  name         required. lower case letters, digits, - and _ (max 32)\n"
                             "  version      free text, default 0\n"
                             "  author, description    free text\n"
                             "  lang         mpcsh (default) | c | sh | python | exec\n"
                             "  commands     comma separated commands this Chihuahua adds (default:\n"
                             "               the name). Built-in names cannot be used.\n"
                             "  run          lang exec only: the interpreter line, e.g. /usr/bin/env lua\n"
                             "  cflags, libs lang c only: extra compiler flags, e.g. libs: -lm\n"
                             "  needs        free text for humans, e.g. mpsp\n"
                             "\n"
                             "Languages\n"
                             "  mpcsh   body may only contain function definitions. They are loaded when\n"
                             "          the shell starts. Functions get $arg1... and return a status.\n"
                             "  c       compiled when you install it (cc -O2 -std=gnu11, no pkg-config).\n"
                             "          Each command runs the program with its own name as argv[0].\n"
                             "  sh      body saved as /bin/sh script.\n"
                             "  python  body saved as a python3 script.\n"
                             "  exec    body saved as a script run by the \"run\" interpreter.\n"
                             "Programs (not mpcsh functions) run with these variables set:\n"
                             "  MPCSH_CHU       the Chihuahua name\n"
                             "  MPCSH_CHU_CMD   the command that was typed\n"
                             "  MPCSH_CHU_DIR   the install directory\n"
                             "  MPCSH_CHU_DATA  a private writable directory for this Chihuahua\n"
                             "plus MPCECC and MPCECC_TAB when inside MPCECC.\n"
                             "\n"
                             "The C helper API  (#include \"mpcchu.h\", written by mpc-chuctrl)\n"
                             "  mpcchu_name()                  the Chihuahua name\n"
                             "  mpcchu_data_dir()              private data directory\n"
                             "  mpcchu_in_ecc()                1 when running inside MPCECC\n"
                             "  mpcchu_show_image(file)        show any image as an MPSP overlay, 0 = ok\n"
                             "  mpcchu_launch(w,h,fps,cmd,&id) start a PixelWindow, id = its wlid\n"
                             "  mpcchu_terminate(id)           close a PixelWindow\n"
                             "  mpcchu_list_windows(buf,size)  the LIST reply from MPCECC\n"
                             "  mpcchu_ecc_request(req,reply,size)   any control-socket request\n"
                             "These need MPCECC 1.9 or newer. See \"protocol\" for the requests.\n"
                             "\n"
                             "mpc-chuctrl\n"
                             "  mpc-chuctrl list [-v]             loaded Chihuahuas\n"
                             "  mpc-chuctrl info NAME             details of one\n"
                             "  mpc-chuctrl install SOURCE [...]  install a .chu file, or every .chu in\n"
                             "                                    a directory (builds it, loads it now)\n"
                             "  mpc-chuctrl remove NAME [--purge] uninstall (--purge also deletes its\n"
                             "                                    data directory)\n"
                             "  mpc-chuctrl new NAME [--lang L]   write a template NAME.chu here\n"
                             "  mpc-chuctrl check FILE            validate a .chu without installing\n"
                             "  mpc-chuctrl build NAME            rebuild a program Chihuahua\n"
                             "  mpc-chuctrl reload                load everything again\n"
                             "  mpc-chuctrl path                  show the search path\n"
                             "  mpc-chuctrl path add DIR          add a directory of .chu files\n"
                             "  mpc-chuctrl path remove DIR       forget a directory\n"
                             "  mpc-chuctrl help\n"
                             "\n"
                             "Where things live\n"
                             "  install directory   ~/.local/share/mpcsh/chu  (or $XDG_DATA_HOME/mpcsh/chu)\n"
                             "  built programs      <install directory>/bin\n"
                             "  header              <install directory>/include/mpcchu.h\n"
                             "  private data        ~/.local/share/mpcsh/chu-data/NAME\n"
                             "  extra directories   one per line in ~/.config/mpcsh/chu.path\n"
                             "                      and the colon separated $MPCSH_CHU_PATH\n"
                             "All directories are searched in that order; the first file with a given\n"
                             "name wins. Chihuahuas in extra directories are used in place: mpcsh\n"
                             "loads them, but a C one must be built once with  mpc-chuctrl build NAME.\n"
                             "Files that are not valid are reported at start-up and skipped.\n"
                             "\n"
                             "Order of lookup for a command\n"
                             "  built-in, then mpcsh function, then Chihuahua program, then $PATH.\n"
                             "\n"
                             "Safety\n"
                             "  Installing a .chu runs its code (and compiles C). Only install files you\n"
                             "  trust, exactly like any other program. mpcsh Chihuahuas can only define\n"
                             "  functions at load time; they cannot run commands while loading.\n"
                             "\n"
                             "Quick start\n"
                             "  mpc-chuctrl new demo --lang c\n"
                             "  mpc-chuctrl install demo.chu\n"
                             "  demo\n" },
                             { "time", "The time command",
                                 "TIME\n"
                                 "\n"
                                 "  time COMMAND [ARGS...]\n"
                                 "\n"
                                 "Runs COMMAND exactly as if you had typed it without \"time\", then prints\n"
                                 "three lines on standard error:\n"
                                 "\n"
                                 "  real    0m1.204s      wall-clock time\n"
                                 "  user    0m0.310s      CPU time in user mode\n"
                                 "  sys     0m0.052s      CPU time in the kernel\n"
                                 "\n"
                                 "User and sys add up the shell itself and all children that finished while\n"
                                 "COMMAND ran, so built-ins and pipelines are measured too.\n"
                                 "\n"
                                 "Details\n"
                                 "  - The status of COMMAND becomes the status of \"time\".\n"
                                 "  - A whole pipeline is timed: time ls | wc\n"
                                 "  - In a command list only the first command is timed:\n"
                                 "        time make and echo done      (times make, not echo)\n"
                                 "  - \"time\" with no command prints a usage message and returns 2.\n"
                                 "  - \"time\" shadows /usr/bin/time. Use the full path to get that one.\n"
                                 "\n"
                                 "Not to be confused with the prompt's command-duration field. That one is\n"
                                 "controlled by show_cmd_time in the config file (see \"config\").\n"
                                 "\n"
                                 "Examples\n"
                                 "  time sleep 1\n"
                                 "  time ls /usr/bin | wc -l\n"
                                 "  time mpcimgcat ~/wallpaper.png\n" },
                                 { "config", "mpcsh.config reference",
                                     "CONFIG FILE\n"
                                     "\n"
                                     "Location: ~/.config/mpcsh/mpcsh.config, or the file named by $MPCSH_CONFIG.\n"
                                     "Format: one key=value per line. Lines starting with # are comments.\n"
                                     "Keys and values are not case sensitive. Unknown keys are ignored.\n"
                                     "\n"
                                     "accent  (aliases: color, theme)\n"
                                     "  purple orange green magenta pink cyan blue red yellow white\n"
                                     "  gradient-purple-orange   (aliases: gradient, purple-orange)\n"
                                     "  gradient-orange-purple   (alias: orange-purple)\n"
                                     "  purple-aero              (aliases: aero, glass)\n"
                                     "  Unknown values fall back to purple.\n"
                                     "\n"
                                     "animated  (alias: animate)         true|false, default false\n"
                                     "  Animates gradient accents, 24 frames.\n"
                                     "\n"
                                     "connector  (alias: style)\n"
                                     "  default starship simple blocks\n"
                                     "  crystal  (alias: zsh-time)       zsh-like crystal rule prompt\n"
                                     "  aero     (alias: glass)\n"
                                     "  Unknown values fall back to default.\n"
                                     "\n"
                                     "show_clock     (alias: clock)       default true\n"
                                     "show_uptime    (alias: uptime)      default false\n"
                                     "show_cmd_time  (aliases: cmd_time, duration)   default true\n"
                                     "aero           (alias: glass)       default true\n"
                                     "\n"
                                     "Boolean values: true, 1, yes, on mean TRUE. Anything else means FALSE.\n"
                                     "\n"
                                     "Example\n"
                                     "  accent=gradient-purple-orange\n"
                                     "  animated=true\n"
                                     "  connector=crystal\n"
                                     "  show_clock=yes\n"
                                     "  show_uptime=no\n" },
                                     { "prompt", "Prompt layout",
                                         "PROMPT\n"
                                         "\n"
                                         "The first prompt row is built from three parts:\n"
                                         "  left     the connector and the user/host/directory text\n"
                                         "  fill     spaces, or a glyph such as the crystal rule\n"
                                         "  meta     right-aligned info, like zsh RPROMPT: clock, uptime and the\n"
                                         "           duration of the last command (each can be switched off)\n"
                                         "\n"
                                         "The row always ends one column before the terminal edge, so it never\n"
                                         "wraps. If the meta part does not fit it is dropped completely; it can\n"
                                         "never overlap or cut the connector on the left. On a very narrow terminal\n"
                                         "only a short tail is drawn.\n"
                                         "\n"
                                         "Accents have three shades (slot 0, 1, 2) used for the connector, the\n"
                                         "text and the dim parts. Gradient accents blend purple and orange, and\n"
                                         "move when animated=true.\n"
                                         "\n"
                                         "Change everything in the config file (see \"config\").\n" },
                                         { "windows", "PixelWindows: windowlaunch and friends",
                                             "PIXELWINDOWS\n"
                                             "\n"
                                             "Run a normal GUI program inside the current MPCECC tab, FreeDOS style.\n"
                                             "This only works when mpcsh runs inside MPCECC ($MPCECC is set).\n"
                                             "\n"
                                             "  windowlaunch PROGRAM [ARGS...]\n"
                                             "  windowlaunch --size WxH --fps N PROGRAM [ARGS...]\n"
                                             "  windowlaunch --check                 test that MPCECC answers\n"
                                             "\n"
                                             "Defaults: 1440x900 at 15 fps. Size is limited to 64..8192 per side and\n"
                                             "fps to 1..60. Programs get their own private X server (Xvfb); MPCECC\n"
                                             "captures its frames into an overlay in the tab. In-tab frames are capped\n"
                                             "at 1920x1200. The picture is fitted with letterboxing, never stretched.\n"
                                             "\n"
                                             "Each launch gets a number, the wlid. The shell stays free afterwards.\n"
                                             "  windowterminate WLID       close one program\n"
                                             "  wlidlist                   id, program, title and focus flag\n"
                                             "  windowpicker               window picker\n"
                                             "\n"
                                             "Several programs can live in one tab at the same time. Cycle them with\n"
                                             "Ctrl+Shift+Tab (add Shift to go backwards) or Ctrl+Shift+backtick. F3\n"
                                             "opens the star-map picker. Alt+Tab is deliberately not used because the\n"
                                             "desktop steals it.\n"
                                             "\n"
                                             "The overlay has a title bar with minimize, maximize, restore and close,\n"
                                             "can be dragged and resized, and accepts clicks and typing through XTest.\n"
                                             "Closing the tab stops only the programs that belong to that tab.\n"
                                             "\n"
                                             "Firefox is always started with its own isolated profile (MPCECCWLPROFILE\n"
                                             "under ~/.mozilla/firefox/). Your real Firefox profile is never touched.\n"
                                             "\n"
                                             "Since 1.8\n"
                                             "  - Arguments with spaces are quoted correctly, so\n"
                                             "        windowlaunch gimp \"my picture.png\"\n"
                                             "    works.\n"
                                             "  - The program opens in the tab the command was typed in, even if you\n"
                                             "    switch tabs while it starts.\n" },
                                             { "imgcat", "mpcimgcat and the MPSP protocol",
                                                 "MPCIMGCAT\n"
                                                 "\n"
                                                 "  mpcimgcat IMAGE [IMAGE ...]\n"
                                                 "\n"
                                                 "A leading ~ is expanded. Standard output must be a terminal with a size.\n"
                                                 "mpcimgcat tries these methods in order:\n"
                                                 "\n"
                                                 "  1. MPSP    when $MPCECC is set. A real-pixel floating overlay with a\n"
                                                 "             title bar, drag and a close button appears in the tab.\n"
                                                 "  2. Kitty   graphics protocol (KITTY_WINDOW_ID or TERM contains kitty)\n"
                                                 "  3. iTerm2  inline images (LC_TERMINAL=iTerm2)\n"
                                                 "  4. Half-blocks  24-bit colour using the upper-half-block character,\n"
                                                 "             aspect-corrected (PIXEL_ASPECT 0.52). Used by Konsole and\n"
                                                 "             other terminals without a graphics protocol.\n"
                                                 "\n"
                                                 "The image is fitted into the terminal width and the height minus two rows.\n"
                                                 "Since 1.8 transparent pixels in the half-block mode are drawn over a\n"
                                                 "dark checkerboard instead of whatever colour was behind them.\n"
                                                 "\n"
                                                 "How MPSP works\n"
                                                 "  mpcimgcat saves the image as a PNG named mpcimgcat-XXXXXX.png in the\n"
                                                 "  temporary directory and sends one line to the MPCECC control socket:\n"
                                                 "        MPSP /tmp/mpcimgcat-AbC123.png\n"
                                                 "  MPCECC loads it, shows the overlay and deletes the temporary file.\n"
                                                 "  Since 1.8 MPCECC only deletes files that look exactly like that\n"
                                                 "  (temporary directory, mpcimgcat- prefix, .png, regular file, your own).\n"
                                                 "  The overlay appears in the tab that sent the request.\n"
                                                 "\n"
                                                 "If MPCECC refuses, mpcimgcat quietly falls back to method 2, 3 or 4.\n" },
                                                 { "mpcecc", "MPCECC terminal overview",
                                                     "MPCECC\n"
                                                     "\n"
                                                     "MarekPC Extended Capabilities Console: a GTK4 + VTE terminal.\n"
                                                     "\n"
                                                     "  - Tabs, each running its own shell. Default is mpcsh; every profile can\n"
                                                     "    name another shell.\n"
                                                     "  - Profiles: name, wallpaper, font and shell. Stored in profiles.conf in\n"
                                                     "    the mpcecc configuration directory; edit them in Settings.\n"
                                                     "  - Wallpaper drawn behind a transparent terminal. The whole image is shown\n"
                                                     "    and its edge pixels are smeared outward, then darkened towards the\n"
                                                     "    window border so text stays readable. Default image:\n"
                                                     "    /bin/mpcshresources/wallpaper.png\n"
                                                     "  - Font falls back to \"Monospace 11\" if the profile font cannot be used.\n"
                                                     "  - Right-click menu on every tab.\n"
                                                     "  - If a shell dies, a dialog offers Restart Shell or Close Tab. Closing a\n"
                                                     "    tab yourself never triggers that dialog.\n"
                                                     "  - PixelWindows, the star-map picker and MPSP images (see \"windows\" and\n"
                                                     "    \"imgcat\").\n"
                                                     "  - Children get $MPCECC=1 and $MPCECC_TAB. Secrets such as SSH_AUTH_SOCK\n"
                                                     "    are removed from their environment (see \"env\").\n"
                                                     "\n"
                                                     "Build (CachyOS / Arch): see the comment at the top of mpcecc.c.\n" },
                                                     { "keys", "Keyboard shortcuts",
                                                         "KEYBOARD SHORTCUTS (MPCECC)\n"
                                                         "\n"
                                                         "All shortcuts run in the capture phase, so they work while the terminal\n"
                                                         "has focus.\n"
                                                         "\n"
                                                         "  Ctrl+Shift+C       copy the selection\n"
                                                         "  Ctrl+Shift+V       paste\n"
                                                         "  Ctrl+V             paste\n"
                                                         "  Ctrl+Shift+T       new tab\n"
                                                         "  Ctrl+Shift+W       close tab\n"
                                                         "  Ctrl+,             settings\n"
                                                         "  Ctrl+?             help\n"
                                                         "  F3                 star-map window picker\n"
                                                         "  Ctrl+Shift+Tab     next PixelWindow (Shift reverses direction)\n"
                                                         "  Ctrl+Shift+`       next PixelWindow\n"
                                                         "\n"
                                                         "In mpcsh\n"
                                                         "  Tab / Shift+Tab    complete; further presses cycle the matches\n"
                                                         "                     (commands in command position, files elsewhere,\n"
                                                         "                     directories only after cd)\n"
                                                         "  Ctrl+C             never kills the shell\n" },
                                                         { "protocol", "MPCECC control socket",
                                                             "CONTROL SOCKET\n"
                                                             "\n"
                                                             "Path: $XDG_RUNTIME_DIR/mpcecc-control.sock (or /tmp/mpcecc-control-UID.sock\n"
                                                             "when there is no runtime directory). Mode 0600, so only you can use it.\n"
                                                             "\n"
                                                             "One request per connection, ended by a newline. Since 1.8 a request may\n"
                                                             "be up to 64 KB long and is read until the newline (2 second timeout).\n"
                                                             "An optional prefix \"@N \" names the tab the request belongs to. mpcsh fills\n"
                                                             "it in from $MPCECC_TAB. Without it, the selected tab is used.\n"
                                                             "\n"
                                                             "Requests and replies\n"
                                                             "  LAUNCH W H FPS -- PROGRAM ARGS     reply: OK WLID or ERR text\n"
                                                             "  TERMINATE WLID  (or KILL WLID)     reply: OK or ERR no such wlid\n"
                                                             "  LIST  (or WLIDLIST)                one line per window:\n"
                                                             "                                     id TAB command TAB title TAB focused\n"
                                                             "  FOCUS WLID                         reply: OK or ERR no such wlid\n"
                                                             "  HIDE                               hide the focused window\n"
                                                             "  MPSP PATH                          show an image overlay\n"
                                                             "                                     reply: OK, ERR file not found,\n"
                                                             "                                     ERR no tab, ERR load: message\n"
                                                             "  anything else                      ERR unknown\n"
                                                             "\n"
                                                             "Example from a shell\n"
                                                             "  printf '@%s LIST\\n' \"$MPCECC_TAB\"\n" },
                                                             { "env", "Environment variables and exit codes",
                                                                 "ENVIRONMENT\n"
                                                                 "\n"
                                                                 "Set by MPCECC for everything it starts\n"
                                                                 "  MPCECC=1              you are inside MPCECC\n"
                                                                 "  MPCECC_VERSION        MPCECC version\n"
                                                                 "  MPCECC_TAB            number of the tab (1.8)\n"
                                                                 "Set for embedded programs\n"
                                                                 "  MPCECC_MANAGED=1, MPCECC_PIXEL_WINDOW=1\n"
                                                                 "Used for Firefox\n"
                                                                 "  MPCECCWLPROFILE       name of the isolated profile\n"
                                                                 "Read by mpcsh\n"
                                                                 "  MPCSH_CONFIG          alternative config file\n"
                                                                 "Passed to extensions\n"
                                                                 "  MPCSH_VERSION MPCSH_SHELL MPCSH_NAME MPCSH_FRONTEND\n"
                                                                 "\n"
                                                                 "Removed from the environment of shells started by MPCECC\n"
                                                                 "  SSH_AUTH_SOCK GPG_AGENT_INFO PASSWORD PASSWD TOKEN SECRET API_KEY\n"
                                                                 "  AWS_SECRET_ACCESS_KEY\n"
                                                                 "\n"
                                                                 "EXIT STATUS\n"
                                                                 "  0         success\n"
                                                                 "  2         wrong usage of a built-in\n"
                                                                 "  126       found but cannot be executed, permission denied, or the\n"
                                                                 "            command line could not be parsed\n"
                                                                 "  127       command not found\n"
                                                                 "  128+N     killed by signal N\n" },
                                                                 { "mpcshman", "Using mpcshman",
                                                                     "MPCSHMAN\n"
                                                                     "\n"
                                                                     "  mpcshman                 open the manual at the contents page\n"
                                                                     "  mpcshman TOPIC           page number (3), name (builtins) or the start of\n"
                                                                     "                           a name (bui) or a word from a title\n"
                                                                     "  mpcshman -l              list all pages\n"
                                                                     "  mpcshman -k WORD         search all pages, print every matching line with\n"
                                                                     "                           its page and line number\n"
                                                                     "  mpcshman -s WORD         open the pager at the first match for WORD\n"
                                                                     "  mpcshman -p TOPIC        print one page without the pager\n"
                                                                     "  mpcshman -a              print the whole manual\n"
                                                                     "  mpcshman -h              short usage\n"
                                                                     "\n"
                                                                     "When standard output is not a terminal (for example in a pipe) the pager is\n"
                                                                     "skipped and the text is printed instead:\n"
                                                                     "  mpcshman -a | grep -i socket\n"
                                                                     "\n"
                                                                     "PAGER KEYS\n"
                                                                     "  space, f, PgDn     down one screen (at the bottom: next page)\n"
                                                                     "  b, PgUp            up one screen\n"
                                                                     "  j, Down, Enter     down one line\n"
                                                                     "  k, Up              up one line\n"
                                                                     "  g, Home / G, End   top / bottom of the page\n"
                                                                     "  ] or Right         next page\n"
                                                                     "  [ or Left          previous page\n"
                                                                     "  :                  jump to a page by number or name\n"
                                                                     "  t or c             contents page\n"
                                                                     "  /                  search forward through ALL pages\n"
                                                                     "  n / N              next / previous match (without a search, n is the\n"
                                                                     "                     next page)\n"
                                                                     "  p                  previous page\n"
                                                                     "  r                  redraw\n"
                                                                     "  ?                  key help\n"
                                                                     "  q                  quit\n"
                                                                     "\n"
                                                                     "Matches are highlighted. Searching ignores upper and lower case.\n" },
                                                                     { "changes", "Changes in 1.8",
                                                                         "CHANGES\n"
                                                                         "\n"
                                                                         "1.8\n"
                                                                         "  - NEW  Scripting: while loops, else if, script arguments ($argc, $argv,\n"
                                                                         "         $arg1...), scripts with any file name starting with #!, math and\n"
                                                                         "         source built-ins. See \"scripting\".\n"
                                                                         "  - NEW  Chihuahuas: .chu plugin files in mpcsh, C, sh, python or any\n"
                                                                         "         interpreter, with a C helper header for MPSP and PixelWindows.\n"
                                                                         "  - NEW  mpc-chuctrl: install, remove, list, info, new, check, build,\n"
                                                                         "         reload and path management. See \"chihuahuas\".\n"
                                                                         "  - Works with MPCECC 1.9 (no MPCECC change needed).\n"
                                                                         "\n"
                                                                         "1.8\n"
                                                                         "  mpcsh\n"
                                                                         "  - NEW  time COMMAND: real, user and sys report.\n"
                                                                         "  - NEW  mpcshman: multi-page manual with pager and search.\n"
                                                                         "  - FIX  mpcimgcat used fmin() without math.h and the documented build\n"
                                                                         "         line had no -lm. Header added and build lines updated (also\n"
                                                                         "         -lcurl, which the update code needs).\n"
                                                                         "  - FIX  mpcimgcat half-block mode ignored transparency.\n"
                                                                         "  - FIX  windowlaunch now quotes every argument and sends the whole\n"
                                                                         "         request (write-all instead of one write).\n"
                                                                         "  - FIX  windowlaunch and mpcimgcat send $MPCECC_TAB so the result opens\n"
                                                                         "         in the tab you typed in.\n"
                                                                         "  MPCECC\n"
                                                                         "  - FIX  MPSP only deletes real mpcimgcat temporary files.\n"
                                                                         "  - FIX  MPSP and LAUNCH use the requesting tab (every tab has an id and\n"
                                                                         "         exports it as MPCECC_TAB).\n"
                                                                         "  - FIX  the overlay close button uses gtk_overlay_remove_overlay().\n"
                                                                         "  - FIX  control requests up to 64 KB, read until newline, 2 second timeout.\n"
                                                                         "\n"
                                                                         "1.8 / 1.8.0\n"
                                                                         "  - Multi-window PixelWindows with wlid registry, F3 star-map and cycling\n"
                                                                         "    with Ctrl+Shift+Tab.\n"
                                                                         "  - Default framebuffer 1440x900, in-tab cap 1920x1200.\n"
                                                                         "  - MPSP image overlays from mpcimgcat; Kitty, iTerm2 and aspect-corrected\n"
                                                                         "    half-block fallbacks.\n"
                                                                         "  - Isolated Firefox profile for embedded Firefox.\n"
                                                                         "  - ls and wallpaper built-ins, path completion fix, 128+signal status.\n" },
 };

 #define MPCSH_MAN_N ((int)(sizeof mpcsh_man_pages / sizeof mpcsh_man_pages[0]))

 static int mpcsh_man_count_lines(const char *b)
 {
     int n = 0;
     for (const char *p = b; *p; ++p) if (*p == '\n') ++n;
     if (*b && b[strlen(b) - 1] != '\n') ++n;
     return n;
 }

 static void mpcsh_man_get_line(const char *b, int idx, char *out, size_t sz)
 {
     const char *p = b;
     while (idx > 0 && *p) {
         const char *nl = strchr(p, '\n');
         if (!nl) { p += strlen(p); break; }
         p = nl + 1;
         --idx;
     }
     const char *e = strchr(p, '\n');
     size_t n = e ? (size_t)(e - p) : strlen(p);
     if (idx > 0) n = 0;
     if (n >= sz) n = sz - 1;
     memcpy(out, p, n);
     out[n] = 0;
 }

 static void mpcsh_man_print_hl(const char *line, const char *term)
 {
     if (!term || !*term) { fputs(line, stdout); return; }
     size_t tl = strlen(term);
     const char *p = line;
     for (;;) {
         const char *m = strcasestr(p, term);
         if (!m) { fputs(p, stdout); break; }
         fwrite(p, 1, (size_t)(m - p), stdout);
         fputs("\033[7m", stdout);
         fwrite(m, 1, tl, stdout);
         fputs("\033[0m", stdout);
         p = m + tl;
     }
 }

 /* Find the next (dir=+1) or previous (dir=-1) line containing `term`,
  * starting after (pg, ln) and wrapping around the whole manual. */
 static int mpcsh_man_next_match(const char *term, int pg, int ln, int dir, int *opg, int *oln)
 {
     if (!term || !*term) return 0;
     int total = 0;
     for (int i = 0; i < MPCSH_MAN_N; ++i) total += mpcsh_man_count_lines(mpcsh_man_pages[i].body);
     int cpg = pg, cln = ln;
     char line[512];
     for (int step = 0; step < total; ++step) {
         cln += dir;
         if (cln >= mpcsh_man_count_lines(mpcsh_man_pages[cpg].body)) {
             cpg = (cpg + 1) % MPCSH_MAN_N; cln = 0;
         } else if (cln < 0) {
             cpg = (cpg + MPCSH_MAN_N - 1) % MPCSH_MAN_N;
             cln = mpcsh_man_count_lines(mpcsh_man_pages[cpg].body) - 1;
         }
         mpcsh_man_get_line(mpcsh_man_pages[cpg].body, cln, line, sizeof line);
         if (strcasestr(line, term)) { *opg = cpg; *oln = cln; return 1; }
     }
     return 0;
 }

 static int mpcsh_man_find_page(const char *s)
 {
     if (!s || !*s) return -1;
     int digits = 1;
     for (const char *p = s; *p; ++p) if (!isdigit((unsigned char)*p)) digits = 0;
     if (digits) {
         int n = atoi(s);
         return (n >= 1 && n <= MPCSH_MAN_N) ? n - 1 : -1;
     }
     size_t l = strlen(s);
     for (int i = 0; i < MPCSH_MAN_N; ++i)
         if (!strncasecmp(mpcsh_man_pages[i].name, s, l)) return i;
         for (int i = 0; i < MPCSH_MAN_N; ++i)
             if (strcasestr(mpcsh_man_pages[i].title, s)) return i;
             return -1;
 }

 static void mpcsh_man_print_page(int i)
 {
     printf("=== page %d/%d: %s - %s ===\n\n%s\n", i + 1, MPCSH_MAN_N,
            mpcsh_man_pages[i].name, mpcsh_man_pages[i].title, mpcsh_man_pages[i].body);
 }

 static int mpcsh_man_search_list(const char *kw)
 {
     int hits = 0;
     char line[512];
     for (int i = 0; i < MPCSH_MAN_N; ++i) {
         int n = mpcsh_man_count_lines(mpcsh_man_pages[i].body);
         for (int j = 0; j < n; ++j) {
             mpcsh_man_get_line(mpcsh_man_pages[i].body, j, line, sizeof line);
             if (!strcasestr(line, kw)) continue;
             const char *t = line;
             while (*t == ' ') ++t;
             printf("%2d %-9s %3d: ", i + 1, mpcsh_man_pages[i].name, j + 1);
             mpcsh_man_print_hl(t, isatty(STDOUT_FILENO) ? kw : NULL);
             putchar('\n');
             ++hits;
         }
     }
     if (!hits) { fprintf(stderr, "mpcshman: nothing found for '%s'\n", kw); return 1; }
     printf("\n%d match%s. Open a page with: mpcshman NUMBER  (or mpcshman -s %s)\n",
            hits, hits == 1 ? "" : "es", kw);
     return 0;
 }

 enum { MK_UP = 1000, MK_DOWN, MK_LEFT, MK_RIGHT, MK_PGUP, MK_PGDN, MK_HOME, MK_END };

 static int mpcsh_man_getkey(void)
 {
     unsigned char c;
     ssize_t n = read(STDIN_FILENO, &c, 1);
     if (n < 0 && errno == EINTR) return 'q';
     if (n <= 0) return 'q';
     if (c != 27) return c;
     fd_set fs; struct timeval tv = { 0, 60000 };
     FD_ZERO(&fs); FD_SET(STDIN_FILENO, &fs);
     if (select(STDIN_FILENO + 1, &fs, NULL, NULL, &tv) <= 0) return 27;
     unsigned char s[4] = {0};
     if (read(STDIN_FILENO, &s[0], 1) != 1) return 27;
     if (s[0] != '[' && s[0] != 'O') return 27;
     if (read(STDIN_FILENO, &s[1], 1) != 1) return 27;
     switch (s[1]) {
         case 'A': return MK_UP;   case 'B': return MK_DOWN;
         case 'C': return MK_RIGHT; case 'D': return MK_LEFT;
         case 'H': return MK_HOME; case 'F': return MK_END;
         case '1': case '7': (void)!read(STDIN_FILENO, &s[2], 1); return MK_HOME;
         case '4': case '8': (void)!read(STDIN_FILENO, &s[2], 1); return MK_END;
         case '5': (void)!read(STDIN_FILENO, &s[2], 1); return MK_PGUP;
         case '6': (void)!read(STDIN_FILENO, &s[2], 1); return MK_PGDN;
     }
     return 27;
 }

 static int mpcsh_man_prompt(const struct termios *raw, const struct termios *cooked,
                             int rows, const char *lead, char *out, size_t sz)
 {
     printf("\033[%d;1H\033[2K%s\033[?25h", rows, lead);
     fflush(stdout);
     tcsetattr(STDIN_FILENO, TCSADRAIN, cooked);
     char *r = fgets(out, (int)sz, stdin);
     tcsetattr(STDIN_FILENO, TCSADRAIN, raw);
     fputs("\033[?25l", stdout);
     if (!r) return 0;
     out[strcspn(out, "\r\n")] = 0;
     return 1;
 }

 static void mpcsh_man_term_size(int *rows, int *cols)
 {
     struct winsize ws;
     *rows = 24; *cols = 80;
     if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
         if (ws.ws_row > 4) *rows = ws.ws_row;
         if (ws.ws_col > 20) *cols = ws.ws_col;
     }
 }

 static const char *const mpcsh_man_keyhelp =
 "mpcshman pager keys\n\n"
 "  space f PgDn   down a screen (at the bottom: next page)\n"
 "  b PgUp         up a screen\n"
 "  j Down Enter   down a line        k Up   up a line\n"
 "  g Home         top of page        G End  bottom of page\n"
 "  ] Right        next page          [ Left previous page\n"
 "  p              previous page      t c    contents\n"
 "  :              go to a page by number or name\n"
 "  /              search all pages   n N    next / previous match\n"
 "  r              redraw             q      quit\n\n"
 "press any key";

     static int mpcsh_man_pager(int pg, const char *term_init)
     {
         struct termios cooked, raw;
         if (tcgetattr(STDIN_FILENO, &cooked) != 0) return 1;
         raw = cooked;
         raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
         raw.c_cc[VMIN] = 1;
         raw.c_cc[VTIME] = 0;
         tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
         fputs("\033[?1049h\033[?25l", stdout);

         char term[128] = "";
         char msg[256] = "";
         int top = 0;
         int cur_mp = -1, cur_ml = -1;   /* last search match, so n/N keep moving */
         if (term_init && *term_init) {
             snprintf(term, sizeof term, "%s", term_init);
             int mp, ml;
             if (mpcsh_man_next_match(term, 0, -1, 1, &mp, &ml)) { pg = mp; top = ml > 2 ? ml - 2 : 0; cur_mp = mp; cur_ml = ml; }
             else snprintf(msg, sizeof msg, "pattern not found: %s", term);
         }

         for (;;) {
             int rows, cols;
             mpcsh_man_term_size(&rows, &cols);
             const char *body = mpcsh_man_pages[pg].body;
             int total = mpcsh_man_count_lines(body);
             int view = rows - 2; if (view < 1) view = 1;
             int maxtop = total > view ? total - view : 0;
             if (top > maxtop) top = maxtop;
             if (top < 0) top = 0;

             fputs("\033[H\033[2J", stdout);
             printf("\033[1m mpcshman  %d/%d  %s\033[0m\r\n", pg + 1, MPCSH_MAN_N, mpcsh_man_pages[pg].title);
             char line[512];
             for (int i = 0; i < view; ++i) {
                 if (top + i >= total) break;
                 mpcsh_man_get_line(body, top + i, line, sizeof line);
                 mpcsh_man_print_hl(line, term);
                 fputs("\r\n", stdout);
             }
             char bar[512];
             int last = top + view < total ? top + view : total;
             snprintf(bar, sizeof bar, " lines %d-%d/%d  %s  [space next] [/ search] [? help] [q quit]",
                      total ? top + 1 : 0, last, total, msg[0] ? msg : "");
             msg[0] = 0;
             int bl = (int)strlen(bar);
             printf("\033[%d;1H\033[7m%s%*s\033[0m", rows, bar, bl < cols ? cols - bl : 0, "");
             fflush(stdout);

             int k = mpcsh_man_getkey();
             int quit = 0;
             switch (k) {
                 case 'q': case 'Q': quit = 1; break;
                 case ' ': case 'f': case MK_PGDN:
                     if (top >= maxtop) { if (pg < MPCSH_MAN_N - 1) { ++pg; top = 0; } }
                     else top += view > 1 ? view - 1 : 1;
                     break;
                 case 'b': case MK_PGUP:
                     if (top == 0 && pg > 0) { --pg; top = 1 << 20; }
                     else top -= view > 1 ? view - 1 : 1;
                     break;
                 case 'j': case MK_DOWN: case '\n': case '\r': ++top; break;
                 case 'k': case MK_UP: --top; break;
                 case 'g': case MK_HOME: top = 0; break;
                 case 'G': case MK_END: top = maxtop; break;
                 case ']': case MK_RIGHT: if (pg < MPCSH_MAN_N - 1) { ++pg; top = 0; } break;
                 case '[': case MK_LEFT:  if (pg > 0) { --pg; top = 0; } break;
                 case 'p': if (pg > 0) { --pg; top = 0; } break;
                 case 't': case 'c': pg = 0; top = 0; break;
                 case 'r': break;
                 case '?': case 'h':
                     printf("\033[H\033[2J%s", mpcsh_man_keyhelp);
                     fflush(stdout);
                     (void)mpcsh_man_getkey();
                     break;
                 case ':': {
                     char in[64];
                     if (mpcsh_man_prompt(&raw, &cooked, rows, "page: ", in, sizeof in)) {
                         int np = mpcsh_man_find_page(in);
                         if (np >= 0) { pg = np; top = 0; }
                         else snprintf(msg, sizeof msg, "no such page: %s", in);
                     }
                     break;
                 }
                 case '/': {
                     char in[128] = "";
                     if (!mpcsh_man_prompt(&raw, &cooked, rows, "/", in, sizeof in) || !*in)
                         break;
                     snprintf(term, sizeof term, "%s", in);
                     int mp, ml;
                     if (mpcsh_man_next_match(term, pg, top - 1, 1, &mp, &ml)) {
                         pg = mp; top = ml > 2 ? ml - 2 : 0; cur_mp = mp; cur_ml = ml;
                     } else snprintf(msg, sizeof msg, "pattern not found: %s", term);
                     break;
                 }
                 case 'n': case 'N': {
                     if (!term[0]) {
                         if (k == 'n' && pg < MPCSH_MAN_N - 1) { ++pg; top = 0; }
                         break;
                     }
                     int dir = (k == 'N') ? -1 : 1;
                     int sp = pg, sl = top;                 /* start after the current match, */
                     if (cur_mp == pg && cur_ml >= 0) sl = cur_ml;   /* or after the top line  */
                         else if (dir > 0) sl = top - 1;
                         int mp, ml;
                     if (mpcsh_man_next_match(term, sp, sl, dir, &mp, &ml)) {
                         pg = mp; top = ml > 2 ? ml - 2 : 0; cur_mp = mp; cur_ml = ml;
                     } else snprintf(msg, sizeof msg, "pattern not found: %s", term);
                     break;
                 }
                 default: break;
             }
             if (quit) break;
         }
         fputs("\033[?25h\033[?1049l", stdout);
         fflush(stdout);
         tcsetattr(STDIN_FILENO, TCSADRAIN, &cooked);
         return 0;
     }

     static int mpcsh_mpcshman_builtin(char **argv)
     {
         int tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
         const char *usage =
         "Usage: mpcshman [TOPIC]          open the manual (page number or name)\n"
         "       mpcshman -l               list pages\n"
         "       mpcshman -k WORD          search all pages and list the hits\n"
         "       mpcshman -s WORD          open the pager at the first hit\n"
         "       mpcshman -p TOPIC         print one page without the pager\n"
         "       mpcshman -a               print the whole manual\n";
         if (!argv[1]) {
             if (tty) return mpcsh_man_pager(0, NULL);
             mpcsh_man_print_page(0);
             return 0;
         }
         const char *a = argv[1];
         if (!strcmp(a, "-h") || !strcmp(a, "--help")) { fputs(usage, stdout); return 0; }
         if (!strcmp(a, "-l") || !strcmp(a, "--list")) {
             for (int i = 0; i < MPCSH_MAN_N; ++i)
                 printf("%2d  %-10s %s\n", i + 1, mpcsh_man_pages[i].name, mpcsh_man_pages[i].title);
             return 0;
         }
         if (!strcmp(a, "-a") || !strcmp(a, "--all")) {
             for (int i = 0; i < MPCSH_MAN_N; ++i) mpcsh_man_print_page(i);
             return 0;
         }
         if (!strcmp(a, "-k") || !strcmp(a, "-K") || !strcmp(a, "--search")) {
             if (!argv[2]) { fprintf(stderr, "mpcshman: -k needs a word\n%s", usage); return 2; }
             GString *w = g_string_new(argv[2]);
             for (int i = 3; argv[i]; ++i) { g_string_append_c(w, ' '); g_string_append(w, argv[i]); }
             int rc = mpcsh_man_search_list(w->str);
             g_string_free(w, TRUE);
             return rc;
         }
         if (!strcmp(a, "-s") || !strcmp(a, "--open-search")) {
             if (!argv[2]) { fprintf(stderr, "mpcshman: -s needs a word\n%s", usage); return 2; }
             if (!tty) return mpcsh_man_search_list(argv[2]);
             return mpcsh_man_pager(0, argv[2]);
         }
         if (!strcmp(a, "-p") || !strcmp(a, "--print")) {
             if (!argv[2]) { fprintf(stderr, "mpcshman: -p needs a page\n%s", usage); return 2; }
             int i = mpcsh_man_find_page(argv[2]);
             if (i < 0) { fprintf(stderr, "mpcshman: no such page: %s (try: mpcshman -l)\n", argv[2]); return 1; }
             mpcsh_man_print_page(i);
             return 0;
         }
         if (a[0] == '-') { fprintf(stderr, "mpcshman: unknown option %s\n%s", a, usage); return 2; }
         int i = mpcsh_man_find_page(a);
         if (i < 0) {
             fprintf(stderr, "mpcshman: no manual page for '%s'. Try: mpcshman -l   or   mpcshman -k %s\n", a, a);
             return 1;
         }
         if (tty) return mpcsh_man_pager(i, NULL);
         mpcsh_man_print_page(i);
         return 0;
     }


     static int
     mpcsh_run_pipeline(char *cmdline)
     {
         if (!cmdline || !*cmdline)
             return 0;

         {   /* time COMMAND ... (1.8) */
             const char *tw = cmdline;
             while (*tw == ' ' || *tw == '\t') ++tw;
             if (!strncmp(tw, "time", 4) && (tw[4] == '\0' || tw[4] == ' ' || tw[4] == '\t')) {
                 int trc = mpcsh_time_run(tw + 4);
                 mpcsh_last_status = trc;
                 return trc;
             }
         }

         int rejected = mpcsh_reject_bash_constructs(cmdline);
         if (rejected) {
             mpcsh_last_status = rejected;
             return rejected;
         }

         char *work = g_strdup(cmdline);

         char *segments[MPCSH_MAX_PIPELINE + 1];
         int count = 0;
         char *p = work;
         char quote = 0;
         int subst_parens = 0;
         char *start = p;

         while (*p) {
             if (*p == '\\' && p[1]) { p += 2; continue; }
             if (quote) {
                 if (*p == quote) quote = 0;
             } else if (*p == '\'' || *p == '"') {
                 quote = *p;
             } else if (*p == '(') {
                 ++subst_parens;
             } else if (*p == ')' && subst_parens) {
                 --subst_parens;
             } else if (*p == '|' && !subst_parens) {
                 *p = '\0';
                 if (count >= MPCSH_MAX_PIPELINE) {
                     fprintf(stderr, "mpcsh: too many pipeline segments (maximum: %d)\n", MPCSH_MAX_PIPELINE);
                     g_free(work);
                     return 127;
                 }
                 segments[count++] = start;
                 start = p + 1;
             }
             ++p;
         }
         segments[count++] = start;

         for (int i = 0; i < count; ++i) {
             char *s = segments[i];
             while (*s == ' ' || *s == '\t') ++s;
             if (*s == '\0') {
                 fprintf(stderr, "mpcsh: invalid pipeline segment %d\n", i);
                 g_free(work);
                 return 126;
             }
         }

         if (count == 1) {
             char *argv[256];
             if (mpcsh_word_split(segments[0], argv, 256) == 0) {
                 fprintf(stderr, "mpcsh: failed to parse command\n");
                 g_free(work);
                 return 126;
             }
             if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
                 int code = argv[1] ? atoi(argv[1]) : 0;
                 if (code < 0) code = 0;
                 if (code > 255) code = 255;
                 mpcsh_free_argv(argv);
                 g_free(work);
                 exit(code);
             }
             if (strcmp(argv[0], "ctk") == 0) {
                 int result = mpcsh_ctk_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work);
                 mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "portmap") ||
                 !strcasecmp(argv[0], "quickinfo") || !strcasecmp(argv[0], "qi") ||
                 !strcasecmp(argv[0], "setbg") ||
                 !strcasecmp(argv[0], "gpuportmap") || !strcasecmp(argv[0], "gpuinfo")) {
                 int result;
                 if (!strcasecmp(argv[0], "portmap")) result = mpcsh_portmap_builtin(argv);
                 else if (!strcasecmp(argv[0], "setbg")) result = mpcsh_setbg_builtin(argv);
                 else if (!strcasecmp(argv[0], "gpuportmap") || !strcasecmp(argv[0], "gpuinfo")) result = mpcsh_gpuportmap_builtin(argv);
                 else result = mpcsh_quickinfo_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work);
                 mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "guiinterm")) {
                 int result = mpcsh_guiinterm_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work);
                 mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "windowlaunch")) {
                 int result = mpcsh_windowlaunch_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "windowterminate")) {
                 int result = mpcsh_windowterminate_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "wlidlist") || !strcasecmp(argv[0], "windowlist")) {
                 int result = mpcsh_wlidlist_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "windowpicker")) {
                 int result = mpcsh_windowpicker_builtin(argv);
                 mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = result; return result;
             }
             if (!strcasecmp(argv[0], "extension") ||
                 !strcasecmp(argv[0], "mpcextinstl")) {
                 if (!strcasecmp(argv[0], "mpcextinstl")) {
                     char *fake[4] = { (char *)"extension", (char *)"install", argv[1], NULL };
                     int result = mpcsh_extension_builtin(fake);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 int result = mpcsh_extension_builtin(argv);
             mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = result; return result;
                 }
                 if (!strcasecmp(argv[0], "ls")) {
                     int result = mpcsh_ls_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcasecmp(argv[0], "mpcshman")) {
                     int result = mpcsh_mpcshman_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "math")) {
                     int result = mpcsh_math_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "source") || !strcmp(argv[0], ".")) {
                     int result = mpcsh_source_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "mpc-chuctrl") || !strcmp(argv[0], "chuctrl")) {
                     int result = mpcsh_chuctrl_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "mpcimgcat")) {
                     int result = mpcsh_imgcat_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcasecmp(argv[0], "wallpaper")) {
                     int result = mpcsh_wallpaper_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "set")) {
                     int result = mpcsh_set_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "limits")) {
                     int result = mpcsh_limits_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "cd")) {
                     int result = mpcsh_cd_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "pwd")) {
                     int result = mpcsh_pwd_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "status")) {
                     int result = mpcsh_status_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "mpcupdate")) {
                     int result = mpcsh_mpcupdate_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "mpcbits")) {
                     int result = mpcsh_mpcbits_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 if (!strcmp(argv[0], "mpcversion")) {
                     int result = mpcsh_mpcversion_builtin(argv);
                     mpcsh_free_argv(argv); g_free(work);
                     mpcsh_last_status = result; return result;
                 }
                 int frc = mpc_function_call(argv);
                 if (frc >= 0) { mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = frc; return frc; }
                 int pid = mpcsh_spawn_segment(argv, STDIN_FILENO, STDOUT_FILENO);
                 if (pid < 0) { mpcsh_free_argv(argv); g_free(work); mpcsh_last_status = 127; return 127; }
                 int status = 0;
                 /* EINTR-safe: Ctrl+C while a foreground command runs must not
                  * kill mpcsh, and the child dies on its own SIGINT anyway. */
                 pid_t waited = mpcsh_waitpid_retry(pid, &status);
                 if (waited != pid) {
                     int saved_errno = errno;
                     fprintf(stderr, "mpcsh: waitpid(%d): %s\n", pid,
                             waited < 0 ? strerror(saved_errno) : "unexpected child result");
                     mpcsh_free_argv(argv);
                     g_free(work);
                     mpcsh_last_status = 127;
                     return 127;
                 }
                 int result = mpcsh_child_result(status);
                 mpcsh_free_argv(argv);
                 g_free(work);
                 mpcsh_last_status = result;
                 return result;
         }

         int prev_read = STDIN_FILENO;
         pid_t pids[MPCSH_MAX_PIPELINE];
         int spawned = 0;

         for (int i = 0; i < count; ++i) {
             char *argv[256];
             if (mpcsh_word_split(segments[i], argv, 256) == 0) {
                 fprintf(stderr, "mpcsh: failed to parse command segment\n");
                 if (prev_read != STDIN_FILENO) close(prev_read);
                 g_free(work);
                 mpcsh_last_status = 126;
                 return 126;
             }

             int in_fd  = prev_read;
             int out_fd = STDOUT_FILENO;
             int pipefd[2] = { -1, -1 };

             if (i < count - 1) {
                 if (pipe(pipefd) < 0) {
                     fprintf(stderr, "mpcsh: pipe: %s\n", strerror(errno));
                     if (prev_read != STDIN_FILENO) close(prev_read);
                     for (int j = 0; j < spawned; ++j) (void)kill(pids[j], SIGTERM);
                     for (int j = 0; j < spawned; ++j) {
                         int reap_status;
                         if (mpcsh_waitpid_retry(pids[j], &reap_status) < 0)
                             fprintf(stderr, "mpcsh: waitpid(%ld) after pipe failure: %s\n",
                                     (long)pids[j], strerror(errno));
                     }
                     g_free(work);
                     return 126;
                 }
                 out_fd = pipefd[1];
             }

             int pid = mpcsh_spawn_segment(argv, in_fd, out_fd);
             mpcsh_free_argv(argv);
             if (in_fd != STDIN_FILENO) close(in_fd);
             if (out_fd != STDOUT_FILENO) close(out_fd);

             if (pid < 0) {
                 if (pipefd[0] >= 0) close(pipefd[0]);
                 for (int j = 0; j < spawned; ++j) (void)kill(pids[j], SIGTERM);
                 for (int j = 0; j < spawned; ++j) {
                     int reap_status;
                     if (mpcsh_waitpid_retry(pids[j], &reap_status) < 0)
                         fprintf(stderr, "mpcsh: waitpid(%ld) after fork failure: %s\n",
                                 (long)pids[j], strerror(errno));
                 }
                 g_free(work);
                 return 127;
             }

             pids[spawned++] = pid;
             prev_read = (i < count - 1) ? pipefd[0] : STDIN_FILENO;
         }

         int status = 0;
         gboolean wait_failed = FALSE;
         for (int i = 0; i < spawned; ++i) {
             pid_t waited = mpcsh_waitpid_retry(pids[i], &status);
             if (waited != pids[i]) {
                 fprintf(stderr, "mpcsh: waitpid(%ld): %s\\n", (long)pids[i],
                         waited < 0 ? strerror(errno) : "unexpected child result");
                 wait_failed = TRUE;
             }
         }

         int result = wait_failed ? 127 : mpcsh_child_result(status);
         g_free(work);
         mpcsh_last_status = result;
         return result;
     }


     /* ------------------------------------------------------------
      * Fish-style command-list dispatcher
      *
      * mpcsh uses Fish words:
      *   cmd1 ; cmd2
      *   cmd1 and cmd2
      *   cmd1 or cmd2
      *   not cmd
      *
      * && and || are deliberately not grammar.
      * ------------------------------------------------------------ */
     static int
     mpc_simple(char *line)
     {
         if (!line || !*line) return 0;

         GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
         GString *part = g_string_new(NULL);
         char quote = 0;
         int paren = 0;

         for (char *p = line; ; ++p) {
             char c = *p;
             if (quote) {
                 g_string_append_c(part, c);
                 if (c == quote && (p == line || p[-1] != '\\'))
                     quote = 0;
             } else if (c == '\'' || c == '"') {
                 quote = c;
                 g_string_append_c(part, c);
             } else if (c == '(') {
                 ++paren;
                 g_string_append_c(part, c);
             } else if (c == ')' && paren > 0) {
                 --paren;
                 g_string_append_c(part, c);
             } else if (((c == ';' || c == '\n') && paren == 0) || c == '\0') {
                 char *x = g_strdup(part->str);
                 g_strstrip(x);
                 if (*x) g_ptr_array_add(parts, x);
                 else g_free(x);
                 g_string_truncate(part, 0);
                 if (c == '\0') break;
             } else {
                 g_string_append_c(part, c);
             }
         }
         g_string_free(part, TRUE);

         int last = 0;

         for (guint i = 0; i < parts->len; ++i) {
             char *cmd = g_ptr_array_index(parts, i);
             gboolean negate = FALSE;

             if (!strncasecmp(cmd, "not ", 4)) {
                 negate = TRUE;
                 memmove(cmd, cmd + 4, strlen(cmd + 4) + 1);
                 g_strstrip(cmd);
             }

             /*
              * Only recognize Fish's command-list words when they occur at
              * top-level command boundaries. This keeps "echo and" ordinary
              * arguments when no operator follows a complete left command.
              */
             char *andp = strstr(cmd, " and ");
             char *orp  = strstr(cmd, " or ");

             if (andp && (!orp || andp < orp)) {
                 *andp = '\0';
                 char *rhs = andp + 5;
                 g_strstrip(cmd);
                 g_strstrip(rhs);

                 last = mpcsh_run_pipeline(cmd);
                 if (negate) last = !last;
                 if (last == 0 && *rhs)
                     last = mpcsh_run_pipeline(rhs);
             } else if (orp) {
                 *orp = '\0';
                 char *rhs = orp + 4;
                 g_strstrip(cmd);
                 g_strstrip(rhs);

                 last = mpcsh_run_pipeline(cmd);
                 if (negate) last = !last;
                 if (last != 0 && *rhs)
                     last = mpcsh_run_pipeline(rhs);
             } else {
                 last = mpcsh_run_pipeline(cmd);
                 if (negate) last = !last;
             }

             mpcsh_last_status = last;
         }

         g_ptr_array_free(parts, TRUE);
         return last;
     }


     /* Native scripting blocks. Parse structure before running commands. */
     typedef struct { char *text; GPtrArray *body, *alt; } MpcBlock;
     static GHashTable *mpc_funcs;
     static int mpc_flow, mpc_ret, mpc_calls, mpc_loops;
     static gboolean mpc_check;
     static gboolean mpc_kw(const char *s,const char *k) {
         size_t n=strlen(k); return !strncmp(s,k,n)&&(!s[n]||isspace((unsigned char)s[n]));
     }
     static void mpc_free(gpointer p) {
         MpcBlock *n=p; g_free(n->text); if(n->body)g_ptr_array_free(n->body,TRUE);
         if(n->alt)g_ptr_array_free(n->alt,TRUE);
         g_free(n);
     }
     static GPtrArray *mpc_split(const char *s,gboolean *ok) {
         GPtrArray *a=g_ptr_array_new_with_free_func(g_free); GString *b=g_string_new(NULL);
         char q=0; int depth=0; gboolean esc=FALSE;
         for(const char *p=s;;p++) {
             char c=*p;
             if(!c) { if(q||depth||esc)*ok=FALSE; }
             if(c&&esc){g_string_append_c(b,c);esc=FALSE;continue;}
             if(c=='\\'&&q!='\''){g_string_append_c(b,c);esc=TRUE;continue;}
             if(c&&q){g_string_append_c(b,c);if(c==q)q=0;continue;}
             if(c=='\''||c=='"'){q=c;g_string_append_c(b,c);continue;}
             if(c=='(')depth++;
             if(c==')'){if(depth)depth--;else *ok=FALSE;}
             if(!c||((c==';'||c=='\n')&&!depth)) {
                 char *t=g_strdup(b->str);g_strstrip(t);
                 if(*t&&*t!='#')g_ptr_array_add(a,t);else g_free(t);
                 g_string_truncate(b,0);if(!c)break;
             }else g_string_append_c(b,c);
         }
         g_string_free(b,TRUE);return a;
     }
     static gboolean mpc_name(const char *s) {
         if(!(isalpha((unsigned char)*s)||*s=='_'))return FALSE;
         for(s++;*s;s++)if(!mpcsh_identifier_char(*s))return FALSE;
         return TRUE;
     }
     static GPtrArray *mpc_parse(GPtrArray *t,guint *i,int depth,gboolean *ok) {
         GPtrArray *a=g_ptr_array_new_with_free_func(mpc_free);
         if(depth>64){*ok=FALSE;return a;}
         while(*i<t->len&&*ok) {
             const char *s=g_ptr_array_index(t,*i);
             if(mpc_kw(s,"else")||mpc_kw(s,"end")){if(!depth)*ok=FALSE;break;}
             MpcBlock *n=g_new0(MpcBlock,1);n->text=g_strdup(s);g_ptr_array_add(a,n);++*i;
             if(mpc_kw(s,"if")||mpc_kw(s,"for")||mpc_kw(s,"function")||mpc_kw(s,"while")) {
                 char *arg=g_strdup(s+(mpc_kw(s,"function")?8:mpc_kw(s,"while")?5:mpc_kw(s,"for")?3:2));g_strstrip(arg);
                 if(!*arg)*ok=FALSE;
                 if(mpc_kw(s,"function")&&!mpc_name(arg))*ok=FALSE;
                 if(mpc_kw(s,"for")) {
                     char *p=arg;while(mpcsh_identifier_char(*p))p++;
                     if(!isspace((unsigned char)*p))*ok=FALSE;
                     if(*p){*p++=0;if(!mpc_name(arg))*ok=FALSE;while(isspace((unsigned char)*p))p++;
                         if(strncmp(p,"in",2)||(p[2]&&!isspace((unsigned char)p[2])))*ok=FALSE;}
                 }
                 g_free(arg);n->body=mpc_parse(t,i,depth+1,ok);
                 if(mpc_kw(s,"if")) {   /* else / else if chain, closed by one shared "end" */
                     MpcBlock *cur=n;
                     while(*ok&&cur&&*i<t->len) {
                         const char *tok=g_ptr_array_index(t,*i);
                         if(!strcmp(tok,"else")){++*i;cur->alt=mpc_parse(t,i,depth+1,ok);break;}
                         if(!strncmp(tok,"else if",7)&&isspace((unsigned char)tok[7])) {
                             char *cond=g_strdup(tok+7);g_strstrip(cond);if(!*cond)*ok=FALSE;
                             MpcBlock *m=g_new0(MpcBlock,1);m->text=g_strdup_printf("if %s",cond);g_free(cond);
                             cur->alt=g_ptr_array_new_with_free_func(mpc_free);g_ptr_array_add(cur->alt,m);
                             ++*i;m->body=mpc_parse(t,i,depth+1,ok);cur=m;continue;
                         }
                         break;
                     }
                 }
                 if(*i>=t->len||strcmp(g_ptr_array_index(t,*i),"end")){*ok=FALSE;break;}
                 ++*i;
             }
         }
         return a;
     }
     static void mpc_save(GPtrArray *a,GString *s) {
         for(guint i=0;i<a->len;i++) {
             MpcBlock *n=g_ptr_array_index(a,i);g_string_append_printf(s,"%s\n",n->text);
             if(n->body){mpc_save(n->body,s);if(n->alt){g_string_append(s,"else\n");mpc_save(n->alt,s);}g_string_append(s,"end\n");}
         }
     }
     static int mpc_function_call(char **v) {
         const char *body=mpc_funcs?g_hash_table_lookup(mpc_funcs,v[0]):NULL;
         if(!body)return -1;
         if(mpc_calls>=64){fprintf(stderr,"mpcsh: function recursion limit\n");return 2;}
         char *copy=g_strdup(body);mpcsh_vars_init();
         int count=0;while(v[count+1])count++;
         char names[257][16];char *old[257];
         strcpy(names[0],"argc");strcpy(names[1],"argv");
         for(int j=0;j<count;j++)snprintf(names[j+2],16,"arg%d",j+1);
         for(int j=0;j<count+2;j++)old[j]=g_strdup(g_hash_table_lookup(mpcsh_vars,names[j]));
         char num[32];snprintf(num,sizeof(num),"%d",count);mpcsh_var_set_local("argc",num);
         GString *joined=g_string_new(NULL);
         for(int j=1;v[j];j++){if(j>1)g_string_append_c(joined,' ');g_string_append(joined,v[j]);mpcsh_var_set_local(names[j+1],v[j]);}
         mpcsh_var_set_local("argv",joined->str);g_string_free(joined,TRUE);
         int saved_loop=mpc_loops;mpc_loops=0;mpc_calls++;
         int rc=mpcsh_run_line(copy);mpc_calls--;mpc_loops=saved_loop;
         if(mpc_flow==3){rc=mpc_ret;mpc_flow=0;}
         for(int j=0;j<count+2;j++){if(old[j])mpcsh_var_set_local(names[j],old[j]);else g_hash_table_remove(mpcsh_vars,names[j]);g_free(old[j]);}
         g_free(copy);return rc;
     }
     static int mpc_exec(GPtrArray *a) {
         int rc=0;
         for(guint i=0;i<a->len&&!mpc_flow&&!got_sigint;i++) {
             MpcBlock *n=g_ptr_array_index(a,i);char *s=n->text;
             if(mpc_kw(s,"function")) {
                 if(!mpc_funcs)mpc_funcs=g_hash_table_new_full(g_str_hash,g_str_equal,g_free,g_free);
                 char *name=g_strdup(s+8);g_strstrip(name);GString *b=g_string_new(NULL);mpc_save(n->body,b);
                 g_hash_table_replace(mpc_funcs,name,g_string_free(b,FALSE));rc=0;
             }else if(mpc_kw(s,"if")) {
                 char *c=g_strdup(s+2);g_strstrip(c);rc=mpc_simple(c);g_free(c);
                 rc=!rc?mpc_exec(n->body):n->alt?mpc_exec(n->alt):0;
             }else if(mpc_kw(s,"for")) {
                 char *v[256]={0},*c=g_strdup(s);int argc=mpcsh_word_split(c,v,256);g_free(c);
                 if(argc<3||strcmp(v[2],"in")||!mpc_name(v[1])){mpcsh_free_argv(v);return 2;}
                 char *old=g_strdup(g_hash_table_lookup(mpcsh_vars,v[1]));mpc_loops++;rc=0;
                 for(int j=3;j<argc&&!got_sigint;j++) {
                     mpcsh_var_set_local(v[1],v[j]);rc=mpc_exec(n->body);
                     if(mpc_flow==1){mpc_flow=0;break;}if(mpc_flow==2)mpc_flow=0;if(mpc_flow==3)break;
                 }
                 mpc_loops--;if(old)mpcsh_var_set_local(v[1],old);else g_hash_table_remove(mpcsh_vars,v[1]);
                 g_free(old);mpcsh_free_argv(v);
             }else if(mpc_kw(s,"while")) {
                 char *c0=g_strdup(s+5);g_strstrip(c0);mpc_loops++;rc=0;
                 while(!got_sigint) {
                     char *c=g_strdup(c0);int cr=mpc_simple(c);g_free(c);
                     if(cr)break;
                     rc=mpc_exec(n->body);
                     if(mpc_flow==1){mpc_flow=0;break;}if(mpc_flow==2)mpc_flow=0;if(mpc_flow==3)break;
                 }
                 mpc_loops--;g_free(c0);
             }else if(!strcmp(s,"break")||!strcmp(s,"continue")) {
                 if(!mpc_loops){fprintf(stderr,"mpcsh: loop control outside loop\n");return 2;}
                 mpc_flow=!strcmp(s,"break")?1:2;
             }else if(mpc_kw(s,"return")) {
                 if(!mpc_calls){fprintf(stderr,"mpcsh: return outside function\n");return 2;}
                 char *v[256]={0},*c=g_strdup(s);int argc=mpcsh_word_split(c,v,256);g_free(c);
                 char *end=NULL;long code=argc==1?mpcsh_last_status:argc==2?strtol(v[1],&end,10):-1;
                 if(argc<1||argc>2||code<0||code>255||(argc==2&&(!*v[1]||*end))){mpcsh_free_argv(v);return 2;}
                 mpcsh_free_argv(v);mpc_flow=3;rc=mpc_ret=(int)code;
             }else {char *c=g_strdup(s);rc=mpc_simple(c);g_free(c);}
             mpcsh_last_status=rc;
         }
         return got_sigint?130:rc;
     }
     static int mpcsh_run_line(char *line) {
         gboolean ok=TRUE;GPtrArray *t=mpc_split(line?line:"",&ok);guint i=0;
         GPtrArray *a=mpc_parse(t,&i,0,&ok);
         if(!ok||i!=t->len)fprintf(stderr,"mpcsh: syntax error near statement %u (quotes or block structure)\n",i+1);
         int rc=(!ok||i!=t->len)?2:mpc_check?0:mpc_exec(a);
         g_ptr_array_free(a,TRUE);g_ptr_array_free(t,TRUE);mpcsh_last_status=rc;return rc;
     }

     /* ====================================================================
      * 1.8: script arguments, math, source, Chihuahuas (.chu), mpc-chuctrl
      * ==================================================================== */

     static void
     mpcsh_script_set_args(const char *script, int n, char **args)
     {
         mpcsh_vars_init();
         char num[32];
         snprintf(num, sizeof num, "%d", n);
         mpcsh_var_set_local("argc", num);
         GString *j = g_string_new(NULL);
         for (int i = 0; i < n; ++i) {
             char name[32];
             snprintf(name, sizeof name, "arg%d", i + 1);
             mpcsh_var_set_local(name, args[i]);
             if (i) g_string_append_c(j, ' ');
             g_string_append(j, args[i]);
         }
         mpcsh_var_set_local("argv", j->str);
         g_string_free(j, TRUE);
         mpcsh_var_set_local("script", script);
     }

     /* ---------------------------------------------------------------- math */

     typedef struct { const char *p; int err; } MpcMath;
     static double mpc_math_expr(MpcMath *m);
     static void mpc_math_ws(MpcMath *m) { while (isspace((unsigned char)*m->p)) ++m->p; }

     static double mpc_math_atom(MpcMath *m)
     {
         mpc_math_ws(m);
         if (*m->p == '(') {
             ++m->p;
             double v = mpc_math_expr(m);
             mpc_math_ws(m);
             if (*m->p == ')') ++m->p; else m->err = 1;
             return v;
         }
         char *end = NULL;
         double v = strtod(m->p, &end);
         if (end == m->p) { m->err = 1; return 0; }
         m->p = end;
         return v;
     }

     static double mpc_math_unary(MpcMath *m);
     static double mpc_math_pow(MpcMath *m)
     {
         double b = mpc_math_atom(m);
         mpc_math_ws(m);
         if (*m->p == '^') { ++m->p; double x = mpc_math_unary(m); return pow(b, x); }
         return b;
     }

     static double mpc_math_unary(MpcMath *m)
     {
         mpc_math_ws(m);
         if (*m->p == '-') { ++m->p; return -mpc_math_unary(m); }
         if (*m->p == '+') { ++m->p; return mpc_math_unary(m); }
         return mpc_math_pow(m);
     }

     static double mpc_math_term(MpcMath *m)
     {
         double v = mpc_math_unary(m);
         for (;;) {
             mpc_math_ws(m);
             char c = *m->p;
             if (c != '*' && c != '/' && c != '%') break;
             ++m->p;
             double r = mpc_math_unary(m);
             if (c == '*') v *= r;
             else if (r == 0) { m->err = 2; return 0; }
             else if (c == '/') v /= r;
             else v = fmod(v, r);
         }
         return v;
     }

     static double mpc_math_expr(MpcMath *m)
     {
         double v = mpc_math_term(m);
         for (;;) {
             mpc_math_ws(m);
             char c = *m->p;
             if (c != '+' && c != '-') break;
             ++m->p;
             double r = mpc_math_term(m);
             v = (c == '+') ? v + r : v - r;
         }
         return v;
     }

     static int mpcsh_math_builtin(char **argv)
     {
         int i = 1, scale = -1;
         while (argv[i] && argv[i][0] == '-' && argv[i][1] == 's') {
             const char *v = argv[i] + 2;
             if (!*v) { ++i; v = argv[i]; }
             if (!v || !isdigit((unsigned char)*v)) {
                 fprintf(stderr, "mpcsh: math: -s needs a number\n");
                 return 2;
             }
             scale = atoi(v);
             if (scale > 15) scale = 15;
             ++i;
         }
         if (!argv[i]) {
             fprintf(stderr, "Usage: math [-sN] EXPRESSION     (+ - * / %% ^ and parentheses)\n");
             return 2;
         }
         GString *e = g_string_new(NULL);
         for (; argv[i]; ++i) {
             if (e->len) g_string_append_c(e, ' ');
             g_string_append(e, argv[i]);
         }
         MpcMath m = { e->str, 0 };
         double v = mpc_math_expr(&m);
         mpc_math_ws(&m);
         if (*m.p) m.err = m.err ? m.err : 1;
         g_string_free(e, TRUE);
         if (m.err == 2) { fprintf(stderr, "mpcsh: math: division by zero\n"); return 1; }
         if (m.err)      { fprintf(stderr, "mpcsh: math: invalid expression\n"); return 2; }
         if (scale >= 0) printf("%.*f\n", scale, v);
         else if (v == floor(v) && fabs(v) < 9e15) printf("%lld\n", (long long)v);
         else printf("%.10g\n", v);
         return 0;
     }

     /* -------------------------------------------------------------- source */

     static int mpc_source_counter = 0;

     static int mpcsh_source_builtin(char **argv)
     {
         if (!argv[1]) { fprintf(stderr, "Usage: source FILE [ARGS...]\n"); return 2; }
         char *text = NULL;
         if (!g_file_get_contents(argv[1], &text, NULL, NULL)) {
             fprintf(stderr, "mpcsh: source: cannot read %s\n", argv[1]);
             return 1;
         }
         if (!mpc_funcs)
             mpc_funcs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
         char fname[48];
         snprintf(fname, sizeof fname, "source:%d", ++mpc_source_counter);
         g_hash_table_replace(mpc_funcs, g_strdup(fname), text);   /* the table owns text now */
         char *v[260] = { 0 };
         int n = 0;
         v[n++] = fname;
         for (int i = 2; argv[i] && n < 256; ++i) v[n++] = argv[i];
         v[n] = NULL;
         int rc = mpc_function_call(v);
         g_hash_table_remove(mpc_funcs, fname);
         return rc < 0 ? 1 : rc;
     }

     /* --------------------------------------------------------- Chihuahuas */

     static const char *const mpcsh_chu_header_text =
     "/* mpcchu.h - helper API for C Chihuahuas. Written by mpc-chuctrl; do not edit.\n"
     " * API version 1. Needs MPCECC 1.9+ for the mpcchu_ecc_* / show / launch calls. */\n"
     "#ifndef MPCCHU_H\n"
     "#define MPCCHU_H\n"
     "#define _GNU_SOURCE\n"
     "#include <stdio.h>\n"
     "#include <stdlib.h>\n"
     "#include <string.h>\n"
     "#include <unistd.h>\n"
     "#include <errno.h>\n"
     "#include <fcntl.h>\n"
     "#include <sys/socket.h>\n"
     "#include <sys/un.h>\n"
     "#include <sys/stat.h>\n"
     "\n"
     "#define MPCCHU_API_VERSION 1\n"
     "\n"
     "static inline const char *mpcchu_name(void)     { const char *s = getenv(\"MPCSH_CHU\"); return s ? s : \"\"; }\n"
     "static inline const char *mpcchu_data_dir(void) { return getenv(\"MPCSH_CHU_DATA\"); }\n"
     "static inline int mpcchu_in_ecc(void)           { return getenv(\"MPCECC\") != NULL; }\n"
     "\n"
     "/* Send one request line to the MPCECC control socket. The reply text is\n"
     " * copied to reply (if not NULL). Returns 0 when the reply starts with OK. */\n"
     "static inline int mpcchu_ecc_request(const char *req, char *reply, size_t rsz)\n"
     "{\n"
     "    char line[65536], buf[8192];\n"
     "    if (reply && rsz) reply[0] = 0;\n"
     "    struct sockaddr_un sa;\n"
     "    memset(&sa, 0, sizeof sa);\n"
     "    sa.sun_family = AF_UNIX;\n"
     "    const char *rt = getenv(\"XDG_RUNTIME_DIR\");\n"
     "    if (rt && *rt) snprintf(sa.sun_path, sizeof sa.sun_path, \"%s/mpcecc-control.sock\", rt);\n"
     "    else snprintf(sa.sun_path, sizeof sa.sun_path, \"/tmp/mpcecc-control-%d.sock\", (int)getuid());\n"
     "    int fd = socket(AF_UNIX, SOCK_STREAM, 0);\n"
     "    if (fd < 0) return -1;\n"
     "    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {\n"
     "        if (reply && rsz) snprintf(reply, rsz, \"ERR cannot connect to MPCECC\");\n"
     "        close(fd);\n"
     "        return -1;\n"
     "    }\n"
     "    const char *tab = getenv(\"MPCECC_TAB\");\n"
     "    int n = (tab && *tab) ? snprintf(line, sizeof line, \"@%s %s\\n\", tab, req)\n"
     "                          : snprintf(line, sizeof line, \"%s\\n\", req);\n"
     "    if (n <= 0 || (size_t)n >= sizeof line) {\n"
     "        if (reply && rsz) snprintf(reply, rsz, \"ERR request too long\");\n"
     "        close(fd);\n"
     "        return -1;\n"
     "    }\n"
     "    for (int off = 0; off < n;) {\n"
     "        ssize_t w = write(fd, line + off, (size_t)(n - off));\n"
     "        if (w <= 0) { close(fd); return -1; }\n"
     "        off += (int)w;\n"
     "    }\n"
     "    size_t got = 0;\n"
     "    for (;;) {\n"
     "        ssize_t r = read(fd, buf + got, sizeof buf - 1 - got);\n"
     "        if (r <= 0) break;\n"
     "        got += (size_t)r;\n"
     "        if (got >= sizeof buf - 1) break;\n"
     "    }\n"
     "    buf[got] = 0;\n"
     "    close(fd);\n"
     "    if (reply && rsz) snprintf(reply, rsz, \"%s\", buf);\n"
     "    return strncmp(buf, \"OK\", 2) == 0 ? 0 : -1;\n"
     "}\n"
     "\n"
     "/* Show an image file (any format MPCECC can load) as a floating overlay. */\n"
     "static inline int mpcchu_show_image(const char *file)\n"
     "{\n"
     "    const char *td = getenv(\"TMPDIR\");\n"
     "    char tmp[4096], req[4200], reply[256];\n"
     "    if (!td || !*td) td = \"/tmp\";\n"
     "    snprintf(tmp, sizeof tmp, \"%s/mpcimgcat-XXXXXX.png\", td);   /* the name MPCECC deletes */\n"
     "    int out = mkstemps(tmp, 4);\n"
     "    if (out < 0) return -1;\n"
     "    int in = open(file, O_RDONLY);\n"
     "    if (in < 0) { close(out); unlink(tmp); return -1; }\n"
     "    char chunk[8192];\n"
     "    ssize_t r;\n"
     "    while ((r = read(in, chunk, sizeof chunk)) > 0)\n"
     "        if (write(out, chunk, (size_t)r) != r) { r = -1; break; }\n"
     "    close(in);\n"
     "    close(out);\n"
     "    if (r < 0) { unlink(tmp); return -1; }\n"
     "    snprintf(req, sizeof req, \"MPSP %s\", tmp);\n"
     "    int rc = mpcchu_ecc_request(req, reply, sizeof reply);\n"
     "    if (rc != 0) unlink(tmp);\n"
     "    return rc;\n"
     "}\n"
     "\n"
     "/* Start a PixelWindow. cmd is one command line. *wlid gets its id (may be NULL). */\n"
     "static inline int mpcchu_launch(int w, int h, int fps, const char *cmd, int *wlid)\n"
     "{\n"
     "    char req[8192], reply[256];\n"
     "    snprintf(req, sizeof req, \"LAUNCH %d %d %d -- %s\", w, h, fps, cmd);\n"
     "    int rc = mpcchu_ecc_request(req, reply, sizeof reply);\n"
     "    if (rc == 0 && wlid) *wlid = atoi(reply + 2);\n"
     "    return rc;\n"
     "}\n"
     "\n"
     "static inline int mpcchu_terminate(int wlid)\n"
     "{\n"
     "    char req[64], reply[128];\n"
     "    snprintf(req, sizeof req, \"TERMINATE %d\", wlid);\n"
     "    return mpcchu_ecc_request(req, reply, sizeof reply);\n"
     "}\n"
     "\n"
     "static inline int mpcchu_list_windows(char *buf, size_t size)\n"
     "{\n"
     "    mpcchu_ecc_request(\"LIST\", buf, size);       /* the reply is one line per window */\n"
     "    return (buf && strncmp(buf, \"ERR\", 3) == 0) ? -1 : 0;\n"
     "}\n"
     "\n"
     "#endif\n";

     static gint mpcsh_chu_strcmp(gconstpointer a, gconstpointer b)
     { return strcmp(*(char *const *)a, *(char *const *)b); }


     typedef struct {
         char *name, *version, *author, *description, *lang, *run, *cflags, *libs, *needs;
         char *path, *body;
         GPtrArray *commands;
     } MpcChu;

     typedef struct { char *exe; char *chu; } MpcChuCmd;

     static GHashTable *mpcsh_chu_cmds = NULL;     /* command -> MpcChuCmd */
     static GPtrArray  *mpcsh_chu_loaded = NULL;   /* MpcChu * active in this session */

     static const char *const mpcsh_chu_reserved[] = {
         "cd", "pwd", "status", "set", "exit", "quit", "ctk", "portmap", "guiinterm",
         "windowlaunch", "windowterminate", "wlidlist", "windowlist", "windowpicker",
         "extension", "mpcextinstl", "wallpaper", "ls", "mpcimgcat", "colorit", "help",
         "limits", "mpcupdate", "mpcbits", "mpcversion", "time", "mpcshman", "math",
         "source", ".", "mpc-chuctrl", "chuctrl", "function", "if", "for", "while",
         "end", "else", "break", "continue", "return", "not", "and", "or", NULL
     };

     static void mpcsh_chu_free(MpcChu *c)
     {
         if (!c) return;
         g_free(c->name); g_free(c->version); g_free(c->author); g_free(c->description);
         g_free(c->lang); g_free(c->run); g_free(c->cflags); g_free(c->libs); g_free(c->needs);
         g_free(c->path); g_free(c->body);
         if (c->commands) g_ptr_array_free(c->commands, TRUE);
         g_free(c);
     }

     static void mpcsh_chu_cmd_free(gpointer p)
     {
         MpcChuCmd *m = p;
         g_free(m->exe); g_free(m->chu); g_free(m);
     }

     static char *mpcsh_chu_dir(void)     { return g_build_filename(g_get_user_data_dir(), "mpcsh", "chu", NULL); }
     static char *mpcsh_chu_bindir(void)  { return g_build_filename(g_get_user_data_dir(), "mpcsh", "chu", "bin", NULL); }
     static char *mpcsh_chu_incdir(void)  { return g_build_filename(g_get_user_data_dir(), "mpcsh", "chu", "include", NULL); }
     static char *mpcsh_chu_datadir(const char *name)
     {
         return g_build_filename(g_get_user_data_dir(), "mpcsh", "chu-data", name, NULL);
     }
     static char *mpcsh_chu_pathfile(void)
     {
         return g_build_filename(g_get_user_config_dir(), "mpcsh", "chu.path", NULL);
     }

     static gboolean mpcsh_chu_valid_name(const char *s, gboolean command)
     {
         size_t n = strlen(s);
         if (!n || n > (command ? 64 : 32)) return FALSE;
         for (size_t i = 0; i < n; ++i) {
             unsigned char c = (unsigned char)s[i];
             gboolean ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
             if (command) ok = ok || (c >= 'A' && c <= 'Z') || c == '.';
             if (!ok) return FALSE;
             if (i == 0 && (c == '-' || c == '.')) return FALSE;
         }
         return TRUE;
     }

     static gboolean mpcsh_chu_is_reserved(const char *cmd)
     {
         for (int i = 0; mpcsh_chu_reserved[i]; ++i)
             if (!strcmp(cmd, mpcsh_chu_reserved[i])) return TRUE;
             return FALSE;
     }

     static void mpcsh_chu_set(char **slot, const char *v)
     {
         g_free(*slot);
         *slot = g_strdup(v);
     }

     /* Parse the text of a .chu file. Returns NULL and sets *err on failure. */
     static MpcChu *mpcsh_chu_parse(const char *text, const char *path, char **err)
     {
         *err = NULL;
         if (strncmp(text, "#!chu", 5) != 0) {
             *err = g_strdup("the first line must be '#!chu 1'");
             return NULL;
         }
         char **lines = g_strsplit(text, "\n", -1);
         {
             char *first = g_strstrip(g_strdup(lines[0] + 5));
             if (*first && atoi(first) != 1) {
                 *err = g_strdup_printf("unsupported .chu version '%s' (this mpcsh reads version 1)", first);
                 g_free(first); g_strfreev(lines);
                 return NULL;
             }
             g_free(first);
         }
         MpcChu *c = g_new0(MpcChu, 1);
         c->commands = g_ptr_array_new_with_free_func(g_free);
         c->path = g_strdup(path ? path : "");
         int i = 1;
         gboolean sep = FALSE;
         for (; lines[i]; ++i) {
             char *l = g_strdup(lines[i]);
             char *t = g_strstrip(l);
             if (!strcmp(t, "---")) { sep = TRUE; g_free(l); ++i; break; }
             if (!*t || *t == '#') { g_free(l); continue; }
             char *colon = strchr(t, ':');
             if (!colon) {
                 *err = g_strdup_printf("line %d: expected 'key: value' or '---'", i + 1);
                 g_free(l); break;
             }
             *colon = 0;
             char *key = g_strstrip(t);
             char *val = g_strstrip(colon + 1);
             if (!g_ascii_strcasecmp(key, "name")) mpcsh_chu_set(&c->name, val);
             else if (!g_ascii_strcasecmp(key, "version")) mpcsh_chu_set(&c->version, val);
             else if (!g_ascii_strcasecmp(key, "author")) mpcsh_chu_set(&c->author, val);
             else if (!g_ascii_strcasecmp(key, "description")) mpcsh_chu_set(&c->description, val);
             else if (!g_ascii_strcasecmp(key, "lang")) mpcsh_chu_set(&c->lang, val);
             else if (!g_ascii_strcasecmp(key, "run")) mpcsh_chu_set(&c->run, val);
             else if (!g_ascii_strcasecmp(key, "cflags")) mpcsh_chu_set(&c->cflags, val);
             else if (!g_ascii_strcasecmp(key, "libs")) mpcsh_chu_set(&c->libs, val);
             else if (!g_ascii_strcasecmp(key, "needs")) mpcsh_chu_set(&c->needs, val);
             else if (!g_ascii_strcasecmp(key, "commands")) {
                 char **parts = g_strsplit_set(val, ", \t", -1);
                 for (int k = 0; parts[k]; ++k)
                     if (*parts[k]) g_ptr_array_add(c->commands, g_strdup(parts[k]));
                     g_strfreev(parts);
             }
             g_free(l);
         }
         if (!*err && !sep) *err = g_strdup("missing the '---' line that starts the body");
         if (!*err) {
             GString *b = g_string_new(NULL);
             for (; lines[i]; ++i) { g_string_append(b, lines[i]); if (lines[i + 1]) g_string_append_c(b, '\n'); }
             c->body = g_string_free(b, FALSE);
             char *chk = g_strstrip(g_strdup(c->body));
             if (!*chk) *err = g_strdup("the body is empty");
             g_free(chk);
         }
         g_strfreev(lines);
         if (!*err) {
             if (!c->name) *err = g_strdup("missing 'name:'");
             else if (!mpcsh_chu_valid_name(c->name, FALSE))
                 *err = g_strdup_printf("bad name '%s' (use lower case letters, digits, - and _, max 32)", c->name);
         }
         if (!*err) {
             if (!c->lang) c->lang = g_strdup("mpcsh");
             if (!c->version) c->version = g_strdup("0");
             const char *langs[] = { "mpcsh", "c", "sh", "python", "exec", NULL };
             gboolean ok = FALSE;
             for (int k = 0; langs[k]; ++k) if (!strcmp(c->lang, langs[k])) ok = TRUE;
             if (!ok) *err = g_strdup_printf("unknown lang '%s' (mpcsh, c, sh, python or exec)", c->lang);
         }
         if (!*err && !strcmp(c->lang, "exec") && (!c->run || !*c->run))
             *err = g_strdup("lang exec needs a 'run:' line, for example  run: /usr/bin/env lua");
         if (!*err) {
             if (!c->commands->len) g_ptr_array_add(c->commands, g_strdup(c->name));
             for (guint k = 0; k < c->commands->len && !*err; ++k) {
                 const char *cmd = g_ptr_array_index(c->commands, k);
                 if (!mpcsh_chu_valid_name(cmd, TRUE)) *err = g_strdup_printf("bad command name '%s'", cmd);
                 else if (mpcsh_chu_is_reserved(cmd))
                     *err = g_strdup_printf("'%s' is a built-in and cannot be used as a command name", cmd);
             }
         }
         if (*err) { mpcsh_chu_free(c); return NULL; }
         return c;
     }

     static gboolean mpcsh_chu_write_header(void)
     {
         char *inc = mpcsh_chu_incdir();
         g_mkdir_with_parents(inc, 0755);
         char *h = g_build_filename(inc, "mpcchu.h", NULL);
         gboolean ok = g_file_set_contents(h, mpcsh_chu_header_text, -1, NULL);
         g_free(h); g_free(inc);
         return ok;
     }

     /* Build the program for a non-mpcsh Chihuahua into 'out'. */
     static gboolean mpcsh_chu_build(const MpcChu *c, const char *out, char **msg)
     {
         *msg = NULL;
         if (!strcmp(c->lang, "mpcsh")) return TRUE;
         char *bin = mpcsh_chu_bindir();
         g_mkdir_with_parents(bin, 0755);
         g_free(bin);
         if (!strcmp(c->lang, "c")) {
             char *src = NULL;
             int fd = g_file_open_tmp("mpcchu-XXXXXX.c", &src, NULL);
             if (fd < 0) { *msg = g_strdup("cannot create a temporary file"); return FALSE; }
             close(fd);
             if (!g_file_set_contents(src, c->body, -1, NULL)) {
                 *msg = g_strdup("cannot write the temporary source file");
                 g_free(src); return FALSE;
             }
             mpcsh_chu_write_header();
             char *inc = mpcsh_chu_incdir();
             GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
             const char *cc = g_getenv("CC");
             g_ptr_array_add(a, g_strdup(cc && *cc ? cc : "gcc"));
             g_ptr_array_add(a, g_strdup("-O2"));
             g_ptr_array_add(a, g_strdup("-std=gnu11"));
             g_ptr_array_add(a, g_strdup_printf("-I%s", inc));
             g_ptr_array_add(a, g_strdup(src));
             g_ptr_array_add(a, g_strdup("-o"));
             g_ptr_array_add(a, g_strdup(out));
             const char *extra[2] = { c->cflags, c->libs };
             for (int k = 0; k < 2; ++k) {
                 if (!extra[k] || !*extra[k]) continue;
                 gchar **tok = NULL;
                 if (g_shell_parse_argv(extra[k], NULL, &tok, NULL)) {
                     for (int t = 0; tok[t]; ++t) g_ptr_array_add(a, g_strdup(tok[t]));
                     g_strfreev(tok);
                 }
             }
             g_ptr_array_add(a, NULL);
             gchar *errout = NULL;
             gint status = 0;
             GError *e = NULL;
             gboolean ok = g_spawn_sync(NULL, (char **)a->pdata, NULL, G_SPAWN_SEARCH_PATH,
                                        NULL, NULL, NULL, &errout, &status, &e);
             if (!ok) {
                 *msg = g_strdup_printf("cannot run the C compiler: %s", e ? e->message : "?");
                 g_clear_error(&e);
             } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                 *msg = g_strdup_printf("the C compiler failed:\n%s", errout ? errout : "");
                 ok = FALSE;
             }
             unlink(src);
             g_free(src); g_free(inc); g_free(errout);
             g_ptr_array_free(a, TRUE);
             return ok;
         }
         const char *shebang;
         char *own = NULL;
         if (!strcmp(c->lang, "sh")) shebang = "#!/bin/sh";
         else if (!strcmp(c->lang, "python")) shebang = "#!/usr/bin/env python3";
         else { own = g_strdup_printf("#!%s", c->run); shebang = own; }
         char *content = g_str_has_prefix(c->body, "#!")
         ? g_strdup(c->body)
         : g_strdup_printf("%s\n%s", shebang, c->body);
         GError *e = NULL;
         gboolean ok = g_file_set_contents(out, content, -1, &e);
         if (ok) chmod(out, 0755);
         else { *msg = g_strdup_printf("cannot write %s: %s", out, e ? e->message : "?"); g_clear_error(&e); }
         g_free(content); g_free(own);
         return ok;
     }

     /* Register commands (and define functions for mpcsh Chihuahuas). Takes ownership. */
     static void mpcsh_chu_activate(MpcChu *c)
     {
         if (!mpcsh_chu_cmds)
             mpcsh_chu_cmds = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, mpcsh_chu_cmd_free);
         if (!mpcsh_chu_loaded) mpcsh_chu_loaded = g_ptr_array_new();
         if (!strcmp(c->lang, "mpcsh")) {
             int saved = mpcsh_last_status;
             gboolean ok = TRUE;
             GPtrArray *t = mpc_split(c->body, &ok);
             guint i = 0;
             GPtrArray *a = mpc_parse(t, &i, 0, &ok);
             if (!ok || i != t->len) {
                 fprintf(stderr, "mpcsh: chihuahua %s: syntax error in the body\n", c->name);
             } else {
                 gboolean only_fn = TRUE;
                 for (guint k = 0; k < a->len; ++k)
                     if (!mpc_kw(((MpcBlock *)g_ptr_array_index(a, k))->text, "function")) only_fn = FALSE;
                     if (!only_fn) fprintf(stderr, "mpcsh: chihuahua %s: the body may only contain function definitions\n", c->name);
                     else mpc_exec(a);
             }
             g_ptr_array_free(a, TRUE);
             g_ptr_array_free(t, TRUE);
             mpcsh_last_status = saved;
         } else {
             char *bin = mpcsh_chu_bindir();
             char *exe = g_build_filename(bin, c->name, NULL);
             for (guint k = 0; k < c->commands->len; ++k) {
                 MpcChuCmd *m = g_new0(MpcChuCmd, 1);
                 m->exe = g_strdup(exe);
                 m->chu = g_strdup(c->name);
                 g_hash_table_replace(mpcsh_chu_cmds,
                                      g_strdup(g_ptr_array_index(c->commands, k)), m);
             }
             g_free(exe); g_free(bin);
         }
         g_ptr_array_add(mpcsh_chu_loaded, c);
     }

     static void mpcsh_chu_deactivate_all(void)
     {
         if (!mpcsh_chu_loaded) return;
         for (guint i = 0; i < mpcsh_chu_loaded->len; ++i) {
             MpcChu *c = g_ptr_array_index(mpcsh_chu_loaded, i);
             for (guint k = 0; k < c->commands->len; ++k) {
                 const char *cmd = g_ptr_array_index(c->commands, k);
                 if (mpcsh_chu_cmds) g_hash_table_remove(mpcsh_chu_cmds, cmd);
                 if (!strcmp(c->lang, "mpcsh") && mpc_funcs) g_hash_table_remove(mpc_funcs, cmd);
             }
             mpcsh_chu_free(c);
         }
         g_ptr_array_set_size(mpcsh_chu_loaded, 0);
     }

     /* The search path: install dir, chu.path lines, $MPCSH_CHU_PATH. tags are parallel. */
     static GPtrArray *mpcsh_chu_search_dirs(GPtrArray **tags_out)
     {
         GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
         GPtrArray *tags = g_ptr_array_new();
         char *inst = mpcsh_chu_dir();
         g_ptr_array_add(dirs, inst);
         g_ptr_array_add(tags, (gpointer)"install directory");
         char *pf = mpcsh_chu_pathfile();
         char *txt = NULL;
         if (g_file_get_contents(pf, &txt, NULL, NULL)) {
             char **ls = g_strsplit(txt, "\n", -1);
             for (int i = 0; ls[i]; ++i) {
                 char *d = g_strstrip(g_strdup(ls[i]));
                 if (*d && *d != '#') {
                     gboolean dup = FALSE;
                     for (guint k = 0; k < dirs->len; ++k) if (!strcmp(d, g_ptr_array_index(dirs, k))) dup = TRUE;
                     if (!dup) { g_ptr_array_add(dirs, d); g_ptr_array_add(tags, (gpointer)"chu.path"); d = NULL; }
                 }
                 g_free(d);
             }
             g_strfreev(ls);
             g_free(txt);
         }
         g_free(pf);
         const char *env = g_getenv("MPCSH_CHU_PATH");
         if (env && *env) {
             char **ps = g_strsplit(env, ":", -1);
             for (int i = 0; ps[i]; ++i) {
                 if (!*ps[i]) continue;
                 gboolean dup = FALSE;
                 for (guint k = 0; k < dirs->len; ++k) if (!strcmp(ps[i], g_ptr_array_index(dirs, k))) dup = TRUE;
                 if (!dup) { g_ptr_array_add(dirs, g_strdup(ps[i])); g_ptr_array_add(tags, (gpointer)"$MPCSH_CHU_PATH"); }
             }
             g_strfreev(ps);
         }
         if (tags_out) *tags_out = tags; else g_ptr_array_free(tags, TRUE);
         return dirs;
     }

     static GPtrArray *mpcsh_chu_list_files(const char *dir)
     {
         GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
         GDir *d = g_dir_open(dir, 0, NULL);
         if (!d) return names;
         const char *n;
         while ((n = g_dir_read_name(d)))
             if (g_str_has_suffix(n, ".chu")) g_ptr_array_add(names, g_strdup(n));
             g_dir_close(d);
         g_ptr_array_sort(names, (GCompareFunc)mpcsh_chu_strcmp);
         return names;
     }

     static void mpcsh_chu_load_all(void)
     {
         mpcsh_chu_deactivate_all();
         GPtrArray *dirs = mpcsh_chu_search_dirs(NULL);
         GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
         char *bin = mpcsh_chu_bindir();
         for (guint di = 0; di < dirs->len; ++di) {
             const char *dir = g_ptr_array_index(dirs, di);
             GPtrArray *files = mpcsh_chu_list_files(dir);
             for (guint fi = 0; fi < files->len; ++fi) {
                 char *path = g_build_filename(dir, g_ptr_array_index(files, fi), NULL);
                 char *text = NULL, *err = NULL;
                 MpcChu *c = NULL;
                 if (g_file_get_contents(path, &text, NULL, NULL)) c = mpcsh_chu_parse(text, path, &err);
                 else err = g_strdup("cannot read the file");
                 g_free(text);
                 if (!c) {
                     fprintf(stderr, "mpcsh: chihuahua %s: %s\n", path, err ? err : "invalid");
                     g_free(err); g_free(path);
                     continue;
                 }
                 g_free(path);
                 if (g_hash_table_contains(seen, c->name)) { mpcsh_chu_free(c); continue; }
                 g_hash_table_add(seen, g_strdup(c->name));
                 if (strcmp(c->lang, "mpcsh") != 0) {
                     char *exe = g_build_filename(bin, c->name, NULL);
                     struct stat sb, sc;
                     gboolean have = stat(exe, &sb) == 0;
                     if (strcmp(c->lang, "c") != 0) {
                         /* scripts are cheap: (re)write them when missing or older than the .chu */
                         if (!have || (stat(c->path, &sc) == 0 && sc.st_mtime > sb.st_mtime)) {
                             char *msg = NULL;
                             have = mpcsh_chu_build(c, exe, &msg);
                             if (!have) fprintf(stderr, "mpcsh: chihuahua %s: %s\n", c->name, msg ? msg : "build failed");
                             g_free(msg);
                         }
                     } else if (!have) {
                         fprintf(stderr, "mpcsh: chihuahua %s is not built yet: run  mpc-chuctrl build %s\n", c->name, c->name);
                     }
                     g_free(exe);
                     if (!have) { mpcsh_chu_free(c); continue; }
                 }
                 mpcsh_chu_activate(c);
             }
             g_ptr_array_free(files, TRUE);
         }
         g_free(bin);
         g_hash_table_destroy(seen);
         g_ptr_array_free(dirs, TRUE);
     }

     /* Called in a forked child: run a Chihuahua program if cmd is one. Returns only
      * when argv[0] is not a Chihuahua command. */
     static void mpcsh_chu_exec(char **argv)
     {
         if (!mpcsh_chu_cmds || !argv || !argv[0]) return;
         MpcChuCmd *m = g_hash_table_lookup(mpcsh_chu_cmds, argv[0]);
         if (!m) return;
         char *dd = mpcsh_chu_datadir(m->chu);
         g_mkdir_with_parents(dd, 0700);
         char *cd = mpcsh_chu_dir();
         setenv("MPCSH_CHU", m->chu, 1);
         setenv("MPCSH_CHU_CMD", argv[0], 1);
         setenv("MPCSH_CHU_DATA", dd, 1);
         setenv("MPCSH_CHU_DIR", cd, 1);
         execv(m->exe, argv);
         fprintf(stderr, "mpcsh: %s: cannot run chihuahua '%s' (%s): %s\n",
                 argv[0], m->chu, m->exe, strerror(errno));
         _exit(126);
     }

     static char *mpcsh_chu_find(const char *name)
     {
         GPtrArray *dirs = mpcsh_chu_search_dirs(NULL);
         char *found = NULL;
         char *file = g_strconcat(name, ".chu", NULL);
         for (guint i = 0; i < dirs->len && !found; ++i) {
             char *p = g_build_filename(g_ptr_array_index(dirs, i), file, NULL);
             if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) found = p; else g_free(p);
         }
         g_free(file);
         g_ptr_array_free(dirs, TRUE);
         return found;
     }

     static void mpcsh_chu_rmtree(const char *path)
     {
         if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
             GDir *d = g_dir_open(path, 0, NULL);
             if (d) {
                 const char *n;
                 while ((n = g_dir_read_name(d))) {
                     char *p = g_build_filename(path, n, NULL);
                     mpcsh_chu_rmtree(p);
                     g_free(p);
                 }
                 g_dir_close(d);
             }
         }
         g_remove(path);
     }

     /* ------------------------------------------------------- mpc-chuctrl */

     static const char *const mpcsh_chuctrl_usage =
     "Usage: mpc-chuctrl COMMAND [ARGS]\n"
     "  list [-v]              loaded Chihuahuas\n"
     "  info NAME              details of one Chihuahua\n"
     "  install SOURCE ...     install a .chu file, or every .chu in a directory\n"
     "  remove NAME [--purge]  uninstall (--purge also deletes its data directory)\n"
     "  new NAME [--lang L]    write a template NAME.chu here (mpcsh c sh python)\n"
     "  check FILE             validate a .chu file without installing\n"
     "  build NAME             rebuild a program Chihuahua (needed for C ones on a path)\n"
     "  reload                 load everything again\n"
     "  path                   show the search path\n"
     "  path add DIR           add a directory of .chu files\n"
     "  path remove DIR        forget a directory\n"
     "More: mpcshman chihuahuas\n";

         static const char *mpcsh_chu_template(const char *lang)
         {
             if (!strcmp(lang, "c"))
                 return "#!chu 1\nname: @NAME@\nversion: 0.1.0\nauthor: \n"
                 "description: A C chihuahua\nlang: c\ncommands: @NAME@\nneeds: mpsp\n---\n"
                 "#include \"mpcchu.h\"\n\n"
                 "int main(int argc, char **argv)\n{\n"
                 "    printf(\"%s: hello from a C chihuahua (inside MPCECC: %s)\\n\",\n"
                 "           mpcchu_name(), mpcchu_in_ecc() ? \"yes\" : \"no\");\n"
                 "    if (argc > 1 && mpcchu_in_ecc() && mpcchu_show_image(argv[1]) != 0)\n"
                 "        fprintf(stderr, \"could not show %s\\n\", argv[1]);\n"
                 "    return 0;\n}\n";
             if (!strcmp(lang, "sh"))
                 return "#!chu 1\nname: @NAME@\nversion: 0.1.0\nauthor: \n"
                 "description: A shell-script chihuahua\nlang: sh\ncommands: @NAME@\n---\n"
                 "echo \"$MPCSH_CHU: hello $1 from /bin/sh\"\n";
             if (!strcmp(lang, "python"))
                 return "#!chu 1\nname: @NAME@\nversion: 0.1.0\nauthor: \n"
                 "description: A Python chihuahua\nlang: python\ncommands: @NAME@\n---\n"
                 "import os, sys\nprint(os.environ.get('MPCSH_CHU'), 'says hello', sys.argv[1:])\n";
             return "#!chu 1\nname: @NAME@\nversion: 0.1.0\nauthor: \n"
             "description: An mpcsh chihuahua\nlang: mpcsh\ncommands: @NAME@\n---\n"
             "function @NAME@\n    echo @NAME@ says hello to $arg1, arguments: $argc\nend\n";
         }

         static int mpcsh_chuctrl_install_file(const char *src)
         {
             char *text = NULL, *err = NULL;
             if (!g_file_get_contents(src, &text, NULL, NULL)) {
                 fprintf(stderr, "mpc-chuctrl: cannot read %s\n", src);
                 return 1;
             }
             MpcChu *c = mpcsh_chu_parse(text, src, &err);
             if (!c) {
                 fprintf(stderr, "mpc-chuctrl: %s: %s\n", src, err);
                 g_free(err); g_free(text);
                 return 1;
             }
             char *dir = mpcsh_chu_dir(), *bindir = mpcsh_chu_bindir();
             g_mkdir_with_parents(dir, 0755);
             g_mkdir_with_parents(bindir, 0755);
             mpcsh_chu_write_header();
             char *target = g_build_filename(dir, c->name, NULL);
             char *target_chu = g_strconcat(target, ".chu", NULL);
             char *bin = g_build_filename(bindir, c->name, NULL);
             gboolean existed = g_file_test(target_chu, G_FILE_TEST_EXISTS);
             int rc = 0;
             if (strcmp(c->lang, "mpcsh") != 0) {
                 char *tmp = g_strconcat(bin, ".new", NULL);
                 char *msg = NULL;
                 if (!mpcsh_chu_build(c, tmp, &msg)) {
                     fprintf(stderr, "mpc-chuctrl: %s: build failed: %s\n", c->name, msg ? msg : "");
                     unlink(tmp);
                     rc = 1;
                 } else if (rename(tmp, bin) != 0) {
                     fprintf(stderr, "mpc-chuctrl: cannot place %s: %s\n", bin, strerror(errno));
                     rc = 1;
                 }
                 g_free(msg); g_free(tmp);
             } else {
                 unlink(bin);   /* a previous version may have been a program */
             }
             if (!rc) {
                 GError *e = NULL;
                 if (!g_file_set_contents(target_chu, text, -1, &e)) {
                     fprintf(stderr, "mpc-chuctrl: cannot write %s: %s\n", target_chu, e ? e->message : "?");
                     g_clear_error(&e);
                     rc = 1;
                 }
             }
             if (!rc) {
                 GString *cmds = g_string_new(NULL);
                 for (guint k = 0; k < c->commands->len; ++k) {
                     if (k) g_string_append(cmds, ", ");
                     g_string_append(cmds, g_ptr_array_index(c->commands, k));
                 }
                 printf("%s %s %s (%s)  commands: %s\n", existed ? "updated" : "installed",
                        c->name, c->version, c->lang, cmds->str);
                 g_string_free(cmds, TRUE);
             }
             g_free(dir); g_free(bindir); g_free(target); g_free(target_chu); g_free(bin);
             g_free(text);
             mpcsh_chu_free(c);
             return rc;
         }

         static int mpcsh_chuctrl_path(char **argv)
         {
             char *pf = mpcsh_chu_pathfile();
             int rc = 0;
             if (!argv[2] || !strcmp(argv[2], "show") || !strcmp(argv[2], "list")) {
                 GPtrArray *tags = NULL;
                 GPtrArray *dirs = mpcsh_chu_search_dirs(&tags);
                 for (guint i = 0; i < dirs->len; ++i) {
                     GPtrArray *f = mpcsh_chu_list_files(g_ptr_array_index(dirs, i));
                     printf("%2u  %s  [%s, %u file%s]\n", i + 1, (char *)g_ptr_array_index(dirs, i),
                            (char *)g_ptr_array_index(tags, i), f->len, f->len == 1 ? "" : "s");
                     g_ptr_array_free(f, TRUE);
                 }
                 g_ptr_array_free(dirs, TRUE);
                 g_ptr_array_free(tags, TRUE);
                 g_free(pf);
                 return 0;
             }
             gboolean add = !strcmp(argv[2], "add");
             gboolean del = !strcmp(argv[2], "remove") || !strcmp(argv[2], "rm") || !strcmp(argv[2], "del");
             if ((!add && !del) || !argv[3]) {
                 fprintf(stderr, "Usage: mpc-chuctrl path [show | add DIR | remove DIR]\n");
                 g_free(pf);
                 return 2;
             }
             char *want = (argv[3][0] == '~' && (argv[3][1] == '/' || !argv[3][1]))
             ? g_build_filename(g_get_home_dir(), argv[3] + (argv[3][1] ? 2 : 1), NULL)
             : g_strdup(argv[3]);
             char *canon = g_canonicalize_filename(want, NULL);
             g_free(want);
             char *txt = NULL;
             g_file_get_contents(pf, &txt, NULL, NULL);
             char **ls = g_strsplit(txt ? txt : "", "\n", -1);
             GString *out = g_string_new(NULL);
             gboolean found = FALSE;
             for (int i = 0; ls[i]; ++i) {
                 char *d = g_strstrip(g_strdup(ls[i]));
                 if (*d) {
                     if (!strcmp(d, canon)) { found = TRUE; if (del) { g_free(d); continue; } }
                     g_string_append_printf(out, "%s\n", d);
                 }
                 g_free(d);
             }
             if (add) {
                 if (!g_file_test(canon, G_FILE_TEST_IS_DIR)) {
                     fprintf(stderr, "mpc-chuctrl: %s is not a directory\n", canon);
                     rc = 1;
                 } else if (found) {
                     printf("already on the path: %s\n", canon);
                 } else {
                     g_string_append_printf(out, "%s\n", canon);
                 }
             } else if (!found) {
                 fprintf(stderr, "mpc-chuctrl: %s is not in %s\n", canon, pf);
                 rc = 1;
             }
             if (!rc && ((add && !found) || (del && found))) {
                 char *cfg = g_path_get_dirname(pf);
                 g_mkdir_with_parents(cfg, 0755);
                 g_free(cfg);
                 if (!g_file_set_contents(pf, out->str, -1, NULL)) {
                     fprintf(stderr, "mpc-chuctrl: cannot write %s\n", pf);
                     rc = 1;
                 } else {
                     printf("%s %s\n", add ? "added" : "removed", canon);
                     mpcsh_chu_load_all();
                 }
             }
             g_string_free(out, TRUE); g_strfreev(ls); g_free(txt); g_free(canon); g_free(pf);
             return rc;
         }

         static int mpcsh_chuctrl_builtin(char **argv)
         {
             const char *sub = argv[1];
             if (!sub || !strcmp(sub, "help") || !strcmp(sub, "-h") || !strcmp(sub, "--help")) {
                 fputs(mpcsh_chuctrl_usage, sub ? stdout : stderr);
                 return sub ? 0 : 2;
             }

             if (!strcmp(sub, "list") || !strcmp(sub, "ls")) {
                 gboolean verbose = argv[2] && (!strcmp(argv[2], "-v") || !strcmp(argv[2], "--verbose"));
                 if (!mpcsh_chu_loaded || !mpcsh_chu_loaded->len) { printf("no Chihuahuas loaded. Try: mpc-chuctrl new demo\n"); return 0; }
                 for (guint i = 0; i < mpcsh_chu_loaded->len; ++i) {
                     MpcChu *c = g_ptr_array_index(mpcsh_chu_loaded, i);
                     GString *cm = g_string_new(NULL);
                     for (guint k = 0; k < c->commands->len; ++k) {
                         if (k) g_string_append(cm, ", ");
                         g_string_append(cm, g_ptr_array_index(c->commands, k));
                     }
                     printf("%-16s %-8s %-7s %s\n", c->name, c->version, c->lang, cm->str);
                     if (verbose) {
                         if (c->description && *c->description) printf("    %s\n", c->description);
                         printf("    %s\n", c->path);
                     }
                     g_string_free(cm, TRUE);
                 }
                 return 0;
             }

             if (!strcmp(sub, "info")) {
                 if (!argv[2]) { fprintf(stderr, "Usage: mpc-chuctrl info NAME\n"); return 2; }
                 char *p = mpcsh_chu_find(argv[2]);
                 if (!p) { fprintf(stderr, "mpc-chuctrl: no Chihuahua named '%s' on the search path\n", argv[2]); return 1; }
                 char *text = NULL, *err = NULL;
                 MpcChu *c = NULL;
                 if (g_file_get_contents(p, &text, NULL, NULL)) c = mpcsh_chu_parse(text, p, &err);
                 if (!c) { fprintf(stderr, "mpc-chuctrl: %s: %s\n", p, err ? err : "cannot read"); g_free(err); g_free(text); g_free(p); return 1; }
                 GString *cm = g_string_new(NULL);
                 for (guint k = 0; k < c->commands->len; ++k) {
                     if (k) g_string_append(cm, ", ");
                     g_string_append(cm, g_ptr_array_index(c->commands, k));
                 }
                 printf("name:        %s\nversion:     %s\nlang:        %s\ncommands:    %s\n", c->name, c->version, c->lang, cm->str);
                 if (c->author && *c->author) printf("author:      %s\n", c->author);
                 if (c->description && *c->description) printf("description: %s\n", c->description);
                 if (c->needs && *c->needs) printf("needs:       %s\n", c->needs);
                 printf("file:        %s\n", c->path);
                 if (strcmp(c->lang, "mpcsh") != 0) {
                     char *bin = mpcsh_chu_bindir();
                     char *exe = g_build_filename(bin, c->name, NULL);
                     printf("program:     %s (%s)\n", exe, g_file_test(exe, G_FILE_TEST_IS_EXECUTABLE) ? "built" : "NOT built");
                     g_free(exe); g_free(bin);
                 }
                 g_string_free(cm, TRUE); mpcsh_chu_free(c); g_free(text); g_free(p);
                 return 0;
             }

             if (!strcmp(sub, "install") || !strcmp(sub, "add")) {
                 if (!argv[2]) { fprintf(stderr, "Usage: mpc-chuctrl install FILE.chu | DIRECTORY ...\n"); return 2; }
                 int rc = 0, count = 0;
                 for (int i = 2; argv[i]; ++i) {
                     if (g_file_test(argv[i], G_FILE_TEST_IS_DIR)) {
                         GPtrArray *f = mpcsh_chu_list_files(argv[i]);
                         if (!f->len) { fprintf(stderr, "mpc-chuctrl: no .chu files in %s\n", argv[i]); rc = 1; }
                         for (guint k = 0; k < f->len; ++k) {
                             char *p = g_build_filename(argv[i], g_ptr_array_index(f, k), NULL);
                             if (mpcsh_chuctrl_install_file(p)) rc = 1; else ++count;
                             g_free(p);
                         }
                         g_ptr_array_free(f, TRUE);
                     } else {
                         if (mpcsh_chuctrl_install_file(argv[i])) rc = 1; else ++count;
                     }
                 }
                 if (count) mpcsh_chu_load_all();
                 return rc;
             }

             if (!strcmp(sub, "remove") || !strcmp(sub, "rm") || !strcmp(sub, "uninstall")) {
                 gboolean purge = FALSE;
                 const char *name = NULL;
                 for (int i = 2; argv[i]; ++i) {
                     if (!strcmp(argv[i], "--purge")) purge = TRUE; else if (!name) name = argv[i];
                 }
                 if (!name) { fprintf(stderr, "Usage: mpc-chuctrl remove NAME [--purge]\n"); return 2; }
                 if (!mpcsh_chu_valid_name(name, FALSE)) {
                     fprintf(stderr, "mpc-chuctrl: bad name '%s' (lower case letters, digits, - and _)\\n", name);
                     return 2;
                 }
                 if (mpcsh_chu_is_reserved(name)) {
                     fprintf(stderr, "mpc-chuctrl: '%s' is a built-in name\\n", name);
                     return 2;
                 }
                 char *dir = mpcsh_chu_dir();
                 char *chu = g_build_filename(dir, name, NULL);
                 char *chu_file = g_strconcat(chu, ".chu", NULL);
                 int rc = 0;
                 if (!g_file_test(chu_file, G_FILE_TEST_IS_REGULAR)) {
                     char *other = mpcsh_chu_find(name);
                     if (other) fprintf(stderr, "mpc-chuctrl: '%s' is not installed; it is used in place from %s.\n"
                         "Delete that file or run: mpc-chuctrl path remove DIR\n", name, other);
                     else fprintf(stderr, "mpc-chuctrl: no Chihuahua named '%s' is installed\n", name);
                     g_free(other);
                     rc = 1;
                 } else {
                     g_remove(chu_file);
                     char *bin = mpcsh_chu_bindir();
                     char *exe = g_build_filename(bin, name, NULL);
                     g_remove(exe);
                     g_free(exe); g_free(bin);
                     if (purge) {
                         char *dd = mpcsh_chu_datadir(name);
                         mpcsh_chu_rmtree(dd);
                         g_free(dd);
                     }
                     printf("removed %s%s\n", name, purge ? " (and its data)" : "");
                     mpcsh_chu_load_all();
                 }
                 g_free(dir); g_free(chu); g_free(chu_file);
                 return rc;
             }

             if (!strcmp(sub, "new")) {
                 const char *name = NULL, *lang = "mpcsh";
                 for (int i = 2; argv[i]; ++i) {
                     if (!strcmp(argv[i], "--lang") && argv[i + 1]) lang = argv[++i];
                     else if (!name) name = argv[i];
                 }
                 if (!name) { fprintf(stderr, "Usage: mpc-chuctrl new NAME [--lang mpcsh|c|sh|python]\n"); return 2; }
                 if (!mpcsh_chu_valid_name(name, FALSE)) { fprintf(stderr, "mpc-chuctrl: bad name '%s' (lower case letters, digits, - and _)\n", name); return 2; }
                 if (mpcsh_chu_is_reserved(name)) { fprintf(stderr, "mpc-chuctrl: '%s' is a built-in name\n", name); return 2; }
                 if (strcmp(lang, "mpcsh") && strcmp(lang, "c") && strcmp(lang, "sh") && strcmp(lang, "python")) {
                     fprintf(stderr, "mpc-chuctrl: --lang must be mpcsh, c, sh or python\n");
                     return 2;
                 }
                 char *file = g_strconcat(name, ".chu", NULL);
                 if (g_file_test(file, G_FILE_TEST_EXISTS)) { fprintf(stderr, "mpc-chuctrl: %s already exists\n", file); g_free(file); return 1; }
                 char **parts = g_strsplit(mpcsh_chu_template(lang), "@NAME@", -1);
                 char *content = g_strjoinv(name, parts);
                 int rc = g_file_set_contents(file, content, -1, NULL) ? 0 : 1;
                 if (!rc) printf("wrote %s. Install it with: mpc-chuctrl install %s\n", file, file);
                 else fprintf(stderr, "mpc-chuctrl: cannot write %s\n", file);
                 g_strfreev(parts); g_free(content); g_free(file);
                 return rc;
             }

             if (!strcmp(sub, "check")) {
                 if (!argv[2]) { fprintf(stderr, "Usage: mpc-chuctrl check FILE.chu\n"); return 2; }
                 char *text = NULL, *err = NULL;
                 if (!g_file_get_contents(argv[2], &text, NULL, NULL)) { fprintf(stderr, "mpc-chuctrl: cannot read %s\n", argv[2]); return 1; }
                 MpcChu *c = mpcsh_chu_parse(text, argv[2], &err);
                 int rc = 0;
                 if (!c) { fprintf(stderr, "%s: %s\n", argv[2], err); g_free(err); rc = 1; }
                 else {
                     if (!strcmp(c->lang, "mpcsh")) {
                         gboolean ok = TRUE;
                         GPtrArray *t = mpc_split(c->body, &ok);
                         guint i = 0;
                         GPtrArray *a = mpc_parse(t, &i, 0, &ok);
                         if (!ok || i != t->len) { fprintf(stderr, "%s: syntax error in the body (quotes or block structure)\n", argv[2]); rc = 1; }
                         for (guint k = 0; !rc && k < a->len; ++k)
                             if (!mpc_kw(((MpcBlock *)g_ptr_array_index(a, k))->text, "function")) {
                                 fprintf(stderr, "%s: the body may only contain function definitions\n", argv[2]);
                                 rc = 1;
                             }
                             g_ptr_array_free(a, TRUE);
                         g_ptr_array_free(t, TRUE);
                     }
                     if (!rc) printf("ok: %s %s (%s)\n", c->name, c->version, c->lang);
                     mpcsh_chu_free(c);
                 }
                 g_free(text);
                 return rc;
             }

             if (!strcmp(sub, "build")) {
                 if (!argv[2]) { fprintf(stderr, "Usage: mpc-chuctrl build NAME\n"); return 2; }
                 char *p = mpcsh_chu_find(argv[2]);
                 if (!p) { fprintf(stderr, "mpc-chuctrl: no Chihuahua named '%s' on the search path\n", argv[2]); return 1; }
                 char *text = NULL, *err = NULL;
                 MpcChu *c = NULL;
                 if (g_file_get_contents(p, &text, NULL, NULL)) c = mpcsh_chu_parse(text, p, &err);
                 int rc = 0;
                 if (!c) { fprintf(stderr, "mpc-chuctrl: %s: %s\n", p, err ? err : "cannot read"); g_free(err); rc = 1; }
                 else if (!strcmp(c->lang, "mpcsh")) { printf("%s is an mpcsh Chihuahua: nothing to build\n", c->name); }
                 else {
                     char *bin = mpcsh_chu_bindir();
                     g_mkdir_with_parents(bin, 0755);
                     char *exe = g_build_filename(bin, c->name, NULL);
                     char *tmp = g_strconcat(exe, ".new", NULL);
                     char *msg = NULL;
                     if (!mpcsh_chu_build(c, tmp, &msg)) { fprintf(stderr, "mpc-chuctrl: build failed: %s\n", msg ? msg : ""); unlink(tmp); rc = 1; }
                     else if (rename(tmp, exe) != 0) { fprintf(stderr, "mpc-chuctrl: cannot place %s\n", exe); rc = 1; }
                     else { printf("built %s\n", exe); mpcsh_chu_load_all(); }
                     g_free(msg); g_free(tmp); g_free(exe); g_free(bin);
                 }
                 mpcsh_chu_free(c); g_free(text); g_free(p);
                 return rc;
             }

             if (!strcmp(sub, "reload")) {
                 mpcsh_chu_load_all();
                 printf("%u Chihuahua%s loaded\n", mpcsh_chu_loaded ? mpcsh_chu_loaded->len : 0,
                        (mpcsh_chu_loaded && mpcsh_chu_loaded->len == 1) ? "" : "s");
                 return 0;
             }

             if (!strcmp(sub, "path")) return mpcsh_chuctrl_path(argv);

             fprintf(stderr, "mpc-chuctrl: unknown command '%s'\n%s", sub, mpcsh_chuctrl_usage);
             return 2;
         }
         /* ===== end of 1.8 section ===== */


         static int
         mpcsh_windowlaunch_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcasecmp(argv[0], "windowlaunch")) return -1;

             if (!argv[1] || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
                 puts(
                     "Usage:\n"
                     "  windowlaunch PROGRAM [ARGS...]\n"
                     "  windowlaunch --size WxH --fps N PROGRAM [ARGS...]\n"
                     "  windowlaunch --check\n\n"
                     "Embeds PROGRAM inside the current MPCECC tab (FreeDOS style).\n"
                     "Default size: 1440x900  |  Default fps: 15\n"
                     "Multi-window: each launch gets a wlid; cycle with Ctrl+Shift+Tab.\n"
                     "Firefox uses isolated profile MPCECCWLPROFILE — real data safe.\n"
                     "Examples:\n"
                     "  windowlaunch firefox\n"
                     "  windowlaunch --size 1920x1080 krita\n"
                     "  windowlaunch --size 640x480 xterm"
                 );
                 return argv[1] ? 0 : 2;
             }

             if (!strcmp(argv[1], "--check")) {
                 const char *runtime = g_get_user_runtime_dir();
                 char path[4096];
                 if (runtime && *runtime)
                     snprintf(path, sizeof(path), "%s/mpcecc-control.sock", runtime);
                 else
                     snprintf(path, sizeof(path), "/tmp/mpcecc-control-%d.sock", (int)getuid());
                 if (g_file_test(path, G_FILE_TEST_EXISTS))
                     printf("windowlaunch: OK (control socket present)\n");
                 else
                     printf("windowlaunch: no control socket – start the main MPCECC console first\n");
                 return 0;
             }

             /* defaults: room for tool-heavy UIs (Krita/GIMP); was 960x600 / 1280x720 */
             int w = 1440, h = 900, fps = 15;
             int i = 1;
             while (argv[i] && argv[i][0] == '-' && strcmp(argv[i], "--")) {
                 if (!strcmp(argv[i], "--pixels") || !strcmp(argv[i], "--embed") || !strcmp(argv[i], "--auto")) {
                     /* noop – always embed */
                 } else if (!strcmp(argv[i], "--size")) {
                     if (!argv[++i]) { fprintf(stderr, "mpcsh: --size needs WxH\n"); return 2; }
                     if (sscanf(argv[i], "%dx%d", &w, &h) != 2 || w < 64 || h < 64) {
                         fprintf(stderr, "mpcsh: bad --size (use e.g. 1440x900)\n");
                         return 2;
                     }
                 } else if (!strcmp(argv[i], "--fps")) {
                     if (!argv[++i]) { fprintf(stderr, "mpcsh: --fps needs a number\n"); return 2; }
                     fps = atoi(argv[i]);
                     if (fps < 1 || fps > 60) fps = 15;
                 } else if (!strcmp(argv[i], "--cwd")) {
                     if (!argv[++i]) { fprintf(stderr, "mpcsh: --cwd needs a directory\n"); return 2; }
                     if (chdir(argv[i]) != 0) {
                         fprintf(stderr, "mpcsh: --cwd: %s\n", strerror(errno));
                         return 2;
                     }
                 } else if (!strcmp(argv[i], "--title")) {
                     if (!argv[++i]) { fprintf(stderr, "mpcsh: --title needs text\n"); return 2; }
                 } else {
                     fprintf(stderr, "mpcsh: windowlaunch: unknown option %s\n", argv[i]);
                     return 2;
                 }
                 ++i;
             }
             if (argv[i] && !strcmp(argv[i], "--")) ++i;
             if (!argv[i]) {
                 fprintf(stderr, "mpcsh: windowlaunch: missing PROGRAM\n"
                 "  try:  windowlaunch firefox\n");
                 return 2;
             }

             GString *msg = g_string_new(NULL);
             {
                 const char *tabid = g_getenv("MPCECC_TAB");
                 if (tabid && *tabid) g_string_append_printf(msg, "@%s ", tabid);
             }
             g_string_append_printf(msg, "LAUNCH %d %d %d -- ", w, h, fps);
             for (int j = i; argv[j]; ++j) {
                 if (j > i) g_string_append_c(msg, ' ');
                 char *q = g_shell_quote(argv[j]);   /* MPCECC re-parses with g_shell_parse_argv */
                 g_string_append(msg, q);
                 g_free(q);
             }
             g_string_append_c(msg, '\n');

             const char *runtime = g_get_user_runtime_dir();
             char path[4096];
             if (runtime && *runtime)
                 snprintf(path, sizeof(path), "%s/mpcecc-control.sock", runtime);
             else
                 snprintf(path, sizeof(path), "/tmp/mpcecc-control-%d.sock", (int)getuid());

             GError *error = NULL;
             GSocketClient *client = g_socket_client_new();
             GSocketAddress *addr = g_unix_socket_address_new(path);
             GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, &error);
             g_object_unref(addr);
             g_object_unref(client);

             if (!conn) {
                 fprintf(stderr, "mpcsh: windowlaunch: cannot talk to MPCECC (is the console running?): %s\n",
                         error ? error->message : "no socket");
                 g_clear_error(&error);
                 g_string_free(msg, TRUE);
                 return 1;
             }

             GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
             GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
             g_output_stream_write_all(out, msg->str, msg->len, NULL, NULL, NULL);
             g_string_free(msg, TRUE);

             char buf[512];
             gssize n = g_input_stream_read(in, buf, sizeof(buf) - 1, NULL, NULL);
             if (n > 0) {
                 buf[n] = 0;
                 if (g_str_has_prefix(buf, "OK")) {
                     int wlid = 0;
                     if (sscanf(buf, "OK %d", &wlid) == 1 && wlid > 0)
                         printf("windowlaunch: wlid %d — %s (%dx%d)  [shell free; windowterminate %d to close]\n",
                                wlid, argv[i], w, h, wlid);
                         else
                             printf("windowlaunch: embedded %s (%dx%d) into current tab\n", argv[i], w, h);
                 } else
                     fprintf(stderr, "mpcsh: windowlaunch: %s", buf);
             }
             g_object_unref(conn);
             return 0;
         }

         static int
         mpcsh_windowterminate_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcasecmp(argv[0], "windowterminate") != 0) return -1;
             if (!argv[1] || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
                 puts("Usage: windowterminate <wlid>\n"
                 "Close a PixelWindow by its WindowLaunch ID (see wlidlist).");
                 return argv[1] ? 0 : 2;
             }
             int id = atoi(argv[1]);
             if (id <= 0) {
                 fprintf(stderr, "mpcsh: windowterminate: invalid wlid\n");
                 return 2;
             }
             const char *runtime = g_get_user_runtime_dir();
             char path[4096];
             if (runtime && *runtime)
                 snprintf(path, sizeof(path), "%s/mpcecc-control.sock", runtime);
             else
                 snprintf(path, sizeof(path), "/tmp/mpcecc-control-%d.sock", (int)getuid());

             GError *error = NULL;
             GSocketClient *client = g_socket_client_new();
             GSocketAddress *addr = g_unix_socket_address_new(path);
             GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, &error);
             g_object_unref(addr);
             g_object_unref(client);
             if (!conn) {
                 fprintf(stderr, "mpcsh: windowterminate: MPCECC not running (%s)\n",
                         error ? error->message : "no socket");
                 g_clear_error(&error);
                 return 1;
             }
             GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
             GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
             char req[64];
             snprintf(req, sizeof req, "TERMINATE %d\n", id);
             g_output_stream_write(out, req, strlen(req), NULL, NULL);
             char rbuf[256];
             gssize rn = g_input_stream_read(in, rbuf, sizeof(rbuf) - 1, NULL, NULL);
             g_object_unref(conn);
             if (rn > 0) {
                 rbuf[rn] = 0;
                 if (g_str_has_prefix(rbuf, "OK"))
                     printf("windowterminate: closed wlid %d\n", id);
                 else
                     fprintf(stderr, "mpcsh: windowterminate: %s", rbuf);
             }
             return 0;
         }

         static int
         mpcsh_wlidlist_builtin(char **argv)
         {
             if (!argv || !argv[0] || (strcasecmp(argv[0], "wlidlist") != 0 && strcasecmp(argv[0], "windowlist") != 0))
                 return -1;

             const char *runtime = g_get_user_runtime_dir();
             char path[4096];
             if (runtime && *runtime)
                 snprintf(path, sizeof(path), "%s/mpcecc-control.sock", runtime);
             else
                 snprintf(path, sizeof(path), "/tmp/mpcecc-control-%d.sock", (int)getuid());

             GError *error = NULL;
             GSocketClient *client = g_socket_client_new();
             GSocketAddress *addr = g_unix_socket_address_new(path);
             GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, &error);
             g_object_unref(addr);
             g_object_unref(client);
             if (!conn) {
                 fprintf(stderr, "mpcsh: wlidlist: MPCECC not running (%s)\n",
                         error ? error->message : "no socket");
                 g_clear_error(&error);
                 return 1;
             }
             GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
             GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
             g_output_stream_write(out, "LIST\n", 5, NULL, NULL);
             char lbuf[4096];
             gssize ln = g_input_stream_read(in, lbuf, sizeof(lbuf) - 1, NULL, NULL);
             g_object_unref(conn);
             if (ln <= 0) {
                 printf("No PixelWindows.\n");
                 return 0;
             }
             lbuf[ln] = 0;
             printf("%-8s  %-16s  %s\n", "WLID", "COMMAND", "TITLE");
             printf("%-8s  %-16s  %s\n", "--------", "----------------", "-----");
             char **lines = g_strsplit(lbuf, "\n", -1);
             int count = 0;
             for (int i = 0; lines[i] && *lines[i]; ++i) {
                 char **f = g_strsplit(lines[i], "\t", 4);
                 if (f[0] && f[1]) {
                     printf("%-8s  %-16s  %s%s\n", f[0], f[1],
                            f[2] ? f[2] : "",
                            (f[3] && f[3][0] == '1') ? "  ●" : "");
                     ++count;
                 }
                 g_strfreev(f);
             }
             g_strfreev(lines);
             if (count == 0) printf("(none)\n");
             else printf("\nClose with: windowterminate <wlid>   |   F3 = star-map picker\n");
             return 0;
         }

         static int
         mpcsh_windowpicker_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcasecmp(argv[0], "windowpicker") != 0) return -1;

             const char *runtime = g_get_user_runtime_dir();
             char path[4096];
             if (runtime && *runtime)
                 snprintf(path, sizeof(path), "%s/mpcecc-control.sock", runtime);
             else
                 /* Must match mpcecc_control_sock_path() and windowlaunch/wlidlist. */
                 snprintf(path, sizeof(path), "/tmp/mpcecc-control-%d.sock", (int)getuid());

             GError *error = NULL;
             GSocketClient *client = g_socket_client_new();
             GSocketAddress *addr = g_unix_socket_address_new(path);
             GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, &error);
             g_object_unref(addr);
             g_object_unref(client);
             if (!conn) {
                 fprintf(stderr, "mpcsh: windowpicker: MPCECC not running (%s)\n",
                         error ? error->message : "no socket");
                 g_clear_error(&error);
                 return 1;
             }

             GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
             GInputStream *in  = g_io_stream_get_input_stream(G_IO_STREAM(conn));
             g_output_stream_write(out, "LIST\n", 5, NULL, NULL);

             char buf[4096];
             gssize n = g_input_stream_read(in, buf, sizeof(buf) - 1, NULL, NULL);
             g_object_unref(conn);
             if (n <= 0) {
                 printf("No virtual windows.\n");
                 return 0;
             }
             buf[n] = 0;

             printf("%-10s  %-20s  %s\n", "WWID", "COMMAND", "TITLE");
             printf("%-10s  %-20s  %s\n", "----------", "--------------------", "-----");
             char **lines = g_strsplit(buf, "\n", -1);
             int count = 0;
             for (int i = 0; lines[i] && *lines[i]; ++i) {
                 char **f = g_strsplit(lines[i], "\t", 4);
                 if (f[0] && f[1]) {
                     printf("%-10s  %-20s  %s%s\n", f[0], f[1],
                            f[2] ? f[2] : "",
                            (f[3] && f[3][0] == '1') ? "  ●" : "");
                     ++count;
                 }
                 g_strfreev(f);
             }
             g_strfreev(lines);
             if (count == 0) printf("(none)\n");
             else printf("\nPress F3 for the Aero picker, or: windowterminate <WWID>\n");
             return 0;
         }




         static int mpcsh_extension_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcmp(argv[0], "extension")) return -1;
             const char *home = g_get_home_dir();
             char *dirpath = g_build_filename(home ? home : ".", ".config", "mpcsh", "extensions", NULL);
             if (!argv[1] || !strcmp(argv[1], "list")) {
                 GDir *dir = g_dir_open(dirpath, 0, NULL);
                 printf("MPCSH native extensions: %s\n", dirpath);
                 if (!dir) puts("  (no extension directory or no extensions installed)");
                 else {
                     const char *name; gboolean any = FALSE;
                     while ((name = g_dir_read_name(dir))) {
                         if (g_str_has_suffix(name, ".mpcsh")) { printf("  %s\n", name); any = TRUE; }
                     }
                     if (!any) puts("  (none)");
                     g_dir_close(dir);
                 }
                 g_free(dirpath); return 0;
             }
             if (!strcmp(argv[1], "load") && argv[2]) {
                 char *path = NULL;
                 if (strchr(argv[2], G_DIR_SEPARATOR)) path = g_strdup(argv[2]);
                 else path = g_build_filename(dirpath, argv[2], NULL);
                 if (!g_str_has_suffix(path, ".mpcsh")) {
                     char *tmp = g_strconcat(path, ".mpcsh", NULL); g_free(path); path = tmp;
                 }
                 char *contents = NULL; gsize length = 0; GError *error = NULL;
                 if (!g_file_get_contents(path, &contents, &length, &error)) {
                     fprintf(stderr, "mpcsh: extension: cannot load %s: %s\n", path, error->message);
                     g_error_free(error); g_free(path); g_free(dirpath); return 1;
                 }
                 /* Extension files are native .mpcsh source. Function definitions are
                  * registered in the current interpreter; top-level statements run too. */
                 int rc = mpcsh_run_line(contents);
                 if (!rc) printf("mpcsh: loaded extension %s\n", path);
                 g_free(contents); g_free(path); g_free(dirpath); return rc;
             }
             if (!strcmp(argv[1], "install") || !strcmp(argv[1], "mpcextinstl")) {
                 /* User-space only — never root. */
                 if (!argv[2]) {
                     fprintf(stderr, "Usage: extension install <git-url-or-path>\n");
                     g_free(dirpath); return 2;
                 }
                 if (geteuid() == 0) {
                     fprintf(stderr, "mpcsh: extension install refuses to run as root\n");
                     g_free(dirpath); return 1;
                 }
                 g_mkdir_with_parents(dirpath, 0755);
                 const char *src = argv[2];
                 char *name = g_path_get_basename(src);
                 /* strip .git */
                 if (g_str_has_suffix(name, ".git")) name[strlen(name)-4] = 0;
                 char *dest = g_build_filename(dirpath, name, NULL);
                 if (g_str_has_prefix(src, "http://") || g_str_has_prefix(src, "https://") ||
                     g_str_has_prefix(src, "git@")) {
                     char *cmd = g_strdup_printf("git clone --depth 1 '%s' '%s'", src, dest);
                 int rc = system(cmd);
                 g_free(cmd);
                 if (rc != 0) {
                     fprintf(stderr, "mpcsh: git clone failed (%d)\n", rc);
                     g_free(name); g_free(dest); g_free(dirpath); return 1;
                 }
                     } else {
                         /* local file/dir copy */
                         char *cmd = g_strdup_printf("cp -a '%s' '%s'", src, dest);
                         int rc = system(cmd);
                         g_free(cmd);
                         if (rc != 0) {
                             fprintf(stderr, "mpcsh: copy failed (%d)\n", rc);
                             g_free(name); g_free(dest); g_free(dirpath); return 1;
                         }
                     }
                     printf("mpcsh: installed extension to %s\n", dest);
                     printf("  load with: extension load %s/init.mpcsh\n", name);
                     printf("  env: MPCSH_VERSION=%s MPCSH_SHELL=mpcsh\n", MPCSH_VERSION_STRING);
                     g_free(name); g_free(dest); g_free(dirpath); return 0;
             }
             if (!strcmp(argv[1], "help")) {
                 puts("extension list\n"
                 "extension load <name.mpcsh>\n"
                 "extension install <git-url-or-path>   (user dir only, never root)\n"
                 "Also: mpcextinstl <git-url>\n"
                 "Extensions are native MPCSH scripts; function name ... end.\n"
                 "Identity env: MPCSH_VERSION MPCSH_SHELL MPCSH_NAME MPCSH_FRONTEND");
                 g_free(dirpath); return 0;
             }
             fprintf(stderr, "Usage: extension list|load|install|help\n");
             g_free(dirpath); return 2;
         }

         /* ------------------------------------------------------------
          * PortMap — physical-ish Linux port map
          *
          * PortMap deliberately reports what Linux can actually discover.
          * USB connector shape is not generally exposed by sysfs, so USB-C
          * and Micro-B can be supplied by a small PortMap.conf override.
          * ------------------------------------------------------------ */

         static const char *mpcsh_portmap_config_path(void)
         {
             static char path[MPCSH_MAX_PATH];
             const char *home = g_get_home_dir();
             snprintf(path, sizeof(path), "%s/.config/mpcsh/PortMap.conf",
                      home ? home : ".");
             return path;
         }

         static gboolean mpcsh_read_first_line(const char *path, char *out, size_t n)
         {
             FILE *f = fopen(path, "r");
             if (!f) return FALSE;
             gboolean ok = fgets(out, (int)n, f) != NULL;
             fclose(f);
             if (ok) g_strstrip(out);
             return ok;
         }

         static const char *mpcsh_usb_color(const char *speed)
         {
             if (!speed) return "\033[37m";
             if (atof(speed) >= 5000.0) return "\033[1;34m"; /* USB 3.x: BLUE */
                 return "\033[1;37m";                            /* USB 2.x: WHITE */
         }

         static void mpcsh_portmap_draw_port(const char *inside_color,
                                             const char *connector,
                                             const char *detail,
                                             gboolean always_on)
         {
             /* Front-panel UniArt: draw the actual receptacle, not a text card. */
             const char *shell = always_on ? "\033[1;31m" : "\033[1;37m";
             const char *reset = "\033[0m";
             const char *inner = inside_color ? inside_color : "\033[1;37m";
             const char *accent = "\033[1;36m";

             if (!connector) connector = "USB-A";

             if (!strcasecmp(connector, "USB-C")) {
                 printf("  %s┌───────────────────────┐%s\n", shell, reset);
                 printf("  %s│%s ○       %s╭───────╮%s       %s○%s %s│%s\n",
                        shell, reset, accent, reset, shell, reset, shell, reset);
                 printf("  %s│%s         %s│███████│%s         %s│%s\n",
                        shell, reset, inner, reset, shell, reset);
                 printf("  %s│%s         %s│███████│%s         %s│%s\n",
                        shell, reset, inner, reset, shell, reset);
                 printf("  %s│%s         %s╰───────╯%s         %s│%s\n",
                        shell, reset, accent, reset, shell, reset);
                 printf("  %s│%s                             %s│%s\n", shell, reset, shell, reset);
                 printf("  %s└───────────────────────┘%s\n", shell, reset);
             } else if (!strcasecmp(connector, "MICRO-B") ||
                 !strcasecmp(connector, "MICROB")) {
                 printf("  %s┌───────────────────────┐%s\n", shell, reset);
             printf("  %s│%s ○                  ○ %s│%s\n", shell, reset, shell, reset);
             printf("  %s│%s        %s╲██████╱%s       %s│%s\n",
                    shell, reset, inner, reset, shell, reset);
             printf("  %s│%s         %s╲████╱%s        %s│%s\n",
                    shell, reset, inner, reset, shell, reset);
             printf("  %s│%s          %s╲██╱%s         %s│%s\n",
                    shell, reset, inner, reset, shell, reset);
             printf("  %s└───────────────────────┘%s\n", shell, reset);
                 } else if (!strcasecmp(connector, "ETHERNET") ||
                     !strcasecmp(connector, "RJ45")) {
                     printf("  %s┌───────────────────────┐%s\n", shell, reset);
                 printf("  %s│%s ○      %s╔═══════════╗%s      %s○%s %s│%s\n",
                        shell, reset, accent, reset, shell, reset, shell, reset);
                 printf("  %s│%s        %s║╬╬╬╬╬╬╬╬╬║%s        %s│%s\n",
                        shell, reset, inner, reset, shell, reset);
                 printf("  %s│%s        %s║╬╬╬╬╬╬╬╬╬║%s        %s│%s\n",
                        shell, reset, inner, reset, shell, reset);
                 printf("  %s│%s        %s╚═══════════╝%s        %s│%s\n",
                        shell, reset, accent, reset, shell, reset);
                 printf("  %s└───────────────────────┘%s\n", shell, reset);
                     } else {
                         /* USB-A: square front plate + screw holes + deep rectangular opening.
                          *           The inner opening is the part that becomes BLUE for USB 3.x. */
                         printf("  %s┌───────────────────────┐%s\n", shell, reset);
                         printf("  %s│%s ○                   ○ %s│%s\n", shell, reset, shell, reset);
                         printf("  %s│%s     %s╔═════════════╗%s     %s│%s\n",
                                shell, reset, inner, reset, shell, reset);
                         printf("  %s│%s     %s║█████████████║%s     %s│%s\n",
                                shell, reset, inner, reset, shell, reset);
                         printf("  %s│%s     %s║█████████████║%s     %s│%s\n",
                                shell, reset, inner, reset, shell, reset);
                         printf("  %s│%s     %s║█████████████║%s     %s│%s\n",
                                shell, reset, inner, reset, shell, reset);
                         printf("  %s│%s     %s╚═════════════╝%s     %s│%s\n",
                                shell, reset, inner, reset, shell, reset);
                         printf("  %s│%s ○                   ○ %s│%s\n", shell, reset, shell, reset);
                         printf("  %s└───────────────────────┘%s\n", shell, reset);
                     }

                     printf("      %s%s%s\n", "\033[1;36m", detail ? detail : connector, reset);
                     if (always_on)
                         printf("      %s● ALWAYS ON%s\n", "\033[1;31m", reset);
         }

         static int mpcsh_portmap_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcasecmp(argv[0], "portmap") != 0)
                 return -1;

             const char *cfg = mpcsh_portmap_config_path();
             gboolean show_help = argv[1] &&
             (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"));

             if (show_help) {
                 printf("MarekPC PortMap — UniArt hardware port map\n"
                 "Usage: portmap [--raw]\n"
                 "USB 3.x interiors are blue; ALWAYS ON ports are red.\n"
                 "USB-C/Micro-B connector shapes may be declared in:\n"
                 "  %s\n\n"
                 "Example PortMap.conf:\n"
                 "  PORT=1 USB-A USB3\n"
                 "  PORT=2 USB-A USB2 ALWAYS_ON\n"
                 "  PORT=3 USB-C USB3\n"
                 "  PORT=4 MICRO-B USB2\n"
                 "  PORT=5 ETHERNET\n", cfg);
                 return 0;
             }

             gboolean raw = argv[1] && !strcmp(argv[1], "--raw");
             if (!raw) {
                 printf("\n\033[1;35m╔══════════════════════════════════════════════╗\033[0m\n"
                 "\033[1;35m║              MarekPC PortMap                ║\033[0m\n"
                 "\033[1;35m╚══════════════════════════════════════════════╝\033[0m\n\n");
                 /* Detect board via DMI so B550M-A and others get accurate notes */
                 gchar *product = NULL, *board = NULL;
                 g_file_get_contents("/sys/devices/virtual/dmi/id/product_name", &product, NULL, NULL);
                 g_file_get_contents("/sys/devices/virtual/dmi/id/board_name", &board, NULL, NULL);
                 if (product) g_strstrip(product);
                 if (board) g_strstrip(board);
                 if (product || board) {
                     printf("  Board: %s / %s\n", product ? product : "?", board ? board : "?");
                     if ((product && strcasestr(product, "B550")) ||
                         (board && strcasestr(board, "B550"))) {
                         printf("  \033[1;36mASUS B550 family detected\033[0m — rear USB 3.2 Gen2 ports are TEAL.\n"
                                "  Typical rear: 2× USB 3.2 Gen2 (teal, 10 Gbps) + 4× USB 3.2 Gen1 + LAN + audio.\n"
                                "  Live sysfs discovery below is authoritative for what is plugged in.\n\n");
                     } else if ((product && strcasestr(product, "X570")) ||
                                (board && strcasestr(board, "X570"))) {
                         printf("  \033[1;36mX570 family\033[0m — expect multiple blue/teal USB 3.x ports.\n\n");
                     } else {
                         printf("  Live USB/network discovery follows (works on every board).\n\n");
                     }
                 } else {
                     printf("  Live USB/network discovery follows (works on every board).\n\n");
                 }
                 g_free(product); g_free(board);

                 FILE *f = fopen(cfg, "r");
                 gboolean had_override = FALSE;
                 if (f) {
                     char line[MPCSH_MAX_LINE_HARD];
                     while (fgets(line, sizeof(line), f)) {
                         g_strstrip(line);
                         if (!*line || line[0] == '#') continue;
                         if (strncasecmp(line, "PORT=", 5) != 0) continue;

                         char copy[MPCSH_MAX_LINE_HARD];
                         g_strlcpy(copy, line, sizeof(copy));
                         char *save = NULL;
                         char *tok = strtok_r(copy, " \t", &save);
                         char *connector = strtok_r(NULL, " \t", &save);
                         char *speed = strtok_r(NULL, " \t", &save);
                         gboolean always_on = FALSE;
                         for (char *t = strtok_r(NULL, " \t", &save); t;
                              t = strtok_r(NULL, " \t", &save)) {
                             if (!strcasecmp(t, "ALWAYS_ON")) always_on = TRUE;
                              }
                              if (!tok || !connector) continue;

                              char port_label[64];
                         snprintf(port_label, sizeof(port_label), "%s", tok + 5);
                         const char *inside = "\033[1;37m";

                         if (!strcasecmp(connector, "USB-C")) inside = "\033[1;36m";
                         else if (!strcasecmp(connector, "MICRO-B") ||
                             !strcasecmp(connector, "MICROB")) inside = "\033[1;33m";
                         else if (!strcasecmp(connector, "ETHERNET")) inside = "\033[1;32m";
                         else if (speed && (!strcasecmp(speed, "USB3") ||
                             !strcasecmp(speed, "USB3.0") ||
                             !strcasecmp(speed, "USB3.1") ||
                             !strcasecmp(speed, "USB3.2")))
                             inside = "\033[1;34m";

                         char detail_buf[128];
                         if (speed && *speed)
                             snprintf(detail_buf, sizeof(detail_buf), "%s %s", connector, speed);
                         else
                             snprintf(detail_buf, sizeof(detail_buf), "%s", connector);

                         printf("  \033[1;36mPORT %s\033[0m\n", port_label);
                         mpcsh_portmap_draw_port(inside, connector, detail_buf, always_on);
                         had_override = TRUE;
                     }
                     fclose(f);
                 }

                 if (!had_override)
                     printf("  \033[2mNo board-specific PortMap.conf entries.\033[0m\n");
                 printf("\n  \033[2mLive Linux USB/network discovery follows below.\033[0m\n");
             }

             if (raw)
                 printf("PortMap raw Linux discovery\n");

             DIR *d = opendir("/sys/bus/usb/devices");
             int shown = 0;
             if (d) {
                 struct dirent *e;
                 printf("\n\033[1;36mDetected USB topology / port speeds:\033[0m\n");
                 while ((e = readdir(d))) {
                     if (e->d_name[0] == '.') continue;
                     const char *name = e->d_name;
                     const char *dash = strchr(name, '-');
                     if (!dash || dash == name || !isdigit((unsigned char)name[0]))
                         continue;

                     char base[MPCSH_MAX_PATH], speedp[MPCSH_MAX_PATH], speed[64] = "";
                     snprintf(base, sizeof(base), "/sys/bus/usb/devices/%s", name);
                     if (g_snprintf(speedp, sizeof(speedp), "%s/speed", base) >= (gint)sizeof(speedp))
                         continue;
                     if (!mpcsh_read_first_line(speedp, speed, sizeof(speed))) continue;

                     char prod[128] = "USB device";
                     char prodp[MPCSH_MAX_PATH];
                     if (g_snprintf(prodp, sizeof(prodp), "%s/product", base) >= (gint)sizeof(prodp))
                         continue;
                     mpcsh_read_first_line(prodp, prod, sizeof(prod));

                     gboolean usb3 = atof(speed) >= 5000.0;
                     const char *inside = mpcsh_usb_color(speed);

                     /*
                      * Linux tells us the topology/speed, not the physical socket
                      * shape.  Treat discovered devices as USB-A-shaped by default;
                      * USB-C/Micro-B can still be declared explicitly in PortMap.conf.
                      */
                     char detail_buf[256];
                     g_snprintf(detail_buf, sizeof(detail_buf),
                                "%s  %s  @ %s Mbit/s",
                                name,
                                usb3 ? "BLUE USB3 INSIDE" : "USB2 INSIDE",
                                speed);

                     printf("\n");
                     mpcsh_portmap_draw_port(inside, "USB-A", detail_buf, FALSE);
                     printf("  \033[1;33m└─ attached: %s\033[0m\n", prod);
                     shown++;
                 }
                 closedir(d);
             }

             DIR *net = opendir("/sys/class/net");
             int eth = 0;
             if (net) {
                 struct dirent *e;
                 printf("\n\033[1;36mEthernet:\033[0m\n");
                 while ((e = readdir(net))) {
                     if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                     char path[MPCSH_MAX_PATH], type[32] = "";
                     snprintf(path, sizeof(path), "/sys/class/net/%s/type", e->d_name);
                     if (mpcsh_read_first_line(path, type, sizeof(type)) && !strcmp(type, "1")) {
                         mpcsh_portmap_draw_port("\033[1;32m", "ETHERNET", e->d_name, FALSE);
                         eth++;
                     }
                 }
                 closedir(net);
             }

             printf("\n\033[1;35mLegend (honest):\033[0m\n"
             "  \033[1;36mTEAL / BLUE\033[0m  = USB 3.2 Gen 1/2  (ASUS B550M-A teal = Gen2 10 Gbps)\n"
             "  \033[1;34mBLUE\033[0m         = USB 3.x\n"
             "  \033[1;37mWHITE / GRAY\033[0m = USB 2.0\n"
             "  \033[1;31mRED\033[0m          = ALWAYS ON (powered even in soft-off)\n"
             "  \033[1;32mGREEN\033[0m        = Ethernet\n"
             "  Real connector colour comes from the motherboard manual + sysfs speed.\n"
             "  There is no automatic USB-C detection from sysfs; declare it in PortMap.conf if needed.\n");

             if (!shown && !eth) {
                 printf("  No live USB/Ethernet devices were discoverable.\n");
                 if (!access(cfg, F_OK))
                     printf("  Board-specific connectors can still be shown from %s\n", cfg);
                 else
                     printf("  Add board-specific connectors to %s\n", cfg);
             }
             printf("\n");
             return 0;
         }

         /* ------------------------------------------------------------
          * GUIInTerminal — clickable terminal UI
          *
          * This is a terminal-native GUI: Unicode widgets + ANSI colors +
          * xterm/VT mouse reporting. It never opens a second GTK window.
          *
          * Script format:
          *   title: My Tool
          *   text: Choose an action
          *   button: Build
          *   command: echo building
          *   button: Port Map
          *   command: portmap
          *   checkbox: Verbose
          *
          * Buttons are clickable with a mouse in terminals supporting SGR
          * mouse reporting and remain keyboard-selectable with arrows/Enter.
          * ------------------------------------------------------------ */

         typedef struct {
             char label[128];
             char command[MPCSH_MAX_LINE_HARD];
             gboolean checkbox;
             gboolean checked;
         } MpcshGuiItem;

         typedef struct {
             char title[128];
             char text[1024];
             MpcshGuiItem items[MPCSH_GUI_MAX_BUTTONS];
             int count;
         } MpcshGuiTerminal;

         static void mpcsh_gui_trim(char *s)
         {
             g_strstrip(s);
             size_t n = strlen(s);
             if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
                 memmove(s, s + 1, n - 2);
                 s[n - 2] = '\0';
             }
         }

         static gboolean mpcsh_gui_load(const char *path, MpcshGuiTerminal *g)
         {
             FILE *f = fopen(path, "r");
             if (!f) return FALSE;
             memset(g, 0, sizeof(*g));
             g_strlcpy(g->title, "GUIInTerminal", sizeof(g->title));

             char line[MPCSH_MAX_LINE_HARD];
             int current = -1;
             while (fgets(line, sizeof(line), f)) {
                 g_strstrip(line);
                 if (!*line || line[0] == '#') continue;

                 if (!strncasecmp(line, "title:", 6)) {
                     g_strlcpy(g->title, line + 6, sizeof(g->title));
                     mpcsh_gui_trim(g->title);
                 } else if (!strncasecmp(line, "text:", 5)) {
                     g_strlcpy(g->text, line + 5, sizeof(g->text));
                     mpcsh_gui_trim(g->text);
                 } else if (!strncasecmp(line, "button:", 7) && g->count < MPCSH_GUI_MAX_BUTTONS) {
                     current = g->count++;
                     memset(&g->items[current], 0, sizeof(g->items[current]));
                     g_strlcpy(g->items[current].label, line + 7, sizeof(g->items[current].label));
                     mpcsh_gui_trim(g->items[current].label);
                 } else if (!strncasecmp(line, "checkbox:", 9) && g->count < MPCSH_GUI_MAX_BUTTONS) {
                     current = g->count++;
                     memset(&g->items[current], 0, sizeof(g->items[current]));
                     g->items[current].checkbox = TRUE;
                     g_strlcpy(g->items[current].label, line + 9, sizeof(g->items[current].label));
                     mpcsh_gui_trim(g->items[current].label);
                 } else if (!strncasecmp(line, "command:", 8) && current >= 0) {
                     g_strlcpy(g->items[current].command, line + 8, sizeof(g->items[current].command));
                     mpcsh_gui_trim(g->items[current].command);
                 }
             }
             fclose(f);
             return TRUE;
         }

         static void mpcsh_gui_draw(const MpcshGuiTerminal *g, int selected)
         {
             printf("\033[2J\033[H\033[?1000h\033[?1006h");
             printf("\033[1;35m╭────────────────────────────────────────────────────────────╮\033[0m\n");
             printf("\033[1;35m│\033[0m \033[1;97m%-58s\033[0m\033[1;35m│\033[0m\n", g->title);
             printf("\033[1;35m├────────────────────────────────────────────────────────────┤\033[0m\n");
             if (g->text[0])
                 printf("\033[1;35m│\033[0m %-58s \033[1;35m│\033[0m\n", g->text);
             printf("\033[1;35m│\033[0m                                                            \033[1;35m│\033[0m\n");

             for (int i = 0; i < g->count; ++i) {
                 const MpcshGuiItem *it = &g->items[i];
                 const char *bg = (i == selected) ? "\033[7m" : "";
                 const char *mark = it->checkbox ? (it->checked ? "[✓]" : "[ ]") : "[ ]";
                 char label[56];
                 snprintf(label, sizeof(label), "%s %s", mark, it->label);
                 printf("\033[1;35m│\033[0m %s%-56s\033[0m \033[1;35m│\033[0m\n", bg, label);
             }

             printf("\033[1;35m│\033[0m                                                            \033[1;35m│\033[0m\n");
             printf("\033[1;35m├────────────────────────────────────────────────────────────┤\033[0m\n");
             printf("\033[1;35m│\033[0m ↑/↓ select  Enter/click activate  Space toggle  Esc exit \033[1;35m│\033[0m\n");
             /* Fixed: the bottom border used to contain corrupted UTF-8 bytes. */
             printf("\033[1;35m╰────────────────────────────────────────────────────────────╯\033[0m\n");
             fflush(stdout);
         }

         static int mpcsh_gui_read_key(void)
         {
             unsigned char c;
             if (mpcsh_read_retry(STDIN_FILENO, &c, 1) != 1) return -1;
             if (c != '\033') return c;

             unsigned char seq[32];
             int n = 0;
             while (n < (int)sizeof(seq) - 1) {
                 if (mpcsh_read_retry(STDIN_FILENO, &seq[n], 1) != 1) break;
                 if (seq[n] == 'A' || seq[n] == 'B' || seq[n] == 'C' || seq[n] == 'D' ||
                     seq[n] == 'M' || seq[n] == '~') {
                     n++;
                 break;
                     }
                     n++;
             }
             seq[n] = '\0';
             if (n >= 1 && seq[n-1] == 'A') return 1001; /* up */
                 if (n >= 1 && seq[n-1] == 'B') return 1002; /* down */
                     if (n >= 1 && seq[n-1] == 'M') {
                         /* SGR mouse: ESC [ < button ; x ; y M */
                         int button = 0, x = 0, y = 0;
                         if (sscanf((char *)seq, "[<%d;%d;%dM", &button, &x, &y) == 3) {
                             if ((button & 3) == 0) return 2000 + y;
                         }
                     }
                     return 27;
         }

         static int mpcsh_gui_terminal_run(const char *path)
         {
             MpcshGuiTerminal g;
             if (!mpcsh_gui_load(path, &g)) {
                 fprintf(stderr, "mpcsh: GUIInTerminal: cannot open '%s': %s\n",
                         path, strerror(errno));
                 return 126;
             }
             if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
                 fprintf(stderr, "mpcsh: GUIInTerminal requires a terminal on stdin/stdout\n");
                 return 2;
             }

             struct termios oldt, raw;
             if (tcgetattr(STDIN_FILENO, &oldt) < 0) return 1;
             raw = oldt;
             raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
             raw.c_cc[VMIN] = 1;
             raw.c_cc[VTIME] = 0;
             tcsetattr(STDIN_FILENO, TCSANOW, &raw);

             int selected = 0, result = 0;
             mpcsh_gui_draw(&g, selected);

             for (;;) {
                 int k = mpcsh_gui_read_key();
                 if (k == 27) break;
                 if (k == 1001 || k == 'k') {
                     if (--selected < 0) selected = g.count ? g.count - 1 : 0;
                     mpcsh_gui_draw(&g, selected);
                 } else if (k == 1002 || k == 'j') {
                     if (++selected >= g.count) selected = 0;
                     mpcsh_gui_draw(&g, selected);
                 } else if (k == ' ') {
                     if (g.count && g.items[selected].checkbox) {
                         g.items[selected].checked = !g.items[selected].checked;
                         mpcsh_gui_draw(&g, selected);
                     }
                 } else if (k == '\r' || k == '\n' || (k >= 2000 && k < 2100)) {
                     int index = selected;
                     if (k >= 2000) {
                         int y = k - 2000;
                         /* rows 6+ are the item rows */
                         index = y - 6;
                         if (index < 0 || index >= g.count) index = selected;
                     }
                     if (index >= 0 && index < g.count) {
                         MpcshGuiItem *it = &g.items[index];
                         if (it->checkbox) {
                             it->checked = !it->checked;
                             mpcsh_gui_draw(&g, index);
                         } else if (it->command[0]) {
                             tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
                             printf("\033[?1000l\033[?1006l\033[0m\n");
                             result = mpcsh_run_line(it->command);
                             tcsetattr(STDIN_FILENO, TCSANOW, &raw);
                             mpcsh_gui_draw(&g, index);
                         }
                     }
                 }
             }

             tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
             printf("\033[?1000l\033[?1006l\033[0m\033[2J\033[H");
             return result;
         }

         static int mpcsh_guiinterm_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcasecmp(argv[0], "guiinterm") != 0)
                 return -1;
             if (!argv[1] || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
                 printf("GUIInTerminal — terminal-native clickable GUI\n"
                 "Usage: guiinterm FILE\n"
                 "FILE syntax: title:, text:, button:, command:, checkbox:\n");
                 return argv[1] ? 0 : 2;
             }
             return mpcsh_gui_terminal_run(argv[1]);
         }

         /* ------------------------------------------------------------
          * Attached script runners
          * ------------------------------------------------------------ */

         static int
         mpcsh_run_fish(const char *dir, const char *script)
         {
             char path[MPCSH_MAX_PATH];
             snprintf(path, sizeof(path), "%s/attach.fish", dir);
             if (!g_file_set_contents(path, script, -1, NULL)) return 125;
             char *argv[] = { (char *)"fish", path, NULL };
             int status = 0;
             if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL))
                 return 125;
             return g_spawn_check_wait_status(status, NULL) ? WEXITSTATUS(status) : 126;
         }

         static int
         mpcsh_run_mpcsh(const char *script)
         {
             char *copy=g_strdup(script);int rc=mpcsh_run_line(copy);g_free(copy);return rc;
         }

         static int
         mpcsh_run_python(const char *dir, const char *script)
         {
             char path[MPCSH_MAX_PATH];
             snprintf(path, sizeof(path), "%s/attach.py", dir);
             if (!g_file_set_contents(path, script, -1, NULL)) return 125;
             char *argv[] = { (char *)"python3", path, NULL };
             int status = 0;
             if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL))
                 return 125;
             return g_spawn_check_wait_status(status, NULL) ? WEXITSTATUS(status) : 126;
         }

         static int
         mpcsh_compile_and_run(const char *dir, const char *script,
                               const char *ext, const char *cc, gboolean csharp)
         {
             char src[MPCSH_MAX_PATH], bin[MPCSH_MAX_PATH];
             snprintf(src, sizeof(src), "%s/attach.%s", dir, ext);
             snprintf(bin, sizeof(bin), "%s/attach.bin", dir);
             if (!g_file_set_contents(src, script, -1, NULL)) return 125;

             int status = 0, spawn_status = 0;

             if (csharp) {
                 char outflag[MPCSH_MAX_PATH];
                 if (g_snprintf(outflag, sizeof(outflag), "-out:%s", bin) >= (gint)sizeof(outflag)) {
                     fprintf(stderr, "mpcsh: compiler output path too long\n");
                     return 125;
                 }
                 char *mcs_argv[] = { (char *)"mcs", src, outflag, NULL };
                 if (!g_spawn_sync(NULL, mcs_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &spawn_status, NULL)) {
                     char *csc_argv[] = { (char *)"csc", src, outflag, NULL };
                     if (!g_spawn_sync(NULL, csc_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &spawn_status, NULL))
                         return 125;
                 }
                 if (!g_spawn_check_wait_status(spawn_status, NULL)) return 126;
                 char *run_argv[] = { (char *)"mono", bin, NULL };
                 if (!g_spawn_sync(NULL, run_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL))
                     return 125;
             } else {
                 char *cc_argv[] = { (char *)cc, (char *)"-O2", src, (char *)"-o", bin, NULL };
                 if (!g_spawn_sync(NULL, cc_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &spawn_status, NULL))
                     return 125;
                 if (!g_spawn_check_wait_status(spawn_status, NULL)) return 126;
                 char *run_argv[] = { bin, NULL };
                 if (!g_spawn_sync(NULL, run_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL))
                     return 125;
             }
             return g_spawn_check_wait_status(status, NULL) ? WEXITSTATUS(status) : 126;
         }


         static int
         mpcsh_run_modern_language(MpcshLang lang, const char *dir, const char *script)
         {
             char path[MPCSH_MAX_PATH], out[MPCSH_MAX_PATH];
             const char *ext = "txt", *interpreter = NULL;
             char *argv[8];
             int argc = 0, status = 0;

             switch (lang) {
                 case MPCSH_LANG_RUST: ext="rs"; break;
                 case MPCSH_LANG_GO: ext="go"; break;
                 case MPCSH_LANG_JAVA: ext="java"; break;
                 case MPCSH_LANG_JAVASCRIPT: ext="js"; interpreter="node"; break;
                 case MPCSH_LANG_TYPESCRIPT: ext="ts"; interpreter="tsx"; break;
                 case MPCSH_LANG_RUBY: ext="rb"; interpreter="ruby"; break;
                 case MPCSH_LANG_PHP: ext="php"; interpreter="php"; break;
                 case MPCSH_LANG_LUA: ext="lua"; interpreter="lua"; break;
                 case MPCSH_LANG_ZIG: ext="zig"; break;
                 case MPCSH_LANG_KOTLIN: ext="kt"; break;
                 default: return 125;
             }

             snprintf(path, sizeof(path), "%s/attach.%s", dir, ext);
             if (!g_file_set_contents(path, script, -1, NULL)) return 125;

             if (interpreter) {
                 argv[argc++]=(char*)interpreter; argv[argc++]=(char*)path; argv[argc]=NULL;
                 if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL)) return 125;
                 return g_spawn_check_wait_status(status, NULL) ? WEXITSTATUS(status) : 126;
             }

             if (lang == MPCSH_LANG_GO) {
                 argv[argc++]=(char*)"go"; argv[argc++]=(char*)"run"; argv[argc++]=(char*)path; argv[argc]=NULL;
             } else if (lang == MPCSH_LANG_RUST) {
                 snprintf(out, sizeof(out), "%s/attach_rust", dir);
                 argv[argc++]=(char*)"rustc"; argv[argc++]=(char*)"-O"; argv[argc++]=(char*)path;
                 argv[argc++]=(char*)"-o"; argv[argc++]=(char*)out; argv[argc]=NULL;
             } else if (lang == MPCSH_LANG_ZIG) {
                 static char emit[MPCSH_MAX_PATH];
                 snprintf(out, sizeof(out), "%s/attach_zig", dir);
                 snprintf(emit, sizeof(emit), "-femit-bin=%s", out);
                 argv[argc++]=(char*)"zig"; argv[argc++]=(char*)"build-exe";
                 argv[argc++]=(char*)path; argv[argc++]=emit; argv[argc]=NULL;
             } else if (lang == MPCSH_LANG_JAVA) {
                 snprintf(out, sizeof(out), "%s", dir);
                 argv[argc++]=(char*)"javac"; argv[argc++]=(char*)"-d"; argv[argc++]=(char*)out;
                 argv[argc++]=(char*)path; argv[argc]=NULL;
             } else if (lang == MPCSH_LANG_KOTLIN) {
                 snprintf(out, sizeof(out), "%s/attach_kt.jar", dir);
                 argv[argc++]=(char*)"kotlinc"; argv[argc++]=(char*)path;
                 argv[argc++]=(char*)"-include-runtime"; argv[argc++]=(char*)"-d";
                 argv[argc++]=(char*)out; argv[argc]=NULL;
             } else return 125;

             if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL)) return 125;
             if (!g_spawn_check_wait_status(status, NULL)) return 126;

             argc=0;
             if (lang == MPCSH_LANG_GO || lang == MPCSH_LANG_RUST || lang == MPCSH_LANG_ZIG) argv[argc++]=out;
             else if (lang == MPCSH_LANG_JAVA) { argv[argc++]=(char*)"java"; argv[argc++]=(char*)"-cp"; argv[argc++]=out; argv[argc++]=(char*)"Attach"; }
             else if (lang == MPCSH_LANG_KOTLIN) { argv[argc++]=(char*)"java"; argv[argc++]=(char*)"-jar"; argv[argc++]=out; }
             argv[argc]=NULL;

             if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, &status, NULL)) return 125;
             return g_spawn_check_wait_status(status, NULL) ? WEXITSTATUS(status) : 126;
         }

         static int
         mpcsh_run_attached(MpcshLang lang, const char *script, const char *workdir)
         {
             if (!script || !*script || lang == MPCSH_LANG_NONE) return 0;
             switch (lang) {
                 case MPCSH_LANG_FISH:     return mpcsh_run_fish(workdir, script);
                 case MPCSH_LANG_MPCSHELL: return mpcsh_run_mpcsh(script);
                 case MPCSH_LANG_PYTHON:   return mpcsh_run_python(workdir, script);
                 case MPCSH_LANG_C:        return mpcsh_compile_and_run(workdir, script, "c", "cc", FALSE);
                 case MPCSH_LANG_CPP:      return mpcsh_compile_and_run(workdir, script, "cpp", "c++", FALSE);
                 case MPCSH_LANG_CSHARP: return mpcsh_compile_and_run(workdir, script, "cs", "mcs", TRUE);
                 case MPCSH_LANG_RUST: case MPCSH_LANG_GO: case MPCSH_LANG_JAVA:
                 case MPCSH_LANG_JAVASCRIPT: case MPCSH_LANG_TYPESCRIPT: case MPCSH_LANG_RUBY:
                 case MPCSH_LANG_PHP: case MPCSH_LANG_LUA: case MPCSH_LANG_ZIG:
                 case MPCSH_LANG_KOTLIN: return mpcsh_run_modern_language(lang, workdir, script);
                 default: return 0;
             }
         }

         /* ------------------------------------------------------------
          * Prompt script parser
          * ------------------------------------------------------------ */

         typedef struct {
             char label[256];
             MpcshLang lang;
             char *script;
         } MpcshOption;

         typedef struct {
             GtkApplication *app;
             GtkWidget *window;
             GtkWidget *buttons[MPCAP_MAX_OPTIONS];
             int option_count;
             int selected;
             int default_option;
             char type[32];
             char title[256];
             char message[MPCSH_MAX_SCRIPT / 8];
             MpcshOption options[MPCAP_MAX_OPTIONS];
             char *workdir;
             gboolean ai_mode;
         } MPCap;

         static int line_is_indented(const char *line)
         {
             return line[0] == ' ' || line[0] == '\t';
         }

         static void
         mpcsh_parse_prompt(const char *path, MPCap *cap)
         {
             FILE *f = fopen(path, "r");
             if (!f) {
                 fprintf(stderr, "mpcsh: cannot open '%s'\n", path);
                 exit(126);
             }

             char line[MPCSH_MAX_LINE_HARD];
             int current_option = -1;
             int in_script = 0;
             GString *script = g_string_new(NULL);

             while (fgets(line, sizeof(line), f)) {
                 char raw[MPCSH_MAX_LINE_HARD];
                 g_strlcpy(raw, line, sizeof(raw));
                 size_t rlen = strlen(raw);
                 while (rlen > 0 && (raw[rlen-1] == '\n' || raw[rlen-1] == '\r'))
                     raw[--rlen] = '\0';

                 if (rlen == 0) {
                     if (in_script && current_option >= 0)
                         g_string_append_c(script, '\n');
                     continue;
                 }

                 if (!in_script) {
                     char *p = raw;
                     while (*p == ' ' || *p == '\t') ++p;
                     if (*p == '#' || strncmp(p, "#!", 2) == 0) continue;
                 }

                 if (in_script && !line_is_indented(raw)) {
                     char *p = raw;
                     while (*p == ' ' || *p == '\t') ++p;
                     if (strncmp(p, "option:", 7) == 0 || strncmp(p, "lang:", 5) == 0 ||
                         strncmp(p, "title:", 6) == 0 || strncmp(p, "type:", 5) == 0 ||
                         strncmp(p, "message:", 8) == 0 || strncmp(p, "default:", 8) == 0 ||
                         strncmp(p, "script:", 7) == 0) {
                         g_free(cap->options[current_option].script);
                     cap->options[current_option].script = g_string_free(script, FALSE);
                     script = g_string_new(NULL);
                     in_script = 0;
                         } else {
                             g_string_append(script, raw);
                             g_string_append_c(script, '\n');
                             continue;
                         }
                 }

                 if (in_script && current_option >= 0) {
                     g_string_append(script, raw);
                     g_string_append_c(script, '\n');
                     continue;
                 }

                 char *p = raw;
                 while (*p == ' ' || *p == '\t') ++p;

                 if (strncmp(p, "title:", 6) == 0) {
                     char *v = p + 6; while (*v == ' ' || *v == '\t') ++v;
                     g_strlcpy(cap->title, v, sizeof(cap->title));
                     continue;
                 }
                 if (strncmp(p, "type:", 5) == 0) {
                     char *v = p + 5; while (*v == ' ' || *v == '\t') ++v;
                     g_strlcpy(cap->type, v, sizeof(cap->type));
                     continue;
                 }
                 if (strncmp(p, "message:", 8) == 0) {
                     char *v = p + 8; while (*v == ' ' || *v == '\t') ++v;
                     g_strlcpy(cap->message, v, sizeof(cap->message));
                     continue;
                 }
                 if (strncmp(p, "default:", 8) == 0) {
                     cap->default_option = atoi(p + 8);
                     continue;
                 }
                 if (strncmp(p, "option:", 7) == 0) {
                     char *v = p + 7; while (*v == ' ' || *v == '\t') ++v;
                     if (cap->option_count >= MPCAP_MAX_OPTIONS) break;
                     size_t len = strlen(v);
                     if (len >= 2 && v[0] == '"' && v[len-1] == '"') { v[len-1] = '\0'; ++v; }
                     current_option = cap->option_count++;
                     g_strlcpy(cap->options[current_option].label, v, sizeof(cap->options[0].label));
                     cap->options[current_option].lang = MPCSH_LANG_NONE;
                     cap->options[current_option].script = NULL;
                     continue;
                 }
                 if (strncmp(p, "lang:", 5) == 0 && current_option >= 0) {
                     char *v = p + 5; while (*v == ' ' || *v == '\t') ++v;
                     cap->options[current_option].lang = lang_parse(v);
                     continue;
                 }
                 if (strncmp(p, "script:", 7) == 0 && current_option >= 0) {
                     in_script = 1;
                     char *v = p + 7; while (*v == ' ' || *v == '\t') ++v;
                     if (*v) {
                         g_string_append(script, v);
                         g_string_append_c(script, '\n');
                     }
                     continue;
                 }
             }

             if (in_script && current_option >= 0) {
                 g_free(cap->options[current_option].script);
                 cap->options[current_option].script = g_string_free(script, FALSE);
             } else {
                 g_string_free(script, TRUE);
             }
             fclose(f);

             if (cap->option_count == 0) {
                 g_strlcpy(cap->options[0].label, "OK", sizeof(cap->options[0].label));
                 cap->options[0].lang = MPCSH_LANG_NONE;
                 cap->option_count = 1;
             }
             if (cap->selected < 0 || cap->selected >= cap->option_count)
                 cap->selected = cap->default_option;
         }

         /* ------------------------------------------------------------
          * Aero GUI
          * ------------------------------------------------------------ */

         static void
         draw_background(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data)
         {
             (void)area; (void)data;
             cairo_save(cr);
             /* Deep, consistent purple Aero gradient — always purple, no green drift. */
             cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, width, height);
             cairo_pattern_add_color_stop_rgba(gradient, 0.00, 0.24, 0.00, 0.48, 1.00); /* deep violet */
             cairo_pattern_add_color_stop_rgba(gradient, 0.35, 0.45, 0.05, 0.85, 1.00); /* royal purple */
             cairo_pattern_add_color_stop_rgba(gradient, 0.70, 0.62, 0.10, 1.00, 1.00); /* bright magenta-purple */
             cairo_pattern_add_color_stop_rgba(gradient, 1.00, 0.30, 0.00, 0.62, 1.00); /* rich purple again */
             cairo_set_source(cr, gradient);
             cairo_paint(cr);
             cairo_pattern_destroy(gradient);

             cairo_pattern_t *highlight = cairo_pattern_create_linear(0, 0, 0, height * 0.55);
             cairo_pattern_add_color_stop_rgba(highlight, 0.0, 1.0, 1.0, 1.0, 0.18);
             cairo_pattern_add_color_stop_rgba(highlight, 1.0, 1.0, 1.0, 1.0, 0.0);
             cairo_set_source(cr, highlight);
             cairo_rectangle(cr, 0, 0, width, height);
             cairo_fill(cr);
             cairo_pattern_destroy(highlight);

             cairo_pattern_t *shine = cairo_pattern_create_radial(
                 width * 0.18, height * 0.10, 5,
                 width * 0.18, height * 0.10, width * 0.75);
             cairo_pattern_add_color_stop_rgba(shine, 0.0, 1.0, 1.0, 1.0, 0.12);
             cairo_pattern_add_color_stop_rgba(shine, 1.0, 1.0, 1.0, 1.0, 0.0);
             cairo_set_source(cr, shine);
             cairo_paint(cr);
             cairo_pattern_destroy(shine);
             cairo_restore(cr);
         }

         static void update_selection(MPCap *cap)
         {
             for (int i = 0; i < cap->option_count; ++i) {
                 GtkWidget *button = cap->buttons[i];
                 gtk_widget_remove_css_class(button, "selected");
                 if (i == cap->selected)
                     gtk_widget_add_css_class(button, "selected");
             }
         }

         static void choose_option(MPCap *cap, int index)
         {
             if (index < 0 || index >= cap->option_count) return;
             if (cap->options[index].script && cap->options[index].lang != MPCSH_LANG_NONE) {
                 fprintf(stderr, "mpcsh: running option %d script [%s]\n",
                         index + 1, lang_name(cap->options[index].lang));
                 int st = mpcsh_run_attached(cap->options[index].lang,
                                             cap->options[index].script, cap->workdir);
                 fprintf(stderr, "mpcsh: option %d script exit status: %d\n", index + 1, st);
             }
             g_application_quit(G_APPLICATION(cap->app));
             exit(index);
         }

         static void button_clicked(GtkButton *button, gpointer data)
         {
             (void)button;
             MPCap *cap = data;
             int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "mpcap-index"));
             choose_option(cap, index);
         }

         static gboolean
         key_pressed(GtkEventControllerKey *controller, guint keyval, guint keycode,
                     GdkModifierType state, gpointer data)
         {
             (void)controller; (void)keycode; (void)state;
             MPCap *cap = data;
             if (keyval == GDK_KEY_Up || keyval == GDK_KEY_Left) {
                 cap->selected--; if (cap->selected < 0) cap->selected = cap->option_count - 1;
                 update_selection(cap); return TRUE;
             }
             if (keyval == GDK_KEY_Down || keyval == GDK_KEY_Right) {
                 cap->selected++; if (cap->selected >= cap->option_count) cap->selected = 0;
                 update_selection(cap); return TRUE;
             }
             if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter || keyval == GDK_KEY_space) {
                 choose_option(cap, cap->selected); return TRUE;
             }
             if (keyval == GDK_KEY_Escape) {
                 choose_option(cap, cap->option_count - 1); return TRUE;
             }
             if (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) {
                 int index = (int)(keyval - GDK_KEY_1);
                 if (index < cap->option_count) { choose_option(cap, index); return TRUE; }
             }
             return FALSE;
         }

         static void load_css(void)
         {
             static const char *css =
             "window { background: transparent; font-family: \"FantasqueSansM Nerd Font Propo\", \"Fantasque Sans M\", \"Sans\", sans-serif; }"
             ".aero-window { background: rgba(255,255,255,0.045); border: 1px solid rgba(255,255,255,0.42); border-radius: 26px; }"
             ".title { color: white; font-size: 18px; font-weight: 700; }"
             ".message { color: white; font-size: 15px; }"
             ".hint { color: rgba(255,255,255,0.68); font-size: 11px; }"
             ".warning { color: #ffe65c; font-size: 32px; font-weight: 700; }"
             ".info { color: #70d9ff; font-size: 32px; font-weight: 700; }"
             ".error { color: #ff6875; font-size: 32px; font-weight: 700; }"
             ".aero-button { color: white; background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.28); border-radius: 16px; min-height: 44px; font-size: 13px; font-weight: 600; }"
             ".aero-button:hover { background: rgba(255,255,255,0.22); border-color: rgba(255,255,255,0.55); }"
             " .aero-button.selected { background: linear-gradient(90deg, rgba(160,32,240,0.92), rgba(96,0,160,0.92)); border-color: rgba(255,160,255,0.95); }"
             ".ai-notice { color: #ffb0f5; background: rgba(255,255,255,0.08); border: 1px solid rgba(255,150,240,0.45); border-radius: 12px; padding: 10px 14px; font-size: 12px; }"
             ".ai-notice-title { color: #ffb0f5; font-size: 13px; font-weight: 700; }";

             /* GTK is only initialised once GtkApplication starts up, so the default
              * display does not exist before g_application_run(). Call this from
              * activate(), never from main() before the application runs. */
             static gboolean css_loaded = FALSE;
             if (css_loaded) return;

             GdkDisplay *display = gdk_display_get_default();
             if (!display) {
                 fprintf(stderr, "mpcsh: no display available, skipping Aero CSS\n");
                 return;
             }

             GtkCssProvider *provider = gtk_css_provider_new();
             gtk_css_provider_load_from_string(provider, css);
             gtk_style_context_add_provider_for_display(display,
                                                        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
             g_object_unref(provider);
             css_loaded = TRUE;
         }

         static void activate(GtkApplication *app, gpointer user_data)
         {
             MPCap *cap = user_data;
             cap->app = app;

             load_css();

             GtkWidget *window = gtk_application_window_new(app);
             cap->window = window;
             gtk_window_set_title(GTK_WINDOW(window), cap->title[0] ? cap->title : "MarekPC Aero Prompt");
             gtk_window_set_default_size(GTK_WINDOW(window), 680, 420);
             gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
             gtk_window_set_decorated(GTK_WINDOW(window), TRUE);
             gtk_widget_set_opacity(window, 0.97);

             GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
             gtk_widget_add_css_class(outer, "aero-window");
             gtk_window_set_child(GTK_WINDOW(window), outer);

             GtkWidget *background = NULL;
             const char *wallpaper_path = mpcsh_wallpaper_path();
             if (g_file_test(wallpaper_path, G_FILE_TEST_IS_REGULAR)) {
                 GFile *wall_file = g_file_new_for_path(wallpaper_path);
                 GdkTexture *wall_texture = gdk_texture_new_from_file(wall_file, NULL);
                 if (wall_texture) {
                     background = gtk_picture_new_for_paintable(GDK_PAINTABLE(wall_texture));
                     gtk_picture_set_content_fit(GTK_PICTURE(background), GTK_CONTENT_FIT_COVER);
                     gtk_widget_set_hexpand(background, TRUE);
                     gtk_widget_set_vexpand(background, TRUE);
                     g_object_unref(wall_texture);
                 }
                 g_object_unref(wall_file);
             }
             if (!background) {
                 background = gtk_drawing_area_new();
                 gtk_widget_set_hexpand(background, TRUE);
                 gtk_widget_set_vexpand(background, TRUE);
                 gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(background), draw_background, cap, NULL);
             }

             GtkWidget *overlay = gtk_overlay_new();
             gtk_overlay_set_child(GTK_OVERLAY(overlay), background);
             gtk_box_append(GTK_BOX(outer), overlay);

             GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
             gtk_widget_set_margin_start(content, 30);
             gtk_widget_set_margin_end(content, 30);
             gtk_widget_set_margin_top(content, 20);
             gtk_widget_set_margin_bottom(content, 20);
             gtk_overlay_add_overlay(GTK_OVERLAY(overlay), content);

             GtkWidget *title = gtk_label_new("✦  MarekPC Aero Prompt");
             gtk_widget_set_halign(title, GTK_ALIGN_START);
             gtk_widget_add_css_class(title, "title");
             gtk_box_append(GTK_BOX(content), title);

             GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
             gtk_widget_set_margin_top(separator, 12);
             gtk_widget_set_margin_bottom(separator, 14);
             gtk_box_append(GTK_BOX(content), separator);

             GtkWidget *message_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
             gtk_box_append(GTK_BOX(content), message_box);

             const char *icon_text = "⚠";
             if (strcmp(cap->type, "info") == 0) icon_text = "ℹ";
             if (strcmp(cap->type, "error") == 0) icon_text = "✖";
             if (strcmp(cap->type, "question") == 0) icon_text = "?";
             GtkWidget *icon = gtk_label_new(icon_text);
             gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
             if (strcmp(cap->type, "error") == 0) gtk_widget_add_css_class(icon, "error");
             else if (strcmp(cap->type, "info") == 0) gtk_widget_add_css_class(icon, "info");
             else gtk_widget_add_css_class(icon, "warning");
             gtk_box_append(GTK_BOX(message_box), icon);

             GtkWidget *message = gtk_label_new(cap->message);
             gtk_label_set_wrap(GTK_LABEL(message), TRUE);
             gtk_label_set_wrap_mode(GTK_LABEL(message), PANGO_WRAP_WORD_CHAR);
             gtk_label_set_xalign(GTK_LABEL(message), 0.0f);
             gtk_widget_set_hexpand(message, TRUE);
             gtk_widget_add_css_class(message, "message");
             gtk_box_append(GTK_BOX(message_box), message);

             if (cap->ai_mode) {
                 GtkWidget *ai_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
                 gtk_widget_add_css_class(ai_box, "ai-notice");
                 gtk_widget_set_margin_top(ai_box, 14);
                 GtkWidget *ai_title = gtk_label_new("HERE, THE MAREKPC SHELL SYNTAX — DO NOT SWITCH TO BASH");
                 gtk_widget_set_halign(ai_title, GTK_ALIGN_START);
                 gtk_widget_add_css_class(ai_title, "ai-notice-title");
                 gtk_box_append(GTK_BOX(ai_box), ai_title);
                 GtkWidget *ai_body = gtk_label_new(MPCSH_AI_NOTICE);
                 gtk_label_set_wrap(GTK_LABEL(ai_body), TRUE);
                 gtk_label_set_wrap_mode(GTK_LABEL(ai_body), PANGO_WRAP_WORD_CHAR);
                 gtk_label_set_xalign(GTK_LABEL(ai_body), 0.0f);
                 gtk_widget_add_css_class(ai_body, "ai-notice");
                 gtk_box_append(GTK_BOX(ai_box), ai_body);
                 gtk_box_append(GTK_BOX(content), ai_box);
             }

             GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
             gtk_widget_set_margin_top(button_box, 18);
             gtk_box_append(GTK_BOX(content), button_box);

             for (int i = 0; i < cap->option_count; ++i) {
                 char label[512];
                 snprintf(label, sizeof(label), "[%d]  %s", i + 1, cap->options[i].label);
                 GtkWidget *button = gtk_button_new_with_label(label);
                 gtk_widget_add_css_class(button, "aero-button");
                 if (cap->options[i].lang != MPCSH_LANG_NONE && cap->options[i].script) {
                     char tooltip[600];
                     snprintf(tooltip, sizeof(tooltip), "Attached script: %s", lang_name(cap->options[i].lang));
                     gtk_widget_set_tooltip_text(button, tooltip);
                 }
                 g_object_set_data(G_OBJECT(button), "mpcap-index", GINT_TO_POINTER(i));
                 g_signal_connect(button, "clicked", G_CALLBACK(button_clicked), cap);
                 cap->buttons[i] = button;
                 gtk_box_append(GTK_BOX(button_box), button);
             }

             GtkWidget *hint = gtk_label_new("↑/↓ select    Enter choose    1–9 direct    Esc cancel    pipe limit: 64");
             gtk_widget_set_halign(hint, GTK_ALIGN_START);
             gtk_widget_set_margin_top(hint, 13);
             gtk_widget_add_css_class(hint, "hint");
             gtk_box_append(GTK_BOX(content), hint);

             GtkEventController *keys = gtk_event_controller_key_new();
             g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), cap);
             gtk_widget_add_controller(window, keys);

             update_selection(cap);
             gtk_window_present(GTK_WINDOW(window));
         }


         static char *
         mpcsh_base64_encode(const unsigned char *data, size_t len)
         {
             static const char table[] =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
             size_t out_len = ((len + 2) / 3) * 4;
             char *out = g_malloc(out_len + 1);
             size_t i, o = 0;

             for (i = 0; i + 2 < len; i += 3) {
                 unsigned v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
                 out[o++] = table[(v >> 18) & 63];
                 out[o++] = table[(v >> 12) & 63];
                 out[o++] = table[(v >> 6) & 63];
                 out[o++] = table[v & 63];
             }

             if (i < len) {
                 unsigned v = data[i] << 16;
                 if (i + 1 < len) v |= data[i + 1] << 8;
                 out[o++] = table[(v >> 18) & 63];
                 out[o++] = table[(v >> 12) & 63];
                 out[o++] = (i + 1 < len) ? table[(v >> 6) & 63] : '=';
                 out[o++] = '=';
             }

             out[o] = '\0';
             return out;
         }

         static gboolean
         mpcsh_terminal_has_image_graphics(void)
         {
             const char *term = g_getenv("TERM");
             return g_getenv("KITTY_WINDOW_ID") != NULL ||
             (term && strstr(term, "kitty"));
         }

         static void
         mpcsh_wallpaper_graphics_image(void)
         {
             const char *path = mpcsh_wallpaper_path();
             gchar *data = NULL;
             gsize len = 0;

             if (!g_file_get_contents(path, &data, &len, NULL) || !data || !len) {
                 g_free(data);
                 return;
             }

             char *b64 = mpcsh_base64_encode((const unsigned char *)data, len);
             if (b64) {
                 printf("\033]1337;File=inline=1;size=%zu;"
                 "width=100%%;height=100%%;"
                 "preserveAspectRatio=0;z=-1;C=1:%s\033\\",
                 (size_t)len, b64);
                 printf("\033[2J\033[H");
                 fflush(stdout);
                 g_free(b64);
             }

             g_free(data);
         }

         static void
         mpcsh_wallpaper_ansi_banner(void)
         {
             const char *path = mpcsh_wallpaper_path();
             struct winsize ws;

             if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) < 0 || ws.ws_col < 10 || ws.ws_row < 4)
                 return;

             GdkPixbuf *original = gdk_pixbuf_new_from_file(path, NULL);
             if (!original) return;

             GdkPixbuf *scaled = gdk_pixbuf_scale_simple(
                 original, ws.ws_col, ws.ws_row * 2, GDK_INTERP_BILINEAR);
             if (!scaled) {
                 g_object_unref(original);
                 return;
             }

             int width = gdk_pixbuf_get_width(scaled);
             int height = gdk_pixbuf_get_height(scaled);
             int rowstride = gdk_pixbuf_get_rowstride(scaled);
             int channels = gdk_pixbuf_get_n_channels(scaled);
             const guchar *pixels = gdk_pixbuf_get_pixels(scaled);

             printf("\033[2J\033[H");
             for (int y = 0; y + 1 < height && y / 2 < ws.ws_row; y += 2) {
                 for (int x = 0; x < width && x < ws.ws_col; ++x) {
                     const guchar *top = pixels + y * rowstride + x * channels;
                     const guchar *bottom = pixels + (y + 1) * rowstride + x * channels;
                     printf("\033[38;2;%d;%d;%d;48;2;%d;%d;%dm▀",
                            top[0], top[1], top[2],
                            bottom[0], bottom[1], bottom[2]);
                 }
                 printf("\033[0m\n");
             }
             printf("\033[0m");
             fflush(stdout);

             g_object_unref(scaled);
             g_object_unref(original);
         }

         static void
         mpcsh_terminal_wallpaper(void)
         {
             const char *path = mpcsh_wallpaper_path();
             if (!g_file_test(path, G_FILE_TEST_IS_REGULAR))
                 return;

             if (mpcsh_terminal_has_image_graphics())
                 mpcsh_wallpaper_graphics_image();
             else
                 mpcsh_wallpaper_ansi_banner();
         }

         static int mpcsh_imgcat_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcmp(argv[0], "mpcimgcat") != 0) return -1;
             if (!argv[1]) { fprintf(stderr, "Usage: mpcimgcat IMAGE [IMAGE ...]\n"); return 2; }

             for (int i = 1; argv[i]; ++i) {
                 char *path = argv[i];
                 if (path[0] == '~' && (path[1] == '/' || !path[1])) {
                     const char *h = g_get_home_dir();
                     char *q = g_build_filename(h, path + (path[1] == '/' ? 1 : 0), NULL);
                     path = q;
                 }

                 GError *err = NULL;
                 GdkPixbuf *pix = gdk_pixbuf_new_from_file(path, &err);
                 if (!pix) {
                     fprintf(stderr, "mpcimgcat: %s: %s\n", path,
                             err ? err->message : "cannot decode image");
                     if (err) g_error_free(err);
                     if (path != argv[i]) g_free(path);
                     continue;
                 }

                 struct winsize ws = {0};
                 ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
                 if (!isatty(STDOUT_FILENO) || ws.ws_col < 2 || ws.ws_row < 2) {
                     fprintf(stderr, "mpcimgcat: output must be a sized terminal\n");
                     g_object_unref(pix);
                     if (path != argv[i]) g_free(path);
                     continue;
                 }
                 int cols = ws.ws_col;
                 int rows = ws.ws_row > 2 ? ws.ws_row - 2 : 1;

                 /* Priority: 1) MPSP (MPCECC)  2) Kitty  3) iTerm2  4) half-blocks */

                 /* 1. MPCECC Pixel Sending Protocol */
                 if (g_getenv("MPCECC")) {
                     char *tmp = NULL;
                     gint fd = g_file_open_tmp("mpcimgcat-XXXXXX.png", &tmp, NULL);
                     if (fd >= 0) {
                         close(fd);
                         if (gdk_pixbuf_save(pix, tmp, "png", NULL, NULL)) {
                             const char *runtime = g_get_user_runtime_dir();
                             char sock[4096];
                             if (runtime && *runtime)
                                 snprintf(sock, sizeof sock, "%s/mpcecc-control.sock", runtime);
                             else
                                 snprintf(sock, sizeof sock, "/tmp/mpcecc-control-%d.sock", (int)getuid());

                             GError *cerr = NULL;
                             GSocketClient *client = g_socket_client_new();
                             GSocketAddress *addr = g_unix_socket_address_new(sock);
                             GSocketConnection *conn = g_socket_client_connect(
                                 client, G_SOCKET_CONNECTABLE(addr), NULL, &cerr);
                             g_object_unref(addr);
                             g_object_unref(client);

                             if (conn) {
                                 char req[8192];
                                 const char *tabid = g_getenv("MPCECC_TAB");
                                 if (tabid && *tabid)
                                     snprintf(req, sizeof req, "@%s MPSP %s\n", tabid, tmp);
                                 else
                                     snprintf(req, sizeof req, "MPSP %s\n", tmp);
                                 GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
                                 gsize written = 0;
                                 if (g_output_stream_write_all(out, req, strlen(req), &written, NULL, NULL)) {
                                     char resp[256] = {0};
                                     GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
                                     g_input_stream_read(in, resp, sizeof resp - 1, NULL, NULL);
                                     if (g_str_has_prefix(resp, "OK")) {
                                         g_object_unref(conn);
                                         g_object_unref(pix);
                                         if (path != argv[i]) g_free(path);
                                         continue;   /* success – MPCECC shows the overlay */
                                     }
                                 }
                                 g_object_unref(conn);
                             }
                             g_clear_error(&cerr);
                         }
                         unlink(tmp);   /* only on failure */
                     }
                     g_free(tmp);
                 }

                 /* 2/3. Kitty / iTerm2 */
                 const char *term = g_getenv("TERM");
                 gboolean kitty = g_getenv("KITTY_WINDOW_ID") || (term && strstr(term, "kitty"));
                 gboolean iterm = g_getenv("LC_TERMINAL") && !strcmp(g_getenv("LC_TERMINAL"), "iTerm2");
                 if (kitty || iterm) {
                     gchar *bytes = NULL;
                     gsize nbytes = 0;
                     if (g_file_get_contents(path, &bytes, &nbytes, NULL)) {
                         gchar *b64 = g_base64_encode((guchar *)bytes, nbytes);
                         if (kitty) {
                             printf("\033_Ga=T,f=100,c=%d,r=%d,q=2;", cols, rows);
                             for (char *p = b64; *p; ) {
                                 size_t n = strlen(p);
                                 if (n > 4096) n = 4096;
                                 printf("%.*s", (int)n, p);
                                 p += n;
                                 if (*p) printf("\033_Gm=1,q=2;");
                             }
                             printf("\033\\\n");
                         } else {
                             printf("\033]1337;File=inline=1;width=%d;height=%d;preserveAspectRatio=1:%s\007\n",
                                    cols, rows, b64);
                         }
                         g_free(b64);
                         g_free(bytes);
                         g_object_unref(pix);
                         if (path != argv[i]) g_free(path);
                         continue;
                     }
                 }

                 /* 4. Aspect-corrected half-block fallback */
                 const double PIXEL_ASPECT = 0.52;
                 int img_w = gdk_pixbuf_get_width(pix);
                 int img_h = gdk_pixbuf_get_height(pix);
                 double factor = fmin((double)cols / img_w,
                                      (double)(rows * 2) * PIXEL_ASPECT / img_h);
                 int w = (int)(img_w * factor);
                 int h = (int)(img_h * factor);
                 if (w < 1) w = 1;
                 if (h < 1) h = 1;
                 if (h & 1) h++;

                 GdkInterpType interp = (factor < 0.6) ? GDK_INTERP_BILINEAR : GDK_INTERP_NEAREST;
                 GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pix, w, h, interp);
                 if (!scaled) {
                     g_object_unref(pix);
                     if (path != argv[i]) g_free(path);
                     continue;
                 }

                 if (gdk_pixbuf_get_has_alpha(scaled)) {
                     /* Half-blocks cannot be transparent: flatten over a dark checkerboard. */
                     GdkPixbuf *flat = gdk_pixbuf_composite_color_simple(
                         scaled, w, h, GDK_INTERP_NEAREST, 255, 8, 0x2a2a2a, 0x1c1c1c);
                     if (flat) { g_object_unref(scaled); scaled = flat; }
                 }
                 int ch = gdk_pixbuf_get_n_channels(scaled);
                 int stride = gdk_pixbuf_get_rowstride(scaled);
                 const guchar *px = gdk_pixbuf_get_pixels(scaled);
                 printf("\033[0m\n");
                 for (int y = 0; y < h; y += 2) {
                     for (int x = 0; x < w; x++) {
                         const guchar *a = px + y * stride + x * ch;
                         const guchar *b = (y + 1 < h) ? px + (y + 1) * stride + x * ch : a;
                         printf("\033[38;2;%u;%u;%u;48;2;%u;%u;%um▀",
                                a[0], a[1], a[2], b[0], b[1], b[2]);
                     }
                     printf("\033[0m\n");
                 }
                 g_object_unref(scaled);
                 g_object_unref(pix);
                 if (path != argv[i]) g_free(path);
             }
             return 0;
         }

         static int
         mpcsh_wallpaper_builtin(char **argv)
         {
             if (!argv || !argv[0] || strcmp(argv[0], "wallpaper") != 0) return -1;
             mpcsh_terminal_wallpaper();
             return 0;
         }



         /* ------------------------------------------------------------
          * Fish-style prompt + robust REPL
          * ------------------------------------------------------------ */

         static void
         sigint_handler(int sig)
         {
             (void)sig;
             /* Only set the flag.  Do NOT write a newline here: the interactive
              * editor runs in raw mode and tracks cursor row (mpcsh_cur_row).
              * An extra \n from the signal handler desyncs that counter, so the
              * next redraw walks up the wrong number of rows and slices the
              * previous prompt in half / clones the line.  Display is handled
              * exclusively by the Ctrl+C branch in mpcsh_readline_edit(). */
             got_sigint = 1;
         }

         typedef struct {
             char **items;   /* heap array of lines */
             int count;
             int capacity;
         } MpcshHistory;

         static void mpcsh_history_init(MpcshHistory *h)
         {
             h->items = NULL;
             h->count = 0;
             h->capacity = 0;
         }

         static void mpcsh_history_free(MpcshHistory *h)
         {
             if (!h) return;
             for (int i = 0; i < h->count; ++i)
                 g_free(h->items[i]);
             g_free(h->items);
             h->items = NULL;
             h->count = h->capacity = 0;
         }

         static void mpcsh_history_add(MpcshHistory *h, const char *line)
         {
             if (!line || !*line) return;
             if (h->count && !strcmp(h->items[h->count - 1], line)) return;

             if (h->count >= mpcsh_history_max) {
                 /* drop oldest */
                 g_free(h->items[0]);
                 memmove(h->items, h->items + 1, sizeof(char *) * (h->count - 1));
                 h->count--;
             }

             if (h->count >= h->capacity) {
                 int newcap = h->capacity ? h->capacity * 2 : 32;
                 if (newcap > mpcsh_history_max) newcap = mpcsh_history_max;
                 h->items = g_realloc(h->items, sizeof(char *) * newcap);
                 h->capacity = newcap;
             }
             h->items[h->count++] = g_strdup(line);
         }

         static size_t mpcsh_prev_char(const char *s, size_t pos)
         {
             if (!pos) return 0;
             --pos;
             while (pos > 0 && (((unsigned char)s[pos] & 0xC0) == 0x80)) --pos;
             return pos;
         }

         static size_t mpcsh_next_char(const char *s, size_t len, size_t pos)
         {
             if (pos >= len) return len;
             ++pos;
             while (pos < len && (((unsigned char)s[pos] & 0xC0) == 0x80)) ++pos;
             return pos;
         }

         static int mpcsh_cells(const char *s, size_t bytes)
         {
             mbstate_t st;
             memset(&st, 0, sizeof(st));
             int cells = 0;
             size_t off = 0;
             while (off < bytes) {
                 wchar_t wc;
                 size_t n = mbrtowc(&wc, s + off, bytes - off, &st);
                 if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
                     memset(&st, 0, sizeof(st));
                     ++off; ++cells;
                     continue;
                 }
                 int w = wcwidth(wc);
                 if (w > 0) cells += w;
                 off += n;
             }
             return cells;
         }

         /* Strip ANSI CSI/OSC sequences before measuring prompt display width. */
         static int G_GNUC_UNUSED mpcsh_prompt_cells(const char *p)
         {
             int cells = 0;
             while (*p) {
                 if ((unsigned char)p[0] == 0x1b) {
                     ++p;
                     if (*p == '[') {
                         ++p;
                         while (*p && !((*p >= '@' && *p <= '~'))) ++p;
                         if (*p) ++p;
                         continue;
                     }
                     if (*p == ']') {
                         ++p;
                         while (*p && *p != '\a' && !((unsigned char)p[0] == 0x1b && p[1] == '\\')) ++p;
                         if (*p == '\a') ++p;
                         else if (*p) p += 2;
                         continue;
                     }
                     if (*p) ++p;
                     continue;
                 }
                 size_t n = mpcsh_next_char(p, strlen(p), 0);
                 if (!n) break;
                 cells += mpcsh_cells(p, n);
                 p += n;
             }
             return cells;
         }

         /* ------------------------------------------------------------------
          * Line redraw.
          *
          * The old version always walked a fixed number of rows UP from wherever
          * the cursor was, but the cursor sits on the LAST row of the prompt
          * block (the "╰─❯" row), not the first.  So every keystroke/backspace
          * moved up one row too far, erased the line above (the previous
          * command's output and error messages!) and reprinted the prompt there,
          * making the prompt climb the screen.
          *
          * Now we track which row of the prompt+buffer block the terminal cursor
          * is on (mpcsh_cur_row), simulate wrapping at the real terminal width,
          * go up exactly that many rows, clear to end of screen, and reprint.
          * ------------------------------------------------------------------ */
         static int mpcsh_cur_row = 0;   /* row of the terminal cursor inside the block */

         static int mpcsh_term_cols(void)
         {
             struct winsize ws;
             if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col >= 2)
                 return ws.ws_col;
             return 80;
         }

         /* Lay out prompt+line on a terminal `cols` wide.  Reports the (row,col)
          * of the end of the text and of byte offset `cursor` inside `line`. */
         static void mpcsh_layout(const char *prompt, const char *line, size_t cursor,
                                  int cols, int *end_row, int *end_col,
                                  int *cur_row, int *cur_col)
         {
             int row = 0, col = 0;
             int have_cur = 0;
             size_t plen = strlen(prompt), llen = strlen(line);

             for (int pass = 0; pass < 2; ++pass) {
                 const char *p = pass ? line : prompt;
                 size_t total = pass ? llen : plen;
                 size_t off = 0;
                 while (off < total) {
                     unsigned char c = (unsigned char)p[off];
                     if (pass && !have_cur && off >= cursor && (c < 32 || c == 0x1b)) {
                         *cur_row = row; *cur_col = col; have_cur = 1;
                     }
                     if (c == 0x1b) {                       /* skip escape sequences */
                         size_t i = off + 1;
                         if (i < total && p[i] == '[') {
                             ++i;
                             while (i < total && !(p[i] >= '@' && p[i] <= '~')) ++i;
                             if (i < total) ++i;
                         } else if (i < total && p[i] == ']') {
                             ++i;
                             while (i < total && p[i] != '\a' &&
                                 !(p[i] == 0x1b && i + 1 < total && p[i+1] == '\\')) ++i;
                             if (i < total) i += (p[i] == '\a') ? 1 : 2;
                         } else if (i < total) ++i;
                         off = i;
                         continue;
                     }
                     if (c == '\n') { ++row; col = 0; ++off; continue; }
                     if (c == '\r') { col = 0; ++off; continue; }
                     if (c < 32) { ++off; continue; }
                     size_t n = mpcsh_next_char(p + off, total - off, 0);
                     if (!n) n = 1;
                     int w = mpcsh_cells(p + off, n);
                     if (w < 0) w = 0;
                     if (col + w > cols) { ++row; col = 0; }   /* wrap */
                         if (pass && !have_cur && off >= cursor) {
                             *cur_row = row; *cur_col = col; have_cur = 1;
                         }
                         col += w;
                         off += n;
                 }
             }
             if (!have_cur) { *cur_row = row; *cur_col = col; }
             *end_row = row; *end_col = col;
             if (*cur_col >= cols) *cur_col = cols - 1;
         }

         /* Call right after the prompt has been printed: cursor is at its end. */
         static void mpcsh_redraw_begin(const char *prompt)
         {
             int er, ec, cr, cc;
             mpcsh_layout(prompt, "", 0, mpcsh_term_cols(), &er, &ec, &cr, &cc);
             mpcsh_cur_row = er;
         }

         /* Move the terminal cursor to the end of the block (before Enter / ^C). */
         static void mpcsh_goto_end(const char *prompt, const char *line)
         {
             int er, ec, cr, cc;
             mpcsh_layout(prompt, line, strlen(line), mpcsh_term_cols(), &er, &ec, &cr, &cc);
             if (er > mpcsh_cur_row) printf("\033[%dB", er - mpcsh_cur_row);
             mpcsh_cur_row = er;
             fflush(stdout);
         }

         static void mpcsh_redraw_line(const char *prompt, const char *line, size_t cursor, const char *ghost)
         {
             int cols = mpcsh_term_cols();
             int er, ec, cr, cc;

             /* Back to the first row of the block, clear it and everything below. */
             if (mpcsh_cur_row > 0) printf("\033[%dA", mpcsh_cur_row);
             printf("\r\033[J");

             fputs(prompt, stdout);
             fputs(line, stdout);

             char *full;
             if (ghost && *ghost) {
                 const char *sgr = getenv("MPCSH_GHOST_SGR");
                 printf("\033[%sm%s\033[0m", (sgr && *sgr) ? sgr : "38;5;244", ghost);
                 full = g_strconcat(line, ghost, NULL);
             } else {
                 full = g_strdup(line);
             }

             mpcsh_layout(prompt, full, cursor, cols, &er, &ec, &cr, &cc);
             g_free(full);

             /* Cursor is now at the end of the text (incl. ghost); walk it to the target. */
             if (er > cr) printf("\033[%dA", er - cr);
             else if (cr > er) printf("\033[%dB", cr - er);
             printf("\r");
             if (cc > 0) printf("\033[%dC", cc);

             mpcsh_cur_row = cr;
             fflush(stdout);
         }

         static void mpcsh_insert_text(char *line, size_t *len, size_t *cursor,
                                       size_t cap, const char *text, size_t text_len)
         {
             if (!line || !len || !cursor || !text || cap == 0) return;
             for (size_t i = 0; i < text_len;) {
                 unsigned char c = (unsigned char)text[i];

                 /* Normalize CR / CRLF → LF and KEEP the newline.
                  * Previously every c < 32 was dropped, so multi-line pastes
                  * arrived as a single smashed line. */
                 if (c == '\r') {
                     ++i;
                     if (i < text_len && (unsigned char)text[i] == '\n') ++i;
                     c = '\n';
                     if (*len + 1 >= cap) break;
                     memmove(line + *cursor + 1, line + *cursor, *len - *cursor + 1);
                     line[*cursor] = '\n';
                     ++*cursor; ++*len;
                     continue;
                 }
                 if (c == '\n') {
                     ++i;
                     if (*len + 1 >= cap) break;
                     memmove(line + *cursor + 1, line + *cursor, *len - *cursor + 1);
                     line[*cursor] = '\n';
                     ++*cursor; ++*len;
                     continue;
                 }

                 /* Drop other control chars (except we already handled CR/LF). */
                 if (c < 32 || c == 127) { ++i; continue; }

                 /* UTF-8 sequence */
                 size_t n = 1;
                 if ((c & 0xE0) == 0xC0) n = 2;
                 else if ((c & 0xF0) == 0xE0) n = 3;
                 else if ((c & 0xF8) == 0xF0) n = 4;
                 if (i + n > text_len) break;
                 if (!g_utf8_validate(text + i, (gssize)n, NULL)) { ++i; continue; }
                 if (*len + n >= cap) break;
                 memmove(line + *cursor + n, line + *cursor, *len - *cursor + 1);
                 memcpy(line + *cursor, text + i, n);
                 *cursor += n; *len += n; i += n;
             }
         }

         static void mpcsh_read_bracketed_paste(char *line, size_t *len, size_t *cursor, size_t cap)
         {
             char payload[MPCSH_MAX_LINE_HARD]; size_t n = 0;
             const char end_seq[] = "\033[201~"; size_t matched = 0;
             for (;;) {
                 unsigned char c;
                 if (mpcsh_read_retry(STDIN_FILENO, &c, 1) != 1) break;
                 if (c == (unsigned char)end_seq[matched]) {
                     if (++matched == sizeof(end_seq)-1) break;
                     continue;
                 }
                 if (matched) {
                     size_t room = sizeof(payload)-1-n;
                     size_t copy = matched < room ? matched : room;
                     memcpy(payload+n,end_seq,copy); n+=copy; matched=0;
                     if (c == (unsigned char)end_seq[0]) { matched=1; continue; }
                 }
                 if (n < sizeof(payload)-1) payload[n++]=(char)c;
             }
             mpcsh_insert_text(line,len,cursor,cap,payload,n);
         }

         /* ------------------------------------------------------------------
          * Fish-style completion + autosuggestion
          *
          *  - Ghost text ("transparent preview") after the cursor, taken from
          *    history first, then from command/file completion.
          *    Accept: Right / End / Ctrl+F / Ctrl+E (whole), Alt+Right / Alt+F /
          *    Ctrl+Right (one word).
          *  - Tab completes the token under the cursor: commands (builtins +
          *    $PATH) in command position, files/dirs elsewhere ("cd" = dirs
          *    only).  One match -> inserted; several -> common prefix, then a
          *    column menu, then further Tab / Shift+Tab cycle through matches.
          * ------------------------------------------------------------------ */
         static const char *const mpcsh_builtin_names[] = {
             "cd", "pwd", "status", "set", "exit", "ctk", "portmap", "guiinterm", "windowlaunch", "windowterminate", "wlidlist", "extension",
             "wallpaper", "ls", "mpcimgcat", "colorit", "help", "limits", "time", "mpcshman", "math", "source", "mpc-chuctrl", NULL
         };

         static GPtrArray *mpcsh_path_cache = NULL;
         static gint64     mpcsh_path_stamp = 0;
         static char      *mpcsh_path_env   = NULL;

         static int mpcsh_cand_cmp(gconstpointer a, gconstpointer b)
         {
             return strcmp(*(char *const *)a, *(char *const *)b);
         }

         static void mpcsh_path_refresh(void)
         {
             const char *path = g_getenv("PATH");
             if (!path) path = "";
             gint64 now = g_get_monotonic_time();
             if (mpcsh_path_cache && mpcsh_path_env && !strcmp(mpcsh_path_env, path) &&
                 now - mpcsh_path_stamp < 10 * G_USEC_PER_SEC)
                 return;

             if (mpcsh_path_cache) g_ptr_array_free(mpcsh_path_cache, TRUE);
             g_free(mpcsh_path_env);
             mpcsh_path_env = g_strdup(path);
             mpcsh_path_cache = g_ptr_array_new_with_free_func(g_free);
             mpcsh_path_stamp = now;

             GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
             for (int i = 0; mpcsh_builtin_names[i]; ++i) {
                 char *nm = g_strdup(mpcsh_builtin_names[i]);
                 g_hash_table_add(seen, nm);
                 g_ptr_array_add(mpcsh_path_cache, nm);
             }

             char **dirs = g_strsplit(path, ":", -1);
             for (int i = 0; dirs[i]; ++i) {
                 if (!*dirs[i]) continue;
                 DIR *d = opendir(dirs[i]);
                 if (!d) continue;
                 struct dirent *e;
                 while ((e = readdir(d))) {
                     if (e->d_name[0] == '.') continue;
                     if (g_hash_table_contains(seen, e->d_name)) continue;
                     struct stat st;
                     if (fstatat(dirfd(d), e->d_name, &st, 0) != 0 || !S_ISREG(st.st_mode)) continue;
                     if (faccessat(dirfd(d), e->d_name, X_OK, 0) != 0) continue;
                     char *nm = g_strdup(e->d_name);
                     g_hash_table_add(seen, nm);
                     g_ptr_array_add(mpcsh_path_cache, nm);
                 }
                 closedir(d);
             }
             g_strfreev(dirs);
             g_hash_table_destroy(seen);
             g_ptr_array_sort(mpcsh_path_cache, mpcsh_cand_cmp);
         }

         typedef struct {
             size_t start;        /* byte offset of the token in the line        */
             int    cmd_pos;      /* token is a command name                     */
             char   qc;           /* open quote char the token started with      */
             char  *plain;        /* token with escapes/quotes removed           */
             char   cmdword[128]; /* command word of this segment (for "cd")     */
         } MpcshTok;

         static void mpcsh_tok_parse(const char *line, size_t cursor, MpcshTok *t)
         {
             int in_word = 0, word_is_cmd = 0, seg_first = 1;
             size_t ws = 0;
             char quote = 0, curcmd[128] = "";

             for (size_t i = 0; i < cursor; ++i) {
                 char c = line[i];
                 if (quote) { if (c == quote) quote = 0; continue; }
                 if (c == '\\' && i + 1 < cursor) {
                     if (!in_word) { in_word = 1; ws = i; word_is_cmd = seg_first; seg_first = 0; }
                     ++i; continue;
                 }
                 if (c == '"' || c == '\'') {
                     if (!in_word) { in_word = 1; ws = i; word_is_cmd = seg_first; seg_first = 0; }
                     quote = c; continue;
                 }
                 int sep = (c == ' ' || c == '\t');
                 int op  = (c == '|' || c == ';' || c == '&' || c == '(' || c == '\n');
                 if (sep || op) {
                     if (in_word) {
                         if (word_is_cmd) {
                             size_t n = i - ws;
                             if (n >= sizeof curcmd) n = sizeof curcmd - 1;
                             memcpy(curcmd, line + ws, n); curcmd[n] = 0;
                         }
                         in_word = 0;
                     }
                     if (op) { seg_first = 1; curcmd[0] = 0; }
                     continue;
                 }
                 if (!in_word) { in_word = 1; ws = i; word_is_cmd = seg_first; seg_first = 0; }
             }

             if (in_word) { t->start = ws; t->cmd_pos = word_is_cmd; }
             else         { t->start = cursor; t->cmd_pos = seg_first; }
             t->qc = (in_word && quote && line[ws] == quote) ? quote : 0;
             g_strlcpy(t->cmdword, curcmd, sizeof t->cmdword);

             GString *g = g_string_new(NULL);
             const char *raw = line + t->start;
             size_t rl = cursor - t->start;
             size_t i = 0;
             if (t->qc) i = 1;
             for (; i < rl; ++i) {
                 char c = raw[i];
                 if (!t->qc) {
                     if (c == '\\' && i + 1 < rl) { g_string_append_c(g, raw[++i]); continue; }
                     if (c == '"' || c == '\'') continue;
                 }
                 g_string_append_c(g, c);
             }
             t->plain = g_string_free(g, FALSE);
         }

         static GPtrArray *mpcsh_candidates(const MpcshTok *t)
         {
             GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
             const char *plain = t->plain;
             size_t pl = strlen(plain);
             int pathy = strchr(plain, '/') || plain[0] == '~' || plain[0] == '.';

             if (t->cmd_pos && !pathy) {
                 if (!pl) return out;
                 mpcsh_path_refresh();
                 GPtrArray *ci = g_ptr_array_new_with_free_func(g_free);
                 for (guint i = 0; i < mpcsh_path_cache->len; ++i) {
                     const char *nm = g_ptr_array_index(mpcsh_path_cache, i);
                     if (!strncmp(nm, plain, pl)) g_ptr_array_add(out, g_strdup(nm));
                     else if (!g_ascii_strncasecmp(nm, plain, pl)) g_ptr_array_add(ci, g_strdup(nm));
                 }
                 if (!out->len) { g_ptr_array_free(out, TRUE); out = ci; }
                 else g_ptr_array_free(ci, TRUE);
                 return out;
             }

             const char *slash = strrchr(plain, '/');
             char *dirpart = slash ? g_strndup(plain, (gsize)(slash - plain) + 1) : g_strdup("");
             const char *base = slash ? slash + 1 : plain;
             size_t bl = strlen(base);
             char *dirpath;
             if (!*dirpart) dirpath = g_strdup(".");
             else if (dirpart[0] == '~' && (dirpart[1] == '/' || !dirpart[1]))
                 dirpath = g_strconcat(g_get_home_dir(), dirpart + 1, NULL);
             else dirpath = g_strdup(dirpart);

             int dirs_only = !t->cmd_pos && !strcmp(t->cmdword, "cd");
             GPtrArray *ci = g_ptr_array_new_with_free_func(g_free);
             DIR *d = opendir(dirpath);
             if (d) {
                 struct dirent *e;
                 while ((e = readdir(d)) && out->len < 2000 && ci->len < 2000) {
                     const char *nm = e->d_name;
                     if (!strcmp(nm, ".") || !strcmp(nm, "..")) continue;
                     if (nm[0] == '.' && base[0] != '.') continue;
                     int exact = !strncmp(nm, base, bl);
                     if (!exact && g_ascii_strncasecmp(nm, base, bl)) continue;
                     struct stat st;
                     int isdir = 0, ok = (fstatat(dirfd(d), nm, &st, 0) == 0);
                     if (ok) isdir = S_ISDIR(st.st_mode);
                     if (dirs_only && !isdir) continue;
                     if (t->cmd_pos && !isdir && !(ok && faccessat(dirfd(d), nm, X_OK, 0) == 0)) continue;
                     char *full = isdir ? g_strconcat(dirpart, nm, "/", NULL) : g_strconcat(dirpart, nm, NULL);
                     g_ptr_array_add(exact ? out : ci, full);
                 }
                 closedir(d);
             }
             if (!out->len) { g_ptr_array_free(out, TRUE); out = ci; }
             else g_ptr_array_free(ci, TRUE);
             g_ptr_array_sort(out, mpcsh_cand_cmp);
             g_free(dirpart); g_free(dirpath);
             return out;
         }

         /* Escape a plain candidate for insertion into the command line. */
         static char *mpcsh_apply(const char *cand, char qc)
         {
             GString *g = g_string_new(NULL);
             if (qc) { g_string_append_c(g, qc); g_string_append(g, cand); }
             else for (const char *p = cand; *p; ++p) {
                 if (strchr(" \t\"'\\|&;<>()$*?#{}[]!`", *p)) g_string_append_c(g, '\\');
                 g_string_append_c(g, *p);
             }
             return g_string_free(g, FALSE);
         }

         static int mpcsh_replace_range(char *line, size_t *len, size_t *cursor, size_t cap,
                                        size_t from, size_t to, const char *text)
         {
             size_t tl = strlen(text);
             if (*len - (to - from) + tl >= cap) return 0;
             memmove(line + from + tl, line + to, *len - to + 1);
             memcpy(line + from, text, tl);
             *len = *len - (to - from) + tl;
             *cursor = from + tl;
             return 1;
         }

         /* Ghost text for the current buffer (suffix to show after the cursor). */
         static char *mpcsh_suggest(const char *line, size_t len, size_t cursor, MpcshHistory *h)
         {
             if (!len || cursor != len || strchr(line, '\n')) return NULL;

             for (int i = h->count - 1; i >= 0; --i) {
                 const char *e = h->items[i];
                 if (!strncmp(e, line, len) && e[len] && !strchr(e, '\n')) {
                     if (strlen(e + len) > 512) return NULL;
                     return g_strdup(e + len);
                 }
             }

             MpcshTok t;
             mpcsh_tok_parse(line, cursor, &t);
             char *result = NULL;
             size_t pl = strlen(t.plain);
             if (pl) {
                 GPtrArray *c = mpcsh_candidates(&t);
                 const char *best = NULL;
                 for (guint i = 0; i < c->len; ++i) {
                     const char *cand = g_ptr_array_index(c, i);
                     if (strlen(cand) <= pl) continue;
                     if (!best || (t.cmd_pos && strlen(cand) < strlen(best))) best = cand;
                     if (!t.cmd_pos) break;
                 }
                 if (best) {
                     char *ap = mpcsh_apply(best, t.qc);
                     size_t rawlen = cursor - t.start;
                     if (strlen(ap) > rawlen && !strncmp(ap, line + t.start, rawlen) && strlen(ap) - rawlen <= 512)
                         result = g_strdup(ap + rawlen);
                     g_free(ap);
                 }
                 g_ptr_array_free(c, TRUE);
             }
             g_free(t.plain);
             return result;
         }

         static size_t mpcsh_ghost_word_len(const char *g)
         {
             size_t i = 0;
             while (g[i] == ' ' || g[i] == '\t') ++i;
             while (g[i] && g[i] != ' ' && g[i] != '\t' && g[i] != '/') ++i;
             if (g[i] == '/') ++i;
             return i ? i : strlen(g);
         }

         static void mpcsh_show_candidates(GPtrArray *c)
         {
             int cols = mpcsh_term_cols();
             guint n = c->len, shown = n > 200 ? 200 : n;
             char **disp = g_new0(char *, shown);
             int *w = g_new0(int, shown), maxw = 0;

             for (guint i = 0; i < shown; ++i) {
                 const char *cand = g_ptr_array_index(c, i);
                 size_t l = strlen(cand), end = l;
                 if (end && cand[end - 1] == '/') --end;
                 size_t s = end;
                 while (s > 0 && cand[s - 1] != '/') --s;
                 disp[i] = g_strndup(cand + s, l - s);
                 w[i] = mpcsh_cells(disp[i], strlen(disp[i]));
                 if (w[i] > maxw) maxw = w[i];
             }
             int colw = maxw + 2, per = cols / (colw ? colw : 1);
             if (per < 1) per = 1;
             int rows = (int)((shown + (guint)per - 1) / (guint)per);

             fputs("\r\n", stdout);
             for (int r = 0; r < rows; ++r) {
                 for (int k = 0; k < per; ++k) {
                     guint idx = (guint)(k * rows + r);
                     if (idx >= shown) break;
                     int isdir = disp[idx][0] && disp[idx][strlen(disp[idx]) - 1] == '/';
                     if (isdir) fputs("\033[1;34m", stdout);
                     fputs(disp[idx], stdout);
                     if (isdir) fputs("\033[0m", stdout);
                     if ((guint)((k + 1) * rows + r) < shown)
                         for (int p = w[idx]; p < colw; ++p) fputc(' ', stdout);
                 }
                 fputs("\r\n", stdout);
             }
             if (n > shown) printf("\033[2m… and %u more\033[0m\r\n", n - shown);
             for (guint i = 0; i < shown; ++i) g_free(disp[i]);
             g_free(disp); g_free(w);
         }

         /* Editor state (heap-allocated: the line buffer is large). */
         typedef struct {
             char   line[MPCSH_MAX_LINE_HARD];
             size_t len, cursor;
             char  *ghost;
             GPtrArray *tab_c;        /* candidates being cycled through */
             int    tab_idx;
             size_t tab_start, tab_end;
             char   tab_qc;
         } MpcshEd;

         static void mpcsh_tab_reset(MpcshEd *ed)
         {
             if (ed->tab_c) { g_ptr_array_free(ed->tab_c, TRUE); ed->tab_c = NULL; }
         }

         static void mpcsh_do_tab(MpcshEd *ed, const char *prompt, int backward)
         {
             char *l = ed->line;
             size_t cap = sizeof ed->line;

             if (ed->tab_c && ed->tab_c->len) {              /* cycling */
                 int n = (int)ed->tab_c->len;
                 ed->tab_idx = backward ? (ed->tab_idx <= 0 ? n - 1 : ed->tab_idx - 1)
                 : (ed->tab_idx + 1) % n;
                 char *ap = mpcsh_apply(g_ptr_array_index(ed->tab_c, ed->tab_idx), ed->tab_qc);
                 if (mpcsh_replace_range(l, &ed->len, &ed->cursor, cap, ed->tab_start, ed->tab_end, ap))
                     ed->tab_end = ed->cursor;
                 g_free(ap);
                 return;
             }

             MpcshTok t;
             mpcsh_tok_parse(l, ed->cursor, &t);
             GPtrArray *c = mpcsh_candidates(&t);
             size_t plen = strlen(t.plain);

             if (c->len == 0) {
                 if (ed->ghost && *ed->ghost && ed->cursor == ed->len)   /* nothing to complete: take the ghost */
                     mpcsh_insert_text(l, &ed->len, &ed->cursor, cap, ed->ghost, strlen(ed->ghost));
             } else if (c->len == 1) {
                 const char *cand = g_ptr_array_index(c, 0);
                 char *ap = mpcsh_apply(cand, t.qc);
                 int isdir = cand[0] && cand[strlen(cand) - 1] == '/';
                 char *text = isdir ? g_strdup(ap)
                 : g_strdup_printf("%s%s ", ap, t.qc ? (t.qc == '"' ? "\"" : "'") : "");
                 mpcsh_replace_range(l, &ed->len, &ed->cursor, cap, t.start, ed->cursor, text);
                 g_free(text); g_free(ap);
             } else {
                 const char *first = g_ptr_array_index(c, 0);
                 size_t common = strlen(first);
                 for (guint i = 1; i < c->len; ++i) {
                     const char *m = g_ptr_array_index(c, i);
                     size_t k = 0;
                     while (k < common && m[k] && m[k] == first[k]) ++k;
                     common = k;
                 }
                 while (common > 0 && (((unsigned char)first[common]) & 0xC0) == 0x80) --common;
                 if (common > plen && !strncmp(first, t.plain, plen)) {
                     char *pre = g_strndup(first, common);
                     char *ap = mpcsh_apply(pre, t.qc);
                     mpcsh_replace_range(l, &ed->len, &ed->cursor, cap, t.start, ed->cursor, ap);
                     g_free(ap); g_free(pre);
                 } else {
                     /* No progress possible: show the menu, arm cycling. */
                     mpcsh_redraw_line(prompt, l, ed->cursor, NULL);
                     mpcsh_goto_end(prompt, l);
                     mpcsh_show_candidates(c);
                     mpcsh_cur_row = 0;
                     ed->tab_c = c; ed->tab_idx = -1;
                     ed->tab_start = t.start; ed->tab_end = ed->cursor; ed->tab_qc = t.qc;
                     c = NULL;
                 }
             }
             if (c) g_ptr_array_free(c, TRUE);
             g_free(t.plain);
         }

         static size_t mpcsh_word_left(const char *s, size_t pos)
         {
             while (pos > 0 && (s[pos-1] == ' ' || s[pos-1] == '\t' || s[pos-1] == '\n')) --pos;
             while (pos > 0 && s[pos-1] != ' ' && s[pos-1] != '\t' && s[pos-1] != '\n') --pos;
             return pos;
         }
         static size_t mpcsh_word_right(const char *s, size_t len, size_t pos)
         {
             while (pos < len && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n')) ++pos;
             while (pos < len && s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\n') ++pos;
             return pos;
         }

         static int mpcsh_readline_edit(const char *prompt, char *out, size_t outsz, MpcshHistory *history)
         {
             if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
                 while (fgets(out, (int)outsz, stdin) == NULL && errno == EINTR) ;
                 if (feof(stdin) || ferror(stdin)) return 0;
                 out[strcspn(out, "\r\n")] = 0;
                 return 1;
             }
             struct termios oldt, raw;
             if (tcgetattr(STDIN_FILENO, &oldt) < 0) return 0;
             raw = oldt;
             /* ISIG off: Ctrl+C arrives as byte 3 so cancel runs and prompt stays intact. */
             raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG);
             raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
             if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return 0;
             fputs("\033[?2004h", stdout); fflush(stdout);
             mpcsh_cur_row = 0;
             mpcsh_redraw_begin(prompt);

             MpcshEd *ed = g_new0(MpcshEd, 1);
             char *line = ed->line;
             const size_t cap = sizeof ed->line;
             int hist = history->count, result = 0;

             #define REFRESH() do { g_free(ed->ghost); \
             ed->ghost = mpcsh_suggest(line, ed->len, ed->cursor, history); \
             mpcsh_redraw_line(prompt, line, ed->cursor, ed->ghost); } while (0)
             #define SET_LINE(src) do { g_strlcpy(line, (src), cap); ed->len = ed->cursor = strlen(line); } while (0)
             #define ACCEPT_GHOST_ALL() do { if (ed->ghost && *ed->ghost && ed->cursor == ed->len) \
             mpcsh_insert_text(line, &ed->len, &ed->cursor, cap, ed->ghost, strlen(ed->ghost)); } while (0)
             #define ACCEPT_GHOST_WORD() do { if (ed->ghost && *ed->ghost && ed->cursor == ed->len) \
             mpcsh_insert_text(line, &ed->len, &ed->cursor, cap, ed->ghost, mpcsh_ghost_word_len(ed->ghost)); } while (0)
             #define DELETE_RANGE(from, to) do { size_t f_ = (from), t_ = (to); \
             memmove(line + f_, line + t_, ed->len - t_ + 1); ed->len -= t_ - f_; ed->cursor = f_; } while (0)

             for (;;) {
                 unsigned char c;
                 if (got_sigint) {
                     c = 3;
                 } else if (mpcsh_read_retry(STDIN_FILENO, &c, 1) != 1) {
                     break;
                 }
                 if (got_sigint)
                     c = 3;
                 if (c != '\t' && c != 27) mpcsh_tab_reset(ed);

                 if (c == 3) {
                     /* Wipe prompt+input block completely — no clones, no half prompts. */
                     got_sigint = 1;
                     if (mpcsh_cur_row > 0)
                         printf("\033[%dA", mpcsh_cur_row);
                     fputs("\r\033[J^C\r\n", stdout);
                     fflush(stdout);
                     mpcsh_cur_row = 0;
                     ed->len = ed->cursor = 0;
                     line[0] = 0;
                     out[0] = 0;
                     result = 1;
                     break;
                 }
                 if (c == '\r' || c == '\n') {
                     mpcsh_redraw_line(prompt, line, ed->len, NULL);   /* erases the ghost */
                     fputs("\r\n", stdout);
                     g_strlcpy(out, line, outsz); result = 1; break;
                 }
                 if (c == 4) {                                          /* Ctrl+D */
                     if (!ed->len) { result = 0; break; }
                     if (ed->cursor < ed->len) { DELETE_RANGE(ed->cursor, mpcsh_next_char(line, ed->len, ed->cursor)); REFRESH(); }
                     continue;
                 }
                 if (c == 1)  { ed->cursor = 0; REFRESH(); continue; }                       /* Ctrl+A */
                     if (c == 5)  { if (ed->cursor == ed->len) ACCEPT_GHOST_ALL(); ed->cursor = ed->len; REFRESH(); continue; } /* Ctrl+E */
                         if (c == 6)  { if (ed->cursor == ed->len) ACCEPT_GHOST_ALL(); else ed->cursor = mpcsh_next_char(line, ed->len, ed->cursor); REFRESH(); continue; } /* Ctrl+F */
                             if (c == 2)  { if (ed->cursor) ed->cursor = mpcsh_prev_char(line, ed->cursor); REFRESH(); continue; }       /* Ctrl+B */
                                 if (c == 11) { DELETE_RANGE(ed->cursor, ed->len); REFRESH(); continue; }    /* Ctrl+K */
                                     if (c == 21) { DELETE_RANGE(0, ed->cursor); REFRESH(); continue; }          /* Ctrl+U */
                                         if (c == 23) { DELETE_RANGE(mpcsh_word_left(line, ed->cursor), ed->cursor); REFRESH(); continue; } /* Ctrl+W */
                                             if (c == 12) { fputs("\033[H\033[2J", stdout); mpcsh_cur_row = 0; REFRESH(); continue; }           /* Ctrl+L */
                                                 if (c == 15) { mpcsh_insert_text(line, &ed->len, &ed->cursor, cap, "\n", 1); REFRESH(); continue; } /* Ctrl+O */
                                                     if (c == '\t') { mpcsh_do_tab(ed, prompt, 0); REFRESH(); continue; }
                                                     if (c == 127 || c == 8) {
                                                         if (ed->cursor) DELETE_RANGE(mpcsh_prev_char(line, ed->cursor), ed->cursor);
                                                         REFRESH(); continue;
                                                     }
                                                     if (c == 27) {
                                                         unsigned char b0; char seq[32]; int n = 0;
                                                         if (mpcsh_read_retry(STDIN_FILENO, &b0, 1) != 1) break;
                                                         if (b0 == '[') {
                                                             while (n < 31) {
                                                                 unsigned char x;
                                                                 if (mpcsh_read_retry(STDIN_FILENO, &x, 1) != 1) break;
                                                                 seq[n++] = (char)x;
                                                                 if (x >= 0x40 && x <= 0x7e) break;
                                                             }
                                                         } else if (b0 == 'O') {
                                                             unsigned char x;
                                                             if (mpcsh_read_retry(STDIN_FILENO, &x, 1) == 1) seq[n++] = (char)x;
                                                         }
                                                         seq[n] = 0;

                                                         if (b0 == '[' && !strcmp(seq, "Z")) { mpcsh_do_tab(ed, prompt, 1); REFRESH(); continue; }
                                                         mpcsh_tab_reset(ed);

                                                         if (b0 == '[' && !strcmp(seq, "200~")) {
                                                             mpcsh_read_bracketed_paste(line, &ed->len, &ed->cursor, cap);
                                                             REFRESH(); continue;
                                                         }
                                                         char fin = n ? seq[n - 1] : 0;
                                                         int word = (b0 == '[' && (strstr(seq, ";3") || strstr(seq, ";5")));
                                                         if (b0 == '[') {
                                                             if (fin == 'A') {
                                                                 if (hist > 0) { --hist; SET_LINE(history->items[hist]); }
                                                             } else if (fin == 'B') {
                                                                 if (hist < history->count - 1) { ++hist; SET_LINE(history->items[hist]); }
                                                                 else { hist = history->count; line[0] = 0; ed->len = ed->cursor = 0; }
                                                             } else if (fin == 'C') {
                                                                 if (ed->cursor == ed->len) { if (word) ACCEPT_GHOST_WORD(); else ACCEPT_GHOST_ALL(); }
                                                                 else ed->cursor = word ? mpcsh_word_right(line, ed->len, ed->cursor)
                                                                     : mpcsh_next_char(line, ed->len, ed->cursor);
                                                             } else if (fin == 'D') {
                                                                 if (ed->cursor) ed->cursor = word ? mpcsh_word_left(line, ed->cursor)
                                                                     : mpcsh_prev_char(line, ed->cursor);
                                                             } else if (fin == 'H' || !strcmp(seq, "1~") || !strcmp(seq, "7~")) {
                                                                 ed->cursor = 0;
                                                             } else if (fin == 'F' || !strcmp(seq, "4~") || !strcmp(seq, "8~")) {
                                                                 if (ed->cursor == ed->len) ACCEPT_GHOST_ALL();
                                                                 ed->cursor = ed->len;
                                                             } else if (!strcmp(seq, "3~")) {
                                                                 if (ed->cursor < ed->len) DELETE_RANGE(ed->cursor, mpcsh_next_char(line, ed->len, ed->cursor));
                                                             }
                                                         } else if (b0 == 'O') {
                                                             if (fin == 'H') ed->cursor = 0;
                                                             else if (fin == 'F') { if (ed->cursor == ed->len) ACCEPT_GHOST_ALL(); ed->cursor = ed->len; }
                                                         } else if (b0 == 'f') {
                                                             if (ed->cursor == ed->len) ACCEPT_GHOST_WORD();
                                                             else ed->cursor = mpcsh_word_right(line, ed->len, ed->cursor);
                                                         } else if (b0 == 'b') {
                                                             ed->cursor = mpcsh_word_left(line, ed->cursor);
                                                         }
                                                         REFRESH(); continue;
                                                     }
                                                     if (c >= 32) {
                                                         /* Gather the full UTF-8 sequence (the old code dropped every non-ASCII char). */
                                                         unsigned char buf[4]; int need = 1; buf[0] = c;
                                                         if ((c & 0xE0) == 0xC0) need = 2; else if ((c & 0xF0) == 0xE0) need = 3; else if ((c & 0xF8) == 0xF0) need = 4;
                                                         for (int i = 1; i < need; ++i)
                                                             if (mpcsh_read_retry(STDIN_FILENO, &buf[i], 1) != 1) { need = i; break; }
                                                             mpcsh_insert_text(line, &ed->len, &ed->cursor, cap, (const char *)buf, (size_t)need);
                                                         REFRESH();
                                                     }
             }
             #undef REFRESH
             #undef SET_LINE
             #undef ACCEPT_GHOST_ALL
             #undef ACCEPT_GHOST_WORD
             #undef DELETE_RANGE
             mpcsh_tab_reset(ed);
             g_free(ed->ghost); g_free(ed);
             fputs("\033[?2004l", stdout); fflush(stdout); tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
             return result;
         }

         static gboolean mpcsh_needs_more(const char *line)
         {
             /* Fish-friendly: trailing backslash continues; while/begin/switch need end. */
             if (line && *line) {
                 const char *p = line + strlen(line) - 1;
                 while (p > line && (*p == ' ' || *p == '\t')) --p;
                 if (*p == '\\') return TRUE;
             }
             gboolean ok = TRUE;
             GPtrArray *tokens = mpc_split(line, &ok);
             int blocks = 0;
             for (guint i = 0; i < tokens->len; ++i) {
                 const char *t = g_ptr_array_index(tokens, i);
                 if (mpc_kw(t, "for") || mpc_kw(t, "if") || mpc_kw(t, "function") ||
                     mpc_kw(t, "while") || mpc_kw(t, "begin") || mpc_kw(t, "switch"))
                     ++blocks;
                 else if (mpc_kw(t, "end")) {
                     if (!blocks) { g_ptr_array_free(tokens, TRUE); return FALSE; }
                     --blocks;
                 }
             }
             g_ptr_array_free(tokens, TRUE);
             return !ok || blocks > 0;
         }


         static void
         mpcsh_repl(void)
         {
             char line[MPCSH_MAX_LINE_HARD];
             MpcshHistory history;
             mpcsh_history_init(&history);

             struct sigaction sa = {0};
             sa.sa_handler = sigint_handler;
             sigemptyset(&sa.sa_mask);
             /* SA_RESTART would ask the kernel to auto-restart some syscalls,
              * but we handle EINTR explicitly everywhere, so interrupts are
              * always visible and Ctrl+C reliably cancels the current line. */
             sa.sa_flags = 0;
             sigaction(SIGINT, &sa, NULL);
             mpcsh_load_config();

             /* Public identity for extensions — not a secret leak. */
             g_setenv("MPCSH_VERSION", MPCSH_VERSION_STRING, TRUE);
             g_setenv("MPCSH_SHELL", "mpcsh", TRUE);
             g_setenv("MPCSH_NAME", "MarekPC Shell", TRUE);
             if (g_getenv("MPCECC"))
                 g_setenv("MPCSH_FRONTEND", "mpcecc", TRUE);
             else
                 g_setenv("MPCSH_FRONTEND", "terminal", TRUE);

             /* Colours from ~/.config/mpcsh/mpcsh.config (accent=...). */

             /* MPCECC provides the real GTK image background, so never paint
              *      the ANSI fallback banner over it. */
             if (!g_getenv("MPCECC"))
                 mpcsh_terminal_wallpaper();

             fprintf(stderr,
                     "\033[1;95m✦ MarekPC Shell\033[0m \033[1;35m— mpcsh\033[0m\n"
                     "\033[0;36mPipeline limit: %d  |  $() supported\n\033[0m"
                     "\033[0;36m↑/↓ history  ←/→ cursor  Ctrl+C cancels the current line; Ctrl+O inserts newline\033[0m\n"
                     "\033[0;36mBuilt-ins: ctk, portmap, guiinterm, wallpaper, ls, mpcimgcat, colorit, help, limits, mpcupdate, mpcbits, mpcversion\033[0m\n\n",
                     MPCSH_MAX_PIPELINE);

             if (mpcsh_ai_agent_detected())
                 mpcsh_print_ai_notice();

             for (;;) {
                 got_sigint = 0;
                 char cwd[PATH_MAX];
                 if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
                 const char *home = g_get_home_dir();
                 char display[PATH_MAX];
                 if (home && strncmp(cwd, home, strlen(home)) == 0)
                     snprintf(display, sizeof(display), "~%s", cwd + strlen(home));
                 else
                     g_strlcpy(display, cwd, sizeof(display));
                 const char *user = g_get_user_name();
                 char host[256] = "localhost";
                 gethostname(host, sizeof(host));

                 char prompt[PATH_MAX + 300];
                 mpcsh_build_prompt(prompt, sizeof(prompt),
                                    user ? user : "user", host, display);
                 fputs(prompt, stdout);
                 fflush(stdout);

                 if (!mpcsh_readline_edit(prompt, line, sizeof(line), &history))
                     break;
                 /* Always resync after the editor restores cooked mode. */
                 mpcsh_cur_row = 0;
                 if (got_sigint) {
                     got_sigint = 0;
                     line[0] = 0;
                     continue;
                 }
                 if (!*line) continue;

                 gboolean aborted = FALSE, eof = FALSE;
                 while (mpcsh_needs_more(line)) {
                     size_t current = strlen(line);
                     gboolean splice = current > 0 && line[current - 1] == '\\';
                     if (splice) line[--current] = '\0';
                     char continuation[MPCSH_MAX_LINE_HARD];
                     if (!mpcsh_readline_edit("\033[1;95m… ❯\033[0m ",
                         continuation, sizeof(continuation), &history)) {
                         fprintf(stderr, "mpcsh: incomplete input discarded at EOF\n");
                     eof = TRUE; break;
                         }
                         mpcsh_cur_row = 0;
                         if (got_sigint) { got_sigint = 0; aborted = TRUE; break; }
                         size_t extra = strlen(continuation);
                         if (current + 1 + extra >= sizeof(line)) {
                             fprintf(stderr, "mpcsh: multiline input exceeds buffer limit\n");
                             aborted = TRUE; break;
                         }
                         line[current++] = splice ? ' ' : '\n';
                         memcpy(line + current, continuation, extra + 1);
                 }
                 if (eof) break;
                 if (aborted || got_sigint || !*line) continue;

                 mpcsh_history_add(&history, line);
                 {
                     struct timespec t0, t1;
                     clock_gettime(CLOCK_MONOTONIC, &t0);
                     mpcsh_run_line(line);
                     clock_gettime(CLOCK_MONOTONIC, &t1);
                     mpcsh_last_cmd_secs = (t1.tv_sec - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;
                 }
             }
             mpcsh_history_free(&history);
             fprintf(stderr, "\n");
         }

         /* ------------------------------------------------------------
          * main
          * ------------------------------------------------------------ */

         static void
         usage(const char *program)
         {
             fprintf(stderr,
                     "MarekPC Shell — mpcsh\n\n"
                     "Usage:\n"
                     "  %s                      interactive shell\n"
                     "  %s script.mpcsh [ARGS]   run a script (any file starting with #!)\n"
                     "  %s prompt.mpcsh --gui    run a prompt script (GUI)\n"
                     "  %s -c \"cmd | cmd\"        run one command line\n\n"
                     "Pipeline segments: up to %d\n"
                     "Command substitution: $()  |  Fish grammar: set / and / or / not\n"
                     "Built-ins: ctk, portmap, guiinterm, wallpaper, ls, mpcimgcat, colorit, help, limits, mpcupdate, mpcbits, mpcversion, time, math, source, mpcshman, mpc-chuctrl\n"
                     "Attached languages: fish, mpcsh, python, c, cpp, csharp, rust, go, java, js, ts, ruby, php, lua, zig, kotlin\n"
                     "Tab completes paths; Ctrl+Enter inserts newline; trailing \\ continues lines\n",
                     program, program, program, program, MPCSH_MAX_PIPELINE);
         }

         int
         main(int argc, char **argv)
         {
             setlocale(LC_CTYPE, "");
             if(argc==2&&!strcmp(argv[1],"--version")){puts("MPCSH native-blocks loop fix 3");return 0;}
             if(argc==3&&!strcmp(argv[1],"--check")){
                 char *text=NULL;if(!g_file_get_contents(argv[2],&text,NULL,NULL))return 126;
                 mpc_check=TRUE;int rc=mpcsh_run_line(text);g_free(text);return rc;
             }
             mpcsh_chu_load_all();   /* 1.8: load Chihuahuas (.chu) */
             if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
                 if (mpcsh_ai_agent_detected()) mpcsh_print_ai_notice();
                 return mpcsh_run_line(argv[2]);
             }

             if (argc < 2) {
                 mpcsh_repl();
                 return 0;
             }

             if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
                 usage(argv[0]);
                 return 2;
             }

             const char *script_path = argv[1];
             size_t len = strlen(script_path);
             gboolean is_mpcsh = (len > 6 && strcmp(script_path + len - 6, ".mpcsh") == 0);
             if (!is_mpcsh) {   /* 1.8: any file whose first line starts with #! is a script */
                 char *hd = NULL;
                 if (g_file_get_contents(script_path, &hd, NULL, NULL)) {
                     is_mpcsh = (hd[0] == '#' && hd[1] == '!');
                     g_free(hd);
                 }
             }
             if (!is_mpcsh) {
                 fprintf(stderr, "mpcsh: '%s' is not a script (name it .mpcsh or start the file with #!)\n", script_path);
                 return 2;
             }

             char *head = NULL;
             gboolean prompt_script = FALSE;
             if (g_file_get_contents(script_path, &head, NULL, NULL)) {
                 if (strncmp(head, "#!mpcsh prompt", 14) == 0)
                     prompt_script = TRUE;
                 g_free(head);
             }

             gboolean force_gui = (argc >= 3 && strcmp(argv[2], "--gui") == 0);

             if (!prompt_script && !force_gui) {
                 if (mpcsh_ai_agent_detected()) mpcsh_print_ai_notice();
                 mpcsh_script_set_args(script_path, argc - 2, argv + 2);   /* $argc $argv $arg1... */
                 char *contents=NULL;
                 if(!g_file_get_contents(script_path,&contents,NULL,NULL)) {
                     fprintf(stderr,"mpcsh: cannot read %s\n",script_path);return 126;
                 }
                 int rc=mpcsh_run_line(contents);g_free(contents);return rc;
             }

             static MPCap cap;
             memset(&cap, 0, sizeof(cap));
             cap.selected = 0;
             cap.default_option = 0;
             cap.ai_mode = mpcsh_ai_agent_detected();
             g_strlcpy(cap.type, "warning", sizeof(cap.type));
             g_strlcpy(cap.title, "MarekPC Aero Prompt", sizeof(cap.title));
             cap.workdir = g_get_current_dir();

             mpcsh_parse_prompt(script_path, &cap);
             if (cap.ai_mode) mpcsh_print_ai_notice();

             GtkApplication *app = gtk_application_new("com.marekpc.mpcsh", G_APPLICATION_DEFAULT_FLAGS);
             g_signal_connect(app, "activate", G_CALLBACK(activate), &cap);
             int result = g_application_run(G_APPLICATION(app), 0, NULL);

             for (int i = 0; i < cap.option_count; ++i)
                 g_free(cap.options[i].script);
             g_free(cap.workdir);
             g_object_unref(app);
             return result;
         }
