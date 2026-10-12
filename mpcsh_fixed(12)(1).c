/*
 * ============================================================
 *  MarekPC Shell — mpcsh.c
 *  mpcsh + mpcap, one binary, one soul.
 *
 *  Build (recommended):
 *      gcc mpcsh.c -o mpcsh $(pkg-config --cflags --libs gtk4)
 *  If the ANSI wallpaper banner is used (non-kitty terminals):
 *      gcc mpcsh.c -o mpcsh $(pkg-config --cflags --libs gtk4 gdk-pixbuf-2.0)
 *
 *  Fixes in this revision:
 *   1. Ctrl+C no longer kills mpcsh. The signal handler only sets a
 *      flag; the real bug was read(2)/waitpid(2) failing with EINTR and
 *      the code treating that as EOF -> the REPL exited. All blocking
 *      reads and waitpid calls now retry on EINTR. Ctrl+C cancels the
 *      current line / running command and returns to the prompt.
 *   2. `ls` and `wallpaper` built-ins are now actually dispatched.
 *   3. Corrupted UTF-8 glyph in the GUIInTerminal bottom border fixed.
 *   4. Multi-char constant '\\t' in path completion fixed to '\t'.
 *   5. Foreground children killed by signals report 128+sig.
 * ============================================================
 */

#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <wchar.h>
#include <locale.h>
#include <pwd.h>
#include <limits.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>

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
"Here, The MarekPC Shell syntax — do not switch to bash, that is a "
"shortcut and it is not welcome here.\n\n"
"You are operating inside the MarekPC Shell (mpcsh). mpcsh is its own "
"shell with its own syntax, its own scripting format (.mpcsh), and its "
"own GUI prompt layer (mpcap). It understands fish-style commands, it "
"supports pipelines of up to 64 segments, command substitution ($() and "
"backticks), and it executes .mpcsh scripts natively — including prompt "
"scripts that open a real Aero GUI window with buttons whose actions can "
"be written in fish, MarekPC Shell syntax itself, Python, C, C++, or C#.\n\n"
"Do NOT fall back to a compatibility shell, do NOT emit shell command strings, "
"and do NOT rewrite commands as foreign-shell one-liners. This engine "
"executes fish-style and MarekPC syntax directly. If a construct seems "
"unfamiliar, ask for the MarekPC Shell syntax or read the script header; "
"the answer is in the MarekPC Shell syntax. If you reach for a foreign "
"habit, stop, re-read this notice, and write the command the MarekPC way. "
"This is the shell. It is the MarekPC way, or it is the highway.\n";

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

/* Detect classic AI “switch to bash” escape attempts. */
static gboolean
mpcsh_looks_like_bash_escape(const char *line)
{
    if (!line || !*line) return FALSE;
    if (strcasestr(line, "bash -c") || strcasestr(line, "sh -c") ||
        strcasestr(line, "/bin/bash") || strcasestr(line, "/bin/sh") ||
        strcasestr(line, "exec bash") || strcasestr(line, "exec sh") ||
        strcasestr(line, "&&") || strcasestr(line, "||"))
        return TRUE;
    return FALSE;
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

/* Recursion depth guard for command substitution. */
#define MPCSH_MAX_SUBST_DEPTH 8
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
    while ((n = mpcsh_read_retry(pipefd[0], buf, sizeof(buf))) > 0)
        g_string_append_len(out, buf, n);
    close(pipefd[0]);

    int status = 0;
    mpcsh_waitpid_retry(pid, &status);

    while (out->len > 0 && (out->str[out->len-1] == '\n' || out->str[out->len-1] == '\r'))
        g_string_truncate(out, out->len - 1);

    return g_string_free(out, FALSE);
}

static char *
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

            if (!single && c == '$' && p[1] == '(') {
                int depth = 1;
                const char *q = p + 2;
                gboolean qsingle = FALSE, qdouble = FALSE;

                while (*q && depth) {
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

                char *inner = g_strndup(p + 2, (gsize)(q - (p + 2)));
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
            argv[argc++] = value;
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

    if (strstr(s, "&&") || strstr(s, "||"))
        return TRUE;

    if (strstr(s, "export ") == s || !strncmp(s, "export\t", 7))
        return TRUE;

    /* Reject simple NAME=value assignments.  Fish uses: set NAME value */
    const char *p = s;
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (isalpha((unsigned char)*p) || *p == '_') {
            ++p;
            while (mpcsh_identifier_char(*p)) ++p;
            if (*p == '=')
                return TRUE;
        }
        while (*p && *p != ';' && *p != '|') ++p;
        if (*p) ++p;
        if (!*p) break;
    }
    return FALSE;
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
static int mpcsh_guiinterm_builtin(char **argv);
static int mpcsh_set_builtin(char **argv);
static int mpcsh_cd_builtin(char **argv);
static int mpcsh_pwd_builtin(char **argv);
static int mpcsh_status_builtin(char **argv);
static int mpcsh_ls_builtin(char **argv);
static int mpcsh_wallpaper_builtin(char **argv);
static int mpcsh_imgcat_builtin(char **argv);
static int mpcsh_limits_builtin(char **argv);
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
        if (!strcmp(exec_argv[0], "help")) {
            int rc = mpcsh_help_builtin(exec_argv);
            fflush(NULL);
            _exit(rc);
        }

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

static int
mpcsh_run_pipeline(char *cmdline)
{
    if (!cmdline || !*cmdline)
        return 0;

    int rejected = mpcsh_reject_bash_constructs(cmdline);
    if (rejected) {
        mpcsh_last_status = rejected;
        return rejected;
    }

    char *expanded = mpcsh_expand_substitutions(cmdline);
    char *work = expanded;

    char *segments[MPCSH_MAX_PIPELINE + 1];
    int count = 0;
    char *p = work;
    char quote = 0;
    char *start = p;

    while (*p) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '\'' || *p == '"') {
            quote = *p;
        } else if (*p == '|') {
            *p = '\0';
            if (count >= MPCSH_MAX_PIPELINE) {
                fprintf(stderr, "mpcsh: too many pipeline segments (maximum: %d)\n", MPCSH_MAX_PIPELINE);
                g_free(expanded);
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
            g_free(expanded);
            return 126;
        }
    }

    if (count == 1) {
        char *argv[256];
        if (mpcsh_word_split(segments[0], argv, 256) == 0) {
            fprintf(stderr, "mpcsh: failed to parse command\n");
            g_free(expanded);
            return 126;
        }
        if (strcmp(argv[0], "exit") == 0) {
            int code = argv[1] ? atoi(argv[1]) : 0;
            mpcsh_free_argv(argv);
            g_free(expanded);
            exit(code);
        }
        if (strcmp(argv[0], "ctk") == 0) {
            int result = mpcsh_ctk_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcasecmp(argv[0], "portmap")) {
            int result = mpcsh_portmap_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcasecmp(argv[0], "guiinterm")) {
            int result = mpcsh_guiinterm_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcasecmp(argv[0], "ls")) {
            int result = mpcsh_ls_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcmp(argv[0], "mpcimgcat")) {
            int result = mpcsh_imgcat_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcasecmp(argv[0], "wallpaper")) {
            int result = mpcsh_wallpaper_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcmp(argv[0], "set")) {
            int result = mpcsh_set_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcmp(argv[0], "limits")) {
            int result = mpcsh_limits_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcmp(argv[0], "cd")) {
            int result = mpcsh_cd_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcmp(argv[0], "pwd")) {
            int result = mpcsh_pwd_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        if (!strcmp(argv[0], "status")) {
            int result = mpcsh_status_builtin(argv);
            mpcsh_free_argv(argv); g_free(expanded);
            mpcsh_last_status = result; return result;
        }
        int pid = mpcsh_spawn_segment(argv, STDIN_FILENO, STDOUT_FILENO);
        if (pid < 0) { mpcsh_free_argv(argv); g_free(expanded); mpcsh_last_status = 127; return 127; }
        int status = 0;
        /* EINTR-safe: Ctrl+C while a foreground command runs must not
         * kill mpcsh, and the child dies on its own SIGINT anyway. */
        mpcsh_waitpid_retry(pid, &status);
        int result = mpcsh_child_result(status);
        mpcsh_free_argv(argv);
        g_free(expanded);
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
            g_free(expanded);
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
                g_free(expanded);
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
            g_free(expanded);
            return 127;
        }

        pids[spawned++] = pid;
        prev_read = (i < count - 1) ? pipefd[0] : STDIN_FILENO;
    }

    int status = 0;
    for (int i = 0; i < spawned; ++i)
        mpcsh_waitpid_retry(pids[i], &status);

    int result = mpcsh_child_result(status);
    g_free(expanded);
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
mpcsh_run_line(char *line)
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

    printf("\n\033[1;35mLegend:\033[0m "
    "\033[1;34mBLUE inside = USB 3.x+\033[0m  "
    "\033[1;31mRED = ALWAYS ON\033[0m  "
    "\033[1;36mCYAN = USB-C\033[0m  "
    "\033[1;33mYELLOW = Micro-B\033[0m  "
    "\033[1;32mGREEN = Ethernet\033[0m\n");

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
    char *copy = g_strdup(script);
    char *save = NULL;
    int last = 0;
    for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p || *p == '#') continue;
        char buf[MPCSH_MAX_LINE_HARD];
        g_strlcpy(buf, line, sizeof(buf));
        last = mpcsh_run_line(buf);
    }
    g_free(copy);
    return last;
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
    const char *wallpaper_path = "/bin/mpcshresources/wallpaper.png";
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
    const char *path = "/bin/mpcshresources/wallpaper.png";
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
    const char *path = "/bin/mpcshresources/wallpaper.png";
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
    const char *path = "/bin/mpcshresources/wallpaper.png";
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
    if (!argv[1]) { fprintf(stderr,"Usage: mpcimgcat IMAGE [IMAGE ...]\n"); return 2; }
    for (int i=1; argv[i]; ++i) {
        char *path = argv[i];
        if (path[0]=='~' && (path[1]=='/' || !path[1])) { const char *h=g_get_home_dir(); char *q=g_build_filename(h,path+ (path[1]=='/'?1:0),NULL); path=q; }
        GError *err=NULL;
        GdkPixbuf *pix=gdk_pixbuf_new_from_file(path,&err);
        if(!pix){fprintf(stderr,"mpcimgcat: %s: %s\n",path,err?err->message:"cannot decode image");if(err)g_error_free(err);if(path!=argv[i])g_free(path);continue;}
        struct winsize ws={0}; ioctl(STDOUT_FILENO,TIOCGWINSZ,&ws);
        if(!isatty(STDOUT_FILENO)||ws.ws_col<2||ws.ws_row<2){fprintf(stderr,"mpcimgcat: output must be a sized terminal\n");g_object_unref(pix);if(path!=argv[i])g_free(path);continue;}
        int cols=ws.ws_col; int rows=ws.ws_row>2?ws.ws_row-2:1;
        double factor=fmin((double)cols/gdk_pixbuf_get_width(pix),(double)(rows*2)/gdk_pixbuf_get_height(pix));
        int w=(int)(gdk_pixbuf_get_width(pix)*factor), h=(int)(gdk_pixbuf_get_height(pix)*factor);
        if(w<1)w=1;if(h<1)h=1;
        /* Kitty and iTerm2 have real inline pixels; ANSI/VT gets a color half-block raster. */
        const char *term=g_getenv("TERM");
        gboolean kitty=g_getenv("KITTY_WINDOW_ID")||(term&&strstr(term,"kitty"));
        gboolean iterm=g_getenv("LC_TERMINAL")&&!strcmp(g_getenv("LC_TERMINAL"),"iTerm2");
        if(kitty||iterm){
            gchar *bytes=NULL; gsize nbytes=0;
            if(g_file_get_contents(path,&bytes,&nbytes,NULL)){
                gchar *b64=g_base64_encode((guchar*)bytes,nbytes);
                if(kitty){printf("\033_Ga=T,f=100,c=%d,r=%d,q=2;",cols,rows);for(char *p=b64;*p;){size_t n=strlen(p);if(n>4096)n=4096;printf("%.*s",(int)n,p);p+=n;if(*p)printf("\033_Gm=1,q=2;",0);}printf("\033\\\n");}
                else printf("\033]1337;File=inline=1;width=%d;height=%d;preserveAspectRatio=1:%s\007\n",cols,rows,b64);
                g_free(b64);g_free(bytes);g_object_unref(pix);if(path!=argv[i])g_free(path);continue;
            }
        }
        GdkPixbuf *scaled=gdk_pixbuf_scale_simple(pix,w,h,GDK_INTERP_BILINEAR);
        if(!scaled){g_object_unref(pix);if(path!=argv[i])g_free(path);continue;}
        int ch=gdk_pixbuf_get_n_channels(scaled), stride=gdk_pixbuf_get_rowstride(scaled);
        const guchar *px=gdk_pixbuf_get_pixels(scaled);
        printf("\033[0m\n");
        for(int y=0;y<h;y+=2){
            for(int x=0;x<w;x++){
                const guchar *a=px+y*stride+x*ch;
                const guchar *b=(y+1<h)?px+(y+1)*stride+x*ch:a;
                printf("\033[38;2;%u;%u;%u;48;2;%u;%u;%um▀",a[0],a[1],a[2],b[0],b[1],b[2]);
            }
            printf("\033[0m\n");
        }
        g_object_unref(scaled);g_object_unref(pix);if(path!=argv[i])g_free(path);
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
    got_sigint = 1;
    write(STDERR_FILENO, "\n", 1);
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
static int mpcsh_prompt_cells(const char *p)
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
    "cd", "pwd", "status", "set", "exit", "ctk", "portmap", "guiinterm",
    "wallpaper", "ls", "mpcimgcat", "colorit", "help", "limits", NULL
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
    raw = oldt; raw.c_lflag &= (tcflag_t)~(ICANON | ECHO); raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return 0;
    fputs("\033[?2004h", stdout); fflush(stdout);
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
        if (mpcsh_read_retry(STDIN_FILENO, &c, 1) != 1) break;
        if (c != '\t' && c != 27) mpcsh_tab_reset(ed);

        if (c == 3) {
            got_sigint = 1;
            mpcsh_redraw_line(prompt, line, ed->len, NULL);
            fputs("^C\r\n", stdout); out[0] = 0; result = 1; break;
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

    /* mpcsh does not force a terminal theme.  Your terminal owns its colors. */

    /* MPCECC provides the real GTK image background, so never paint
     *      the ANSI fallback banner over it. */
    if (!g_getenv("MPCECC"))
        mpcsh_terminal_wallpaper();

    fprintf(stderr,
            "\033[1;95m✦ MarekPC Shell\033[0m \033[1;35m— mpcsh\033[0m\n"
            "\033[0;36mPipeline limit: %d  |  $() supported\n\033[0m"
            "\033[0;36m↑/↓ history  ←/→ cursor  Ctrl+C cancels the current line; Ctrl+O inserts newline\033[0m\n"
            "\033[0;36mBuilt-ins: ctk, portmap, guiinterm, wallpaper, ls, mpcimgcat, colorit, help, limits\033[0m\n\n",
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
        /* High-contrast colours on the permanent purple background */
        snprintf(prompt, sizeof(prompt),
                 "\033[1;95m╭─\033[0m "
                 "\033[38;5;213m%s\033[0m "
                 "\033[38;5;177m%s\033[0m "
                 "\033[38;5;165m%s\033[0m\n"
                 "\033[1;95m╰─\033[0m\033[1;95m❯\033[0m ",
                 user ? user : "user", host, display);
        fputs(prompt, stdout);
        fflush(stdout);

        if (!mpcsh_readline_edit(prompt, line, sizeof(line), &history))
            break;
        if (got_sigint || !*line) continue;

        while (strlen(line) > 0 && line[strlen(line) - 1] == '\\') {
            line[strlen(line) - 1] = '\0';

            char continuation[MPCSH_MAX_LINE_HARD];
            if (!mpcsh_readline_edit(
                "\033[1;95m╰─❯\033[0m ",
                continuation, sizeof(continuation), &history))
                break;

            if (got_sigint) break;

            size_t current = strlen(line);
            size_t extra = strlen(continuation);

            if (current + 1 + extra < sizeof(line)) {
                line[current++] = ' ';
                memcpy(line + current, continuation, extra + 1);
            }
        }

        if (got_sigint || !*line) continue;

        mpcsh_history_add(&history, line);
        mpcsh_run_line(line);
    }
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
            "  %s script.mpcsh          run a .mpcsh script\n"
            "  %s prompt.mpcsh --gui    run a prompt script (GUI)\n"
            "  %s -c \"cmd | cmd\"        run one command line\n\n"
            "Pipeline segments: up to %d\n"
            "Command substitution: $()  |  Fish grammar: set / and / or / not\n"
            "Built-ins: ctk, portmap, guiinterm, wallpaper, ls, mpcimgcat, colorit, help, limits\n"
            "Attached languages: fish, mpcsh, python, c, cpp, csharp, rust, go, java, js, ts, ruby, php, lua, zig, kotlin\n"
            "Tab completes paths; Ctrl+Enter inserts newline; trailing \\ continues lines\n",
            program, program, program, program, MPCSH_MAX_PIPELINE);
}

int
main(int argc, char **argv)
{
    setlocale(LC_CTYPE, "");
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
    if (!is_mpcsh) {
        fprintf(stderr, "mpcsh: '%s' is not a .mpcsh script\n", script_path);
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
        FILE *f = fopen(script_path, "r");
        if (!f) {
            fprintf(stderr, "mpcsh: cannot open '%s'\n", script_path);
            return 126;
        }
        char line[MPCSH_MAX_LINE_HARD];
        int last = 0;
        while (fgets(line, sizeof(line), f)) {
            g_strstrip(line);
            if (!*line || line[0] == '#') continue;
            last = mpcsh_run_line(line);
        }
        fclose(f);
        return last;
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
