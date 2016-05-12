#pragma once
#ifndef _N_XBEE_H
#define _N_XBEE_H

#include <linux/module.h>    // included for all kernel modules
#include <linux/kernel.h>    // included for KERN_INFO
#include <linux/init.h>      // included for __init and __exit macros
#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/tty.h>
#include <asm/uaccess.h>

#include <xbee/device.h>

#define N_XBEE_LISC 17

// Actual MTU of the hardware
#define N_XBEE_DATA_MTU 72
#define N_XBEE_MAXFRAME (N_XBEE_DATA_MTU*2 + 16)

#define XBEE_NETDEV_PREFIX "xbee"

// Size in bytes of the receive buffer
// This should be a bit bigger than the max
// frame we would ever receive, to give some margin
#define N_XBEE_BUFFER_SIZE (2*N_XBEE_MAXFRAME)
// We buffer received data
typedef struct xbee_data_buffer {
  unsigned char* buffer;
  // total size
  int size;
  // amount held currently
  // NOT an index, its a size 1-index.
  int pos;
} xbee_data_buffer;
/*
 * One bridge is created per registered xbee.
 */
typedef struct xbee_serial_bridge {
  char* name;
  char* netdevName;
  int netdevInitialized;
  struct net_device* netdev;
  struct xbee_serial_bridge* next;
  struct tty_struct* tty;
  struct xbee_data_buffer* recvbuf;
  spinlock_t write_lock;
  spinlock_t read_lock;
} xbee_serial_bridge;
struct xbee_serial_bridge* n_xbee_serial_bridges;

// Private storage for xbee netdev
typedef struct xbee_netdev_priv {
  xbee_serial_bridge* bridge;
} xbee_netdev_priv;

// kernel module functions not in header file
#endif
