
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <util.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

typedef struct Network {
    char *ssid;
    char *args;
    struct Network *next;
} Network;

static const char SCAN_TEMPLATE[] = "ifconfig %s scan";
static const char STATUS_TEMPLATE[] = "ifconfig %s";
static const char START_TEMPLATE[] = "ifconfig %s nwid %s %s";
static const char DHCP_TEMPLATE[] = "dhclient %s";

static const char *ifname = "iwm0";
static const char *config_file = "/etc/wireless.conf";
static char foreground = 0;

static volatile char should_exit = 0;
static volatile char reload_config = 0;
static char last_state = -1;

static Network *LoadConfig(void)
{
    struct stat sb;
    Network *result = NULL;
    Network *last = NULL;
    char *config;
    int fd;
    int i;

    /* Map the configuration file. */
    fd = open(config_file, O_RDONLY);
    if(fd < 0) {
        perror("ERROR: could not open config file");
        return NULL;
    }
    if(fstat(fd, &sb) < 0) {
        perror("ERROR: could not read config file");
        close(fd);
        return NULL;
    }
    config = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if(config == MAP_FAILED) {
        perror("ERROR: could not map config file");
        close(fd);
        return NULL;
    }

    /* Process each line of the file. */
    /* Format is "<network_id> <args>\n". */
    for(i = 0; i < sb.st_size; i++) {
        Network *np = NULL;
        int ssid_start = 0;
        int args_start = 0;

        /* Skip leading whitespace. */
        while(i < sb.st_size && isspace(config[i])) ++i;

        /* Find the end of the SSID. */
        ssid_start = i;
        while(i < sb.st_size && !isspace(config[i])) ++i;
        if(ssid_start == i) break;

        /* Create the node and set the SSID. */
        np = malloc(sizeof(Network));
        np->next = NULL;
        if(last) {
            last->next = np;
        } else {
            result = np;
        }
        last = np;
        np->ssid = malloc(i - ssid_start + 1);
        memcpy(np->ssid, &config[ssid_start], i - ssid_start);
        np->ssid[i - ssid_start] = 0;
        np->args = NULL;

        /* Seek to the end of the line/entry. */
        while(i < sb.st_size && isspace(config[i])) ++i;
        args_start = i;
        while(i < sb.st_size && config[i] != '\n') ++i;
        if(args_start == i) break;

        np->args = malloc(i - args_start + 1);
        memcpy(np->args, &config[args_start], i - args_start);
        np->args[i - args_start] = 0;
    }

    /* Close the confguration file. */
    munmap(config, sb.st_size);
    close(fd);

    return result;
}

static void DestroyConfig(Network *np)
{
    while(np) {
        Network *next = np->next;
        free(np->ssid);
        if(np->args) free(np->args);
        free(np);
        np = next;
    }
}

static char CheckStatus(void)
{
    size_t len;
    FILE *fp;
    char *command;
    char line[256];
    char is_up;

    len = strlen(ifname) + sizeof(STATUS_TEMPLATE);
    command = malloc(len);
    snprintf(command, len, STATUS_TEMPLATE, ifname);
    fp = popen(command, "re");
    if(!fp) {
        perror("ERROR: could not scan");
        free(command);
        pclose(fp);
        return 0;
    }
    free(command);
    if(fgets(line, sizeof(line), fp) == NULL) {
        is_up = 0;
    } else {
        is_up = strstr(line, "UP") != NULL;
    }
    pclose(fp);
    return is_up;
}

static Network *Scan(Network *config)
{
    Network *result = NULL;
    FILE *fp;
    size_t len;
    char *command;
    char line[256];

    len = strlen(ifname) + sizeof(SCAN_TEMPLATE);
    command = malloc(len);
    snprintf(command, len, SCAN_TEMPLATE, ifname);
    fp = popen(command, "re");
    if(!fp) {
        perror("ERROR: could not scan");
        free(command);
        pclose(fp);
        return NULL;
    }
    free(command);
    while(!result) {
        int i;
        int ssid_start;

        /* Read a line. */
        i = 0;
        if(fgets(line, sizeof(line), fp) == NULL) {
            break;
        }

        /* Skip whitespace. */
        while(line[i] && isspace(line[i])) ++i;

        /* If this is not an SSID skip this line. */
        if(strncmp("nwid ", &line[i], 5)) {
            continue;
        }
        i += 5;

        /* Read the SSID. */
        ssid_start = i;
        while(line[i] && !isspace(line[i])) ++i;

        /* See if this SSID matches one of ours. */
        for(result = config; result; result = result->next) {
            if(!strncmp(result->ssid, &line[ssid_start], i - ssid_start)) {
                break;
            }
        }
    }

    pclose(fp);
    return result;
}

static void StartNetwork(Network *np)
{
    char *command;
    size_t len;

    len = strlen(np->args) + strlen(np->ssid) + sizeof(START_TEMPLATE);
    command = malloc(len);
    snprintf(command, len, START_TEMPLATE, ifname, np->ssid, np->args);
    system(command);
    snprintf(command, len, DHCP_TEMPLATE, ifname);
    system(command);
    free(command);
}

static void HandleTerm(int sig)
{
    should_exit = 1;
}

static void HandleHup(int sig)
{
    reload_config = 1;
}

static void ShowHelp(const char *name)
{
    fprintf(stderr, "usage: %s [options]\n", name);
    fprintf(stderr, "options:\n");
    fprintf(stderr, "   -c <file> The configuration file [%s]\n", config_file);
    fprintf(stderr, "   -f        Run in foreground for debugging\n");
    fprintf(stderr, "   -h        Display this message\n");
    fprintf(stderr, "   -i <if>   The interface to use [%s]\n", ifname);
}

int main(int argc, char *argv[])
{
    Network *config;
    int ch;

    /* Parse arguments. */
    while((ch = getopt(argc, argv, "c:fhi:")) != -1) {
        switch(ch) {
        case 'c':
            config_file = optarg;
            break;
        case 'f':
            foreground = 1;
            break;
        case 'i':
            ifname = optarg;
            break;
        default:
            ShowHelp(argv[0]);
            return -1;
        }
    }

    /* Parse initial config. */
    config = LoadConfig();
    if(config == NULL) {
        return -1;
    }

    /* Become a daemon. */
    if(!foreground) {
        pid_t pid;

        pid = fork();
        if(pid < 0) {
            perror("ERROR: fork failed");
            return -1;
        } else if(pid > 0) {
            return 0;
        }

        if(pidfile("wifiswd") < 0) {
            perror("ERROR: pidfile failed");
            return -1;
        }

        umask(0);

        if(setsid() == -1) {
            perror("ERROR: setsid failed");
            DestroyConfig(config);
            return -1;
        }

        if(chdir("/") < 0) {
            perror("ERROR: chdir failed");
            return -1;
        }

        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
    }
    signal(SIGINT, HandleTerm);
    signal(SIGTERM, HandleTerm);
    signal(SIGHUP, HandleHup);

    /* Process until we're told to stop. */
    while(!should_exit) {
        if(reload_config) {
            DestroyConfig(config);
            config = LoadConfig();
            if(config == NULL) {
                return -1;
            }
            reload_config = 0;
        }
        if(CheckStatus()) {
            if(last_state != 1) {
                syslog(LOG_INFO, "Network is up");
                last_state = 1;
            }
            sleep(1);
        } else {
            Network *np;
            if(last_state != 0) {
                syslog(LOG_INFO, "Network is down");
                last_state = 0;
            }
            np = Scan(config);
            if(np) {
                syslog(LOG_INFO, "Starting network %s", np->ssid);
                StartNetwork(np);
                sleep(10);
            } else {
                sleep(5);
            }
        }
    }

    /* Normal shutdown. */
    DestroyConfig(config);
    return 0;
}
