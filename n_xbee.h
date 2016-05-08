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

#define N_XBEE_LISC 17
#define N_XBEE_DATA_MTU 72
#define N_XBEE_MAXFRAME (N_XBEE_DATA_MTU*2 + 16)

#define XBEE_NETDEV_PREFIX "xbee"

//Frame Types
#define MODEM_STATUS 					0X8A
#define AT_COMMAND 					0X08
#define AT_COMMAND_QUEUE_PARAMETER_VALUE 		0X09
#define AT_COMMAND_RESPONSE 				0X88
#define REMOTE_COMMAND_REQUEST				0X17
#define REMOTE_COMMAND_RESPONSE 			0X97
#define ZIGBEE_TRANSMIT_REQUEST 			0X10
#define EXPLICIT_ADDRESSING_COMMAND_FRAME 		0X11
#define ZIGBEE_TRANSMIT_STATUS 				0X8B
#define ZIGBEE_RECEIVE_PACKET 				0X90
#define ZIGBEE_EXPLICIT_RX_INDICATOR 			0X91
#define ZIGBEE_IO_DATA_SAMPLE_RX_INDICATOR 		0X92
#define XBEE_SENSOR_READ_INDICATOR 			0X94
#define NODE_IDENTIFICATION_INDICATOR 			0X95

//Transmit Status
#define TRANSMIT_SUCCESS 0x00
#define TRANSMIT_CCA_FAILURE 0x02
#define TRANSMIT_INVALID 0x15
#define TRANSMIT_NETACKFAIL 0x21
#define TRANSMIT_NOTJOINED 0x22
#define TRANSMIT_SELFADDRESSED 0x23
#define TRANSMIT_ADDRNOTFOUND 0x24
#define TRANSMIT_ROUTENOTFOUND 0x25

// Current reception state
typedef struct xbee_frame_state {
	struct net_device_stats stats;
	struct tty_struct *tty;

	//Buffer for frame data bytes
	unsigned char *rbuff;
	//Number of received data bytes (after escapes)
	int rcount;
  // Receive state machine variable
  unsigned char frame_status;
#define UNFRAMED 	0	// No Start Delimiter received
#define FRAME_LEN_HIGH 	1	// Ready to receive high byte of length field
#define FRAME_LEN_LOW 	2	// Ready to receive low byte of length field
#define FRAME_DATA 	3	// Receiving the frame data
#define FRAME_DATA_ESC  4
#define FRAME_CHECKSUM	5	// Receiving the checksum byte

	//The length of the frame being received (according to packet)
	unsigned short frame_len;

	//Main read/write lock
	// spinlock_t lock;
} xbee_frame_state;

/*
 * One bridge is created per registered xbee.
 */
typedef struct xbee_serial_bridge {
  char* name;
  char* netdevName;
  // boolean if netdev inited
  int netdevInitialized;
  struct net_device* netdev;
  struct xbee_serial_bridge* next;
  struct xbee_frame_state frame_state;
  struct tty_struct* tty;
} xbee_serial_bridge;
struct xbee_serial_bridge* n_xbee_serial_bridges;

// Private storage for xbee netdev
typedef struct xbee_netdev_priv {
  xbee_serial_bridge* bridge;
} xbee_netdev_priv;

// kernel module functions not in header file
#endif
