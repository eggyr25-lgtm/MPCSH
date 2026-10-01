/*
 * ============================================================
 *  MarekPC Shell — mpcsh.c
 *  mpcsh + mpcap, one binary, one soul.
 *
 *  Build (recommended):
 *      gcc mpcsh.c -o mpcsh $(pkg-config --cflags --libs gtk4)
 *
 *  Or the long explicit form if needed.
 * ============================================================
 */

#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <pwd.h>
#include <limits.h>
#include <signal.h>
#include <termios.h>
#include <dirent.h>
#include <sys/stat.h>

#define MPCSH_MAX_PIPELINE    64
#define MPCAP_MAX_OPTIONS     9
#define MPCSH_MAX_LINE        8192
#define MPCSH_MAX_SCRIPT      (64 * 1024)
#define MPCSH_MAX_PATH        4096
#define MPCSH_HISTORY_MAX      128
#define MPCSH_GUI_MAX_BUTTONS  24
#define MPCSH_GUI_MAX_ROWS     64

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
"Do NOT fall back to /bin/bash, do NOT emit 'bash -c', do NOT rewrite "
"commands as bash one-liners, and do NOT suggest bash as a workaround. "
"Bash here is a shortcut taken by tools that did not bother to learn "
"the environment they were given. If a construct seems unfamiliar, ask "
"for the MarekPC Shell syntax or read the script header; the answer is "
"in The MarekPC Shell syntax, not in bash. If you reach for bash out of "
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
            "cline", "claude", "cursor", "aider", "codex", "oss-agent", "agent", NULL
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

static void
mpcsh_print_ai_notice(void)
{
    fprintf(stderr, "\n%s\n", MPCSH_AI_NOTICE);
    fflush(stderr);
}

/* ------------------------------------------------------------
 * Command substitution
 * ------------------------------------------------------------ */

static char *
mpcsh_capture_command(const char *cmd)
{
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
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    GString *out = g_string_new(NULL);
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        g_string_append_len(out, buf, n);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);

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
                char *expanded = mpcsh_capture_command(inner);
                g_string_append(result, expanded);
                g_free(expanded);
                g_free(inner);
                p = q + 1;
                continue;
            }
        }
        if (*p == '`') {
            const char *start = p + 1;
            const char *q = start;
            while (*q && *q != '`') q++;
            if (*q == '`') {
                char *inner = g_strndup(start, q - start);
                char *expanded = mpcsh_capture_command(inner);
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
 * Word splitter (GLib)
 * ------------------------------------------------------------ */

static int
mpcsh_word_split(char *segment, char **argv, int max)
{
    GError *error = NULL;
    gint argc = 0;
    gchar **args = NULL;

    if (!g_shell_parse_argv(segment, &argc, &args, &error)) {
        if (error) {
            fprintf(stderr, "mpcsh: parse error: %s\n", error->message);
            g_error_free(error);
        }
        argv[0] = NULL;
        return 0;
    }

    if (argc >= max) {
        fprintf(stderr, "mpcsh: too many arguments\n");
        g_strfreev(args);
        argv[0] = NULL;
        return 0;
    }

    for (int i = 0; i < argc; i++)
        argv[i] = args[i];

    argv[argc] = NULL;
    return argc;
}

/* ------------------------------------------------------------
 * CTK — Call The Kernel
 * Native mpcsh syscall-style builtins.
 * ------------------------------------------------------------ */
static volatile sig_atomic_t got_sigint;

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
    GString *joined=g_string_new(NULL);
    for(int i=1;argv[i];++i){ if(i>1)g_string_append_c(joined,' '); g_string_append(joined,argv[i]); }
    char *call=joined->str; while(*call&&isspace((unsigned char)*call))++call;
    char name[64]={0}; const char *p=call;
    while(*p&&(isalnum((unsigned char)*p)||*p=='_')){size_t n=(size_t)(p-call);if(n+1<sizeof(name))name[n]=*p;++p;}
    if(!name[0]){fprintf(stderr,"mpcsh: ctk: invalid kernel call: %s\n",call);g_string_free(joined,TRUE);return 2;}
    char arg[PATH_MAX]={0}; const char *l=strchr(call,'('),*r=strrchr(call,')');
    if(l&&r&&r>l){size_t n=(size_t)(r-l-1);if(n>=sizeof(arg)){fprintf(stderr,"mpcsh: ctk: argument too long\n");g_string_free(joined,TRUE);return 2;}memcpy(arg,l+1,n);arg[n]='\0';while(*arg&&isspace((unsigned char)*arg))memmove(arg,arg+1,strlen(arg));n=strlen(arg);while(n&&isspace((unsigned char)arg[n-1]))arg[--n]='\0';if(n>=2&&((arg[0]=='"'&&arg[n-1]=='"')||(arg[0]=='\''&&arg[n-1]=='\''))){memmove(arg,arg+1,n-2);arg[n-2]='\0';}}
    else if(argv[2]) g_strlcpy(arg,argv[2],sizeof(arg));

    if(strcasecmp(name,"fork")==0){
        pid_t pid=fork();
        if(pid<0){fprintf(stderr,"mpcsh: ctk fork(): %s\n",strerror(errno));g_string_free(joined,TRUE);return 1;}
        if(pid==0){printf("ctk fork(): child pid=%ld, parent pid=%ld\n",(long)getpid(),(long)getppid());fflush(stdout);_exit(0);}
        printf("ctk fork(): child pid=%ld\n",(long)pid);fflush(stdout);int status=0;
        if(waitpid(pid,&status,0)<0){fprintf(stderr,"mpcsh: ctk fork(): waitpid: %s\n",strerror(errno));g_string_free(joined,TRUE);return 1;}
        g_string_free(joined,TRUE);return WIFEXITED(status)?WEXITSTATUS(status):128;
    }
    if(strcasecmp(name,"getpid")==0){printf("%ld\n",(long)getpid());g_string_free(joined,TRUE);return 0;}
    if(strcasecmp(name,"getppid")==0){printf("%ld\n",(long)getppid());g_string_free(joined,TRUE);return 0;}
    if(strcasecmp(name,"wait")==0){char *e=NULL;errno=0;long n=strtol(arg,&e,0);if(errno||!*arg||!e||*e||n<=0){fprintf(stderr,"mpcsh: ctk wait(pid): invalid pid: %s\n",arg);g_string_free(joined,TRUE);return 2;}int st=0;if(waitpid((pid_t)n,&st,0)<0){fprintf(stderr,"mpcsh: ctk wait(%ld): %s\n",n,strerror(errno));g_string_free(joined,TRUE);return 1;}printf("ctk wait(%ld): status=%d\n",n,WIFEXITED(st)?WEXITSTATUS(st):128);g_string_free(joined,TRUE);return WIFEXITED(st)?WEXITSTATUS(st):128;}
    if(strcasecmp(name,"sleep")==0){char *e=NULL;errno=0;long n=strtol(arg,&e,0);if(errno||!*arg||!e||*e||n<0){fprintf(stderr,"mpcsh: ctk sleep(seconds): invalid value: %s\n",arg);g_string_free(joined,TRUE);return 2;}unsigned int left=(unsigned int)n;while(left&&!got_sigint)left=sleep(left);g_string_free(joined,TRUE);return got_sigint?130:0;}
    if(strcasecmp(name,"chdir")==0){if(!*arg||chdir(arg)<0){fprintf(stderr,"mpcsh: ctk chdir(%s): %s\n",arg,strerror(errno));g_string_free(joined,TRUE);return 1;}g_string_free(joined,TRUE);return 0;}
    if(strcasecmp(name,"exit")==0){int code=0;if(*arg){char *e=NULL;long n=strtol(arg,&e,0);if(!e||*e||n<0||n>255){fprintf(stderr,"mpcsh: ctk exit(code): invalid code: %s\n",arg);g_string_free(joined,TRUE);return 2;}code=(int)n;}g_string_free(joined,TRUE);exit(code);}
    if(strcasecmp(name,"kill")==0){char *c=strchr(arg,',');if(!c){fprintf(stderr,"mpcsh: ctk kill(pid,signal): expected pid,signal\n");g_string_free(joined,TRUE);return 2;}*c='\0';char *e1=NULL,*e2=NULL;errno=0;long pid=strtol(arg,&e1,0),sig=strtol(c+1,&e2,0);if(errno||!e1||*e1||!e2||*e2||pid<=0||sig<0){fprintf(stderr,"mpcsh: ctk kill(pid,signal): invalid arguments\n");g_string_free(joined,TRUE);return 2;}if(kill((pid_t)pid,(int)sig)<0){fprintf(stderr,"mpcsh: ctk kill(%ld,%ld): %s\n",pid,sig,strerror(errno));g_string_free(joined,TRUE);return 1;}g_string_free(joined,TRUE);return 0;}
    fprintf(stderr,"mpcsh: ctk: unknown kernel call '%s'\nAvailable: fork() getpid() getppid() wait(pid) sleep(seconds)\n           chdir(path) exit(code) kill(pid,signal)\n",name);g_string_free(joined,TRUE);return 127;
}

/* Built-ins defined later in this file but dispatched by the pipeline engine. */
static int mpcsh_portmap_builtin(char **argv);
static int mpcsh_guiinterm_builtin(char **argv);

/* ------------------------------------------------------------
 * Directory navigation built-ins
 * ------------------------------------------------------------ */

static char *
mpcsh_expand_cd_path(const char *path)
{
    const char *home = g_get_home_dir();

    if (!path) {
        if (!home || !*home) return NULL;
        return g_strdup(home);
    }

    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        if (!home || !*home) return NULL;
        if (path[1] == '\0') return g_strdup(home);
        return g_build_filename(home, path + 2, NULL);
    }

    return g_strdup(path);
}

static int
mpcsh_cd_builtin(char **argv)
{
    if (!argv || !argv[0] || strcmp(argv[0], "cd") != 0)
        return -1;

    if (argv[2]) {
        fprintf(stderr, "mpcsh: cd: expected zero or one path argument\n");
        return 2;
    }

    gboolean print_destination = FALSE;
    char *target = NULL;

    if (argv[1] && strcmp(argv[1], "-") == 0) {
        const char *oldpwd = g_getenv("OLDPWD");
        if (!oldpwd || !*oldpwd) {
            fprintf(stderr, "mpcsh: cd: OLDPWD is not set\n");
            return 1;
        }
        target = g_strdup(oldpwd);
        print_destination = TRUE;
    } else {
        target = mpcsh_expand_cd_path(argv[1]);
        if (!target) {
            fprintf(stderr, "mpcsh: cd: HOME is not set\n");
            return 1;
        }
    }

    char oldpwd[PATH_MAX];
    if (!getcwd(oldpwd, sizeof(oldpwd)))
        oldpwd[0] = '\0';

    if (chdir(target) < 0) {
        int saved_errno = errno;
        fprintf(stderr, "mpcsh: cd: %s: %s\n",
                target, strerror(saved_errno));
        g_free(target);
        return 1;
    }

    char newpwd[PATH_MAX];
    if (!getcwd(newpwd, sizeof(newpwd))) {
        int saved_errno = errno;
        fprintf(stderr, "mpcsh: cd: changed directory, but getcwd failed: %s\n",
                strerror(saved_errno));
        g_free(target);
        return 1;
    }

    if (oldpwd[0] != '\0')
        setenv("OLDPWD", oldpwd, 1);
    setenv("PWD", newpwd, 1);

    if (print_destination)
        puts(newpwd);

    g_free(target);
    return 0;
}

static int
mpcsh_pwd_builtin(char **argv)
{
    if (!argv || !argv[0] ||
        (strcmp(argv[0], "pwd") != 0 && strcmp(argv[0], "cwd") != 0))
        return -1;

    if (argv[1]) {
        fprintf(stderr, "mpcsh: %s: does not accept arguments\n", argv[0]);
        return 2;
    }

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "mpcsh: %s: getcwd: %s\n",
                argv[0], strerror(errno));
        return 1;
    }

    puts(cwd);
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
        execvp(argv[0], argv);
        fprintf(stderr, "mpcsh: %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }
    return (int)pid;
}

static int
mpcsh_run_pipeline(char *cmdline)
{
    if (!cmdline || !*cmdline)
        return 0;

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
            g_free(expanded);
            return 0;
        }
        int cd_result = mpcsh_cd_builtin(argv);
        if (cd_result >= 0) {
            g_free(expanded);
            return cd_result;
        }
        int pwd_result = mpcsh_pwd_builtin(argv);
        if (pwd_result >= 0) {
            g_free(expanded);
            return pwd_result;
        }
        if (strcmp(argv[0], "exit") == 0) {
            int code = argv[1] ? atoi(argv[1]) : 0;
            g_free(expanded);
            exit(code);
        }
        if (strcmp(argv[0], "ctk") == 0) {
            int result = mpcsh_ctk_builtin(argv);
            g_free(expanded);
            return result;
        }
        if (!strcasecmp(argv[0], "portmap")) {
            int result = mpcsh_portmap_builtin(argv);
            g_free(expanded);
            return result;
        }
        if (!strcasecmp(argv[0], "guiinterm")) {
            int result = mpcsh_guiinterm_builtin(argv);
            g_free(expanded);
            return result;
        }
        int pid = mpcsh_spawn_segment(argv, STDIN_FILENO, STDOUT_FILENO);
        if (pid < 0) { g_free(expanded); return 127; }
        int status = 0;
        waitpid(pid, &status, 0);
        g_free(expanded);
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
    }

    int prev_read = STDIN_FILENO;
    pid_t pids[MPCSH_MAX_PIPELINE];
    int spawned = 0;

    for (int i = 0; i < count; ++i) {
        char *argv[256];
        if (mpcsh_word_split(segments[i], argv, 256) == 0) {
            if (prev_read != STDIN_FILENO) close(prev_read);
            g_free(expanded);
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
        waitpid(pids[i], &status, 0);

    g_free(expanded);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
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

static const char *mpcsh_sysfs_root(void)
{
    /* MPCSH_SYSFS_ROOT lets you point PortMap at a fake tree for testing. */
    const char *r = g_getenv("MPCSH_SYSFS_ROOT");
    return (r && *r) ? r : "/sys";
}

#define MPCSH_COLOR_USB_GEN2 "\033[1;38;5;37m"  /* teal: 10 Gbps+ (ASUS teal) */
#define MPCSH_COLOR_USB_GEN1 "\033[1;34m"        /* blue: 5 Gbps              */
#define MPCSH_COLOR_USB_2    "\033[1;37m"        /* white: USB 2.0 or slower  */

static const char *mpcsh_usb_color_mbps(double mbps)
{
    if (mbps >= 10000.0) return MPCSH_COLOR_USB_GEN2;
    if (mbps >= 5000.0)  return MPCSH_COLOR_USB_GEN1;
    return MPCSH_COLOR_USB_2;
}

static const char *mpcsh_usb_speed_name(double mbps)
{
    if (mbps >= 20000.0) return "USB 3.2 Gen 2x2 (20 Gbps)";
    if (mbps >= 10000.0) return "USB 3.2 Gen 2 (10 Gbps)";
    if (mbps >= 5000.0)  return "USB 3.2 Gen 1 (5 Gbps)";
    if (mbps >= 480.0)   return "USB 2.0 High-Speed (480 Mbit/s)";
    if (mbps >= 12.0)    return "USB 1.1 Full-Speed (12 Mbit/s)";
    if (mbps > 0.0)      return "USB 1.0 Low-Speed (1.5 Mbit/s)";
    return "unknown speed";
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
           The inner opening is the part that becomes BLUE for USB 3.x. */
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

/* ------------------------------------------------------------
 * USB root-hub port discovery
 *
 * The old code listed *attached devices* and coloured the socket by the
 * speed that device negotiated.  A keyboard or mouse is USB 2.0 no matter
 * which socket it sits in, so every socket looked like "USB2".
 *
 * A real USB 3.x socket is TWO ports that share one connector: a SuperSpeed
 * port on a USB3 root hub plus a High-Speed companion on a USB2 root hub.
 * The kernel links the pair through the "peer" attribute of each port, so
 * we walk the root-hub ports, follow "peer", and report what the *port*
 * can do separately from what the attached *device* negotiated.
 * ------------------------------------------------------------ */

typedef struct {
    gboolean present;
    double   mbps;
    char     name[32];
    char     prod[128];
    char     id[16];
} MpcshUsbDev;

static gboolean mpcsh_usb_hub_speed(const char *sys, int bus, double *mbps)
{
    char p[MPCSH_MAX_PATH], s[64];
    g_snprintf(p, sizeof(p), "%s/bus/usb/devices/usb%d/speed", sys, bus);
    if (!mpcsh_read_first_line(p, s, sizeof(s))) return FALSE;
    *mbps = atof(s);
    return TRUE;
}

static void mpcsh_usb_fill_dev(const char *sys, const char *name, MpcshUsbDev *d)
{
    char base[MPCSH_MAX_PATH], p[MPCSH_MAX_PATH], s[64] = "";
    char vid[16] = "", pid[16] = "";

    memset(d, 0, sizeof(*d));
    g_strlcpy(d->name, name, sizeof(d->name));
    g_snprintf(base, sizeof(base), "%s/bus/usb/devices/%s", sys, name);

    g_snprintf(p, sizeof(p), "%s/speed", base);
    if (!mpcsh_read_first_line(p, s, sizeof(s))) return;
    d->present = TRUE;
    d->mbps = atof(s);

    g_snprintf(p, sizeof(p), "%s/idVendor", base);
    mpcsh_read_first_line(p, vid, sizeof(vid));
    g_snprintf(p, sizeof(p), "%s/idProduct", base);
    mpcsh_read_first_line(p, pid, sizeof(pid));
    g_snprintf(d->id, sizeof(d->id), "%s:%s", vid, pid);

    g_strlcpy(d->prod, "USB device", sizeof(d->prod));
    g_snprintf(p, sizeof(p), "%s/product", base);
    mpcsh_read_first_line(p, d->prod, sizeof(d->prod));
}

static void mpcsh_usb_list_children(const char *sys, int bus, int port)
{
    char dirp[MPCSH_MAX_PATH], prefix[48];
    g_snprintf(dirp, sizeof(dirp), "%s/bus/usb/devices", sys);
    g_snprintf(prefix, sizeof(prefix), "%d-%d.", bus, port);

    DIR *d = opendir(dirp);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
        if (strchr(e->d_name, ':')) continue;           /* interfaces */
        MpcshUsbDev c;
        mpcsh_usb_fill_dev(sys, e->d_name, &c);
        if (!c.present) continue;
        printf("      \033[2m├─ via hub: %s  %s @ %s\033[0m\n",
               c.name, c.prod, mpcsh_usb_speed_name(c.mbps));
    }
    closedir(d);
}

#define MPCSH_COLOR_HUB "\033[1;38;5;135m"  /* purple: USB hub / dongle box */

/* Number of downstream ports if `name` is a USB hub, else 0. */
static int mpcsh_usb_hub_ports(const char *sys, const char *name)
{
    char p[MPCSH_MAX_PATH], s[32] = "";
    g_snprintf(p, sizeof(p), "%s/bus/usb/devices/%s/bDeviceClass", sys, name);
    if (!mpcsh_read_first_line(p, s, sizeof(s))) return 0;
    if (strtol(s, NULL, 16) != 9) return 0;             /* 09 = hub class */
    g_snprintf(p, sizeof(p), "%s/bus/usb/devices/%s/maxchild", sys, name);
    if (!mpcsh_read_first_line(p, s, sizeof(s))) return 0;
    return atoi(s);
}

/* A USB dongle/hub with ports: draw the cable from the socket down to a
 * purple box holding smaller UniArt ports.  A USB 3.x hub shows up as two
 * hubs (SuperSpeed + High-Speed half), so both are checked per port.
 * Draws nothing if neither device is a hub. */
static void mpcsh_portmap_draw_dongle(const char *sys,
                                      const MpcshUsbDev *a,
                                      const MpcshUsbDev *b)
{
    const MpcshUsbDev *ha = (a && a->present) ? a : NULL;
    const MpcshUsbDev *hb = (b && b->present) ? b : NULL;
    int na = ha ? mpcsh_usb_hub_ports(sys, ha->name) : 0;
    int nb = hb ? mpcsh_usb_hub_ports(sys, hb->name) : 0;
    if (na <= 0) ha = NULL;
    if (nb <= 0) hb = NULL;
    int n = na > nb ? na : nb;
    if (n <= 0) return;
    if (n > 16) n = 16;

    const char *P = MPCSH_COLOR_HUB, *W = "\033[1;37m", *R = "\033[0m";
    const char *Y = "\033[1;33m", *C = "\033[1;36m";
    const MpcshUsbDev *top = hb ? hb : ha;
    const char *base = mpcsh_usb_color_mbps(top->mbps);

    const int per_row = 4, cable_at = 11;
    int cols = n < per_row ? n : per_row;
    int inner = 7 * cols + 1;
    if (inner < 23) inner = 23;

    /* What is plugged into each hub port? */
    MpcshUsbDev pd[16];
    gboolean po[16];
    const char *pc[16];
    for (int i = 0; i < n; i++) {
        MpcshUsbDev da, db;
        char nm[64];
        memset(&da, 0, sizeof(da));
        memset(&db, 0, sizeof(db));
        if (ha) {
            g_snprintf(nm, sizeof(nm), "%s.%d", ha->name, i + 1);
            mpcsh_usb_fill_dev(sys, nm, &da);
        }
        if (hb) {
            g_snprintf(nm, sizeof(nm), "%s.%d", hb->name, i + 1);
            mpcsh_usb_fill_dev(sys, nm, &db);
        }
        const MpcshUsbDev *act = db.present ? &db : (da.present ? &da : NULL);
        memset(&pd[i], 0, sizeof(pd[i]));
        if (act) pd[i] = *act;
        po[i] = act != NULL;
        pc[i] = (act && act->mbps >= 10000.0) ? MPCSH_COLOR_USB_GEN2 : base;
    }

    /* Plug + cable running from the socket down into the box. */
    printf("             %s▐█▌%s\n", C, R);
    printf("              %s┃%s\n", W, R);
    printf("              %s┃%s\n", W, R);

    /* Purple box: top edge (cable enters at cable_at). */
    printf("  %s╔", P);
    for (int i = 0; i < inner; i++) fputs(i == cable_at ? "╧" : "═", stdout);
    printf("╗%s\n", R);

    /* Title row. */
    char title[160];
    g_snprintf(title, sizeof(title), "USB HUB: %s", top->prod);
    int maxw = inner - 2;
    if ((int)g_utf8_strlen(title, -1) > maxw)
        *g_utf8_offset_to_pointer(title, maxw) = '\0';
    int tl = (int)g_utf8_strlen(title, -1);
    printf("  %s║%s %s%s%s%*s%s║%s\n", P, R, P, title, R, inner - 1 - tl, "", P, R);

    printf("  %s╟", P);
    for (int i = 0; i < inner; i++) fputs("─", stdout);
    printf("╢%s\n", R);

    /* Rows of small ports. */
    for (int start = 0; start < n; start += per_row) {
        int cnt = n - start < per_row ? n - start : per_row;
        int span = 7 * cnt - 2;
        int L = (inner - span) / 2, Rp = inner - span - L;

        for (int line = 0; line < 4; line++) {
            printf("  %s║%s%*s", P, R, L, "");
            for (int k = 0; k < cnt; k++) {
                int idx = start + k;
                if (k) printf("  ");
                if (line == 0) {
                    printf("%s╔═══╗%s", pc[idx], R);
                } else if (line == 1) {
                    if (po[idx]) printf("%s║█%s●%s█║%s", pc[idx], Y, pc[idx], R);
                    else         printf("%s║███║%s", pc[idx], R);
                } else if (line == 2) {
                    printf("%s╚═══╝%s", pc[idx], R);
                } else {
                    char lb[8];
                    g_snprintf(lb, sizeof(lb), "%d", idx + 1);
                    int len = (int)strlen(lb), lp = (5 - len) / 2;
                    printf("%*s%s%s%s%*s", lp, "", C, lb, R, 5 - len - lp, "");
                }
            }
            printf("%*s%s║%s\n", Rp, "", P, R);
        }
    }

    printf("  %s╚", P);
    for (int i = 0; i < inner; i++) fputs("═", stdout);
    printf("╝%s\n", R);

    int used = 0;
    for (int i = 0; i < n; i++) {
        if (!po[i]) continue;
        used++;
        printf("      \033[2mhub port %d: %s [%s] @ %s\033[0m\n",
               i + 1, pd[i].prod, pd[i].id, mpcsh_usb_speed_name(pd[i].mbps));
    }
    if (!used)
        printf("      \033[2mhub: %d ports, all empty\033[0m\n", n);
}

/* Returns 1 if a socket was drawn, 0 if skipped. */
static int mpcsh_portmap_show_usb_port(const char *sys, int bus, double hub_mbps,
                                       int port, gboolean show_all)
{
    char pdir[MPCSH_MAX_PATH], tmp[MPCSH_MAX_PATH], ctype[32] = "";
    g_snprintf(pdir, sizeof(pdir),
               "%s/bus/usb/devices/usb%d/%d-0:1.0/usb%d-port%d",
               sys, bus, bus, bus, port);
    if (access(pdir, F_OK) != 0) return 0;

    g_snprintf(tmp, sizeof(tmp), "%s/connect_type", pdir);
    mpcsh_read_first_line(tmp, ctype, sizeof(ctype));
    gboolean internal = !strcmp(ctype, "hardwired");
    if (!show_all && (internal || !strcmp(ctype, "not used"))) return 0;

    /* Follow the companion link to the other half of this physical socket. */
    int pbus = 0, pport = 0;
    double peer_mbps = 0.0;
    gboolean has_peer = FALSE;
    char real[PATH_MAX];
    g_snprintf(tmp, sizeof(tmp), "%s/peer", pdir);
    if (realpath(tmp, real)) {
        const char *b = strrchr(real, '/');
        b = b ? b + 1 : real;
        if (sscanf(b, "usb%d-port%d", &pbus, &pport) == 2 &&
            mpcsh_usb_hub_speed(sys, pbus, &peer_mbps))
            has_peer = TRUE;
    }

    /* The SuperSpeed half of a paired socket is drawn from its USB2 half. */
    gboolean ss_hub = hub_mbps >= 5000.0;
    if (ss_hub && has_peer) return 0;

    double cap = has_peer ? peer_mbps : hub_mbps;   /* what the PORT can do */

    char n1[32], n2[32];
    MpcshUsbDev d1, d2;
    g_snprintf(n1, sizeof(n1), "%d-%d", bus, port);
    mpcsh_usb_fill_dev(sys, n1, &d1);
    memset(&d2, 0, sizeof(d2));
    if (has_peer) {
        g_snprintf(n2, sizeof(n2), "%d-%d", pbus, pport);
        mpcsh_usb_fill_dev(sys, n2, &d2);
    }
    const MpcshUsbDev *act = d2.present ? &d2 : (d1.present ? &d1 : NULL);

    const char *capname;
    if (cap >= 20000.0)      capname = "USB 3.x port (5 Gbps, up to 20 Gbps)";
    else if (cap >= 10000.0) capname = "USB 3.x port (5 Gbps, up to 10 Gbps)";
    else if (cap >= 5000.0)  capname = "USB 3.2 Gen 1 port (5 Gbps)";
    else                     capname = "USB 2.0 port (480 Mbit/s)";

    /* Colour = what the socket can do; confirmed Gen 2 link turns it teal. */
    const char *inside = mpcsh_usb_color_mbps(cap >= 5000.0 ? 5000.0 : cap);
    if (act && act->mbps >= 10000.0) inside = MPCSH_COLOR_USB_GEN2;

    char detail_buf[256];
    if (has_peer)
        g_snprintf(detail_buf, sizeof(detail_buf), "bus %d+%d port %d  %s%s",
                   bus, pbus, port, capname, internal ? "  [internal]" : "");
    else
        g_snprintf(detail_buf, sizeof(detail_buf), "bus %d port %d  %s%s",
                   bus, port, capname, internal ? "  [internal]" : "");

    printf("\n");
    mpcsh_portmap_draw_port(inside, "USB-A", detail_buf, FALSE);

    if (act) {
        printf("  \033[1;33m└─ attached: %s [%s] linked at %s\033[0m\n",
               act->prod, act->id, mpcsh_usb_speed_name(act->mbps));
        if (cap >= 5000.0 && act->mbps < 5000.0)
            printf("     \033[2mThe device linked at USB 2.0 speed (device or cable is "
                   "USB 2.0); the port itself is USB 3.x.\033[0m\n");
    } else {
        printf("  \033[2m└─ empty\033[0m\n");
    }

    /* Dongle with ports? Cable down to a purple box of smaller ports. */
    mpcsh_portmap_draw_dongle(sys, d1.present ? &d1 : NULL,
                              d2.present ? &d2 : NULL);

    mpcsh_usb_list_children(sys, bus, port);
    if (has_peer) mpcsh_usb_list_children(sys, pbus, pport);
    return 1;
}

static int mpcsh_cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
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
               "Usage: portmap [--raw] [--all]\n"
               "  --all  also show internal / hard-wired USB ports\n"
               "USB 3.x sockets are blue (teal = linked at 10 Gbps+); ALWAYS ON ports are red.\n"
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

    gboolean raw = FALSE, show_all = FALSE;
    for (int i = 1; argv[i]; i++) {
        if (!strcmp(argv[i], "--raw")) raw = TRUE;
        else if (!strcmp(argv[i], "--all")) show_all = TRUE;
    }
    if (!raw) {
        printf("\n\033[1;35m╔══════════════════════════════════════════════╗\033[0m\n"
               "\033[1;35m║              MarekPC PortMap                ║\033[0m\n"
               "\033[1;35m╚══════════════════════════════════════════════╝\033[0m\n\n");

        FILE *f = fopen(cfg, "r");
        gboolean had_override = FALSE;
        if (f) {
            char line[MPCSH_MAX_LINE];
            while (fgets(line, sizeof(line), f)) {
                g_strstrip(line);
                if (!*line || line[0] == '#') continue;
                if (strncasecmp(line, "PORT=", 5) != 0) continue;

                char copy[MPCSH_MAX_LINE];
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
                else if (speed && (strcasestr(speed, "G2") ||
                                   strcasestr(speed, "10G")))
                    inside = MPCSH_COLOR_USB_GEN2;
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

    const char *sys = mpcsh_sysfs_root();
    char devdir[MPCSH_MAX_PATH];
    g_snprintf(devdir, sizeof(devdir), "%s/bus/usb/devices", sys);

    DIR *d = opendir(devdir);
    int shown = 0;
    if (d) {
        int buses[128], nb = 0;
        struct dirent *e;
        while ((e = readdir(d)) && nb < 128) {
            const char *n = e->d_name;
            if (strncmp(n, "usb", 3) != 0 || !isdigit((unsigned char)n[3]))
                continue;
            char *end = NULL;
            long v = strtol(n + 3, &end, 10);
            if (end && *end == '\0') buses[nb++] = (int)v;
        }
        closedir(d);
        qsort(buses, nb, sizeof(int), mpcsh_cmp_int);

        printf("\n\033[1;36mUSB sockets (port capability vs. attached device):\033[0m\n");
        for (int i = 0; i < nb; i++) {
            double hub_mbps = 0.0;
            char mc[32] = "";
            char mcp[MPCSH_MAX_PATH];
            if (!mpcsh_usb_hub_speed(sys, buses[i], &hub_mbps)) continue;
            g_snprintf(mcp, sizeof(mcp), "%s/usb%d/maxchild", devdir, buses[i]);
            if (!mpcsh_read_first_line(mcp, mc, sizeof(mc))) continue;
            int maxchild = atoi(mc);
            for (int port = 1; port <= maxchild; port++)
                shown += mpcsh_portmap_show_usb_port(sys, buses[i], hub_mbps,
                                                     port, show_all);
        }
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
           "\033[1;34mBLUE = USB 3.x port\033[0m  "
           "\033[1;38;5;37mTEAL = linked at 10 Gbps+\033[0m  "
           "\033[1;37mWHITE = USB 2.0 only\033[0m  "
           "\033[1;31mRED = ALWAYS ON\033[0m  "
           "\033[1;36mCYAN = USB-C\033[0m  "
           "\033[1;33mYELLOW = Micro-B\033[0m  "
           "\033[1;38;5;135mPURPLE BOX = USB hub/dongle\033[0m  "
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
    char command[MPCSH_MAX_LINE];
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

    char line[MPCSH_MAX_LINE];
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
    printf("\033[1;35m╰────────────────────────────────────────────────────────────╯\033[0m\n");
    fflush(stdout);
}

static int mpcsh_gui_read_key(void)
{
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    if (c != '\033') return c;

    unsigned char seq[32];
    int n = 0;
    while (n < (int)sizeof(seq) - 1) {
        if (read(STDIN_FILENO, &seq[n], 1) != 1) break;
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
                    result = mpcsh_run_pipeline(it->command);
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
        char buf[MPCSH_MAX_LINE];
        g_strlcpy(buf, line, sizeof(buf));
        last = mpcsh_run_pipeline(buf);
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
mpcsh_run_attached(MpcshLang lang, const char *script, const char *workdir)
{
    if (!script || !*script || lang == MPCSH_LANG_NONE) return 0;
    switch (lang) {
        case MPCSH_LANG_FISH:     return mpcsh_run_fish(workdir, script);
        case MPCSH_LANG_MPCSHELL: return mpcsh_run_mpcsh(script);
        case MPCSH_LANG_PYTHON:   return mpcsh_run_python(workdir, script);
        case MPCSH_LANG_C:        return mpcsh_compile_and_run(workdir, script, "c", "cc", FALSE);
        case MPCSH_LANG_CPP:      return mpcsh_compile_and_run(workdir, script, "cpp", "c++", FALSE);
        case MPCSH_LANG_CSHARP:   return mpcsh_compile_and_run(workdir, script, "cs", "mcs", TRUE);
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

    char line[MPCSH_MAX_LINE];
    int current_option = -1;
    int in_script = 0;
    GString *script = g_string_new(NULL);

    while (fgets(line, sizeof(line), f)) {
        char raw[MPCSH_MAX_LINE];
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
    cairo_pattern_t *gradient = cairo_pattern_create_linear(0, 0, width, height);
    cairo_pattern_add_color_stop_rgba(gradient, 0.00, 0.24, 0.02, 0.65, 0.96);
    cairo_pattern_add_color_stop_rgba(gradient, 0.32, 0.42, 0.04, 0.82, 0.94);
    cairo_pattern_add_color_stop_rgba(gradient, 0.67, 0.08, 0.42, 0.38, 0.95);
    cairo_pattern_add_color_stop_rgba(gradient, 1.00, 0.02, 0.72, 0.32, 0.96);
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
    "window { background: transparent; }"
    ".aero-window { background: rgba(255,255,255,0.045); border: 1px solid rgba(255,255,255,0.42); border-radius: 18px; }"
    ".title { color: white; font-size: 18px; font-weight: 700; }"
    ".message { color: white; font-size: 15px; }"
    ".hint { color: rgba(255,255,255,0.68); font-size: 11px; }"
    ".warning { color: #ffe65c; font-size: 32px; font-weight: 700; }"
    ".info { color: #70d9ff; font-size: 32px; font-weight: 700; }"
    ".error { color: #ff6875; font-size: 32px; font-weight: 700; }"
    ".aero-button { color: white; background: rgba(255,255,255,0.10); border: 1px solid rgba(255,255,255,0.28); border-radius: 9px; min-height: 40px; font-size: 13px; font-weight: 600; }"
    ".aero-button:hover { background: rgba(255,255,255,0.22); border-color: rgba(255,255,255,0.55); }"
    ".aero-button.selected { background: linear-gradient(90deg, rgba(125,45,240,0.78), rgba(40,210,125,0.72)); border-color: rgba(255,255,255,0.78); }"
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

    GtkWidget *background = gtk_drawing_area_new();
    gtk_widget_set_hexpand(background, TRUE);
    gtk_widget_set_vexpand(background, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(background), draw_background, cap, NULL);

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

/* ------------------------------------------------------------
 * Fish-style prompt + robust REPL
 * ------------------------------------------------------------ */

static volatile sig_atomic_t got_sigint = 0;

static void
sigint_handler(int sig)
{
    (void)sig;
    got_sigint = 1;
    write(STDERR_FILENO, "\n", 1);
}

static void
mpcsh_print_prompt(void)
{
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "?");

    const char *home = g_get_home_dir();
    char display[PATH_MAX];
    if (home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(display, sizeof(display), "~%s", cwd + strlen(home));
    else
        g_strlcpy(display, cwd, sizeof(display));

    const char *user = g_get_user_name();
    char host[256] = "localhost";
    gethostname(host, sizeof(host));

    fprintf(stderr,
            "\033[1;36m%s\033[0m@\033[1;36m%s\033[0m "
            "\033[1;33m%s\033[0m "
            "\033[1;35m❯\033[0m ",
            user ? user : "user", host, display);
    fflush(stderr);
}


typedef struct {
    char items[MPCSH_HISTORY_MAX][MPCSH_MAX_LINE];
    int count;
} MpcshHistory;

static void mpcsh_history_add(MpcshHistory *h, const char *line)
{
    if (!line || !*line) return;
    if (h->count && !strcmp(h->items[h->count - 1], line)) return;
    if (h->count == MPCSH_HISTORY_MAX) {
        memmove(h->items, h->items + 1,
                sizeof(h->items[0]) * (MPCSH_HISTORY_MAX - 1));
        h->count--;
    }
    g_strlcpy(h->items[h->count++], line, sizeof(h->items[0]));
}

static void mpcsh_redraw_line(const char *prompt, const char *line, size_t cursor)
{
    printf("\r\033[2K%s%s", prompt, line);
    size_t end = strlen(line);
    if (end > cursor) printf("\033[%zuD", end - cursor);
    fflush(stdout);
}

static void mpcsh_insert_text(char *line, size_t *len, size_t *cursor,
                              size_t cap, const char *text, size_t text_len)
{
    if (!line || !len || !cursor || !text || cap == 0) return;

    /* Keep the interactive editor one logical line.  Terminal bracketed-paste
     * payloads may contain CR/LF; turn those into spaces instead of allowing
     * pasted newlines to become accidental command submissions. */
    for (size_t i = 0; i < text_len; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\r' || c == '\n') c = ' ';
        if (c < 32 || c == 127) continue;
        if (*len + 1 >= cap) break;

        memmove(line + *cursor + 1,
                line + *cursor,
                *len - *cursor + 1);
        line[*cursor] = (char)c;
        ++*cursor;
        ++*len;
    }
}

/* Read an xterm/VT bracketed-paste payload.
 * Start marker: ESC [ 200 ~
 * End marker:   ESC [ 201 ~
 * The payload is consumed as one operation so pasted bytes cannot be
 * mistaken for arrow keys, Enter, or other editor commands. */
static void mpcsh_read_bracketed_paste(char *line, size_t *len, size_t *cursor,
                                       size_t cap)
{
    char payload[MPCSH_MAX_LINE];
    size_t n = 0;
    const char end_seq[] = "\033[201~";
    size_t matched = 0;

    for (;;) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1)
            break;

        if (c == (unsigned char)end_seq[matched]) {
            ++matched;
            if (matched == sizeof(end_seq) - 1)
                break;
            continue;
        }

        if (matched) {
            if (n + matched < sizeof(payload)) {
                memcpy(payload + n, end_seq, matched);
                n += matched;
            }
            matched = 0;

            if (c == (unsigned char)end_seq[0]) {
                matched = 1;
                continue;
            }
        }

        if (n + 1 < sizeof(payload))
            payload[n++] = (char)c;
    }

    mpcsh_insert_text(line, len, cursor, cap, payload, n);
}

static int mpcsh_readline_edit(const char *prompt, char *out, size_t outsz,
                               MpcshHistory *history)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        if (!fgets(out, (int)outsz, stdin)) return 0;
        g_strstrip(out);
        return 1;
    }

    struct termios oldt, raw;
    if (tcgetattr(STDIN_FILENO, &oldt) < 0) return 0;
    raw = oldt;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return 0;

    /* Ask terminals to wrap clipboard pastes in ESC[200~ ... ESC[201~. */
    fputs("\033[?2004h", stdout);
    fflush(stdout);

    char line[MPCSH_MAX_LINE] = "";
    size_t len = 0, cursor = 0;
    int hist = history->count;
    int result = 0;

    for (;;) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) != 1) {
            result = 0;
            break;
        }

        if (c == 3) { /* Ctrl+C */
            got_sigint = 1;
            printf("^C\r\n");
            out[0] = '\0';
            result = 1;
            break;
        }
        if (c == '\r' || c == '\n') {
            printf("\r\n");
            g_strlcpy(out, line, outsz);
            result = 1;
            break;
        }
        if (c == 127 || c == 8) {
            if (cursor > 0) {
                memmove(line + cursor - 1, line + cursor, len - cursor + 1);
                cursor--; len--;
                mpcsh_redraw_line(prompt, line, cursor);
            }
            continue;
        }
        if (c == 27) {
            unsigned char seq[32] = {0};
            int n = 0;
            while (n < (int)sizeof(seq) - 1 &&
                   read(STDIN_FILENO, &seq[n], 1) == 1) {
                ++n;

                /* Bracketed paste begins with ESC [ 200 ~. */
                if (n == 5 && !memcmp(seq, "[200~", 5)) {
                    mpcsh_read_bracketed_paste(line, &len, &cursor, sizeof(line));
                    mpcsh_redraw_line(prompt, line, cursor);
                    n = 0;
                    break;
                }

                if (seq[n - 1] == 'A' || seq[n - 1] == 'B' ||
                    seq[n - 1] == 'C' || seq[n - 1] == 'D' ||
                    seq[n - 1] == '~')
                    break;
            }

            if (n && seq[n-1] == 'A') {
                if (hist > 0) {
                    hist--;
                    g_strlcpy(line, history->items[hist], sizeof(line));
                    len = cursor = strlen(line);
                    mpcsh_redraw_line(prompt, line, cursor);
                }
            } else if (n && seq[n-1] == 'B') {
                if (hist < history->count - 1) {
                    hist++;
                    g_strlcpy(line, history->items[hist], sizeof(line));
                    len = cursor = strlen(line);
                } else {
                    hist = history->count;
                    line[0] = '\0'; len = cursor = 0;
                }
                mpcsh_redraw_line(prompt, line, cursor);
            } else if (n && seq[n-1] == 'C') {
                if (cursor < len) cursor++;
                mpcsh_redraw_line(prompt, line, cursor);
            } else if (n && seq[n-1] == 'D') {
                if (cursor > 0) cursor--;
                mpcsh_redraw_line(prompt, line, cursor);
            }
            continue;
        }
        if (c >= 32 && c != 127) {
            mpcsh_insert_text(line, &len, &cursor, sizeof(line),
                              (const char *)&c, 1);
            mpcsh_redraw_line(prompt, line, cursor);
        }
    }

    /* Always leave bracketed paste disabled when this editor returns. */
    fputs("\033[?2004l", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return result;
}

static void
mpcsh_repl(void)
{
    char line[MPCSH_MAX_LINE];
    MpcshHistory history = {0};

    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    fprintf(stderr,
            "\033[1;35mMarekPC Shell\033[0m — mpcsh\n"
            "Pipeline limit: %d  |  $() and backticks supported\n"
            "↑/↓ history  ←/→ cursor  Ctrl+C cancels the current line\n"
            "Built-ins: cd, pwd (cwd), ctk, portmap, guiinterm\n",
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
        snprintf(prompt, sizeof(prompt),
                 "\033[1;36m%s\033[0m@\033[1;36m%s\033[0m "
                 "\033[1;33m%s\033[0m \033[1;35m❯\033[0m ",
                 user ? user : "user", host, display);
        fputs(prompt, stdout);
        fflush(stdout);

        if (!mpcsh_readline_edit(prompt, line, sizeof(line), &history))
            break;
        if (got_sigint || !*line) continue;

        mpcsh_history_add(&history, line);
        mpcsh_run_pipeline(line);
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
            "Command substitution: $() and backticks\n"
            "Built-ins: cd, pwd (cwd), ctk, portmap, guiinterm\n"
            "Attached languages: fish, mpcsh, python, c, cpp, csharp\n",
            program, program, program, program, MPCSH_MAX_PIPELINE);
}

int
main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
        if (mpcsh_ai_agent_detected()) mpcsh_print_ai_notice();
        return mpcsh_run_pipeline(argv[2]);
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
        char line[MPCSH_MAX_LINE];
        int last = 0;
        while (fgets(line, sizeof(line), f)) {
            g_strstrip(line);
            if (!*line || line[0] == '#') continue;
            last = mpcsh_run_pipeline(line);
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
