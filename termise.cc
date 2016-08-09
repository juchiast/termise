/*
 * Copyright (C) 2016 Daniel Micay, Do Duy
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <vector>
#include <string>

#include <gtk/gtk.h>
#include <vte/vte.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "util/maybe.hh"

using namespace std::placeholders;

/* Allow scales a bit smaller and a bit larger than the usual pango ranges */
#define TERMINAL_SCALE_XXX_SMALL   (PANGO_SCALE_XX_SMALL/1.2)
#define TERMINAL_SCALE_XXXX_SMALL  (TERMINAL_SCALE_XXX_SMALL/1.2)
#define TERMINAL_SCALE_XXXXX_SMALL (TERMINAL_SCALE_XXXX_SMALL/1.2)
#define TERMINAL_SCALE_XXX_LARGE   (PANGO_SCALE_XX_LARGE*1.2)
#define TERMINAL_SCALE_XXXX_LARGE  (TERMINAL_SCALE_XXX_LARGE*1.2)
#define TERMINAL_SCALE_XXXXX_LARGE (TERMINAL_SCALE_XXXX_LARGE*1.2)
#define TERMINAL_SCALE_MINIMUM     (TERMINAL_SCALE_XXXXX_SMALL/1.2)
#define TERMINAL_SCALE_MAXIMUM     (TERMINAL_SCALE_XXXXX_LARGE*1.2)

static const std::vector<double> zoom_factors = {
    TERMINAL_SCALE_MINIMUM,
    TERMINAL_SCALE_XXXXX_SMALL,
    TERMINAL_SCALE_XXXX_SMALL,
    TERMINAL_SCALE_XXX_SMALL,
    PANGO_SCALE_XX_SMALL,
    PANGO_SCALE_X_SMALL,
    PANGO_SCALE_SMALL,
    PANGO_SCALE_MEDIUM,
    PANGO_SCALE_LARGE,
    PANGO_SCALE_X_LARGE,
    PANGO_SCALE_XX_LARGE,
    TERMINAL_SCALE_XXX_LARGE,
    TERMINAL_SCALE_XXXX_LARGE,
    TERMINAL_SCALE_XXXXX_LARGE,
    TERMINAL_SCALE_MAXIMUM
};

struct config_info {
    gboolean dynamic_title, urgent_on_bell, size_hints;
    gboolean modify_other_keys;
    gboolean fullscreen;
    char *config_file;
    gdouble font_scale;
    std::vector<PangoFontDescription *> fonts;
    long unsigned int current_font;
};

struct keybind_info {
    GtkWindow *window;
    VteTerminal *vte;
    config_info config;
    std::function<void (GtkWindow *)> fullscreen_toggle;
};

static void window_title_cb(VteTerminal *vte, gboolean *dynamic_title);
static gboolean window_state_cb(GtkWindow *window, GdkEventWindowState *event, keybind_info *info);
static gboolean key_press_cb(VteTerminal *vte, GdkEventKey *event, keybind_info *info);
static void bell_cb(GtkWidget *vte, gboolean *urgent_on_bell);
static gboolean focus_cb(GtkWindow *window);

static void get_vte_padding(VteTerminal *vte, int *left, int *top, int *right, int *bottom);
static void load_config(GtkWindow *window, VteTerminal *vte, config_info *info,
                        char **geometry, char **icon);
static void set_config(GtkWindow *window, VteTerminal *vte, config_info *info,
                        char **geometry, char **icon, GKeyFile *config);

static std::function<void ()> reload_config;

static void override_background_color(GtkWidget *widget, GdkRGBA *rgba) {
    GtkCssProvider *provider = gtk_css_provider_new();

    gchar *colorstr = gdk_rgba_to_string(rgba);
    char *css = g_strdup_printf("* { background-color: %s; }", colorstr);
    gtk_css_provider_load_from_data(provider, css, -1, nullptr);
    g_free(colorstr);
    g_free(css);

    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
                                   GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

static const std::map<int, const char *> modify_table = {
    { GDK_KEY_Tab,        "\033[27;5;9~"  },
    { GDK_KEY_Return,     "\033[27;5;13~" },
    { GDK_KEY_apostrophe, "\033[27;5;39~" },
    { GDK_KEY_comma,      "\033[27;5;44~" },
    { GDK_KEY_minus,      "\033[27;5;45~" },
    { GDK_KEY_period,     "\033[27;5;46~" },
    { GDK_KEY_0,          "\033[27;5;48~" },
    { GDK_KEY_1,          "\033[27;5;49~" },
    { GDK_KEY_9,          "\033[27;5;57~" },
    { GDK_KEY_semicolon,  "\033[27;5;59~" },
    { GDK_KEY_equal,      "\033[27;5;61~" },
    { GDK_KEY_exclam,     "\033[27;6;33~" },
    { GDK_KEY_quotedbl,   "\033[27;6;34~" },
    { GDK_KEY_numbersign, "\033[27;6;35~" },
    { GDK_KEY_dollar,     "\033[27;6;36~" },
    { GDK_KEY_percent,    "\033[27;6;37~" },
    { GDK_KEY_ampersand,  "\033[27;6;38~" },
    { GDK_KEY_parenleft,  "\033[27;6;40~" },
    { GDK_KEY_parenright, "\033[27;6;41~" },
    { GDK_KEY_asterisk,   "\033[27;6;42~" },
    { GDK_KEY_plus,       "\033[27;6;43~" },
    { GDK_KEY_colon,      "\033[27;6;58~" },
    { GDK_KEY_less,       "\033[27;6;60~" },
    { GDK_KEY_greater,    "\033[27;6;62~" },
    { GDK_KEY_question,   "\033[27;6;63~" },
};

static const std::map<int, const char *> modify_meta_table = {
    { GDK_KEY_Tab,        "\033[27;13;9~"  },
    { GDK_KEY_Return,     "\033[27;13;13~" },
    { GDK_KEY_apostrophe, "\033[27;13;39~" },
    { GDK_KEY_comma,      "\033[27;13;44~" },
    { GDK_KEY_minus,      "\033[27;13;45~" },
    { GDK_KEY_period,     "\033[27;13;46~" },
    { GDK_KEY_0,          "\033[27;13;48~" },
    { GDK_KEY_1,          "\033[27;13;49~" },
    { GDK_KEY_9,          "\033[27;13;57~" },
    { GDK_KEY_semicolon,  "\033[27;13;59~" },
    { GDK_KEY_equal,      "\033[27;13;61~" },
    { GDK_KEY_exclam,     "\033[27;14;33~" },
    { GDK_KEY_quotedbl,   "\033[27;14;34~" },
    { GDK_KEY_numbersign, "\033[27;14;35~" },
    { GDK_KEY_dollar,     "\033[27;14;36~" },
    { GDK_KEY_percent,    "\033[27;14;37~" },
    { GDK_KEY_ampersand,  "\033[27;14;38~" },
    { GDK_KEY_parenleft,  "\033[27;14;40~" },
    { GDK_KEY_parenright, "\033[27;14;41~" },
    { GDK_KEY_asterisk,   "\033[27;14;42~" },
    { GDK_KEY_plus,       "\033[27;14;43~" },
    { GDK_KEY_colon,      "\033[27;14;58~" },
    { GDK_KEY_less,       "\033[27;14;60~" },
    { GDK_KEY_greater,    "\033[27;14;62~" },
    { GDK_KEY_question,   "\033[27;14;63~" },
};

static gboolean modify_key_feed(GdkEventKey *event, keybind_info *info,
                                const std::map<int, const char *>& table) {
    if (info->config.modify_other_keys) {
        unsigned int keyval = gdk_keyval_to_lower(event->keyval);
        auto entry = table.find((int)keyval);

        if (entry != table.end()) {
            vte_terminal_feed_child(info->vte, entry->second, -1);
            return TRUE;
        }
    }
    return FALSE;
}

static void set_size_hints(GtkWindow *window, VteTerminal *vte) {
    static const GdkWindowHints wh = (GdkWindowHints)(GDK_HINT_RESIZE_INC | GDK_HINT_MIN_SIZE |
                                                      GDK_HINT_BASE_SIZE);
    const int char_width = (int)vte_terminal_get_char_width(vte);
    const int char_height = (int)vte_terminal_get_char_height(vte);
    int padding_left, padding_top, padding_right, padding_bottom;
    get_vte_padding(vte, &padding_left, &padding_top, &padding_right, &padding_bottom);

    GdkGeometry hints;
    hints.base_width = char_width + padding_left + padding_right;
    hints.base_height = char_height + padding_top + padding_bottom;
    hints.min_width = hints.base_width;
    hints.min_height = hints.base_height;
    hints.width_inc  = char_width;
    hints.height_inc = char_height;

    gtk_window_set_geometry_hints(GTK_WINDOW(window), NULL, &hints, wh);
}

/* {{{ CALLBACKS */
void window_title_cb(VteTerminal *vte, gboolean *dynamic_title) {
    const char *const title = *dynamic_title ? vte_terminal_get_window_title(vte) : nullptr;
    gtk_window_set_title(GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(vte))),
                         title ? title : "termite");
}

static void reset_font_scale(VteTerminal *vte, gdouble scale) {
    vte_terminal_set_font_scale(vte, scale);
}

static void increase_font_scale(VteTerminal *vte) {
    gdouble scale = vte_terminal_get_font_scale(vte);

    for (auto it = zoom_factors.begin(); it != zoom_factors.end(); ++it) {
        if ((*it - scale) > 1e-6) {
            vte_terminal_set_font_scale(vte, *it);
            return;
        }
    }
}

static void decrease_font_scale(VteTerminal *vte) {
    gdouble scale = vte_terminal_get_font_scale(vte);

    for (auto it = zoom_factors.rbegin(); it != zoom_factors.rend(); ++it) {
        if ((scale - *it) > 1e-6) {
            vte_terminal_set_font_scale(vte, *it);
            return;
        }
    }
}

gboolean window_state_cb(GtkWindow *, GdkEventWindowState *event, keybind_info *info) {
    if (event->new_window_state & GDK_WINDOW_STATE_FULLSCREEN)
        info->fullscreen_toggle = gtk_window_unfullscreen;
    else
        info->fullscreen_toggle = gtk_window_fullscreen;
    return FALSE;
}

gboolean key_press_cb(VteTerminal *vte, GdkEventKey *event, keybind_info *info) {
    const guint modifiers = event->state & gtk_accelerator_get_default_mod_mask();

    if (info->config.fullscreen && event->keyval == GDK_KEY_F11) {
        info->fullscreen_toggle(info->window);
        return TRUE;
    }

    if (modifiers == (GDK_CONTROL_MASK|GDK_SHIFT_MASK)) {
        switch (gdk_keyval_to_lower(event->keyval)) {
            case GDK_KEY_plus:
                increase_font_scale(vte);
                return TRUE;
            case GDK_KEY_underscore:
                info->config.current_font++;
                info->config.current_font %= info->config.fonts.size();
                vte_terminal_set_font(vte, info->config.fonts[info->config.current_font]);
                return TRUE;
            case GDK_KEY_c:
                vte_terminal_copy_clipboard(vte);
                return TRUE;
            case GDK_KEY_v:
                vte_terminal_paste_clipboard(vte);
                return TRUE;
            case GDK_KEY_r:
                reload_config();
                return TRUE;
            default:
                if (modify_key_feed(event, info, modify_table))
                    return TRUE;
        }
    } else if ((modifiers == (GDK_CONTROL_MASK|GDK_MOD1_MASK)) ||
               (modifiers == (GDK_CONTROL_MASK|GDK_MOD1_MASK|GDK_SHIFT_MASK))) {
        if (modify_key_feed(event, info, modify_meta_table))
            return TRUE;
    } else if (modifiers == GDK_CONTROL_MASK) {
        switch (gdk_keyval_to_lower(event->keyval)) {
            case GDK_KEY_minus:
                decrease_font_scale(vte);
                return TRUE;
            case GDK_KEY_equal:
                reset_font_scale(vte, info->config.font_scale);
                return TRUE;
            default:
                if (modify_key_feed(event, info, modify_table))
                    return TRUE;
        }
    }
    return FALSE;
}

static void bell_cb(GtkWidget *vte, gboolean *urgent_on_bell) {
    if (*urgent_on_bell) {
        gtk_window_set_urgency_hint(GTK_WINDOW(gtk_widget_get_toplevel(vte)), TRUE);
    }
}

gboolean focus_cb(GtkWindow *window) {
    gtk_window_set_urgency_hint(window, FALSE);
    return FALSE;
}
/* }}} */

void get_vte_padding(VteTerminal *vte, int *left, int *top, int *right, int *bottom) {
    GtkBorder border;
    gtk_style_context_get_padding(gtk_widget_get_style_context(GTK_WIDGET(vte)),
                                  gtk_widget_get_state_flags(GTK_WIDGET(vte)),
                                  &border);
    *left = border.left;
    *right = border.right;
    *top = border.top;
    *bottom = border.bottom;
}

/* {{{ CONFIG LOADING */
template<typename T>
maybe<T> get_config(T (*get)(GKeyFile *, const char *, const char *, GError **),
                    GKeyFile *config, const char *group, const char *key) {
    GError *error = nullptr;
    maybe<T> value = get(config, group, key, &error);
    if (error) {
        g_error_free(error);
        return {};
    }
    return value;
}

auto get_config_integer(std::bind(get_config<int>, g_key_file_get_integer,
                                  _1, _2, _3));
auto get_config_string(std::bind(get_config<char *>, g_key_file_get_string,
                                 _1, _2, _3));
auto get_config_double(std::bind(get_config<double>, g_key_file_get_double,
                                 _1, _2, _3));

static maybe<GdkRGBA> get_config_color(GKeyFile *config, const char *section, const char *key) {
    if (auto s = get_config_string(config, section, key)) {
        GdkRGBA color;
        if (gdk_rgba_parse(&color, *s)) {
            g_free(*s);
            return color;
        }
        g_printerr("invalid color string: %s\n", *s);
        g_free(*s);
    }
    return {};
}

static std::vector<PangoFontDescription *> split_fonts (char *str) {
    std::vector<PangoFontDescription *> ret;
    std::string s(str);
    std::string buf = "";
    for (unsigned int i = 0; i<s.size(); i++) {
        if (s[i] == ',') {
            ret.push_back(pango_font_description_from_string(buf.data()));
            buf = "";
        } else buf += s[i];
    }
    ret.push_back(pango_font_description_from_string(buf.data()));
    return ret;
}

static void load_config(GtkWindow *window, VteTerminal *vte, config_info *info,
                        char **geometry, char **icon) {
    const std::string default_path = "/termite/config";
    GKeyFile *config = g_key_file_new();

    gboolean loaded = FALSE;

    if (info->config_file) {
        loaded = g_key_file_load_from_file(config,
                                           info->config_file,
                                           G_KEY_FILE_NONE, nullptr);
    }

    if (!loaded) {
        loaded = g_key_file_load_from_file(config,
                                           (g_get_user_config_dir() + default_path).c_str(),
                                           G_KEY_FILE_NONE, nullptr);
    }

    for (const char *const *dir = g_get_system_config_dirs();
         !loaded && *dir; dir++) {
        loaded = g_key_file_load_from_file(config, (*dir + default_path).c_str(),
                                           G_KEY_FILE_NONE, nullptr);
    }

    if (loaded) {
        set_config(window, vte, info, geometry, icon, config);
    }
    g_key_file_free(config);
}

static void set_config(GtkWindow *window, VteTerminal *vte, config_info *info,
                        char **geometry, char **icon, GKeyFile *config) {
    if (geometry) {
        if (auto s = get_config_string(config, "options", "geometry")) {
            *geometry = *s;
        }
    }

    auto cfg_bool = [config](const char *key, gboolean value) {
        return get_config<gboolean>(g_key_file_get_boolean,
                                    config, "options", key).get_value_or(value);
    };

    vte_terminal_set_scroll_on_output(vte, cfg_bool("scroll_on_output", FALSE));
    vte_terminal_set_scroll_on_keystroke(vte, cfg_bool("scroll_on_keystroke", TRUE));
    vte_terminal_set_audible_bell(vte, cfg_bool("audible_bell", FALSE));
    vte_terminal_set_mouse_autohide(vte, cfg_bool("mouse_autohide", TRUE));
    vte_terminal_set_allow_bold(vte, cfg_bool("allow_bold", TRUE));
    vte_terminal_search_set_wrap_around(vte, cfg_bool("search_wrap", TRUE));
    info->dynamic_title = cfg_bool("dynamic_title", TRUE);
    info->urgent_on_bell = cfg_bool("urgent_on_bell", TRUE);
    info->size_hints = cfg_bool("size_hints", FALSE);
    info->modify_other_keys = cfg_bool("modify_other_keys", FALSE);
    info->fullscreen = cfg_bool("fullscreen", TRUE);
    info->font_scale = vte_terminal_get_font_scale(vte);

    if (auto s = get_config_string(config, "options", "font")) {
        info->fonts = split_fonts(*s);
        info->current_font = 0;
        vte_terminal_set_font(vte, info->fonts[info->current_font]);
        g_free(*s);
    }

    if (auto i = get_config_integer(config, "options", "scrollback_lines")) {
        vte_terminal_set_scrollback_lines(vte, *i);
    }

    if (auto s = get_config_string(config, "options", "cursor_blink")) {
        if (!g_ascii_strcasecmp(*s, "system")) {
            vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_SYSTEM);
        } else if (!g_ascii_strcasecmp(*s, "on")) {
            vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_ON);
        } else if (!g_ascii_strcasecmp(*s, "off")) {
            vte_terminal_set_cursor_blink_mode(vte, VTE_CURSOR_BLINK_OFF);
        }
        g_free(*s);
    }

    if (auto s = get_config_string(config, "options", "cursor_shape")) {
        if (!g_ascii_strcasecmp(*s, "block")) {
            vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_BLOCK);
        } else if (!g_ascii_strcasecmp(*s, "ibeam")) {
            vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_IBEAM);
        } else if (!g_ascii_strcasecmp(*s, "underline")) {
            vte_terminal_set_cursor_shape(vte, VTE_CURSOR_SHAPE_UNDERLINE);
        }
        g_free(*s);
    }

    if (icon) {
        if (auto s = get_config_string(config, "options", "icon_name")) {
            *icon = *s;
        }
    }

    if (info->size_hints) {
        set_size_hints(GTK_WINDOW(window), vte);
    }

    if (auto color = get_config_color(config, "colors", "foreground")) {
        vte_terminal_set_color_foreground(vte, &*color);
        vte_terminal_set_color_bold(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "foreground_bold")) {
        vte_terminal_set_color_bold(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "background")) {
        vte_terminal_set_color_background(vte, &*color);
        override_background_color(GTK_WIDGET(window), &*color);
    }
    if (auto color = get_config_color(config, "colors", "cursor")) {
        vte_terminal_set_color_cursor(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "cursor_foreground")) {
        vte_terminal_set_color_cursor_foreground(vte, &*color);
    }
    if (auto color = get_config_color(config, "colors", "highlight")) {
        vte_terminal_set_color_highlight(vte, &*color);
    }

}/*}}}*/

static void exit_with_status(VteTerminal *, int status) {
    gtk_main_quit();
    exit(WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE);
}

static void exit_with_success(VteTerminal *) {
    gtk_main_quit();
    exit(EXIT_SUCCESS);
}

static char *get_user_shell_with_fallback() {
    if (const char *env = g_getenv("SHELL"))
        return g_strdup(env);

    if (char *command = vte_get_user_shell())
        return command;

    return g_strdup("/bin/sh");
}

static void on_alpha_screen_changed(GtkWindow *window, GdkScreen *, void *) {
    GdkScreen *screen = gtk_widget_get_screen(GTK_WIDGET(window));
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);

    if (!visual)
        visual = gdk_screen_get_system_visual(screen);

    gtk_widget_set_visual(GTK_WIDGET(window), visual);
}

int main(int argc, char **argv) {
    GError *error = nullptr;
    const char *const term = "xterm-termise";
    char *directory = nullptr;
    gboolean version = FALSE, hold = FALSE;

    GOptionContext *context = g_option_context_new(nullptr);
    char *role = nullptr, *geometry = nullptr, *execute = nullptr, *config_file = nullptr;
    char *title = nullptr, *icon = nullptr;
    const GOptionEntry entries[] = {
        {"version", 'v', 0, G_OPTION_ARG_NONE, &version, "Version info", nullptr},
        {"exec", 'e', 0, G_OPTION_ARG_STRING, &execute, "Command to execute", "COMMAND"},
        {"role", 'r', 0, G_OPTION_ARG_STRING, &role, "The role to use", "ROLE"},
        {"title", 't', 0, G_OPTION_ARG_STRING, &title, "Window title", "TITLE"},
        {"directory", 'd', 0, G_OPTION_ARG_STRING, &directory, "Change to directory", "DIRECTORY"},
        {"geometry", 0, 0, G_OPTION_ARG_STRING, &geometry, "Window geometry", "GEOMETRY"},
        {"hold", 0, 0, G_OPTION_ARG_NONE, &hold, "Remain open after child process exits", nullptr},
        {"config", 'c', 0, G_OPTION_ARG_STRING, &config_file, "Path of config file", "CONFIG"},
        {"icon", 'i', 0, G_OPTION_ARG_STRING, &icon, "Icon", "ICON"},
        {nullptr, 0, 0, G_OPTION_ARG_NONE, nullptr, nullptr, nullptr}
    };
    g_option_context_add_main_entries(context, entries, nullptr);
    g_option_context_add_group(context, gtk_get_option_group(TRUE));

    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("option parsing failed: %s\n", error->message);
        g_clear_error (&error);
        return EXIT_FAILURE;
    }

    g_option_context_free(context);

    if (version) {
        g_print("termise %s\n", TERMITE_VERSION);
        return EXIT_SUCCESS;
    }

    if (directory) {
        if (chdir(directory) == -1) {
            perror("chdir");
            return EXIT_FAILURE;
        }
        g_free(directory);
    }

    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    GtkWidget *vte_widget = vte_terminal_new();
    VteTerminal *vte = VTE_TERMINAL(vte_widget);

    if (role) {
        gtk_window_set_role(GTK_WINDOW(window), role);
        g_free(role);
    }

    char **command_argv;
    char *default_argv[2] = {nullptr, nullptr};

    if (execute) {
        int argcp;
        char **argvp;
        g_shell_parse_argv(execute, &argcp, &argvp, &error);
        if (error) {
            g_printerr("failed to parse command: %s\n", error->message);
            return EXIT_FAILURE;
        }
        command_argv = argvp;
    } else {
        default_argv[0] = get_user_shell_with_fallback();
        command_argv = default_argv;
    }

    keybind_info info {
        GTK_WINDOW(window), vte,
        {FALSE, FALSE, FALSE, FALSE, FALSE, config_file, 0, {}, 0},
        gtk_window_fullscreen
    };

    load_config(GTK_WINDOW(window), vte, &info.config, geometry ? nullptr : &geometry,
                icon ? nullptr : &icon);

    reload_config = [&]{
        load_config(GTK_WINDOW(window), vte, &info.config, nullptr, nullptr);
    };
    signal(SIGUSR1, [](int){ reload_config(); });

    GdkRGBA transparent {0, 0, 0, 0};

    override_background_color(vte_widget, &transparent);

    gtk_container_add(GTK_CONTAINER(window), vte_widget);

    if (!hold) {
        g_signal_connect(vte, "child-exited", G_CALLBACK(exit_with_status), nullptr);
    }
    g_signal_connect(window, "destroy", G_CALLBACK(exit_with_success), nullptr);
    g_signal_connect(vte, "key-press-event", G_CALLBACK(key_press_cb), &info);
    g_signal_connect(vte, "bell", G_CALLBACK(bell_cb), &info.config.urgent_on_bell);

    g_signal_connect(window, "focus-in-event",  G_CALLBACK(focus_cb), nullptr);
    g_signal_connect(window, "focus-out-event", G_CALLBACK(focus_cb), nullptr);

    on_alpha_screen_changed(GTK_WINDOW(window), nullptr, nullptr);
    g_signal_connect(window, "screen-changed", G_CALLBACK(on_alpha_screen_changed), nullptr);

    if (info.config.fullscreen) {
        g_signal_connect(window, "window-state-event", G_CALLBACK(window_state_cb), &info);
    }

    if (title) {
        info.config.dynamic_title = FALSE;
        gtk_window_set_title(GTK_WINDOW(window), title);
        g_free(title);
    } else {
        g_signal_connect(vte, "window-title-changed", G_CALLBACK(window_title_cb),
                         &info.config.dynamic_title);
        window_title_cb(vte, &info.config.dynamic_title);
    }

    if (geometry) {
        if (!gtk_window_parse_geometry(GTK_WINDOW(window), geometry)) {
            g_printerr("invalid geometry string: %s\n", geometry);
        }
        g_free(geometry);
    }

    if (icon) {
        gtk_window_set_icon_name(GTK_WINDOW(window), icon);
        g_free(icon);
    }

    gtk_widget_grab_focus(vte_widget);
    gtk_widget_show_all(window);

    char **env = g_get_environ();

#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_SCREEN(gtk_widget_get_screen(window))) {
        GdkWindow *gdk_window = gtk_widget_get_window(window);
        if (!gdk_window) {
            g_printerr("no window\n");
            return EXIT_FAILURE;
        }
        char xid_s[std::numeric_limits<long unsigned>::digits10 + 1];
        snprintf(xid_s, sizeof(xid_s), "%lu", GDK_WINDOW_XID(gdk_window));
        env = g_environ_setenv(env, "WINDOWID", xid_s, TRUE);
    }
#endif

    env = g_environ_setenv(env, "TERM", term, TRUE);

    GPid child_pid;
    if (vte_terminal_spawn_sync(vte, VTE_PTY_DEFAULT, nullptr, command_argv, env,
                                G_SPAWN_SEARCH_PATH, nullptr, nullptr, &child_pid, nullptr,
                                &error)) {
        vte_terminal_watch_child(vte, child_pid);
    } else {
        g_printerr("the command failed to run: %s\n", error->message);
        return EXIT_FAILURE;
    }

    int width, height, padding_left, padding_top, padding_right, padding_bottom;
    const long char_width = vte_terminal_get_char_width(vte);
    const long char_height = vte_terminal_get_char_height(vte);

    gtk_window_get_size(GTK_WINDOW(window), &width, &height);
    get_vte_padding(vte, &padding_left, &padding_top, &padding_right, &padding_bottom);
    vte_terminal_set_size(vte,
                          (width - padding_left - padding_right) / char_width,
                          (height - padding_top - padding_bottom) / char_height);

    g_strfreev(env);

    gtk_main();
    return EXIT_FAILURE; // child process did not cause termination
}

// vim: et:sts=4:sw=4:cino=(0:cc=100
