/* ============================================================================
 * MarekPC Shell v1.5 - "RELEASE CANDIDATE"
 *
 * NEW FEATURES IN 1.5:
 * - Auto-update checker (mpcupdate)
 * - Back-In-Time snapshots (mpcbits)
 * - Enhanced error recovery
 *
 * Build: gcc MPCSH1.5.c -o mpcsh_1_5 $(pkg-config --cflags --libs gtk4 gdk-pixbuf-2.0 libcurl)
 * ============================================================================= */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stddef.h>
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
#include <time.h>

#include <gtk/gtk.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <curl/curl.h>

#define MPCSH_VERSION_MAJOR 1
#define MPCSH_VERSION_MINOR 5
#define MPCSH_VERSION_PATCH 0
#define MPCSH_VERSION_STRING "1.5.0"

// Forward declarations for builtins
static int mpc_function_call(char **argv);
static int mpcsh_run_line(char *cmdline);

/* ===========================================================================
 * AUTO-UPDATE SYSTEM
 * ============================================================================ */

typedef struct {
    char *tag_name;
    char *name;
    char *body;
    char *browser_download_url;
    int is_prerelease;
} MpcshRelease;

// Memory structure for curl response
struct Memory {
    char *response;
    size_t size;
};

static size_t write_memory(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;
    
    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(!ptr) {
        fprintf(stderr, "mpcupdate: not enough memory\n");
        return 0;
    }
    
    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;
    
    return realsize;
}

// Simple JSON parser helpers since we don't want to depend on json-c
static char *extract_json_value(const char *json, const char *key) {
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    char *pos = strstr(json, search_key);
    if(!pos) return NULL;
    
    pos += strlen(search_key);
    // Skip whitespace and colon
    while(*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    
    if(*pos != '"') return NULL; // Not a string value
    pos++; // Skip opening quote
    
    char *end = strchr(pos, '"');
    if(!end) return NULL;
    
    return strndup(pos, end - pos);
}

static MpcshRelease *mpcupdate_fetch_latest_release(const char *repo_owner, const char *repo_name) {
    CURL *curl;
    CURLcode res;
    struct Memory chunk;
    MpcshRelease *release = NULL;
    
    chunk.response = malloc(1);
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
            release = malloc(sizeof(MpcshRelease));
            memset(release, 0, sizeof(MpcshRelease));
            
            release->tag_name = extract_json_value(chunk.response, "tag_name");
            release->name = extract_json_value(chunk.response, "name");
            release->body = extract_json_value(chunk.response, "body");
            
            // Check prerelease
            char *pre = strstr(chunk.response, "\"prerelease\":true");
            release->is_prerelease = (pre != NULL);
            
            // Extract first asset download URL (simplified)
            char *browser_pos = strstr(chunk.response, "\"browser_download_url\":");
            if(browser_pos) {
                browser_pos += strlen("\"browser_download_url\":");
                while(*browser_pos && (*browser_pos == ' ' || *browser_pos == '"')) browser_pos++;
                char *url_end = strchr(browser_pos, '"');
                if(url_end) {
                    release->browser_download_url = strndup(browser_pos, url_end - browser_pos);
                }
            }
        }
        
        curl_easy_cleanup(curl);
    }
    
    curl_global_cleanup();
    free(chunk.response);
    return release;
}

static void mpcupdate_download_and_install(const MpcshRelease *release, const char *target_binary) {
    printf("\n📥 Downloading release: %s\n", release->tag_name);
    
    // Create backup before updating
    char backup_path[PATH_MAX];
    snprintf(backup_path, sizeof(backup_path), "%s.backup", target_binary);
    
    if(rename(target_binary, backup_path) == 0) {
        printf("✅ Backed up current version\n");
    } else {
        printf("⚠️  Could not create backup (continuing)\n");
    }
    
    // Download new binary
    CURL *curl = curl_easy_init();
    if(curl && release->browser_download_url) {
        FILE *file = fopen(target_binary, "wb");
        if(file) {
            // Clean URL (remove embedded quotes if any)
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
                // Restore backup
                if(rename(backup_path, target_binary) == 0) {
                    printf("↩️  Restored previous version\n");
                }
            } else {
                chmod(target_binary, 0755);
                printf("✅ Successfully updated to %s!\n", release->tag_name);
                printf("💡 Restart your shell to use the new version.\n");
                
                // Keep backup
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

static int mpcupdate_builtin(char **argv) {
    const char *repo_owner = "marekpc";
    const char *repo_name = "mpcsh";
    
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
    
    MpcshRelease *release = mpcupdate_fetch_latest_release(repo_owner, repo_name);
    
    if(!release || !release->tag_name) {
        fprintf(stderr, "❌ Could not fetch release information.\n");
        fprintf(stderr, "💡 Check your internet connection and try again.\n");
        if(release) {
            free(release->tag_name);
            free(release->name);
            free(release->body);
            free(release->browser_download_url);
            free(release);
        }
        return 1;
    }
    
    // Extract version numbers
    int latest_major, latest_minor, latest_patch;
    int parsed = sscanf(release->tag_name, "v%d.%d.%d", &latest_major, &latest_minor, &latest_patch);
    if(parsed != 3) {
        parsed = sscanf(release->tag_name, "%d.%d.%d", &latest_major, &latest_minor, &latest_patch);
    }
    
    // Compare with current version
    int current_version = MPCSH_VERSION_MAJOR * 10000 + MPCSH_VERSION_MINOR * 100 + MPCSH_VERSION_PATCH;
    int latest_version = latest_major * 10000 + latest_minor * 100 + latest_patch;
    
    printf("Current version: v%s\n", MPCSH_VERSION_STRING);
    printf("Latest version:  %s\n", release->tag_name);
    
    if(latest_version <= current_version) {
        printf("✅ You're running the latest version!\n");
        free(release->tag_name);
        free(release->name);
        free(release->body);
        free(release->browser_download_url);
        free(release);
        return 0;
    }
    
    // New version available!
    printf("\n🎉 NEW VERSION AVAILABLE!\n");
    if(release->body) {
        printf("\n=== CHANGELOG ===\n%s\n=== END ===\n", release->body);
    }
    
    if(release->is_prerelease) {
        printf("\n⚠️  This is a prerelease version\n");
    }
    
    if(check_only) {
        free(release->tag_name);
        free(release->name);
        free(release->body);
        free(release->browser_download_url);
        free(release);
        return 0;
    }
    
    // Download and install prompt
    printf("\n💿 Update to %s now? [Y/n]: ", release->tag_name);
    char response[16];
    if(fgets(response, sizeof(response), stdin)) {
        if(tolower(response[0]) != 'n') {
            char exe_path[PATH_MAX];
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
    
    free(release->tag_name);
    free(release->name);
    free(release->body);
    free(release->browser_download_url);
    free(release);
    return 0;
}

/* ===========================================================================
 * BACK-IN-TIME SNAPSHOT SYSTEM (MPCBITS)
 * ============================================================================ */

static void mpcbits_create_snapshot(const char *description) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", tm_info);
    
    char snapshot_dir[PATH_MAX];
    const char *home = getenv("HOME");
    snprintf(snapshot_dir, sizeof(snapshot_dir), "%s/.local/share/mpcsh/snapshots/%s", 
             home ? home : "/tmp", timestamp);
    
    if(mkdir(snapshot_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "❌ Cannot create snapshot directory\n");
        return;
    }
    
    // Backup current executable
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if(len > 0) {
        exe_path[len] = '\0';
        char cmd[PATH_MAX * 2 + 256];
        snprintf(cmd, sizeof(cmd), "cp '%s' '%s/mpcsh' 2>/dev/null", exe_path, snapshot_dir);
        system(cmd);
    }
    
    // Save config directory
    char config_cmd[PATH_MAX * 2];
    snprintf(config_cmd, sizeof(config_cmd), 
             "mkdir -p '%s/config' && cp -r ~/.config/mpcsh/* '%s/config/' 2>/dev/null || true",
             snapshot_dir, snapshot_dir);
    system(config_cmd);
    
    // Save metadata
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", snapshot_dir);
    FILE *meta = fopen(meta_path, "w");
    if(meta) {
        fprintf(meta, "{\n");
        fprintf(meta, "  \"timestamp\": \"%s\",\n", timestamp);
        fprintf(meta, "  \"description\": \"%s\",\n", description ? description : "Manual backup");
        fprintf(meta, "  \"version\": \"%s\",\n", MPCSH_VERSION_STRING);
        fprintf(meta, "  \"created\": \"%s\"", asctime(tm_info));
        fprintf(meta, "}\n");
        fclose(meta);
    }
    
    printf("💾 Snapshot created: %s (%s)\n", timestamp, description ? description : "Manual backup");
}

static void mpcbits_list_snapshots() {
    const char *home = getenv("HOME");
    char snapshot_base[PATH_MAX];
    snprintf(snapshot_base, sizeof(snapshot_base), "%s/.local/share/mpcsh/snapshots/", 
             home ? home : "/tmp");
    
    DIR *d = opendir(snapshot_base);
    if(!d) {
        printf("No snapshots found.\n");
        printf("💡 Create one with: mpcbits save \"My backup\"\n");
        return;
    }
    
    printf("=== MPCSH BACKUP SNAPSHOTS ===\n");
    printf("Timestamp          Size     Description\n");
    printf("----------------------------------------\n");
    
    struct dirent *entry;
    int count = 0;
    while((entry = readdir(d))) {
        if(entry->d_name[0] == '.') continue;
        
        char full_path[PATH_MAX];
        snprintf(full_path, sizeof(full_path), "%s/%s", snapshot_base, entry->d_name);
        
        struct stat st;
        if(stat(full_path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        
        // Get description from metadata
        char meta_path[PATH_MAX];
        snprintf(meta_path, sizeof(meta_path), "%s/metadata.json", full_path);
        char desc[256] = "Manual backup";
        char size_str[32] = "0B";
        
        FILE *meta = fopen(meta_path, "r");
        if(meta) {
            char line[1024];
            while(fgets(line, sizeof(line), meta)) {
                if(strstr(line, "\"description\"")) {
                    char *start = strchr(line, ':');
                    if(start) {
                        start += 2;
                        char *end = strchr(start, '"');
                        if(end) {
                            int dlen = end - start;
                            if(dlen > 255) dlen = 255;
                            strncpy(desc, start, dlen);
                            desc[dlen] = '\0';
                        }
                    }
                }
            }
            fclose(meta);
        }
        
        // Get directory size (rough estimate)
        char du_cmd[512];
        snprintf(du_cmd, sizeof(du_cmd), "du -sh '%s' 2>/dev/null | cut -f1", full_path);
        FILE *du = popen(du_cmd, "r");
        if(du) {
            if(fgets(size_str, sizeof(size_str), du)) {
                size_str[strcspn(size_str, "\n")] = 0;
            }
            pclose(du);
        }
        
        printf("%-18s %-8s %s\n", entry->d_name, size_str, desc);
        count++;
    }
    closedir(d);
    printf("----------------------------------------\n");
    printf("Total snapshots: %d\n", count);
}

static int mpcbits_builtin(char **argv) {
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
        
        printf("↩️  Restoring from snapshot: %s\n", argv[2]);
        printf("⚠️  This will overwrite your current MPCSH binary.\n");
        printf("💡 Continue? [y/N]: ");
        
        char response[16];
        if(fgets(response, sizeof(response), stdin) && tolower(response[0]) == 'y') {
            char snapshot_bin[PATH_MAX];
            const char *home = getenv("HOME");
            snprintf(snapshot_bin, sizeof(snapshot_bin), 
                     "%s/.local/share/mpcsh/snapshots/%s/mpcsh", 
                     home ? home : "/tmp", argv[2]);
            
            char exe_path[PATH_MAX];
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
        
        char path[PATH_MAX];
        const char *home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/.local/share/mpcsh/snapshots/%s", 
                 home ? home : "/tmp", argv[2]);
        
        char cmd[PATH_MAX * 2];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
        if(system(cmd) == 0) {
            printf("🗑️  Deleted snapshot: %s\n", argv[2]);
        } else {
            fprintf(stderr, "❌ Cannot delete: %s\n", strerror(errno));
        }
        return 0;
    }
    
    fprintf(stderr, "Unknown subcommand: %s\n", argv[1]);
    return 2;
}

/* ===========================================================================
 * VERSION INFO BUILTIN
 * ============================================================================ */

static int mpcsh_version_builtin(char **argv) {
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

/* ===========================================================================
 * INTEGRATION WITH EXISTING MPCSH ENGINE
 * ============================================================================ */

// Update function call handler to include new builtins
static int mpc_function_call(char **argv) {
    if(!argv || !argv[0]) return -1;
    
    // Try new builtins first
    if(mpcupdate_builtin(argv) >= 0) return 0;
    if(mpcbits_builtin(argv) >= 0) return 0;
    if(mpcsh_version_builtin(argv) >= 0) return 0;
    
    // Fallback to existing function handling
    // [Place original mpc_function_call logic here]
    return -1;
}

// Update help text (this would be integrated into the main help function)
static void mpcsh_print_help(void) {
    // [Original help text would go here]
    printf("\n=== NEW IN 1.5 ===\n");
    printf("mpcupdate [--check-only]     Check for new releases\n");
    printf("mpcbits save/list/restore    Backup snapshots\n");
    printf("mpcversion                   Show version info\n");
}


/* ========== MPCSH 1.5 MAIN ENTRY ========== */
int main(int argc, char **argv) {
    // Initialize GTK4
    gtk_init();

    // Handle command line
    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("MarekPC Shell v1.5\n\n");
            printf("New features:\n");
            printf("  mpcupdate [--check-only]    Check for new releases from GitHub\n");
            printf("  mpcbits save [desc]         Create a backup snapshot\n");
            printf("  mpcbits list                List all snapshots\n");
            printf("  mpcbits restore <timestamp> Restore from a snapshot\n");
            printf("  mpcbits delete <timestamp>  Delete a snapshot\n");
            printf("  mpcversion                  Show version information\n");
            return EXIT_SUCCESS;
        }
    }

    printf("\033[1;35m");
    printf("╭──────────────────────────────────────────╮\n");
    printf("│    MarekPC Shell \033[0;36mv1.5\033[1;35m    │\n");
    printf("╰──────────────────────────────────────────╯\n");
    printf("\033[0m");
    printf("\n");

    printf("Install/Update: \033[33mpcupdate\033[0m\n");
    printf("Backups:        \033[33mpcbits\033[0m\n");
    printf("Version:        \033[33mpcversion\033[0m\n");
    printf("\n");
    printf("Type 'help' for a list of available commands.\n");
    printf("Press Ctrl+C to exit.\n\n");

    // Interactive mode (placeholder - full MPCSH integration needed)
    char input[8192];
    while (fgets(input, sizeof(input), stdin)) {
        printf("\033[1;36mmpcsh\033[0m> \033[0m");
    }
    return EXIT_SUCCESS;
}
