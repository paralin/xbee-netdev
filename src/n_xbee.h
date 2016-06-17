#pragma once
#ifndef _N_XBEE_H
#define _N_XBEE_H
#define POSIX

#include <stdlib.h>
#include <stdint.h>
// #include <stdbool.h>

#include <pthread.h>

#include <sys/socket.h>

#include <xbee/platform.h>
#include <xbee/device.h>
#include <xbee/discovery.h>

// Actual MTU of the hardware
#define N_XBEE_DATA_MTU 72
#define N_XBEE_MAXFRAME (N_XBEE_DATA_MTU*2 + 16)
#define TUN_PATH "/dev/net/tun"

#define N_XBEE_PREAMBLE_LEN 0
#define N_XBEE_ETHHDR_LEN sizeof(struct ether_header)

// endpoint for xbee-netdev
// #define N_XBEE_ENDPOINT 0xE7
#define N_XBEE_ENDPOINT 0xE8
// cluster ID, might want to make this settable down the line
#define N_XBEE_CLUSTER_ID 0x11

// Tick every 100ms
// #define N_XBEE_TICK_INTERVAL 100
// Discover every 2 minutes
#define N_XBEE_DISCOVER_INTERVAL 120000

#define XBEE_NETDEV_PREFIX "xbee"

struct xbee_serial_bridge;

// Discovered remote node
struct xbee_remote_node;
typedef struct xbee_remote_node {
  unsigned char node_addr[8];
  struct xbee_remote_node* next;
} xbee_remote_node;
xbee_remote_node* n_xbee_node_table;

/*
 * One bridge is created per registered xbee.
 */
struct xbee_pending_dev;
typedef struct xbee_serial_bridge {
  // USB0
  char* name;
  char* netdevName;
  int netdevInitialized;
  // file descriptor for tun
  int netdev;
  int netdev_idx;
  // dummy socket
  int netdev_sock;
  // passed to open()
  const char* tty_name;
  xbee_dev_t* xbee_dev;
  pthread_mutex_t write_lock;
} xbee_serial_bridge;
struct xbee_serial_bridge* n_xbee_serial_bridge;

// kernel module functions not in header file
#endif
