
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <signal.h>
#include <util.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <ifaddrs.h>

#include <net/if.h>
#include <net80211/ieee80211.h>
#include <net80211/ieee80211_ioctl.h>

typedef struct Network {
    char *ssid;
    char *args;
    struct Network *next;
} Network;

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
        syslog(LOG_ERR, "could not open %s", config_file);
        return NULL;
    }
    if(fstat(fd, &sb) < 0) {
        syslog(LOG_ERR, "could not stat %s", config_file);
        close(fd);
        return NULL;
    }
    config = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if(config == MAP_FAILED) {
        syslog(LOG_ERR, "mmap failed for %s", config_file);
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

static void SetFlag(int sock, int value)
{
    struct ifreq ifr;
    int old_flags;
    bzero(&ifr, sizeof(ifr));
    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
    if(ioctl(sock, SIOCGIFFLAGS, (caddr_t)&ifr, sizeof(ifr)) < 0) {
        syslog(LOG_WARNING, "could not read flags");
        return;
    }
    old_flags = ifr.ifr_flags;
    if(value < 0) {
        ifr.ifr_flags &= ~-value;
    } else {
        ifr.ifr_flags |= value;
    }
    if(old_flags != ifr.ifr_flags) {
        if(ioctl(sock, SIOCSIFFLAGS, (caddr_t)&ifr) < 0) {
            syslog(LOG_WARNING, "could not update flags");
            return;
        }
    }
}

static char CheckStatus(void)
{
    struct ifaddrs *ifap;
    struct ifaddrs *ap;
    char status = 0;
    if(getifaddrs(&ifap) < 0) {
        syslog(LOG_WARNING, "could not determine state of %s", ifname);
        return status;
    }
    for(ap = ifap; ap; ap = ap->ifa_next) {
        if(!strcmp(ifname, ap->ifa_name) && ap->ifa_data) {
            struct if_data *ifd = (struct if_data*)ap->ifa_data;
            status = LINK_STATE_IS_UP(ifd->ifi_link_state);
            break;
        }
    }
    freeifaddrs(ifap);
    return status;
}

static Network *Scan(Network *config)
{
    Network *np = NULL;
    struct ifreq ifr;
    struct ieee80211_nodereq_all na;
    struct ieee80211_nodereq nr[512];
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0) {
        syslog(LOG_WARNING, "could not scan (socket create failed)");
        return NULL;
    }

    SetFlag(sock, IFF_UP);

    bzero(&ifr, sizeof(ifr));
    strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
    if(ioctl(sock, SIOCS80211SCAN, (caddr_t)&ifr) != 0) {
        syslog(LOG_WARNING, "could not start scan");
        close(sock);
        return NULL;
    }

    bzero(&na, sizeof(na));
    bzero(&nr, sizeof(nr));
    na.na_node = nr;
    na.na_size = sizeof(nr);
    strlcpy(na.na_ifname, ifname, sizeof(na.na_ifname));
    if(ioctl(sock, SIOCG80211ALLNODES, &na) != 0) {
        syslog(LOG_WARNING, "could not list nodes");
        close(sock);
        return NULL;
    }

    for(np = config; np; np = np->next) {
        int i;
        for(i = 0; i < na.na_nodes; i++) {
            if(     (nr[i].nr_flags & IEEE80211_NODEREQ_AP)
                ||  (nr[i].nr_capinfo & IEEE80211_CAPINFO_IBSS)) {
                printf("%s\n", nr[i].nr_nwid);
                if(!strcmp(np->ssid, nr[i].nr_nwid)) {
                    goto FoundMatch;
                }
            }
        }
    }
FoundMatch:
    close(sock);
    return np;
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

    /* Parse initial config. */
    config = LoadConfig();
    if(config == NULL) {
        return -1;
    }

    /* Process until we're told to stop. */
    syslog(LOG_INFO, "started");
    while(!should_exit) {
        if(reload_config) {
            syslog(LOG_INFO, "Reloading config");
            DestroyConfig(config);
            config = LoadConfig();
            if(config == NULL) {
                return -1;
            }
            reload_config = 0;
        }
        if(CheckStatus()) {
            if(last_state != 1) {
                syslog(LOG_INFO, "%s is up", ifname);
                last_state = 1;
            }
            sleep(1);
        } else {
            Network *np;
            if(last_state != 0) {
                syslog(LOG_INFO, "%s is down", ifname);
                last_state = 0;
            }
            np = Scan(config);
            if(np) {
                syslog(LOG_INFO, "%s connecting to %s", ifname, np->ssid);
                StartNetwork(np);
                sleep(10);
            } else {
                sleep(5);
            }
        }
    }

    /* Normal shutdown. */
    syslog(LOG_INFO, "exiting");
    DestroyConfig(config);
    return 0;
}
