/*
 * MarekPC Extended Capabilities Console (MPCECC) 1.6
 * GTK4 + VTE terminal console with profiles, wallpaper,
 * tabs, clipboard shortcuts, context menu, and settings.
 *
 * Clipboard shortcuts (VTE-GTK4 does NOT bind these itself, so MPCECC
 * installs them as capture-phase window shortcuts):
 *   Ctrl+Shift+C = copy selected text
 *   Ctrl+Shift+V = paste
 *   Ctrl+V       = paste
 *
 * Fixes in this revision (1.6):
 *   - windowlaunch / PixelWindow NEVER touches your real Firefox
 *     profile. It uses a dedicated isolated profile directory named
 *     MPCECCWLPROFILE under ~/.mozilla/firefox/ so logins, cookies
 *     and data stay intact.
 *   - Pixel window is resizable with a proper LXQt-style title bar
 *     (minimize / maximize / restore / close), aspect-correct
 *     (contain-fit, no stretch), clickable and typeable via xdotool.
 *   - Default pixel geometry raised to 960x600 (was 240x160).
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
 * Build:
 *   gcc mpcecc.c -o mpcecc $(pkg-config --cflags --libs gtk4 vte-2.91-gtk4)
 */

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <vte/vte.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <gdk/x11/gdkx.h>
#include <X11/Xlib.h>

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
} MpcEccTab;

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
static int mpcecc_pixel_width = 1280;
static int mpcecc_pixel_height = 720;
static void mpcecc_open_command_window(MPCECC *ecc, char **command);
static MPCECC *mpcecc_main_ecc = NULL;
static GSocketService *mpcecc_control_service = NULL;
static GtkWidget *mpcecc_active_pixel_surface = NULL; /* the picture currently showing pixels */

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
        g_signal_connect(btn_close, "clicked", G_CALLBACK(mpcecc_hide_pixels_clicked), NULL);

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
        /* FILL: stretch to the body so no black letterbox bars in the tab. */
        gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_FILL);
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
            gtk_css_provider_load_from_string(css,
                ".mpcecc-pixel-frame {"
                "  background-color: alpha(#1b1226, 0.72);"
                "  border: 1px solid alpha(#b48cff, 0.45);"
                "  border-radius: 8px;"
                "  box-shadow: 0 8px 28px alpha(black, 0.40), inset 0 1px 0 alpha(white, 0.12);"
                "}"
                ".mpcecc-pixel-normal {"
                "  margin: 20px 28px;"
                "}"
                ".mpcecc-pixel-max {"
                "  margin: 0;"
                "  border-radius: 0;"
                "}"
                ".mpcecc-pixel-titlebar {"
                "  background-image: linear-gradient(to bottom,"
                "    alpha(#6a4a9a, 0.85), alpha(#2a1840, 0.75));"
                "  min-height: 32px;"
                "  padding: 2px 6px;"
                "  border-bottom: 1px solid alpha(#c9b0ff, 0.35);"
                "  border-radius: 7px 7px 0 0;"
                "}"
                ".mpcecc-pixel-icon { color: #c9b0ff; font-size: 13px; padding-right: 4px; }"
                ".mpcecc-pixel-title {"
                "  color: #f0e6ff;"
                "  font-weight: 600;"
                "  font-size: 12px;"
                "}"
                ".mpcecc-pixel-btn {"
                "  color: #e4d4ff;"
                "  background: transparent;"
                "  border: none;"
                "  min-width: 36px;"
                "  min-height: 28px;"
                "  border-radius: 0;"
                "  padding: 0 8px;"
                "  font-weight: 600;"
                "  font-size: 14px;"
                "}"
                ".mpcecc-pixel-btn:hover {"
                "  background-color: #4a3568;"
                "  color: #ffffff;"
                "}"
                ".mpcecc-pixel-close:hover {"
                "  background-color: #c42b3a;"
                "  color: #ffffff;"
                "}"
                ".mpcecc-pixel-titlebar {"
                "  min-height: 32px;"
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
        gtk_overlay_set_measure_overlay(GTK_OVERLAY(tab->container), frame, TRUE);
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
    (void)controller; (void)keycode; (void)user_data;
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
    GtkWidget *pic = mpcecc_pixel_picture ? mpcecc_pixel_picture : mpcecc_active_pixel_surface;
    if (!mpcecc_pixel_display || !pic) return;
    double aw = gtk_widget_get_width(pic), ah = gtk_widget_get_height(pic);
    if (aw < 1 || ah < 1) return;
    /* Map widget coords → Xvfb framebuffer. FILL stretches; CONTAIN letterboxes. */
    int tx, ty;
    if (pic == mpcecc_active_pixel_surface) {
        /* in-tab uses FILL */
        tx = (int)(x * (double)mpcecc_pixel_width / aw);
        ty = (int)(y * (double)mpcecc_pixel_height / ah);
    } else {
        double scale = MIN(aw / mpcecc_pixel_width, ah / mpcecc_pixel_height);
        double ox = (aw - mpcecc_pixel_width * scale) / 2.0;
        double oy = (ah - mpcecc_pixel_height * scale) / 2.0;
        tx = (int)((x - ox) / scale);
        ty = (int)((y - oy) / scale);
    }
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
    if (pic == mpcecc_active_pixel_surface) {
        *tx = (int)(x * (double)mpcecc_pixel_width / aw);
        *ty = (int)(y * (double)mpcecc_pixel_height / ah);
    } else {
        double scale = MIN(aw / mpcecc_pixel_width, ah / mpcecc_pixel_height);
        double ox = (aw - mpcecc_pixel_width * scale) / 2.0;
        double oy = (ah - mpcecc_pixel_height * scale) / 2.0;
        *tx = (int)((x - ox) / scale);
        *ty = (int)((y - oy) / scale);
    }
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
    gchar *xvfb_args[] = { "Xvfb", mpcecc_pixel_display, "-screen", "0", geometry, "-ac", "-nolisten", "tcp", NULL };
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
    /* Default to maximized so the framebuffer fills the tab cleanly. */
    g_object_set_data(G_OBJECT(tab->pixel_surface), "mpcecc-pw-state",
                      GINT_TO_POINTER(1)); /* MAXIMIZED */
    {
        GtkWidget *maxbtn = g_object_get_data(G_OBJECT(tab->pixel_surface), "mpcecc-pw-maxbtn");
        if (maxbtn) gtk_button_set_label(GTK_BUTTON(maxbtn), "❐");
    }
    mpcecc_pixel_win_apply_state(tab->pixel_surface);
    gtk_widget_set_visible(tab->pixel_surface, TRUE);
    gtk_widget_set_can_target(tab->pixel_surface, TRUE);
    gtk_widget_set_can_target(picture, TRUE);
    gtk_overlay_set_measure_overlay(GTK_OVERLAY(tab->container), tab->pixel_surface, TRUE);
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

    /* margins: maximized = edge-to-edge; normal = inset; minimized = title only */
    if (state == MPCECC_PW_MAXIMIZED) {
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
        gtk_widget_set_margin_start(frame, 24);
        gtk_widget_set_margin_end(frame, 24);
        gtk_widget_set_margin_top(frame, 8);
        gtk_widget_set_margin_bottom(frame, 8);
        gtk_widget_set_halign(frame, GTK_ALIGN_START);
        gtk_widget_set_valign(frame, GTK_ALIGN_END);
        gtk_widget_set_hexpand(frame, FALSE);
        gtk_widget_set_vexpand(frame, FALSE);
        if (body) gtk_widget_set_visible(body, FALSE);
        if (picture) gtk_widget_set_visible(picture, FALSE);
        gtk_widget_remove_css_class(frame, "mpcecc-pixel-max");
        gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
    } else { /* NORMAL — centered inset, resizable feel via margins */
        gtk_widget_set_margin_start(frame, 28);
        gtk_widget_set_margin_end(frame, 28);
        gtk_widget_set_margin_top(frame, 20);
        gtk_widget_set_margin_bottom(frame, 20);
        gtk_widget_set_halign(frame, GTK_ALIGN_FILL);
        gtk_widget_set_valign(frame, GTK_ALIGN_FILL);
        gtk_widget_set_hexpand(frame, TRUE);
        gtk_widget_set_vexpand(frame, TRUE);
        if (body) gtk_widget_set_visible(body, TRUE);
        if (picture) gtk_widget_set_visible(picture, TRUE);
        gtk_widget_remove_css_class(frame, "mpcecc-pixel-max");
        gtk_widget_add_css_class(frame, "mpcecc-pixel-normal");
    }
    gtk_widget_queue_resize(frame);
    gtk_widget_queue_draw(frame);
}


static void
mpcecc_pixel_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user_data)
{
    (void)gesture; (void)x; (void)y;
    GtkWidget *frame = user_data;
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
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-pw-state")) == 1)
        return; /* maximized */
    int ml = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-drag-ml"));
    int mt = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(frame), "mpcecc-drag-mt"));
    int nml = ml + (int)offset_x;
    int nmt = mt + (int)offset_y;
    if (nml < 0) nml = 0;
    if (nmt < 0) nmt = 0;
    if (nml > 900) nml = 900;
    if (nmt > 700) nmt = 700;
    gtk_widget_set_margin_start(frame, nml);
    gtk_widget_set_margin_top(frame, nmt);
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
    (void)button; (void)user_data;
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
    gchar *xvfb_args[] = { "Xvfb", mpcecc_pixel_display, "-screen", "0", geometry, "-ac", "-nolisten", "tcp", NULL };
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




static gboolean
mpcecc_control_incoming(GSocketService *service, GSocketConnection *conn,
                        GObject *source, gpointer user_data)
{
    (void)service; (void)source; (void)user_data;
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    gchar buf[8192];
    gssize n = g_input_stream_read(in, buf, sizeof(buf)-1, NULL, NULL);
    if (n <= 0) return TRUE;
    buf[n] = 0;

    char *reply = NULL;
    if (g_str_has_prefix(buf, "LAUNCH ")) {
        /* LAUNCH W H FPS -- cmd args... */
        int w = 1280, h = 720, fps = 15;
        char *p = buf + 7;
        sscanf(p, "%d %d %d", &w, &h, &fps);
        char *sep = strstr(p, " -- ");
        if (sep) {
            sep += 4;
            while (*sep == ' ') ++sep;
            /* strip CR/LF so "firefox\n" does not become the executable name */
            g_strstrip(sep);
            for (char *q = sep; *q; ++q)
                if (*q == '\n' || *q == '\r') *q = ' ';
            char **argv = g_strsplit(sep, " ", -1);
            GPtrArray *clean = g_ptr_array_new();
            for (int i = 0; argv[i]; ++i) {
                g_strstrip(argv[i]);
                if (argv[i][0])
                    g_ptr_array_add(clean, g_strdup(argv[i]));
            }
            g_ptr_array_add(clean, NULL);
            g_strfreev(argv);
            if (clean->len < 2) {
                g_ptr_array_free(clean, TRUE);
                reply = g_strdup("ERR no program\n");
            } else {
                /* FreeDOS-style: pixel framebuffer INSIDE the terminal tab.
                 * Never a separate KDE/GTK desktop window. */
                mpcecc_embed_in_tab((char**)clean->pdata, w, h, fps);
                g_ptr_array_free(clean, TRUE);
                reply = g_strdup("OK embedded\n");
            }
        } else {
            reply = g_strdup("ERR syntax\n");
        }
    } else if (g_str_has_prefix(buf, "HIDE")) {
        mpcecc_hide_pixels_on_current_tab();
        mpcecc_pixel_cleanup();
        reply = g_strdup("OK\n");
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
    g_setenv("MPCECC_VERSION", "1.6", TRUE);
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
                         "MarekPC Extended Capabilities Console 1.6");
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

    gtk_window_present(ecc->window);
}

/* ============================================================
 * F3 Aero Window Picker (stub – full virtual windows next)
 * ============================================================ */

static void
mpcecc_show_aero_picker(MPCECC *ecc)
{
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "MPCECC Window Picker");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), ecc->window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 480, 300);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css,
        "window { background: alpha(#1a1a2e, 0.94); border-radius: 8px; }"
        "label { color: #d0d0ff; }"
        "button { background: alpha(#2a2a4a, 0.9); color: #e0e0ff; border-radius: 6px; padding: 8px 16px; }"
        "button:hover { background: alpha(#3a3a6a, 0.95); }");
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 20);
    gtk_widget_set_margin_bottom(root, 20);
    gtk_widget_set_margin_start(root, 20);
    gtk_widget_set_margin_end(root, 20);
    gtk_window_set_child(GTK_WINDOW(dialog), root);

    GtkWidget *label = gtk_label_new(
        "Aero Window Picker\n\n"
        "Wallpaper is restored.\n"
        "Full in-terminal virtual windows\n"
        "(no KDE/Wayland external windows)\n"
        "coming in the next pass.\n\n"
        "Your sketch = LXQt-like window style.");
    gtk_label_set_justify(GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(root), label);

    GtkWidget *btn = gtk_button_new_with_label("Close");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_box_append(GTK_BOX(root), btn);

    gtk_window_present(GTK_WINDOW(dialog));
}

static gboolean
mpcecc_global_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
                  GdkModifierType state, gpointer user_data)
{
    (void)controller; (void)keycode; (void)state;
    if (keyval == GDK_KEY_F3) {
        mpcecc_show_aero_picker((MPCECC *)user_data);
        return TRUE;
    }
    return FALSE;
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
        mpcecc_window_command = &argv[i];
        mpcecc_window_command_argc = argc - i;
        app_argc = 1; app_argv = application_argv;
    }
    GtkApplication *application = gtk_application_new("com.marekpc.mpcecc", mpcecc_window_command ? G_APPLICATION_NON_UNIQUE : G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(mpcecc_activate), &ecc);
    int status = g_application_run(G_APPLICATION(application), app_argc, app_argv);
    g_object_unref(application);
    return status;
}
