#include "n_xbee.h"
#include "hexdump.h"

#include <unistd.h>
#include <libgen.h>
#include <assert.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <fcntl.h>

#include <xbee/device.h>
#include <xbee/atcmd.h>
#include <xbee/atmode.h>
#include <xbee/wpan.h>
#include <xbee/discovery.h>

#include <linux/if.h>
#include <linux/if_arp.h>
#include <linux/if_tun.h>

#include <net/ethernet.h>
#include <netinet/in.h>


// compat with old printk defs
#define KERN_INFO
#define KERN_ALERT
#define printk printf
#define msleep(TIME) usleep(TIME * 1000)

struct ether_arp {
  struct arphdr ea_hdr;
  unsigned char  arp_sha[6]; /* sender hardware address */
  unsigned char  arp_spa[4]; /* sender protocol address */
  unsigned char  arp_tha[6]; /* target hardware address */
  unsigned char  arp_tpa[4];
};

/* == Xbee stuff == */
const xbee_dispatch_table_entry_t xbee_frame_handlers[] =
{
  // handle AT frames
  XBEE_FRAME_HANDLE_LOCAL_AT,
  XBEE_FRAME_HANDLE_ATND_RESPONSE,
  // handle receiving a frame
  XBEE_FRAME_HANDLE_RX_EXPLICIT,
  XBEE_FRAME_HANDLE_AO0_NODEID,
  // print modem statuses
  XBEE_FRAME_MODEM_STATUS_DEBUG,
  // marker for the end
  XBEE_FRAME_TABLE_END
};

wpan_ep_state_t zdo_ep_state = { 0 };
wpan_ep_state_t zcl_ep_state = { 0 };

int n_xbee_netdev_rx(const wpan_envelope_t* envelope, void* context);
void n_xbee_xmit_ether_packet(struct xbee_serial_bridge* bridge, const void* buffer, int len);

const wpan_cluster_table_entry_t xbee_data_clusters[] = {
  { N_XBEE_CLUSTER_ID, NULL, NULL, WPAN_CLUST_FLAG_INOUT | WPAN_CLUST_FLAG_NOT_ZCL },
  // if we don't set ATAO to 0...
  XBEE_DISC_DIGI_DATA_CLUSTER_ENTRY,
  WPAN_CLUST_ENTRY_LIST_END
};

const wpan_endpoint_table_entry_t xbee_endpoints[] = {
  // ZDO_ENDPOINT(zdo_ep_state),

  { N_XBEE_ENDPOINT,
    WPAN_PROFILE_DIGI,
    // ignore general endpoint, instead listen to cluster
    n_xbee_netdev_rx,
    NULL,
    0x0,
    0x0,
    xbee_data_clusters
  },
  { WPAN_ENDPOINT_END_OF_LIST }
};

/* = Serial Bridge Stuff = */
void n_xbee_free_xbee_dev(xbee_dev_t* dev) {
  if (dev)
    free(dev);
}

// Just free, don't remove properly.
// Also frees all netdevs etc.
void n_xbee_free_netdev(xbee_serial_bridge* n);
void n_xbee_free_bridge(xbee_serial_bridge* n) {
  if (!n) return;
  if (n->netdevInitialized)
    n_xbee_free_netdev(n);
  if (n->name)
    free(n->name);
  if (n->xbee_dev)
    n_xbee_free_xbee_dev(n->xbee_dev);
  if (n->netdevName)
    free(n->netdevName);
  free(n);
}

void n_xbee_free_remote_nodetable(void) {
  struct xbee_remote_node* nodn;
  struct xbee_remote_node* nod = n_xbee_node_table;
  n_xbee_node_table = NULL;
  while (nod) {
    nodn = nod->next;
    // do something to free
    free(nod);
    nod = nodn;
  }
}

/* = Node Table Stuff = */
xbee_remote_node* n_xbee_node_find_or_insert(const addr64* id) {
  char addr64_buf[ADDR64_STRING_LENGTH];
  struct xbee_remote_node* nnod;
  struct xbee_remote_node* pnod = NULL;
  struct xbee_remote_node* nod = n_xbee_node_table;
  while (nod) {
    if (memcmp(&nod->node_addr, id, sizeof(addr64)) == 0) {
      return nod;
    }
    pnod = nod;
    nod = nod->next;
  }
  printk(KERN_INFO "%s: registering new remote node %s\n", __FUNCTION__, addr64_format(addr64_buf, id));
  nnod = malloc(sizeof(xbee_remote_node));
  memcpy(&nnod->node_addr, id, sizeof(addr64));
  nnod->next = NULL;
  if (pnod)
    pnod->next = nnod;
  else
    n_xbee_node_table = nnod;
  return 0;
}

// should be ETH_ALEN
xbee_remote_node* n_xbee_node_find_eth(const void* addr, int len) {
  void* tocmp;
  struct xbee_remote_node* nod = n_xbee_node_table;
  if (len > 8)
    len = 8;
  while (nod) {
    tocmp = (nod->node_addr) + (8 - len);
    if (memcmp(tocmp, addr, len) == 0)
      return nod;
    nod = nod->next;
  }
  return 0;
}

/* = XBEE Controls */
#define N_XBEE_CHECK_ITERATIONS(iter, itern) \
  if (iterations >= itern) { \
    printk(KERN_ALERT "%s: Timeout waiting for AT mode\n", __FUNCTION__); \
    return -ETIMEDOUT; \
  }

// Checks the tty to see if there is really an xbee
// on the other end, and if so, it's communicating right.
// Also retreives some params about the remote dev
// THIS IS REALLY MESSY CODE
// FUTURE IMPROVEMENT: Only enter AT mode if API mode fails.
// REWRITE:
//   - Attempt to send API frame with AT command to get API mode.
//   - If it does not return OR returns a mode other than 1, revert to ATMODE
#define CHECK_MISC_ATMODE_ERRS \
  else if (mode == -EPERM) { \
    printk(KERN_ALERT "%s: [bug] After sending, xbee code isn't waiting for a response.\n", __FUNCTION__); \
    return -EIO; \
  } \
else if (mode == -ENOSPC) { \
  printk(KERN_ALERT "%s: [bug] Response from AT cmd was too big, but should only be 1-2 characters.\n", __FUNCTION__); \
  return -EIO; \
} \
else if (mode == -ETIMEDOUT) { \
  printk(KERN_ALERT "%s: Timeout waiting for AT response.\n", __FUNCTION__); \
  return -ETIMEDOUT; \
} else { \
  printk(KERN_ALERT "%s: Unexpected error returned from xbee_atmode_read_response: %d\n", __FUNCTION__, mode); \
  return -EIO; \
}
#define CHECK_RESP_BUF_SIZE 255
void n_xbee_node_discovered(xbee_dev_t* xbee, const xbee_node_id_t *rec);
int n_xbee_check_tty(xbee_serial_bridge* bridge) {
  int err, mode, iterations, bytesread;
  char respbuf[CHECK_RESP_BUF_SIZE];
  xbee_dev_t* xbee = bridge->xbee_dev;
  // xbee_ser_rx_flush(bridge->xbee_dev->);
  printk(KERN_INFO "Putting board into AT mode to check values...\n");
  if ((err = xbee_atmode_enter(xbee)) != 0) {
    printk(KERN_ALERT "Unable to put board into AT mode, error: %d\n", err);
    return err;
  }
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_ITERATIONS(iterations, 4000);

    mode = xbee_atmode_tick(xbee);

    if (mode == XBEE_MODE_COMMAND) {
      printk(KERN_INFO "%s: Successfully entered AT mode...\n", __FUNCTION__);
      break;
    }
    else if (mode == XBEE_MODE_IDLE) {
      printk(KERN_ALERT "%s: Never entered AT mode (in idle mode), assuming failure.\n", __FUNCTION__);
      return -ETIMEDOUT;
    }

    // msleep 5 milliseconds
    msleep(5);
    iterations ++;
  }

  // n_xbee_flush_buffer(bridge->tty);

  printk(KERN_INFO "%s: Setting API mode 1...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AP 1")) != 0) {
    printk(KERN_ALERT "%s: Unable to set API mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_ITERATIONS(iterations, 200);
    mode = xbee_atmode_read_response(xbee, respbuf, CHECK_RESP_BUF_SIZE, &bytesread);
    if (mode == -EAGAIN) {
      msleep(5);
      iterations ++;
      continue;
    }
    else if (mode == 0) {
      printk(KERN_INFO "%s: Successfully set API mode 1 from %c...\n", __FUNCTION__, respbuf[0]);
      break;
    }
    CHECK_MISC_ATMODE_ERRS;
  }

  msleep(100);
  // n_xbee_flush_buffer(bridge->tty);

  printk(KERN_INFO "%s: Verifying API mode...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AP")) != 0) {
    printk(KERN_ALERT "%s: Unable to request API mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_ITERATIONS(iterations, 200);
    mode = xbee_atmode_read_response(xbee, respbuf, CHECK_RESP_BUF_SIZE, &bytesread);
    if (mode == -EAGAIN) {
      msleep(5);
      iterations ++;
      continue;
    }
    else if (mode == 0) {
      if (respbuf[0] != '1' && respbuf[1] != '1' && respbuf[2] != '1') {
        if (respbuf[0] > 0 && respbuf[0] < 200)
          printk(KERN_ALERT "%s: Response from AT is %c, should be 1, failing.\n", __FUNCTION__, respbuf[0]);
        else
          printk(KERN_ALERT "%s: Response from AT is NOT ASCII - (%d), should be 1 (%d), failing.\n", __FUNCTION__, (int)respbuf[0], (int)'1');
        return -EIO;
      }
      printk(KERN_INFO "%s: Successfully verified API mode 1...\n", __FUNCTION__);
      break;
    }
    CHECK_MISC_ATMODE_ERRS;
  }

  printk(KERN_INFO "%s: Setting AO mode 1...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AO 1")) != 0) {
    printk(KERN_ALERT "%s: Unable to set API mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    // N_XBEE_CHECK_ITERATIONS(iterations, 200);
    mode = xbee_atmode_read_response(xbee, respbuf, CHECK_RESP_BUF_SIZE, &bytesread);
    if (mode == -EAGAIN) {
      // msleep(5);
      iterations ++;
      continue;
    }
    else if (mode == 0) {
      printk(KERN_INFO "%s: Successfully set AO mode 1 from %c...\n", __FUNCTION__, respbuf[0]);
      break;
    }
    CHECK_MISC_ATMODE_ERRS;
  }

  msleep(100);
  // n_xbee_flush_buffer(bridge->tty);

  printk(KERN_INFO "%s: Verifying AO mode...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AO")) != 0) {
    printk(KERN_ALERT "%s: Unable to request AO mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_ITERATIONS(iterations, 200);
    mode = xbee_atmode_read_response(xbee, respbuf, CHECK_RESP_BUF_SIZE, &bytesread);
    if (mode == -EAGAIN) {
      msleep(5);
      iterations ++;
      continue;
    }
    else if (mode == 0) {
      if (respbuf[0] != '1' && respbuf[1] != '1' && respbuf[2] != '1') {
        if (respbuf[0] > 0 && respbuf[0] < 200)
          printk(KERN_ALERT "%s: Response from AT is %c, should be 1, failing.\n", __FUNCTION__, respbuf[0]);
        else
          printk(KERN_ALERT "%s: Response from AT is NOT ASCII - (%d), should be 1 (%d), failing.\n", __FUNCTION__, (int)respbuf[0], (int)'1');
        return -EIO;
      }
      printk(KERN_INFO "%s: Successfully verified AO mode 1...\n", __FUNCTION__);
      break;
    }
    CHECK_MISC_ATMODE_ERRS;
  }

  // exit AT mode
  printk(KERN_INFO "%s: Exiting AT mode...\n", __FUNCTION__);
  if ((err = xbee_atmode_exit(xbee)) != 0) {
    printk(KERN_ALERT "%s: Unable to exit AT mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  iterations = 0;
  // we have to wait around 2 seconds here for the xbee driver to exit AT mode
  while (1) {
    N_XBEE_CHECK_ITERATIONS(iterations, 410);
    mode = xbee_atmode_tick(xbee);
    if (mode == XBEE_MODE_IDLE) {
      printk(KERN_INFO "%s: Successfully exited AT mode.\n", __FUNCTION__);
      break;
    }
    msleep(5);
    iterations++;
  }

  // TODO: might be decreasable
  msleep(2000);
  // n_xbee_flush_buffer(bridge->tty);

  if ((err = xbee_cmd_init_device(xbee)) != 0) {
    printk(KERN_ALERT "%s: Error initing device: %d\n", __FUNCTION__, err);
    return err;
  }

  // Wait for the cmd_query_device to finish.
  iterations = 0;
  do {
    // N_XBEE_CHECK_ITERATIONS(iterations, 4000);
    // msleep(1);
    iterations++;
    xbee_dev_tick(xbee);
    pthread_yield();
  } while ((err = xbee_cmd_query_status(xbee)) == -EBUSY);
  if (err != 0) {
    printk(KERN_ALERT "%s: Error waiting for device query: %d\n", __FUNCTION__, err);
    return err;
  }

  // init the wpan layer
  xbee_wpan_init(xbee, xbee_endpoints);

  // register the discovery handler
  xbee_disc_add_node_id_handler(xbee, &n_xbee_node_discovered);

  // trigger discovery for everyone
  xbee_disc_discover_nodes(xbee, NULL);
  return 0;
}

int n_xbee_init_netdev(xbee_serial_bridge* bridge) {
  struct xbee_netdev_priv* priv;
  int err, fd, sock_fd;
  int flags = IFF_TAP | IFF_NO_PI;
  struct sockaddr_ll sall;
  struct ifreq ifr;

  if (!bridge || !bridge->name) return -1;
  if (bridge->netdevInitialized) {
    printk(KERN_ALERT "%s: Net bridge %s already inited!\n", __FUNCTION__, bridge->netdevName);
    return -1;
  }

  printk(KERN_INFO "%s: Initializing net bridge %s as %s...\n", __FUNCTION__, bridge->name, bridge->netdevName);
  if ((fd = open(TUN_PATH, O_RDWR)) < 0) {
    printk(KERN_ALERT "%s: Unable to open %s, error %d (%s)...\n", __FUNCTION__, TUN_PATH, fd, strerror(fd));
    return fd;
  }

  memset(&ifr, 0, sizeof(ifr));
  ifr.ifr_flags = flags;
  // doesn't work - maybe set later
  // ifr.ifr_mtu = N_XBEE_DATA_MTU;

  if (bridge->netdevName)
    strncpy(ifr.ifr_name, bridge->netdevName, IFNAMSIZ);

  if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0) {
    printk(KERN_ALERT "%s: Failed to alloc tap, %d (%s)...\n", __FUNCTION__, errno, strerror(errno));
    close(fd);
    return err;
  }

  // Might be unnecessary, check if ifr_ifindex is prefilled by TUNSETIFF
  if (!ifr.ifr_ifindex) {
    if ((err = ioctl(fd, SIOCGIFINDEX, (void *)&ifr)) < 0) {
      printk(KERN_ALERT "%s: SIOCGIFINDEX error %d (%s)...\n", __FUNCTION__, errno, strerror(errno));
      close(fd);
      return err;
    }
  }

  strcpy(bridge->netdevName, ifr.ifr_name);
  bridge->netdev = fd;
  bridge->netdev_idx = ifr.ifr_ifindex;
  bridge->netdev_sock = socket(AF_INET, SOCK_DGRAM, 0);
  bridge->netdevInitialized = 1;

  // set hwaddr
  memcpy(ifr.ifr_hwaddr.sa_data, bridge->xbee_dev->wpan_dev.address.ieee.b + 2, ETH_ALEN);
  ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
  if (ioctl(fd, SIOCSIFHWADDR, (void *)&ifr) < 0) {
    printk(KERN_ALERT "%s: unable to set MAC addr, %d (%s)...\n", __FUNCTION__, errno, strerror(errno));
  }

  return 0;
}

void n_xbee_free_netdev(xbee_serial_bridge* n) {
  if (!n) return;
  n->netdevInitialized = 0;
  if (!n->netdev) return;
  if (n->netdevName)
    printk(KERN_INFO "%s: Shutting down net bridge %s...\n", __FUNCTION__, n->netdevName);
  if (n->netdev_sock)
    close(n->netdev_sock);
  close(n->netdev);
  n->netdev = n->netdev_sock = 0;
}

// ticks the xbee
// since we register all the callbacks in the xbee code
// we can just call tick.
inline int n_xbee_handle_runtime_frames(xbee_serial_bridge* bridge) {
  return xbee_dev_tick(bridge->xbee_dev);
}

/* = XBEE Detection and Setup  =
*/

// if return anything but zero, call n_xbee_free_bridge if NOT noFreeBridge
int n_xbee_resolve_pending_dev(xbee_serial_bridge* bridge) {
  if (n_xbee_check_tty(bridge) != 0) {
    printk(KERN_ALERT "%s: Couldn't contact xbee on %s, make sure it's a valid xbee and the baud is correct.\n", __FUNCTION__, bridge->tty_name);
    return -ENODEV;
  }
  return 0;
}

void n_xbee_node_discovered(xbee_dev_t* xbee, const xbee_node_id_t *rec) {
  xbee_serial_bridge* bridge;
  char addr64_buf[ADDR64_STRING_LENGTH];
  // bridge = n_xbee_find_bridge_byxbee(xbee);
  bridge = n_xbee_serial_bridge;
  if (!bridge)
    return;
  printk(KERN_INFO "%s: %s discovered remote node %s.\n", __FUNCTION__, bridge->name, addr64_format(addr64_buf, &rec->ieee_addr_be));
  n_xbee_node_find_or_insert(&rec->ieee_addr_be);
}


/*
 * We actually need to check if there is a valid xbee on the
 * other end, and if not, bail out with an error.
 */
static int n_xbee_serial_open(xbee_serial_t* serial) {
  xbee_serial_bridge* bridge;
  int i, nlen, ndevnlen, err, resolvatt;
  const char* rttyname;
  const char* tty_name = basename(serial->device);

  // Find the existing allocated netdev for this (shouldn't happen)
  // .. or make a new one.
  printk(KERN_INFO "%s: TTY %s init...\n", __FUNCTION__, tty_name);

  // remove the tty
  rttyname = strncmp("tty", tty_name, 3) == 0 ? (tty_name + 3) : tty_name;
  nlen = strlen(rttyname);

  bridge = (xbee_serial_bridge*)malloc(sizeof(xbee_serial_bridge));
  pthread_mutex_init(&bridge->write_lock, NULL);
  bridge->tty_name = tty_name;
  bridge->name = (char*)malloc(sizeof(char) * (nlen + 1));
  bridge->name[nlen] = '\0';
  strncpy(bridge->name, rttyname, nlen);

  bridge->netdevInitialized = 0;
  bridge->netdev = 0;

  bridge->xbee_dev = (xbee_dev_t*) malloc(sizeof(xbee_dev_t));
  memset(bridge->xbee_dev, 0, sizeof(xbee_dev_t));
  bridge->xbee_dev->guard_time = 1000;
  bridge->xbee_dev->escape_char = '+';
  bridge->xbee_dev->idle_timeout = 100;

  if ((err = xbee_dev_init(bridge->xbee_dev, serial, NULL, NULL)) != 0) {
    printk(KERN_ALERT "%s: xbee_dev_init returned %d, error.\n", __FUNCTION__, err);
    free(bridge->name);
    free(bridge->xbee_dev);
    free(bridge);
    return err;
  }

  ndevnlen = strlen(XBEE_NETDEV_PREFIX) + nlen;
  assert(ndevnlen <= IFNAMSIZ);
  bridge->netdevName = (char*)malloc(IFNAMSIZ * sizeof(char) + 1); // sizeof(char) * (ndevnlen + 1));
  bridge->netdevName[ndevnlen] = '\0';
  strncpy(bridge->netdevName, XBEE_NETDEV_PREFIX, strlen(XBEE_NETDEV_PREFIX));
  strncpy(bridge->netdevName + strlen(XBEE_NETDEV_PREFIX), rttyname, nlen);
  n_xbee_serial_bridge = bridge;

#define MAX_RESOLVE_ATTEMPTS 4
  resolvatt = 0;
  do {
    resolvatt++;
    if (resolvatt > 4) {
      printk(KERN_INFO "%s: Too many attempts, failing.\n", __FUNCTION__);
      n_xbee_free_bridge(bridge);
      return -1;
    } else if (resolvatt > 1) {
      printk(KERN_INFO "%s: init failed, will try again...\n", __FUNCTION__);
      msleep(5000);
    }
    printk(KERN_INFO "%s: Attempting to init xbee, attempt %d/%d...\n", __FUNCTION__, resolvatt, MAX_RESOLVE_ATTEMPTS);
  } while (n_xbee_resolve_pending_dev(bridge) != 0);

  if (n_xbee_init_netdev(bridge) != 0) {
    printk(KERN_ALERT "%s: %s n_xbee_init_netdev indicated failure, aborting.\n", __FUNCTION__, bridge->tty_name);
    return -ENODEV;
  }

  return 0;
}

#ifdef N_XBEE_ARP_RESPONDER
int n_xbee_netdev_handle_arp(xbee_serial_bridge* bridge, const wpan_envelope_t* envelope) {
  struct arphdr* arph;
  struct ether_arp* arpeh;
  struct ether_arp* txarpeh;
  struct ether_header* eh;
  struct ether_header* txeh;
  struct ifreq ifr;
  unsigned char* txbuf;
  const int txlen = sizeof(struct ether_header) + sizeof(struct ether_arp);

  eh = (struct ether_header*)envelope->payload;
  arph = (struct arphdr*)(envelope->payload + N_XBEE_ETHHDR_LEN);
  printk(KERN_INFO "%s: handling arp len %d op proto 0x%08x hrd proto 0x%08x.\n", __FUNCTION__, envelope->length, ntohs(arph->ar_pro), ntohs(arph->ar_hrd));
  if (ntohs(arph->ar_op) != ARPOP_REQUEST) {
    printk(KERN_INFO "%s: unknown ar_op %u!\n", __FUNCTION__, htons(arph->ar_op));
    return 1;
  }

  if (htons(arph->ar_hrd) != 1) {
    printk(KERN_INFO "%s: unknown arp_hrd %u!\n", __FUNCTION__, htons(arph->ar_hrd));
    return 2;
  }

  if (htons(arph->ar_pro) != 0x0800) {
    printk(KERN_INFO "%s: unknown arp_pro %u!\n", __FUNCTION__, htons(arph->ar_pro));
    return 3;
  }

  if (arph->ar_hln != 6 || arph->ar_pln != 4) {
    printk(KERN_INFO "%s: unknown ar_hlen %u or ar_plen %u!\n", __FUNCTION__, arph->ar_hln, arph->ar_pln);
    return 4;
  }

  printk(KERN_INFO "%s: handling arp request.", __FUNCTION__);
  arpeh = (struct ether_arp*)arph;

  // compare target ip to our ip
  memset(&ifr, 0, sizeof(struct ifreq));
  ifr.ifr_addr.sa_family = AF_INET;
  ifr.ifr_ifindex = bridge->netdev_idx;
  strncpy(ifr.ifr_name, bridge->netdevName, IFNAMSIZ-1);
  if (ioctl(bridge->netdev_sock, SIOCGIFADDR, &ifr) < 0) {
    printk(KERN_ALERT "%s: unable to check IP of netdev, %d (%s)\n", __FUNCTION__, errno, strerror(errno));
    return 6;
  }

  // compare
  if (memcmp(&((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, &arpeh->arp_tpa, sizeof(arpeh->arp_tpa)) != 0) {
    return 7;
  }

  printk(KERN_ALERT "%s: sending arp response.", __FUNCTION__);
  // build arp packet
  // do we need the 4 byte header here?
  txbuf = malloc(txlen);
  txeh = (struct ether_header*)(txbuf + N_XBEE_PREAMBLE_LEN);
  txeh->ether_type = htons(ETHERTYPE_ARP);
  memcpy(txeh->ether_dhost, eh->ether_shost, 6);
  memcpy(txeh->ether_shost, (bridge->xbee_dev->wpan_dev.address.ieee.b) + 2, 6);

  txarpeh = (struct ether_arp*)(txbuf + N_XBEE_PREAMBLE_LEN + sizeof(struct ether_header));
  memcpy(txarpeh, arpeh, sizeof(struct arphdr));
  txarpeh->ea_hdr.ar_op = htons(ARPOP_REPLY);
  memcpy(txarpeh->arp_sha, (bridge->xbee_dev->wpan_dev.address.ieee.b) + 2, 6);
  memcpy(txarpeh->arp_spa, arpeh->arp_tpa, 4);
  memcpy(txarpeh->arp_tha, arpeh->arp_sha, 6);
  memcpy(txarpeh->arp_tpa, arpeh->arp_spa, 4);
  n_xbee_xmit_ether_packet(bridge, txbuf, txlen);
  free(txbuf);
  return 0;
}
#endif

int n_xbee_netdev_rx(const wpan_envelope_t* envelope, void* context) {
#ifdef N_XBEE_ARP_RESPONDER
  int res;
#endif
  struct xbee_remote_node* remnode;
  struct ether_header* mh;
  struct xbee_serial_bridge* bridge = n_xbee_serial_bridge;
  if (!bridge)
    return 0;
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: handling xbee packet of len %d\n", __FUNCTION__, envelope->length);
  hexdump((void*)envelope->payload, envelope->length);
#endif
  remnode = n_xbee_node_find_or_insert(&envelope->ieee_address);
  if (!bridge->netdevInitialized)
    return 0;

  // skip preamble
  mh = (struct ether_header*) (envelope->payload);
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: handling from mac %.2x:%.2x:%.2x:%.2x:%.2x:%.2x to mac %.2x:%.2x:%.2x:%.2x:%.2x:%.2x\n", __FUNCTION__, mh->ether_shost[0], mh->ether_shost[1], mh->ether_shost[2], mh->ether_shost[3], mh->ether_shost[4], mh->ether_shost[5], mh->ether_dhost[0], mh->ether_dhost[1], mh->ether_dhost[2], mh->ether_dhost[3], mh->ether_dhost[4], mh->ether_dhost[5]);
#endif

  uint16_t ether_type = ntohs(mh->ether_type);
#ifdef N_XBEE_ARP_RESPONDER
  if (ether_type == ETHERTYPE_ARP) {
    res = n_xbee_netdev_handle_arp(bridge, envelope);
#ifdef N_XBEE_ARP_RESPONDER_NO_PASSTHROUGH
    if (!res)
      return 0;
#endif
  }
#endif

  write(bridge->netdev, (void*)envelope->payload, envelope->length);
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: wrote packet of type %u len %d to tap.\n", __FUNCTION__, ether_type, envelope->length);
#endif
  return 0;
}

void n_xbee_xmit_ether_packet(struct xbee_serial_bridge* bridge, const void* buffer, int len) {
  struct ether_header* mh;
  int i, err, nbcast = 0;
  wpan_envelope_t envelope;
  struct xbee_remote_node* rnod;

#ifdef N_XBEE_VERBOSE
  if (len < 28) {
    printk(KERN_ALERT "%s: packet length is %d, less than min of 64, dropping.\n", __FUNCTION__, len);
    return;
  }
#endif

  // skip the preamble
  mh = (struct ether_header*) (buffer + N_XBEE_PREAMBLE_LEN);

#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: sending from mac %.2x:%.2x:%.2x:%.2x:%.2x:%.2x to mac %.2x:%.2x:%.2x:%.2x:%.2x:%.2x\n", __FUNCTION__, mh->ether_shost[0], mh->ether_shost[1], mh->ether_shost[2], mh->ether_shost[3], mh->ether_shost[4], mh->ether_shost[5], mh->ether_dhost[0], mh->ether_dhost[1], mh->ether_dhost[2], mh->ether_dhost[3], mh->ether_dhost[4], mh->ether_dhost[5]);
  hexdump((void*) buffer, len);
#endif

  // set initial values
  pthread_mutex_lock(&bridge->write_lock);
  memset(&envelope, 0, sizeof(envelope));
  envelope.dev = &bridge->xbee_dev->wpan_dev;
  envelope.profile_id = WPAN_PROFILE_DIGI;
  envelope.cluster_id = N_XBEE_CLUSTER_ID;
  envelope.dest_endpoint = envelope.source_endpoint = N_XBEE_ENDPOINT;
  envelope.network_address = WPAN_NET_ADDR_UNDEFINED;
  // check if broadcast addr
#ifdef N_XBEE_NO_MULTICAST
  for (i = 0; i < ETH_ALEN; i++) {
    if (mh->ether_dhost[i] == 0xFF)
      continue;
    nbcast = 1;
    break;
  }
#else
  // if LSB of first octet is set to 1 then multicast
  nbcast = !(mh->ether_dhost[0] & 1);
#endif

  // destination is broadcast
  if (!nbcast) {
    envelope.ieee_address = *WPAN_IEEE_ADDR_BROADCAST;
    envelope.options |= WPAN_ENVELOPE_BROADCAST_ADDR;
  }
  else {
    rnod = n_xbee_node_find_eth(&mh->ether_dhost, ETH_ALEN);
    if (!rnod) {
#ifdef N_XBEE_VERBOSE
      printk(KERN_INFO "%s: unable to transmit, can't find in lookup table.\n", __FUNCTION__);
#endif
      return;
    }
    memcpy(&envelope.ieee_address, &rnod->node_addr, 8);
  }
  envelope.payload = buffer;
  envelope.length = len;
  err = wpan_envelope_send(&envelope);
#ifdef N_XBEE_VERBOSE
  if (err != 0)
    printk(KERN_ALERT "%s: unable to transmit, error %d (%s).\n", __FUNCTION__,  err, strerror(err));
#endif
  pthread_mutex_unlock(&bridge->write_lock);
}

void* n_xbee_read_loop(void* ctx) {
  struct xbee_serial_bridge* bridge = n_xbee_serial_bridge;
  if (!bridge)
    return;

  int err;
  uint32_t mstime;
  uint32_t discover = xbee_millisecond_timer();

  while (1) {
    mstime = xbee_millisecond_timer();
    // tick the xbee
    err = n_xbee_handle_runtime_frames(bridge);

    if (mstime - discover > N_XBEE_DISCOVER_INTERVAL) {
      xbee_disc_discover_nodes(bridge->xbee_dev, NULL);
      discover = mstime;
    }
    pthread_yield();
    if (!err)
      usleep(10);
  }

  return NULL;
}

void n_xbee_main_loop(void) {
  int nread;
  // set this to proper mtu later...
  const int mtu = 1500;
  char recv_buffer[mtu + 5];
#ifdef N_XBEE_USE_SELECT
  fd_set readset, activeset;
#endif

  struct xbee_serial_bridge* bridge = n_xbee_serial_bridge;
  if (!bridge)
    return;

  pthread_t xbee_read_thread;
  if (pthread_create(&xbee_read_thread, NULL, n_xbee_read_loop, NULL)) {
    printk("%s: Error creating read thread, exiting.\n", __FUNCTION__);
    return;
  }

#ifdef N_XBEE_USE_SELECT
  FD_ZERO(&activeset);
  FD_SET(bridge->netdev, &activeset);
#endif
  while(1) {
#ifdef N_XBEE_USE_SELECT
    // check for packets from tap
    readset = activeset;
    // first argument is maximum fd we are listening to + 1
    if (select(bridge->netdev + 1, &readset, NULL, NULL, NULL) < 0) {
      if (errno != -EINTR) {
        printk(KERN_ALERT "%s: select() errored.\n", __FUNCTION__);
        return;
      }
    }

    if (FD_ISSET(bridge->netdev, &readset)) {
#endif
      nread = read(bridge->netdev, recv_buffer, mtu + 5);
      if (nread < 0) {
        printk(KERN_ALERT "%s: error reading from recv_buf, %d (%s)...\n", __FUNCTION__, nread, strerror(nread));
        return;
      }
      // not sure what the 46 byte packets are
#ifdef N_XBEE_VERBOSE
      printk(KERN_INFO "%s: read %d bytes from netdev.\n", __FUNCTION__, nread);
#endif

      n_xbee_xmit_ether_packet(bridge, recv_buffer, nread);
#ifdef N_XBEE_USE_SELECT
    }
#endif
    pthread_yield();
  }
}

static int n_xbee_init(void) {
  printk(KERN_INFO "%s: xbee-net initializing...\n", __FUNCTION__);
  n_xbee_node_table = NULL;
  return 0;
}

static void n_xbee_cleanup(void) {
  printk(KERN_INFO "%s: xbee-net shutting down...\n", __FUNCTION__);
  // n_xbee_free_all_bridges();
  if (n_xbee_serial_bridge)
    n_xbee_free_bridge(n_xbee_serial_bridge);
  n_xbee_free_remote_nodetable();
}

/*
   Parse the command-line arguments, looking for "/dev/" to determine the
   serial port to use, and a bare number (assumed to be the baud rate).

   @param[in]	argc		argument count
   @param[in]	argv		array of \a argc arguments
   @param[out]	serial	serial port settings
   */
int parse_serial_arguments(int argc, const char *argv[], xbee_serial_t *serial) {
  int i;
  uint32_t baud;

  memset( serial, 0, sizeof *serial);

  // default baud rate
  serial->baudrate = 115200;

  for (i = 1; i < argc; ++i)
  {
    if (strncmp( argv[i], "/dev", 4) == 0)
    {
      strncpy( serial->device, argv[i], (sizeof serial->device) - 1);
      serial->device[(sizeof serial->device) - 1] = '\0';
    }
    if ( (baud = (uint32_t) strtoul( argv[i], NULL, 0)) > 0)
    {
      serial->baudrate = baud;
    }
  }

  if (*serial->device == '\0') {
    printk(KERN_ALERT "%s: invalid command line args.\n", __FUNCTION__);
    printk(KERN_ALERT "usage: /dev/ttyUSB0 115200\n");
    return -1;
  }

  printk(KERN_INFO "%s: using device %s baud %d\n", __FUNCTION__, serial->device, serial->baudrate);
  return 0;
}

int main(int argc, const char** argv) {
  int res;

  if ((res = n_xbee_init()) != 0) {
    printk(KERN_ALERT "%s: init failed, exiting...\n", __FUNCTION__);
    return res;
  }

  xbee_serial_t serial;
  if ((res = parse_serial_arguments(argc, argv, &serial)) != 0)
    return res;

  if ((res = n_xbee_serial_open(&serial)) != 0)
    return res;

  n_xbee_main_loop();
  n_xbee_cleanup();
  return res;
}
