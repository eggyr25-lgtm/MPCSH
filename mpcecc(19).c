/*
 * MarekPC Extended Capabilities Console (MPCECC)
 * GTK4 + VTE terminal console with profiles, wallpaper,
 * tabs, clipboard shortcuts, context menu, and settings.
 *
 * VTE natively handles:
 *   Ctrl+Shift+C = copy selected text
 *   Ctrl+Shift+V = paste
 * MPCECC additionally handles Ctrl+V paste.
 *
 * Fixes in this revision:
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
    char *profile_name;
    char *shell;
    /* TRUE once the user asked to close this tab: child-exited must
     * not be treated as a crash (VTE kills the shell with SIGHUP
     * when the terminal widget is destroyed). */
    gboolean closing;
    GtkWindow *crash_dialog;
} MpcEccTab;

typedef struct {
    MPCECC *ecc;
    GtkDropDown *selector;
    GtkEntry *name;
    GtkEntry *wallpaper;
    GtkEntry *font;
    GtkEntry *shell;
} MpcEccSettings;

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

static gboolean
mpcecc_crash_dialog_close_request(GtkWindow *dialog, gpointer user_data)
{
    MpcEccTab *tab = user_data;

    if (tab && tab->crash_dialog == dialog)
        tab->crash_dialog = NULL;

    gtk_window_destroy(dialog);
    return TRUE;
}

static void
mpcecc_crash_dialog_response(GtkButton *button, gpointer user_data)
{
    int response = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
                                                     "mpcecc-response"));
    GtkWidget *dialog = GTK_WIDGET(user_data);
    MpcEccTab *maybe_tab = g_object_get_data(G_OBJECT(dialog), "mpcecc-tab");
    MPCECC *ecc = g_object_get_data(G_OBJECT(dialog), "mpcecc-ecc");

    if (maybe_tab && maybe_tab->crash_dialog == GTK_WINDOW(dialog))
        maybe_tab->crash_dialog = NULL;

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
    if (!tab || tab->closing)
        return;

    /* A terminal can report more than one failure while its PTY is being
     * torn down. Keep one actionable dialog per tab instead of stacking
     * modal windows that make the UI appear permanently stuck. */
    if (tab->crash_dialog) {
        gtk_window_present(tab->crash_dialog);
        return;
    }

    char title[512];

    snprintf(title, sizeof(title), "%s crashed",
             tab->shell ? tab->shell : "The shell");

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_window_set_transient_for(GTK_WINDOW(dialog), tab->ecc->window);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_hide_on_close(GTK_WINDOW(dialog), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 160);

    tab->crash_dialog = GTK_WINDOW(dialog);
    g_signal_connect(dialog, "close-request",
                     G_CALLBACK(mpcecc_crash_dialog_close_request), tab);

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

    if (tab->crash_dialog) {
        GtkWindow *dialog = tab->crash_dialog;
        tab->crash_dialog = NULL;
        gtk_window_destroy(dialog);
    }

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
        "      </item>"
        "      <item>"
        "        <attribute name='label'>Paste</attribute>"
        "        <attribute name='action'>win.paste</attribute>"
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
        { "<Control>v",         "win.paste" },
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

static void
mpcecc_activate(GtkApplication *application, gpointer user_data)
{
    MPCECC *ecc = user_data;

    ecc->app = application;
    ecc->profiles = mpcecc_load_profiles();
    ecc->active_profile = g_strdup("MarekPC Purple");

    ecc->window = GTK_WINDOW(gtk_application_window_new(application));

    gtk_window_set_title(ecc->window,
                         "MarekPC Extended Capabilities Console");
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

int
main(int argc, char **argv)
{
    MPCECC ecc = {0};

    GtkApplication *application = gtk_application_new(
        "com.marekpc.mpcecc",
        G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect(application, "activate",
                     G_CALLBACK(mpcecc_activate), &ecc);

    int status = g_application_run(G_APPLICATION(application),
                                   argc, argv);

    g_object_unref(application);

    return status;
}
