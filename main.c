#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include "hash.h"

#define DEBUG 0 // if set 1, app will output debug values while parsing topology file
#define debug_print(fmt, ...) \
            do { if (DEBUG) printf(fmt, __VA_ARGS__); } while (0)
#define TOPOLOGY_DUMP_NAME   "topology.last"
#define SW_SSCANF_FMT     "Switch\t%d %s"
#define SW_SSCANF_CON_FMT "[%d]\t%20s[%d]%18s"
#define CA_SSCANF_FMT     "Ca\t%d %s"
#define CA_SSCANF_CON_FMT "[%d]%20s\t%20s[%d]"
#define PROGNAME "topo_parser"
#define MAXLINE 1024
#define FREE(x) do { if(x) { free(x); x = NULL; } } while(0);
#define NODE_DESC_LEN 64
#define GUID_LEN 20
#define WSPEED_LEN 8
#define VALUE_LEN 38
/* defining colors for progress bar */
#define BLU   "\x1B[34m"
#define RESET "\x1B[0m"
/* High intensty background */
#define GRNHB "\e[0;102m"
#define BILLION  1000000000L;
struct hashmap *map; /* Here is hash table where device guids will be saved */
struct timespec start, end; /* Variables for calculating function execution duration */
static unsigned int device_counter = 0; /* Keeping here parsed devices counter for statistics */
static long int line_counter = 0; /* Keeping here line counters for detailed statistic (TBD) */
/* device types */
typedef enum {
    SW, CADAPTER
} DEV_TYPE;
/* base port types */
typedef enum {
    ENHANCED, BASE
} PORT_TYPE;
/* connection data */
struct connection {
    int lport;
    int rport;
    int llid;
    int llmc;
    int rlid;
    DEV_TYPE device_type;
    char nodeGUIDHex[GUID_LEN + 1];
    char portGUIDHex[GUID_LEN + 1];
    char node_desc[NODE_DESC_LEN + 1];
    char widthspeed[WSPEED_LEN + 1];
    struct connection *next;
};
/* device data */
struct ibdevice {
    int vid;
    int did;
    int ports_total;
    char nodeGUIDHex[GUID_LEN + 1];
    int64_t portGUIDHex;
    PORT_TYPE base_port_type;
    int base_port_no;
    int lid;
    int lmc;
    int conn_counter;
    DEV_TYPE device_type;
    int64_t sysimgguid;
    int64_t devguid;
    char node_desc[NODE_DESC_LEN + 1];
    struct connection *connections;
    struct ibdevice *next;
};
struct guid {
    char key[GUID_LEN + 1];
    char value[VALUE_LEN + 1];
};
/* variables for playing with device lists */
struct ibdevice *dev_temp = NULL, *dev_list = NULL;
/* file handle for topology file */
FILE *th;

extern void save_device_info(char key[], char value[]);

//
int guid_compare(const void *a, const void *b, void *udata) {
    (void)udata;
    const struct guid *ua = a;
    const struct guid *ub = b;
    return strcmp(ua->key, ub->key);
}

uint64_t guid_hash(const void *item, uint64_t seed0, uint64_t seed1) {
    const struct guid *g = item;
    return hashmap_sip(g->key, strlen(g->key), seed0, seed1);
}

/* trim string */
char *trim(char *s) {
    char *ptr;
    if (!s)
        return NULL;   // handle NULL string
    if (!*s)
        return s;      // handle empty string
    for (ptr = s + strlen(s) - 1; (ptr >= s) && isspace(*ptr); --ptr);
    ptr[1] = '\0';
    return s;
}

/* check if file exits */
int file_exists(char *path) {
    FILE *fp = fopen(path, "r");
    if (fp) {
        fclose(fp);
        return true;
    } else {
        return false;
    }
}

/* skip empty or commented lines to save time */
bool skip_line(const char *line) {
    bool retval = false;
    if (line[0] == '#' || line[0] == '\n' || (line[0] == '\r' && line[1] == '\n')) {
        retval = true;
    }
    return retval;
}

/* print usage and exit */
void print_usage() {
    printf("Usage:\n\t%16s -f <topology file> --parse topology file\n"
           "\t%16s -p -- print parsed topology\n"
           "\t%16s -h -- print usage and exit\n", PROGNAME, PROGNAME, PROGNAME);
    exit(EXIT_SUCCESS);
}

/* remove custom character from string */
char *remove_char(char *s, int ch) {
    unsigned int i, j;
    size_t len = strlen(s);

    for (i = 0; i < len; i++) {
        if (s[i] == ch) {
            for (j = i; j < len; j++) {
                s[j] = s[j + 1];
            }
            len--;
            i--;
        }
    }
    return s;
}

/* print message and exit */
void die(const char *msg) {
    printf("%s\n", msg);
    exit(EXIT_FAILURE);
}

/* dump connections for each found device */
void dump_connections(struct connection *p) {
    while (p != NULL) {

        if (p->device_type == SW) {
            printf("[%d]\t\"%s\"[%d]", p->lport, p->nodeGUIDHex, p->rport);
            if (p->portGUIDHex[0] != '\0') {
                printf("(%s)", p->portGUIDHex);
            }
            printf(" \t\t# \"%s\" lid %d %s\n", p->node_desc, p->llid, p->widthspeed);
        } else {
            printf("[%d](%s) \t\"%s\"[%d]\t\t# lid %d lmc %d \"%s\" lid %d %s\n",
                   p->lport,
                   p->portGUIDHex,
                   p->nodeGUIDHex,
                   p->rport,
                   p->llid,
                   p->llmc,
                   p->node_desc,
                   p->rlid,
                   p->widthspeed
            );
        }

        fflush(stdout);
        p = p->next;
    }
}

/* draws output as it is requested in the task description */
void draw_output(struct ibdevice *p, FILE *desc) {
    char *value = NULL;
    struct guid *Guid, g;
    if (desc == NULL) {
        desc = stdout;
    }
    while (p != NULL) {
        if (p->vid == 0x0 && p->did == 0x0 && p->sysimgguid == 0x0 && p->devguid == 0x0 && p->connections == NULL) {
            return;
        }
        fprintf(desc, "%s:\n", (p->device_type == SW) ? "Switch" : "Host");
        fprintf(desc, "sysimgguid: 0x%lx\n", p->sysimgguid);
        fprintf(desc, "%s: 0x%lx",
                (p->device_type == SW) ? "switch_id: " : "port_id: ", p->devguid);
        if (p->device_type == SW) {
            fprintf(desc, "(%lx)\n", p->portGUIDHex);
        } else {
            fprintf(desc, "\n");
        }
        struct connection *cp = p->connections->next;
        while (cp != NULL) {
            bzero(g.key, sizeof(g.key));
            strncpy(g.key, cp->nodeGUIDHex, GUID_LEN);
            Guid = hashmap_get(map,&g);
            if (NULL != Guid) {
                value = Guid->value;
            }
            fprintf(desc, "\tConnected to %s: %s=%s",
                    (cp->nodeGUIDHex[0] == 'S') ? "switch" : "host",
                    (cp->nodeGUIDHex[0] == 'S') ? "switchguid" : "caguid",
                    value);

            fprintf(desc, ", port=%d\n", cp->rport);
            cp = cp->next;
        }

        fprintf(desc, "\n");
        p = p->next;
    }
}

/* dumps devices and connections as it is defined in topology file format  (see original file)
 * __attribute__((unused)) used to silent compiler warning
 * */
__attribute__((unused)) void dump_devices(struct ibdevice *p) {
    while (p != NULL) {
        printf("vendid=0x%x\n", p->vid);
        printf("devid=0x%x\n", p->did);
        printf("sysimgguid=0x%lx\n", p->sysimgguid);
        printf("%s=0x%lx", (p->device_type == SW) ? "switchguid" : "caguid", p->devguid);
        if (p->device_type == SW) {
            printf("(%lx)\n", p->portGUIDHex);
        } else {
            printf("\n");
        }
        printf("%s\t%d\t\"%s\"\t\t# \"%s\" ",
               (p->device_type == SW) ? "Switch" : "Ca",
               p->ports_total,
               p->nodeGUIDHex,
               p->node_desc);

        if (p->device_type == SW) {
            printf("%s port %d lid %d lmc %d",
                   (p->base_port_type == BASE) ? "base" : "enhanced",
                   p->base_port_no,
                   p->lid,
                   p->lmc);
        }
        //
        printf("\n");
        fflush(stdout);
        if (p->connections && p->connections->next) {
            dump_connections(p->connections->next);
        }
        printf("\n\n");
        p = p->next;
    }
}

/* preparing memory for devices */
void create_ibdevice_list() {
    dev_list = (struct ibdevice *) malloc(sizeof(struct ibdevice));
    if (!dev_list) {
        die("Cannot allocate memory!");
    }
}

/* copying linked list with connections data */
struct connection *copy_connections(struct connection *c) {
    if (c == NULL) {
        return c;
    }
    struct connection *targetList = (struct connection *) malloc(sizeof(struct connection));
    if (!targetList) {
        die("Cannot allocate memory");
    }
    targetList->lport = c->lport;
    targetList->rport = c->rport;
    targetList->llid = c->llid;
    targetList->llmc = c->llmc;
    targetList->rlid = c->rlid;
    targetList->device_type = c->device_type;
    strncpy(targetList->nodeGUIDHex, c->nodeGUIDHex, GUID_LEN);
    strncpy(targetList->portGUIDHex, c->portGUIDHex, GUID_LEN);
    strncpy(targetList->node_desc, c->node_desc, NODE_DESC_LEN);
    strncpy(targetList->widthspeed, c->widthspeed, WSPEED_LEN);
    targetList->next = copy_connections(c->next);
    return targetList;
}

/* adding device to the list */
void add_ibdevice() {
    if (!dev_temp ||
        (dev_temp->vid == 0x0 && dev_temp->did == 0x0 && dev_temp->sysimgguid == 0x0 && dev_temp->devguid == 0x0 &&
         dev_temp->connections == NULL)) {
        return;
    }
    struct ibdevice *last = dev_list;
    struct ibdevice *new_node = (struct ibdevice *) malloc(sizeof(struct ibdevice));
    if (!new_node) {
        die("Cannot allocate memory");
    }

    memcpy(new_node, dev_temp, sizeof(struct ibdevice));
    if (new_node->connections != NULL) {
        new_node->connections = copy_connections(dev_temp->connections);
    }
    new_node->next = NULL;
    if (dev_list == NULL) {
        dev_list = new_node;
        return;
    }
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = new_node;
    //
    device_counter++;

    FREE(dev_temp->connections);
    FREE(dev_temp);
}

/* getting parameters separated by '=' in  from topology file */
int64_t get_param_val(char *line, const char *param) {
    size_t param_sz = strlen(param);
    size_t line_sz = strlen(line);
    int64_t retval = -1;
    if (line_sz < param_sz) {
        retval = -1;
    } else if (!memcmp(line, param, param_sz)) {
        const char *delim = "=";
        char *token = strtok(line, delim);
        if (!token) {
            return -1;
        }
        token = strtok(NULL, delim);
        if (0 != strcmp(token, "")) {
            retval = (int64_t) strtoull(token, NULL, 16);
        }
    }
    return retval;
}

/* getting pair of switch and port GUIDS from topology file for each device */
int64_t *get_switch_port_guid_hex(char *line) {
    static int64_t retval[2] = {-1, -1};
    if (!memcmp(line, "switchguid", 10)) {
        const char *delim = "=";
        char *token = strtok(line, delim);
        if (!token) {
            return NULL;
        }
        token = strtok(NULL, delim);
        if (!token) {
            return NULL;
        }
        char *swguid = strtok(token, "(");
        if (swguid) {
            retval[0] = (int64_t) strtoul(swguid, NULL, 16);
        }
        char *portguid = strtok(NULL, "(");
        if (portguid) {
            remove_char(portguid, ')');
            retval[1] = (int64_t) strtoul(portguid, NULL, 16);
        }
        return retval;
    }
    return NULL;
}

/* getting identificators for each device from topology file */
void scan_device_ids(char *line) {
    int64_t sysimgguid = 0, caguid = 0;
    int64_t *switch_port_guids;

    int venid = (int) get_param_val(line, "vendid");
    if (venid != -1) {
        dev_temp->vid = venid;
        debug_print("-> vid: 0x%x\n", dev_temp->vid);
    }

    int devid = (int) get_param_val(line, "devid");
    if (devid != -1) {
        dev_temp->did = devid;
        debug_print("-> devid: 0x%x\n", dev_temp->did);
    }

    sysimgguid = get_param_val(line, "sysimgguid");
    if (sysimgguid != -1) {
        dev_temp->sysimgguid = sysimgguid;
        debug_print("-> sysimgguid: 0x%lx\n", dev_temp->sysimgguid);
    }

    switch_port_guids = get_switch_port_guid_hex(line);
    if (switch_port_guids) {
        if (*switch_port_guids != -1) {
            dev_temp->devguid = *switch_port_guids;
            dev_temp->device_type = SW;
        }
        if (*(switch_port_guids + 1) != -1) {
            dev_temp->portGUIDHex = *(switch_port_guids + 1);
        }
        debug_print("-> switchguid: 0x%lx(%lx)\n", dev_temp->devguid, dev_temp->portGUIDHex);
    }

    caguid = get_param_val(line, "caguid");
    if (caguid != -1) {
        dev_temp->devguid = caguid;
        dev_temp->device_type = CADAPTER;
        debug_print("-> caguid: 0x%lx\n", dev_temp->devguid);
    }
}

/* getting device data from topology file */
void scan_device_desc(char *line) {
    int val = 0;
    char *tmp;
    const char *delim = "#";
    char value[VALUE_LEN + 1] = {0};
    if (memcmp(line, "Switch", 6) && memcmp(line, "Ca", 2)) {
        return;
    }
    char *first_part = strtok(line, delim);
    if (first_part) {
        val = sscanf(line, ((dev_temp->device_type == SW)) ? SW_SSCANF_FMT : (dev_temp->device_type == CADAPTER)
                                                                             ? CA_SSCANF_FMT : " ",
                     &dev_temp->ports_total, dev_temp->nodeGUIDHex);
        if (val == 2) {
            remove_char(dev_temp->nodeGUIDHex, '"');
            debug_print("-> %s %d\t\"%s\"\t\t#", (dev_temp->device_type == SW) ? "Switch" : "Ca",
                        dev_temp->ports_total, dev_temp->nodeGUIDHex);

            if (dev_temp->device_type == SW) {
                snprintf(value, VALUE_LEN, "0x%lx(%lx)", dev_temp->devguid, dev_temp->portGUIDHex);
            } else {
                snprintf(value, VALUE_LEN, "0x%lx", dev_temp->devguid);
            }

            save_device_info(dev_temp->nodeGUIDHex, value);
        }
    }
    char *second_part = strtok(NULL, delim);
    if (second_part) {
        strtok(second_part, "\"");
        tmp = strtok(NULL, "\"");
        if (tmp) {
            snprintf(dev_temp->node_desc, NODE_DESC_LEN, "%s", tmp);
            debug_print(" \"%s\"", dev_temp->node_desc);
        }

        if (dev_temp->device_type == SW) {
            tmp = strtok(NULL, "\"");
            tmp = strtok(tmp, " ");
            if (tmp) {
                if (0 == strncmp(tmp, "enhanced", 8)) {
                    dev_temp->base_port_type = ENHANCED;
                } else if (0 == strncmp(tmp, "base", 4)) {
                    dev_temp->base_port_type = BASE;
                }
                debug_print(" %s port", (dev_temp->base_port_type == BASE) ? "base" : "enhanced");
            }
            tmp = strtok(NULL, " "); // skip "port"
            tmp = strtok(NULL, " ");
            if (tmp) {
                dev_temp->base_port_no = (int) strtol(tmp, NULL, 10);
                debug_print(" %d lid", dev_temp->base_port_no);
            }
            tmp = strtok(NULL, " "); // skip "lid"
            tmp = strtok(NULL, " ");
            if (tmp) {
                dev_temp->lid = (int) strtol(tmp, NULL, 10);
                debug_print(" %d lmc", dev_temp->lid);
            }
            tmp = strtok(NULL, " "); // skip "lmc"
            tmp = strtok(NULL, " ");
            if (tmp) {
                dev_temp->lmc = (int) strtol(tmp, NULL, 10);
                debug_print(" %d\n", dev_temp->lmc);
            }
        } else {
            debug_print("%s", "\n");
        }
    }
}

/* parsing connections for each device */
void scan_network_connections(char *line) {
    int val;
    char *tmp;

    const char *delim = "#";
    if (line[0] != '[') {
        return;
    }
    //
    struct connection *last = dev_temp->connections;
    struct connection *new_node = (struct connection *) malloc(sizeof(struct connection));
    if (!new_node) {
        die("Cannot allocate memory");
    }
    //
    char *first_part = strtok(line, delim);
    char *second_part = strtok(NULL, delim);

    if (first_part) {
        if (dev_temp->device_type == SW) {
            val = sscanf(line, SW_SSCANF_CON_FMT, &new_node->lport, new_node->nodeGUIDHex,
                         &new_node->rport, new_node->portGUIDHex);
            if (val > 0) {
                debug_print("-> [%d]\t%s[%d]", new_node->lport, new_node->nodeGUIDHex,
                            new_node->rport);
                if (new_node->portGUIDHex[0] != '\0') {
                    debug_print("%s", new_node->portGUIDHex);
                }
                debug_print("\t\t#%s", " ");
            }
        } else if (dev_temp->device_type == CADAPTER) {
            val = sscanf(line, CA_SSCANF_CON_FMT, &new_node->lport, new_node->portGUIDHex,
                         new_node->nodeGUIDHex, &new_node->rport);
            if (val == 4) {
                debug_print("-> [%d]%s \t%s[%d]", new_node->lport, new_node->portGUIDHex,
                            new_node->nodeGUIDHex, new_node->rport);
            }
            debug_print("\t\t#%s", " ");
        }
        remove_char(new_node->nodeGUIDHex, '"');
        remove_char(new_node->portGUIDHex, '(');
        remove_char(new_node->portGUIDHex, ')');
    }

    if (second_part) {
        if (dev_temp->device_type == SW) {
            strtok(second_part, "\"");
            tmp = strtok(NULL, "\"");
            if (tmp) {
                snprintf(new_node->node_desc, NODE_DESC_LEN, "%s", tmp);
            }
            debug_print("\"%s\"", new_node->node_desc);
            tmp = strtok(NULL, "\"");
            tmp = strtok(tmp, " ");

            if (tmp) {
                new_node->llid = (int) strtoul(strtok(NULL, " "), NULL, 10);
                debug_print(" lid %d", new_node->llid);
            }
            tmp = strtok(NULL, " ");
            if (tmp) {
                snprintf(new_node->widthspeed, WSPEED_LEN, "%s", tmp);
            }
            debug_print(" %s\n", new_node->widthspeed);
        } else if (dev_temp->device_type == CADAPTER) {
            tmp = strtok(second_part, " ");
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            if (tmp) {
                new_node->llid = (int) strtoul(tmp, NULL, 10);
            }
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            if (tmp) {
                new_node->llmc = (int) strtoul(tmp, NULL, 10);
            }
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            if (tmp) {
                snprintf(new_node->node_desc, NODE_DESC_LEN, "%s", tmp);
            }
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            if (tmp) {
                new_node->rlid = (int) strtoul(tmp, NULL, 10);
            }
            debug_print("%s ", tmp);
            tmp = strtok(NULL, " ");
            if (tmp) {
                snprintf(new_node->widthspeed, WSPEED_LEN, "%s", tmp);
            }
            debug_print("%s\n", tmp);
        }
        remove_char(new_node->node_desc, '"');
    }
    new_node->device_type = dev_temp->device_type;
    new_node->next = NULL;
    if (dev_temp->connections == NULL) {
        dev_temp->connections = new_node;
        return;
    }
    while (last->next != NULL) {
        last = last->next;
    }
    last->next = new_node;
    dev_temp->conn_counter++;
}

/* reading data from file with topology data for further dumping */
void read_topology_from_file(char *file_name) {
    char *line = NULL;
    size_t len = 0;
    ssize_t read;
    FILE *file;
    if (file_exists(file_name)) {
        file = fopen(file_name, "r");
        while ((read = getline(&line, &len, file)) != -1) {
            printf("%s", line);
        }
    }
    fclose(file);
}

/* Save parsed topology data here for further use */
void dump_topology_to_file(char *file_name) {
    FILE *file;
    file = fopen(file_name, "w");
    if (file == NULL) {
        die("Could not open the file\n");
    }
    draw_output(dev_list->next, file);
    fclose(file);
}

/* Saving each nodeGUID of device with it's identificators for further user */
void save_device_info(char key[], char value[]) {
    struct guid g;
    if ((key == NULL) || (0 == strcmp(key, "")) || 18 != strlen(key)) {
        return;
    }
    bzero(g.key, sizeof(g.key));
    bzero(g.value, sizeof(g.value));
    strncpy(g.key, key, GUID_LEN); //key;
    strncpy(g.value, value, VALUE_LEN); // value;
    hashmap_set(map, &g);
}

/* Parse each line and get appropriate data */
void get_params(char *line) {
    size_t line_sz = strlen(line);

    char tmp_line[MAXLINE];
    strncpy(tmp_line, line, line_sz);
    tmp_line[line_sz] = '\0';

    if (NULL == dev_temp) {
        dev_temp = (struct ibdevice *) malloc(sizeof(struct ibdevice));
        if (!dev_temp) {
            die("Cannot allocate memory!");
        }
        dev_temp->next = NULL;
    }
    if (dev_temp->connections == NULL) {
        dev_temp->connections = (struct connection *) malloc(sizeof(struct connection));
        if (!dev_temp->connections) {
            die("Cannot allocate memory!");
        }
        dev_temp->connections->next = NULL;
    }

    scan_device_ids(tmp_line);
    scan_device_desc(tmp_line);
    scan_network_connections(tmp_line);

}

/*show progress bar */
void show_progress(long int current_bytes, long int maxsize) {
    int progress = (int) (current_bytes * 100.0 / maxsize);

    printf("\n\033[F");
    /* As we always add a device to the list after loop completes, the device counter here will always be -1 than the real count, so adding +1 */
    printf("Lines parsed: " BLU "%ld" RESET ", devices found: "BLU"%d"RESET", progress: "BLU"%3d%% "RESET"[",
           line_counter, device_counter + 1, progress);
    printf(GRNHB);
    for (int i = 0; i < progress; i++) {
        printf(" ");
    }
    printf(RESET"]\033[1C");
    fflush(stdout);
}

/* parsing topology file */
void parse_topology_file(char *topo_filename) {
    char *line = NULL;
    long int fsize = 0, current_bytes = 0;
    size_t len = 0;
    ssize_t read;
    if (file_exists(topo_filename)) {
        printf("File found: %s\n", topo_filename);
        map = hashmap_new(sizeof(struct ibdevice), 0, 0, 0,
                          guid_hash, guid_compare, NULL, NULL);
        create_ibdevice_list();
        th = fopen(topo_filename, "r");
        fseek(th, 0L, SEEK_END);
        fsize = ftell(th);
        fseek(th, 0L, SEEK_SET);
        if (clock_gettime(CLOCK_REALTIME, &start) == -1) {
            die("Could not engage the clock\n");
        }

        while ((read = getline(&line, &len, th)) != -1) {
            current_bytes += read;
            if (skip_line(line)) {
                continue;
            }
            line_counter++;
            line = trim(line);
            if (!memcmp(line, "vendid", 6)) {
                add_ibdevice();
                debug_print("%s", "\n");
            }
            get_params(line);
            if (DEBUG == 0) {
                show_progress(current_bytes, fsize);
            }
        }
        add_ibdevice(); /* Adding the last device after EOF */
        if (clock_gettime(CLOCK_REALTIME, &end) == -1) {
            die("Could not engage the clock\n");
        }
        fclose(th);
        printf("\n");
        /* Dumping data here to use it later */
        dump_topology_to_file(TOPOLOGY_DUMP_NAME);
        FREE(line);
        FREE(dev_list);
        double duration = (end.tv_sec - start.tv_sec) + (double) (end.tv_nsec - start.tv_nsec) / (double) BILLION;
        printf("Topology analysis took %f seconds\n", duration);
    } else {
        printf("File not found: %s\n", topo_filename);
    }
    if (map) {
        hashmap_free(map);
    }
}

/* read saved topology data from saved file and dump the output
 * function is incomplete due to some questions regard to task
 * */
void print_topology() {
    read_topology_from_file(TOPOLOGY_DUMP_NAME);
}
/* checking if opt is valid */
bool is_valid_opt(const char *opts) {
    return (opts != NULL);
}

void sighandler(int signum) {
    printf(RESET"Caught interrupt/terminating signal %d\nSaving what is possible...\n", signum);
    dump_topology_to_file(TOPOLOGY_DUMP_NAME);
    die("Bye!\n");
}

/* main function */
int main(int argc, char **argv) {
    int opt = 0;
    int long_index = 0;
    static struct option long_options[] = {
            {"help",     no_argument,       0, 'h'},
            {"parse",    no_argument,       0, 'p'},
            {"topofile", required_argument, 0, 'f'},
            {0, 0,                          0, 0}
    };
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    while ((opt = getopt_long(argc, argv, "hpf:", long_options, &long_index)) != -1) {
        switch (opt) {
            case 'h' :
                print_usage();
                break;
            case 'p' :
                print_topology();
                break;
            case 'f' :
                if (!is_valid_opt(optarg)) {
                    print_usage();
                }
                parse_topology_file(optarg);
                break;
            default:
                print_usage();
                break;
        }
    }
    if (argc == 1 || (argc > 1 && argv[1][0] != '-')) {
        print_usage();
    }

    return 0;
}
