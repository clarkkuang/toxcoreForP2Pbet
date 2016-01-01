/* tox-bootstrapd.c
 *
 * Tox DHT bootstrap daemon.
 *
 *  Copyright (C) 2014 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// system provided
#include <arpa/inet.h>
#include <getopt.h>
#include <syslog.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 3rd party
#include <libconfig.h>

// ./configure
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

// toxcore
#include "../../toxcore/LAN_discovery.h"
#include "../../toxcore/onion_announce.h"
#include "../../toxcore/TCP_server.h"
#include "../../toxcore/util.h"

// misc
#include "../bootstrap_node_packets.c"
#include "../../testing/misc_tools.c"

#include "global.h"
#include "log.h"


#define SLEEP_TIME_MILLISECONDS 30
#define sleep usleep(1000*SLEEP_TIME_MILLISECONDS)

#define DEFAULT_PID_FILE_PATH         "tox-bootstrapd.pid"
#define DEFAULT_KEYS_FILE_PATH        "tox-bootstrapd.keys"
#define DEFAULT_PORT                  33445
#define DEFAULT_ENABLE_IPV6           1 // 1 - true, 0 - false
#define DEFAULT_ENABLE_IPV4_FALLBACK  1 // 1 - true, 0 - false
#define DEFAULT_ENABLE_LAN_DISCOVERY  1 // 1 - true, 0 - false
#define DEFAULT_ENABLE_TCP_RELAY      1 // 1 - true, 0 - false
#define DEFAULT_TCP_RELAY_PORTS       443, 3389, 33445 // comma-separated list of ports. make sure to adjust DEFAULT_TCP_RELAY_PORTS_COUNT accordingly
#define DEFAULT_TCP_RELAY_PORTS_COUNT 3
#define DEFAULT_ENABLE_MOTD           1 // 1 - true, 0 - false
#define DEFAULT_MOTD                  DAEMON_NAME

#define MIN_ALLOWED_PORT 1
#define MAX_ALLOWED_PORT 65535


// Uses the already existing key or creates one if it didn't exist
//
// retirns 1 on success
//         0 on failure - no keys were read or stored

int manage_keys(DHT *dht, char *keys_file_path)
{
    const uint32_t KEYS_SIZE = crypto_box_PUBLICKEYBYTES + crypto_box_SECRETKEYBYTES;
    uint8_t keys[KEYS_SIZE];
    FILE *keys_file;

    // Check if file exits, proceed to open and load keys
    keys_file = fopen(keys_file_path, "r");

    if (keys_file != NULL) {
        const size_t read_size = fread(keys, sizeof(uint8_t), KEYS_SIZE, keys_file);

        if (read_size != KEYS_SIZE) {
            fclose(keys_file);
            return 0;
        }

        memcpy(dht->self_public_key, keys, crypto_box_PUBLICKEYBYTES);
        memcpy(dht->self_secret_key, keys + crypto_box_PUBLICKEYBYTES, crypto_box_SECRETKEYBYTES);
    } else {
        // Otherwise save new keys
        memcpy(keys, dht->self_public_key, crypto_box_PUBLICKEYBYTES);
        memcpy(keys + crypto_box_PUBLICKEYBYTES, dht->self_secret_key, crypto_box_SECRETKEYBYTES);

        keys_file = fopen(keys_file_path, "w");

        if (!keys_file)
            return 0;

        const size_t write_size = fwrite(keys, sizeof(uint8_t), KEYS_SIZE, keys_file);

        if (write_size != KEYS_SIZE) {
            fclose(keys_file);
            return 0;
        }
    }

    fclose(keys_file);

    return 1;
}

// Parses tcp relay ports from `cfg` and puts them into `tcp_relay_ports` array
//
// Supposed to be called from get_general_config only
//
// Important: iff `tcp_relay_port_count` > 0, then you are responsible for freeing `tcp_relay_ports`

void parse_tcp_relay_ports_config(config_t *cfg, uint16_t **tcp_relay_ports, int *tcp_relay_port_count)
{
    const char *NAME_TCP_RELAY_PORTS = "tcp_relay_ports";

    *tcp_relay_port_count = 0;

    config_setting_t *ports_array = config_lookup(cfg, NAME_TCP_RELAY_PORTS);

    if (ports_array == NULL) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in the configuration file.\n", NAME_TCP_RELAY_PORTS);
        write_log(LOG_LEVEL_WARNING, "Using default '%s':\n", NAME_TCP_RELAY_PORTS);

        uint16_t default_ports[DEFAULT_TCP_RELAY_PORTS_COUNT] = {DEFAULT_TCP_RELAY_PORTS};

        int i;

        for (i = 0; i < DEFAULT_TCP_RELAY_PORTS_COUNT; i ++) {
            write_log(LOG_LEVEL_INFO, "Port #%d: %u\n", i, default_ports[i]);
        }

        // similar procedure to the one of reading config file below
        *tcp_relay_ports = malloc(DEFAULT_TCP_RELAY_PORTS_COUNT * sizeof(uint16_t));

        for (i = 0; i < DEFAULT_TCP_RELAY_PORTS_COUNT; i ++) {

            (*tcp_relay_ports)[*tcp_relay_port_count] = default_ports[i];

            if ((*tcp_relay_ports)[*tcp_relay_port_count] < MIN_ALLOWED_PORT
                    || (*tcp_relay_ports)[*tcp_relay_port_count] > MAX_ALLOWED_PORT) {
                write_log(LOG_LEVEL_WARNING, "Port #%d: Invalid port: %u, should be in [%d, %d]. Skipping.\n", i,
                       (*tcp_relay_ports)[*tcp_relay_port_count], MIN_ALLOWED_PORT, MAX_ALLOWED_PORT);
                continue;
            }

            (*tcp_relay_port_count) ++;
        }

        // the loop above skips invalid ports, so we adjust the allocated memory size
        if ((*tcp_relay_port_count) > 0) {
            *tcp_relay_ports = realloc(*tcp_relay_ports, (*tcp_relay_port_count) * sizeof(uint16_t));
        } else {
            free(*tcp_relay_ports);
            *tcp_relay_ports = NULL;
        }

        return;
    }

    if (config_setting_is_array(ports_array) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_ERROR, "'%s' setting should be an array. Array syntax: 'setting = [value1, value2, ...]'.\n",
               NAME_TCP_RELAY_PORTS);
        return;
    }

    int config_port_count = config_setting_length(ports_array);

    if (config_port_count == 0) {
        write_log(LOG_LEVEL_ERROR, "'%s' is empty.\n", NAME_TCP_RELAY_PORTS);
        return;
    }

    *tcp_relay_ports = malloc(config_port_count * sizeof(uint16_t));

    int i;

    for (i = 0; i < config_port_count; i ++) {
        config_setting_t *elem = config_setting_get_elem(ports_array, i);

        if (elem == NULL) {
            // it's NULL if `ports_array` is not an array (we have that check earlier) or if `i` is out of range, which should not be
            write_log(LOG_LEVEL_WARNING, "Port #%d: Something went wrong while parsing the port. Stopping reading ports.\n", i);
            break;
        }

        if (config_setting_is_number(elem) == CONFIG_FALSE) {
            write_log(LOG_LEVEL_WARNING, "Port #%d: Not a number. Skipping.\n", i);
            continue;
        }

        (*tcp_relay_ports)[*tcp_relay_port_count] = config_setting_get_int(elem);

        if ((*tcp_relay_ports)[*tcp_relay_port_count] < MIN_ALLOWED_PORT
                || (*tcp_relay_ports)[*tcp_relay_port_count] > MAX_ALLOWED_PORT) {
            write_log(LOG_LEVEL_WARNING, "Port #%d: Invalid port: %u, should be in [%d, %d]. Skipping.\n", i,
                   (*tcp_relay_ports)[*tcp_relay_port_count], MIN_ALLOWED_PORT, MAX_ALLOWED_PORT);
            continue;
        }

        (*tcp_relay_port_count) ++;
    }

    // the loop above skips invalid ports, so we adjust the allocated memory size
    if ((*tcp_relay_port_count) > 0) {
        *tcp_relay_ports = realloc(*tcp_relay_ports, (*tcp_relay_port_count) * sizeof(uint16_t));
    } else {
        free(*tcp_relay_ports);
        *tcp_relay_ports = NULL;
    }
}

// Gets general config options
//
// Important: you are responsible for freeing `pid_file_path` and `keys_file_path`
//            also, iff `tcp_relay_ports_count` > 0, then you are responsible for freeing `tcp_relay_ports`
//            and also `motd` iff `enable_motd` is set
//
// returns 1 on success
//         0 on failure, doesn't modify any data pointed by arguments

int get_general_config(const char *cfg_file_path, char **pid_file_path, char **keys_file_path, int *port,
                       int *enable_ipv6,
                       int *enable_ipv4_fallback, int *enable_lan_discovery, int *enable_tcp_relay, uint16_t **tcp_relay_ports,
                       int *tcp_relay_port_count, int *enable_motd, char **motd)
{
    config_t cfg;

    const char *NAME_PORT                 = "port";
    const char *NAME_PID_FILE_PATH        = "pid_file_path";
    const char *NAME_KEYS_FILE_PATH       = "keys_file_path";
    const char *NAME_ENABLE_IPV6          = "enable_ipv6";
    const char *NAME_ENABLE_IPV4_FALLBACK = "enable_ipv4_fallback";
    const char *NAME_ENABLE_LAN_DISCOVERY = "enable_lan_discovery";
    const char *NAME_ENABLE_TCP_RELAY     = "enable_tcp_relay";
    const char *NAME_ENABLE_MOTD          = "enable_motd";
    const char *NAME_MOTD                 = "motd";

    config_init(&cfg);

    // Read the file. If there is an error, report it and exit.
    if (config_read_file(&cfg, cfg_file_path) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_ERROR, "%s:%d - %s\n", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return 0;
    }

    // Get port
    if (config_lookup_int(&cfg, NAME_PORT, port) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_PORT);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %d\n", NAME_PORT, DEFAULT_PORT);
        *port = DEFAULT_PORT;
    }

    // Get PID file location
    const char *tmp_pid_file;

    if (config_lookup_string(&cfg, NAME_PID_FILE_PATH, &tmp_pid_file) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_PID_FILE_PATH);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_PID_FILE_PATH, DEFAULT_PID_FILE_PATH);
        tmp_pid_file = DEFAULT_PID_FILE_PATH;
    }

    *pid_file_path = malloc(strlen(tmp_pid_file) + 1);
    strcpy(*pid_file_path, tmp_pid_file);

    // Get keys file location
    const char *tmp_keys_file;

    if (config_lookup_string(&cfg, NAME_KEYS_FILE_PATH, &tmp_keys_file) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_KEYS_FILE_PATH);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_KEYS_FILE_PATH, DEFAULT_KEYS_FILE_PATH);
        tmp_keys_file = DEFAULT_KEYS_FILE_PATH;
    }

    *keys_file_path = malloc(strlen(tmp_keys_file) + 1);
    strcpy(*keys_file_path, tmp_keys_file);

    // Get IPv6 option
    if (config_lookup_bool(&cfg, NAME_ENABLE_IPV6, enable_ipv6) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_ENABLE_IPV6);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_ENABLE_IPV6, DEFAULT_ENABLE_IPV6 ? "true" : "false");
        *enable_ipv6 = DEFAULT_ENABLE_IPV6;
    }

    // Get IPv4 fallback option
    if (config_lookup_bool(&cfg, NAME_ENABLE_IPV4_FALLBACK, enable_ipv4_fallback) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_ENABLE_IPV4_FALLBACK);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_ENABLE_IPV4_FALLBACK,
               DEFAULT_ENABLE_IPV4_FALLBACK ? "true" : "false");
        *enable_ipv4_fallback = DEFAULT_ENABLE_IPV4_FALLBACK;
    }

    // Get LAN discovery option
    if (config_lookup_bool(&cfg, NAME_ENABLE_LAN_DISCOVERY, enable_lan_discovery) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_ENABLE_LAN_DISCOVERY);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_ENABLE_LAN_DISCOVERY,
               DEFAULT_ENABLE_LAN_DISCOVERY ? "true" : "false");
        *enable_lan_discovery = DEFAULT_ENABLE_LAN_DISCOVERY;
    }

    // Get TCP relay option
    if (config_lookup_bool(&cfg, NAME_ENABLE_TCP_RELAY, enable_tcp_relay) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_ENABLE_TCP_RELAY);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_ENABLE_TCP_RELAY,
               DEFAULT_ENABLE_TCP_RELAY ? "true" : "false");
        *enable_tcp_relay = DEFAULT_ENABLE_TCP_RELAY;
    }

    if (*enable_tcp_relay) {
        parse_tcp_relay_ports_config(&cfg, tcp_relay_ports, tcp_relay_port_count);
    } else {
        *tcp_relay_port_count = 0;
    }

    // Get MOTD option
    if (config_lookup_bool(&cfg, NAME_ENABLE_MOTD, enable_motd) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_ENABLE_MOTD);
        write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_ENABLE_MOTD,
               DEFAULT_ENABLE_MOTD ? "true" : "false");
        *enable_motd = DEFAULT_ENABLE_MOTD;
    }

    if (*enable_motd) {
        // Get MOTD
        const char *tmp_motd;

        if (config_lookup_string(&cfg, NAME_MOTD, &tmp_motd) == CONFIG_FALSE) {
            write_log(LOG_LEVEL_WARNING, "No '%s' setting in configuration file.\n", NAME_MOTD);
            write_log(LOG_LEVEL_WARNING, "Using default '%s': %s\n", NAME_MOTD, DEFAULT_MOTD);
            tmp_motd = DEFAULT_MOTD;
        }

        size_t tmp_motd_length = strlen(tmp_motd) + 1;
        size_t motd_length = tmp_motd_length > MAX_MOTD_LENGTH ? MAX_MOTD_LENGTH : tmp_motd_length;
        *motd = malloc(motd_length);
        strncpy(*motd, tmp_motd, motd_length);
        (*motd)[motd_length - 1] = '\0';
    }

    config_destroy(&cfg);

    write_log(LOG_LEVEL_INFO, "Successfully read:\n");
    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_PID_FILE_PATH,        *pid_file_path);
    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_KEYS_FILE_PATH,       *keys_file_path);
    write_log(LOG_LEVEL_INFO, "'%s': %d\n", NAME_PORT,                 *port);
    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_ENABLE_IPV6,          *enable_ipv6          ? "true" : "false");
    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_ENABLE_IPV4_FALLBACK, *enable_ipv4_fallback ? "true" : "false");
    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_ENABLE_LAN_DISCOVERY, *enable_lan_discovery ? "true" : "false");

    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_ENABLE_TCP_RELAY,     *enable_tcp_relay     ? "true" : "false");

    // show info about tcp ports only if tcp relay is enabled
    if (*enable_tcp_relay) {
        if (*tcp_relay_port_count == 0) {
            write_log(LOG_LEVEL_ERROR, "No TCP ports could be read.\n");
        } else {
            write_log(LOG_LEVEL_INFO, "Read %d TCP ports:\n", *tcp_relay_port_count);
            int i;

            for (i = 0; i < *tcp_relay_port_count; i ++) {
                write_log(LOG_LEVEL_INFO, "Port #%d: %u\n", i, (*tcp_relay_ports)[i]);
            }
        }
    }

    write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_ENABLE_MOTD,          *enable_motd          ? "true" : "false");

    if (*enable_motd) {
        write_log(LOG_LEVEL_INFO, "'%s': %s\n", NAME_MOTD, *motd);
    }

    return 1;
}

// Bootstraps nodes listed in the config file
//
// returns 1 on success, some or no bootstrap nodes were added
//         0 on failure, a error accured while parsing config file

int bootstrap_from_config(const char *cfg_file_path, DHT *dht, int enable_ipv6)
{
    const char *NAME_BOOTSTRAP_NODES = "bootstrap_nodes";

    const char *NAME_PUBLIC_KEY = "public_key";
    const char *NAME_PORT       = "port";
    const char *NAME_ADDRESS    = "address";

    config_t cfg;

    config_init(&cfg);

    if (config_read_file(&cfg, cfg_file_path) == CONFIG_FALSE) {
        write_log(LOG_LEVEL_ERROR, "%s:%d - %s\n", config_error_file(&cfg), config_error_line(&cfg), config_error_text(&cfg));
        config_destroy(&cfg);
        return 0;
    }

    config_setting_t *node_list = config_lookup(&cfg, NAME_BOOTSTRAP_NODES);

    if (node_list == NULL) {
        write_log(LOG_LEVEL_WARNING, "No '%s' setting in the configuration file. Skipping bootstrapping.\n", NAME_BOOTSTRAP_NODES);
        config_destroy(&cfg);
        return 1;
    }

    if (config_setting_length(node_list) == 0) {
        write_log(LOG_LEVEL_WARNING, "No bootstrap nodes found. Skipping bootstrapping.\n");
        config_destroy(&cfg);
        return 1;
    }

    int bs_port;
    const char *bs_address;
    const char *bs_public_key;

    config_setting_t *node;

    int i = 0;

    while (config_setting_length(node_list)) {

        node = config_setting_get_elem(node_list, 0);

        if (node == NULL) {
            config_destroy(&cfg);
            return 0;
        }

        // Check that all settings are present
        if (config_setting_lookup_string(node, NAME_PUBLIC_KEY, &bs_public_key) == CONFIG_FALSE) {
            write_log(LOG_LEVEL_WARNING, "Bootstrap node #%d: Couldn't find '%s' setting. Skipping the node.\n", i, NAME_PUBLIC_KEY);
            goto next;
        }

        if (config_setting_lookup_int(node, NAME_PORT, &bs_port) == CONFIG_FALSE) {
            write_log(LOG_LEVEL_WARNING, "Bootstrap node #%d: Couldn't find '%s' setting. Skipping the node.\n", i, NAME_PORT);
            goto next;
        }

        if (config_setting_lookup_string(node, NAME_ADDRESS, &bs_address) == CONFIG_FALSE) {
            write_log(LOG_LEVEL_WARNING, "Bootstrap node #%d: Couldn't find '%s' setting. Skipping the node.\n", i, NAME_ADDRESS);
            goto next;
        }

        // Process settings
        if (strlen(bs_public_key) != crypto_box_PUBLICKEYBYTES * 2) {
            write_log(LOG_LEVEL_WARNING, "Bootstrap node #%d: Invalid '%s': %s. Skipping the node.\n", i, NAME_PUBLIC_KEY,
                   bs_public_key);
            goto next;
        }

        if (bs_port < MIN_ALLOWED_PORT || bs_port > MAX_ALLOWED_PORT) {
            write_log(LOG_LEVEL_WARNING, "Bootstrap node #%d: Invalid '%s': %d, should be in [%d, %d]. Skipping the node.\n", i, NAME_PORT,
                   bs_port, MIN_ALLOWED_PORT, MAX_ALLOWED_PORT);
            goto next;
        }

        uint8_t *bs_public_key_bin = hex_string_to_bin((char *)bs_public_key);
        const int address_resolved = DHT_bootstrap_from_address(dht, bs_address, enable_ipv6, htons(bs_port),
                                     bs_public_key_bin);
        free(bs_public_key_bin);

        if (!address_resolved) {
            write_log(LOG_LEVEL_WARNING, "Bootstrap node #%d: Invalid '%s': %s. Skipping the node.\n", i, NAME_ADDRESS, bs_address);
            goto next;
        }

        write_log(LOG_LEVEL_INFO, "Successfully added bootstrap node #%d: %s:%d %s\n", i, bs_address, bs_port, bs_public_key);

next:
        // config_setting_lookup_string() allocates string inside and doesn't allow us to free it direcly
        // though it's freed when the element is removed, so we free it right away in order to keep memory
        // consumption minimal
        config_setting_remove_elem(node_list, 0);
        i++;
    }

    config_destroy(&cfg);

    return 1;
}

// Prints public key

void print_public_key(const uint8_t *public_key)
{
    char buffer[2 * crypto_box_PUBLICKEYBYTES + 1];
    int index = 0;

    size_t i;

    for (i = 0; i < crypto_box_PUBLICKEYBYTES; i++) {
        index += sprintf(buffer + index, "%02hhX", public_key[i]);
    }

    write_log(LOG_LEVEL_INFO, "Public Key: %s\n", buffer);

    return;
}

// Prints --help message

bool print_help()
{
    // 2 space ident
    // make sure all lines fit into 80 columns
    write_log(LOG_LEVEL_INFO,
           "Usage: tox-bootstrapd [OPTION]... --config=FILE_PATH\n"
           "\n"
           "Options:\n"
           "  --config=FILE_PATH     Specify path to the config file.\n"
           "                         This is a required option.\n"
           "                         Set FILE_PATH to a path to an empty file in order to\n"
           "                         use default settings.\n"
           "  --help                 Print this help message.\n"
           "  --log-backend=BACKEND  Specify which logging backend to use.\n"
           "                         Valid BACKEND values (case sensetive):\n"
           "                           syslog Writes log messages to syslog.\n"
           "                                  Default option when no --log-backend is\n"
           "                                  specified.\n"
           "                           stdout Writes log messages to stdout/stderr.\n"
           "  --version              Print version information.\n");
}

// Handels command line arguments, setting cfg_file_path and log_backend.
// Terminates the application if incorrect arguments are specified.

void handle_command_line_arguments(int argc, char *argv[], char **cfg_file_path, LOG_BACKEND *log_backend)
{
    if (argc < 2) {
        write_log(LOG_LEVEL_ERROR, "Error: No arguments provided.\n\n");
        print_help();
        exit(1);
    }

    opterr = 0;

    static struct option long_options[] = {
        {"help",        no_argument,       0, 'h'},
        {"config",      required_argument, 0, 'c'}, // required option
        {"log-backend", required_argument, 0, 'l'}, // optional, defaults to syslog
        {"version",     no_argument,       0, 'v'},
        {0,             0,                 0,  0 }
    };

    bool cfg_file_path_set = false;
    bool log_backend_set   = false;

    int opt;

    while ((opt = getopt_long(argc, argv, ":", long_options, NULL)) != -1) {

        switch (opt) {
            case 'h':
                print_help();
                exit(0);

            case 'c':
                *cfg_file_path = optarg;
                cfg_file_path_set = true;
                break;

            case 'l':
                if (strcmp(optarg, "syslog") == 0) {
                    *log_backend = LOG_BACKEND_SYSLOG;
                    log_backend_set = true;
                } else if (strcmp(optarg, "stdout") == 0) {
                    *log_backend = LOG_BACKEND_STDOUT;
                    log_backend_set = true;
                } else {
                    write_log(LOG_LEVEL_ERROR, "Error: Invalid BACKEND value for --log-backend option passed: %s\n\n", optarg);
                    print_help();
                    exit(1);
                }
                break;

            case 'v':
                write_log(LOG_LEVEL_INFO, "Version: %lu\n", DAEMON_VERSION_NUMBER);
                exit(0);

            case '?':
                write_log(LOG_LEVEL_ERROR, "Error: Unrecognized option %s\n\n", argv[optind-1]);
                print_help();
                exit(1);

            case ':':
                write_log(LOG_LEVEL_ERROR, "Error: No argument provided for option %s\n\n", argv[optind-1]);
                print_help();
                exit(1);
        }
    }

    if (!log_backend_set) {
        *log_backend = LOG_BACKEND_SYSLOG;
    }

    if (!cfg_file_path_set) {
        write_log(LOG_LEVEL_ERROR, "Error: The required --config option wasn't specified\n\n");
        print_help();
        exit(1);
    }
}

int main(int argc, char *argv[])
{
    char *cfg_file_path;
    LOG_BACKEND log_backend;

    // choose backend for printing command line argument parsing output based on whether the daemon is being run from a terminal
    log_backend = isatty(STDOUT_FILENO) ? LOG_BACKEND_STDOUT : LOG_BACKEND_SYSLOG;

    open_log(log_backend);
    handle_command_line_arguments(argc, argv, &cfg_file_path, &log_backend);
    close_log();

    open_log(log_backend);

    write_log(LOG_LEVEL_INFO, "Running \"%s\" version %lu.\n", DAEMON_NAME, DAEMON_VERSION_NUMBER);

    char *pid_file_path, *keys_file_path;
    int port;
    int enable_ipv6;
    int enable_ipv4_fallback;
    int enable_lan_discovery;
    int enable_tcp_relay;
    uint16_t *tcp_relay_ports;
    int tcp_relay_port_count;
    int enable_motd;
    char *motd;

    if (get_general_config(cfg_file_path, &pid_file_path, &keys_file_path, &port, &enable_ipv6, &enable_ipv4_fallback,
                           &enable_lan_discovery, &enable_tcp_relay, &tcp_relay_ports, &tcp_relay_port_count, &enable_motd, &motd)) {
        write_log(LOG_LEVEL_INFO, "General config read successfully\n");
    } else {
        write_log(LOG_LEVEL_ERROR, "Couldn't read config file: %s. Exiting.\n", cfg_file_path);
        return 1;
    }

    if (port < MIN_ALLOWED_PORT || port > MAX_ALLOWED_PORT) {
        write_log(LOG_LEVEL_ERROR, "Invalid port: %d, should be in [%d, %d]. Exiting.\n", port, MIN_ALLOWED_PORT, MAX_ALLOWED_PORT);
        return 1;
    }

    // Check if the PID file exists
    FILE *pid_file;

    if ((pid_file = fopen(pid_file_path, "r"))) {
        write_log(LOG_LEVEL_WARNING, "Another instance of the daemon is already running, PID file %s exists.\n", pid_file_path);
        fclose(pid_file);
    }

    IP ip;
    ip_init(&ip, enable_ipv6);

    Networking_Core *net = new_networking(ip, port);

    if (net == NULL) {
        if (enable_ipv6 && enable_ipv4_fallback) {
            write_log(LOG_LEVEL_WARNING, "Couldn't initialize IPv6 networking. Falling back to using IPv4.\n");
            enable_ipv6 = 0;
            ip_init(&ip, enable_ipv6);
            net = new_networking(ip, port);

            if (net == NULL) {
                write_log(LOG_LEVEL_ERROR, "Couldn't fallback to IPv4. Exiting.\n");
                return 1;
            }
        } else {
            write_log(LOG_LEVEL_ERROR, "Couldn't initialize networking. Exiting.\n");
            return 1;
        }
    }


    DHT *dht = new_DHT(net);

    if (dht == NULL) {
        write_log(LOG_LEVEL_ERROR, "Couldn't initialize Tox DHT instance. Exiting.\n");
        return 1;
    }

    Onion *onion = new_onion(dht);
    Onion_Announce *onion_a = new_onion_announce(dht);

    if (!(onion && onion_a)) {
        write_log(LOG_LEVEL_ERROR, "Couldn't initialize Tox Onion. Exiting.\n");
        return 1;
    }

    if (enable_motd) {
        if (bootstrap_set_callbacks(dht->net, DAEMON_VERSION_NUMBER, (uint8_t *)motd, strlen(motd) + 1) == 0) {
            write_log(LOG_LEVEL_INFO, "Set MOTD successfully.\n");
        } else {
            write_log(LOG_LEVEL_ERROR, "Couldn't set MOTD: %s. Exiting.\n", motd);
            return 1;
        }

        free(motd);
    }

    if (manage_keys(dht, keys_file_path)) {
        write_log(LOG_LEVEL_INFO, "Keys are managed successfully.\n");
    } else {
        write_log(LOG_LEVEL_ERROR, "Couldn't read/write: %s. Exiting.\n", keys_file_path);
        return 1;
    }

    TCP_Server *tcp_server = NULL;

    if (enable_tcp_relay) {
        if (tcp_relay_port_count == 0) {
            write_log(LOG_LEVEL_ERROR, "No TCP relay ports read. Exiting.\n");
            return 1;
        }

        tcp_server = new_TCP_server(enable_ipv6, tcp_relay_port_count, tcp_relay_ports, dht->self_secret_key, onion);

        // tcp_relay_port_count != 0 at this point
        free(tcp_relay_ports);

        if (tcp_server != NULL) {
            write_log(LOG_LEVEL_INFO, "Initialized Tox TCP server successfully.\n");
        } else {
            write_log(LOG_LEVEL_ERROR, "Couldn't initialize Tox TCP server. Exiting.\n");
            return 1;
        }
    }

    if (bootstrap_from_config(cfg_file_path, dht, enable_ipv6)) {
        write_log(LOG_LEVEL_INFO, "List of bootstrap nodes read successfully.\n");
    } else {
        write_log(LOG_LEVEL_ERROR, "Couldn't read list of bootstrap nodes in %s. Exiting.\n", cfg_file_path);
        return 1;
    }

    print_public_key(dht->self_public_key);

    // Write the PID file
    FILE *pidf = fopen(pid_file_path, "a+");

    if (pidf == NULL) {
        write_log(LOG_LEVEL_ERROR, "Couldn't open the PID file for writing: %s. Exiting.\n", pid_file_path);
        return 1;
    }

    free(pid_file_path);
    free(keys_file_path);

    // Fork off from the parent process
    const pid_t pid = fork();

    if (pid > 0) {
        fprintf(pidf, "%d", pid);
        fclose(pidf);
        write_log(LOG_LEVEL_INFO, "Forked successfully: PID: %d.\n", pid);
        return 0;
    } else {
        fclose(pidf);
    }

    if (pid < 0) {
        write_log(LOG_LEVEL_ERROR, "Forking failed. Exiting.\n");
        return 1;
    }

    // Change the file mode mask
    umask(0);

    // Create a new SID for the child process
    if (setsid() < 0) {
        write_log(LOG_LEVEL_ERROR, "SID creation failure. Exiting.\n");
        return 1;
    }

    // Change the current working directory
    if ((chdir("/")) < 0) {
        write_log(LOG_LEVEL_ERROR, "Couldn't change working directory to '/'. Exiting.\n");
        return 1;
    }

    // Go quiet
    if (log_backend != LOG_BACKEND_STDOUT) {
        close(STDOUT_FILENO);
        close(STDIN_FILENO);
        close(STDERR_FILENO);
    }

    uint64_t last_LANdiscovery = 0;
    const uint16_t htons_port = htons(port);

    int waiting_for_dht_connection = 1;

    if (enable_lan_discovery) {
        LANdiscovery_init(dht);
        write_log(LOG_LEVEL_INFO, "Initialized LAN discovery.\n");
    }

    while (1) {
        do_DHT(dht);

        if (enable_lan_discovery && is_timeout(last_LANdiscovery, LAN_DISCOVERY_INTERVAL)) {
            send_LANdiscovery(htons_port, dht);
            last_LANdiscovery = unix_time();
        }

        if (enable_tcp_relay) {
            do_TCP_server(tcp_server);
        }

        networking_poll(dht->net);

        if (waiting_for_dht_connection && DHT_isconnected(dht)) {
            write_log(LOG_LEVEL_INFO, "Connected to other bootstrap node successfully.\n");
            waiting_for_dht_connection = 0;
        }

        sleep;
    }

    return 1;
}
