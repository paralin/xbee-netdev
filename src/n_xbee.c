#include "n_xbee.h"
#include <xbee/device.h>
#include <xbee/atcmd.h>
#include <xbee/atmode.h>
#include <xbee/wpan.h>
#include <xbee/discovery.h>
#include <linux/kthread.h>

// forward declarations
extern struct tty_ldisc_ops n_xbee_ldisc;
static void n_xbee_flush_buffer(struct tty_struct* tty);

// Module init stuff
MODULE_LICENSE("GPL");
MODULE_AUTHOR("paralin");
MODULE_DESCRIPTION("IP over Xbee.");

#define ENSURE_MODULE \
  if (!try_module_get(THIS_MODULE)) \
    return -ENODEV;

#define ENSURE_MODULE_NORET \
  if (!try_module_get(THIS_MODULE)) \
    return;

#define ENSURE_MODULE_RET(RET) \
  if (!try_module_get(THIS_MODULE)) \
    return RET;

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

int n_xbee_netdev_rx(const wpan_envelope_t FAR *envelope, void FAR* context);
const wpan_cluster_table_entry_t xbee_data_clusters[] = {
  { N_XBEE_CLUSTER_ID, n_xbee_netdev_rx, NULL, WPAN_CLUST_FLAG_INOUT | WPAN_CLUST_FLAG_NOT_ZCL },
  // if we don't set ATAO to 0...
  XBEE_DISC_DIGI_DATA_CLUSTER_ENTRY,
  WPAN_CLUST_ENTRY_LIST_END
};

const wpan_endpoint_table_entry_t xbee_endpoints[] = {
  // ZDO_ENDPOINT(zdo_ep_state),

  { N_XBEE_ENDPOINT,
    WPAN_PROFILE_DIGI,
    // ignore general endpoint, instead listen to cluster
    NULL,
    NULL,
    0x0,
    0x0,
    xbee_data_clusters
  },
  { WPAN_ENDPOINT_END_OF_LIST }
};

/* == Buffer stuff == */
struct xbee_data_buffer* n_xbee_alloc_buffer(int size) {
  struct xbee_data_buffer* buf = kmalloc(sizeof(xbee_data_buffer), GFP_KERNEL);
  buf->size = size;
  buf->buffer = kmalloc(sizeof(unsigned char) * size, GFP_KERNEL);
  buf->pos = 0;
  spin_lock_init(&buf->lock);
  return buf;
}

void n_xbee_free_buffer(struct xbee_data_buffer* buf) {
  if (buf->buffer)
    kfree(buf->buffer);
  kfree(buf);
}

/* = Serial Bridge Stuff = */
void n_xbee_init_bridge_ll(void) {
  n_xbee_serial_bridges = NULL;
  spin_lock_init(&n_xbee_serial_bridges_l);
}

// Frees the xbee_dev_t
void n_xbee_free_xbee_dev(xbee_dev_t* dev) {
  kfree(dev);
}

// Just free, don't remove properly.
// Also frees all netdevs etc.
void n_xbee_free_netdev(xbee_serial_bridge* n);
void n_xbee_free_bridge(xbee_serial_bridge* n) {
  if (!n) return;
  if (n->tty)
    n->tty->disc_data = NULL;
  if (n->netdevInitialized)
    n_xbee_free_netdev(n);
  if (n->name)
    kfree(n->name);
  if (n->xbee_dev)
    n_xbee_free_xbee_dev(n->xbee_dev);
  if (n->netdevName)
    kfree(n->netdevName);
  if (n->recvbuf)
    n_xbee_free_buffer(n->recvbuf);
  // DO NOT FREE pend_dev
  if (n->pend_dev) {
    n->pend_dev->cancel = 1;
    n->pend_dev->noFreeBridge = 1;
  }
  if (n->tick_state) {
    n->tick_state->should_exit = 1;
  }
  kfree(n);
}

// Insert a bridge into the linked list
// according to Linus Torvalds this code has bad taste
// should fix it eventually ;)
void n_xbee_insert_bridge(xbee_serial_bridge* n) {
  unsigned long flags;
  xbee_serial_bridge* nc;
  if (!n_xbee_serial_bridges)
    n_xbee_serial_bridges = n;
  else {
    spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
    nc = n_xbee_serial_bridges;
    while (nc->next)
      nc = nc->next;
    nc->next = n;
    spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  }
}

// Remove a bridge from the list of bridges. Does not free.
void n_xbee_remove_bridge(xbee_serial_bridge* ntd) {
  unsigned long flags;
  xbee_serial_bridge* n;
  xbee_serial_bridge* n_last;
  spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
  n = n_xbee_serial_bridges;
  n_last = NULL;
  while (n) {
    if (n == ntd) {
      if (!n_last)
        n_xbee_serial_bridges = n->next;
      else
        n_last->next = n->next;
      spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
      return;
    }
    n_last = n;
    n = n->next;
  }
  spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  printk(KERN_ALERT "%s: BUG: couldn't find bridge %p in list.\n", __FUNCTION__, ntd);
}

// Free all bridges
void n_xbee_free_all_bridges(void) {
  xbee_serial_bridge* n;
  unsigned long flags;
  spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
  n = n_xbee_serial_bridges;
  n_xbee_serial_bridges = NULL;
  spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  while (n) {
    xbee_serial_bridge* ni = n;
    n = n->next;
    n_xbee_free_bridge(ni);
  }
}

void n_xbee_free_remote_nodetable(void) {
  struct xbee_remote_node* nodn;
  struct xbee_remote_node* nod = n_xbee_node_table;
  n_xbee_node_table = NULL;
  while (nod) {
    nodn = nod->next;
    // do something to free
    kfree(nod);
    nod = nodn;
  }
}

// Find by name
xbee_serial_bridge* n_xbee_find_bridge_byname(const char* name) {
  unsigned long flags;
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
  while (n) {
    if (strcmp(n->name, name) == 0) {
      spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
      return n;
    }
    n = n->next;
  }
  spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  return NULL;
}

// Find by tty
xbee_serial_bridge* n_xbee_find_bridge_bytty(struct tty_struct* tty) {
#ifdef N_XBEE_NO_USE_DISC_DATA
  unsigned long flags;
  xbee_serial_bridge* n;
  spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
  n = n_xbee_serial_bridges;
  while (n) {
    if (n->tty == tty) {
      spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
      return n;
    }
    n = n->next;
  }
  spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  return NULL;
#else
  return (xbee_serial_bridge*) tty->disc_data;
#endif
}

// Find by netdev
xbee_serial_bridge* n_xbee_find_bridge_byndev(struct net_device* ndev) {
  struct xbee_netdev_priv* priv = netdev_priv(ndev);
  if (!priv)
    return NULL;
  return priv->bridge;
}

// Find by wpan_dev
xbee_serial_bridge* n_xbee_find_bridge_bywpan(struct wpan_dev_t* wpn) {
  unsigned long flags;
  xbee_serial_bridge* n;
  if (wpn->extra_ptr)
    return (xbee_serial_bridge*) wpn->extra_ptr;

  printk(KERN_ALERT "%s: cache miss on extra_ptr, bugfix.\n", __FUNCTION__);
  n = n_xbee_serial_bridges;
  spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
  while (n) {
    if (&n->xbee_dev->wpan_dev == wpn) {
      spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
      return n;
    }
    n = n->next;
  }
  spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  return NULL;
}

// Find by xbee
xbee_serial_bridge* n_xbee_find_bridge_byxbee(struct xbee_dev_t* xbee) {
  unsigned long flags;
  xbee_serial_bridge* n = n_xbee_serial_bridges;
  spin_lock_irqsave(&n_xbee_serial_bridges_l, flags);
  while (n) {
    if (n->xbee_dev == xbee) {
      spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
      return n;
    }
    n = n->next;
  }
  spin_unlock_irqrestore(&n_xbee_serial_bridges_l, flags);
  return NULL;
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
  nnod = kmalloc(sizeof(xbee_remote_node), GFP_KERNEL);
  memcpy(&nnod->node_addr, id, sizeof(addr64));
  nnod->next = NULL;
  if (pnod)
    pnod->next = nnod;
  else
    n_xbee_node_table = nnod;
  return 0;
}

// should be ETH_ALEN
xbee_remote_node* n_xbee_node_find_eth(const unsigned char* addr, int len) {
  struct xbee_remote_node* nod = n_xbee_node_table;
  if (len > 8)
    len = 8;
  while (nod) {
    if (memcmp((8 - len) + (&nod->node_addr.l), addr, len) == 0)
      return nod;
    nod = nod->next;
  }
  return 0;
}

/* = XBEE Controls */

// Prepare to transmit by waking up the device, etc
void n_xbee_prepare_xmit(struct tty_struct* tty) {
  tty->flags |= (1 << TTY_DO_WRITE_WAKEUP);
}

// Some helper macros
#define N_XBEE_CHECK_CANCEL \
    if (pend_dev && pend_dev->cancel) { \
      printk(KERN_ALERT "%s: Pending device canceled during AT setup\n", __FUNCTION__); \
      return -ETIMEDOUT; \
    }
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
int n_xbee_check_tty(xbee_serial_bridge* bridge, xbee_pending_dev* pend_dev) {
  int err, mode, iterations, bytesread;
  char respbuf[CHECK_RESP_BUF_SIZE];
  xbee_dev_t* xbee = bridge->xbee_dev;
  n_xbee_flush_buffer(bridge->tty);
  printk(KERN_INFO "Putting board into AT mode to check values...\n");
  if ((err = xbee_atmode_enter(xbee)) != 0) {
    printk(KERN_ALERT "Unable to put board into AT mode, error: %d\n", err);
    return err;
  }
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_CANCEL;
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

    // sleep 5 milliseconds
    msleep(1);
    iterations ++;
  }

  n_xbee_flush_buffer(bridge->tty);

  printk(KERN_INFO "%s: Setting API mode 1...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AP 1")) != 0) {
    printk(KERN_ALERT "%s: Unable to set API mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_CANCEL;
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
  N_XBEE_CHECK_CANCEL;
  n_xbee_flush_buffer(bridge->tty);

  printk(KERN_INFO "%s: Verifying API mode...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AP")) != 0) {
    printk(KERN_ALERT "%s: Unable to request API mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_CANCEL;
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
    N_XBEE_CHECK_CANCEL;
    N_XBEE_CHECK_ITERATIONS(iterations, 200);
    mode = xbee_atmode_read_response(xbee, respbuf, CHECK_RESP_BUF_SIZE, &bytesread);
    if (mode == -EAGAIN) {
      msleep(5);
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
  N_XBEE_CHECK_CANCEL;
  n_xbee_flush_buffer(bridge->tty);

  printk(KERN_INFO "%s: Verifying AO mode...\n", __FUNCTION__);
  if ((err = xbee_atmode_send_request(xbee, "AO")) != 0) {
    printk(KERN_ALERT "%s: Unable to request AO mode, error: %d\n", __FUNCTION__, err);
    return err;
  }

  bytesread = 0;
  iterations = 0;
  while (1) {
    N_XBEE_CHECK_CANCEL;
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
    N_XBEE_CHECK_CANCEL;
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
  msleep(1000);
  N_XBEE_CHECK_CANCEL;
  n_xbee_flush_buffer(bridge->tty);

  if ((err = xbee_cmd_init_device(xbee)) != 0) {
    printk(KERN_ALERT "%s: Error initing device: %d\n", __FUNCTION__, err);
    return err;
  }

  // Wait for the cmd_query_device to finish.
  iterations = 0;
  do {
    N_XBEE_CHECK_CANCEL;
    N_XBEE_CHECK_ITERATIONS(iterations, 400);
    msleep(5);
    iterations++;
    xbee_dev_tick(xbee);
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

int n_xbee_serial_tick_thread(void* data);
void n_xbee_init_tickthread(xbee_serial_bridge* bridge) {
  xbee_tick_threadstate* st;
  if (!bridge || bridge->tick_state)
    return;
  st = bridge->tick_state = (xbee_tick_threadstate*) kmalloc(sizeof(xbee_tick_threadstate), GFP_KERNEL);
  if (!st)
    return;
  spin_lock_init(&st->tick_lock);
  st->should_exit = 0;
  st->bridge = bridge;
  kthread_run(n_xbee_serial_tick_thread, (void*)st, "xbee_tickthread");
}

/* = XBEE NetDev = */
static int n_xbee_netdev_open(struct net_device* dev) {
  xbee_serial_bridge* bridge;
  ENSURE_MODULE_RET(-EIO);
  bridge = n_xbee_find_bridge_byndev(dev);
  if (!bridge)
    return -ENODEV;
  printk(KERN_INFO "%s: Interface %s (%s) going up...\n", __FUNCTION__, dev->name, bridge->name);
  bridge->netdev_up = 1;
  netif_start_queue(dev);
  return 0;
}

static int n_xbee_netdev_release(struct net_device* dev) {
  xbee_serial_bridge* bridge;
  ENSURE_MODULE_RET(-EIO);
  bridge = n_xbee_find_bridge_byndev(dev);
  if (!bridge)
    return -ENODEV;
  printk(KERN_INFO "%s: Interface %s (%s) going down...\n", __FUNCTION__, dev->name, bridge->name);
  bridge->netdev_up = 0;
  netif_stop_queue(dev);
  return 0;
}

static struct net_device_stats* n_xbee_netdev_stats(struct net_device* dev) {
  struct xbee_netdev_priv* priv;
  ENSURE_MODULE_RET(NULL);
  if (!dev)
    return NULL;
  priv = netdev_priv(dev);
  if (!priv)
    return NULL;
  return &priv->stats;
}

static int n_xbee_netdev_init_late(struct net_device* dev) {
  // Initialize the fragmentation system
  return 0;
}

static int n_xbee_netdev_change_mtu(struct net_device* dev, int new_mtu) {
  struct xbee_netdev_priv* priv;
  if (new_mtu < N_XBEE_DATA_MTU || !dev || !new_mtu || !dev->name)
    return -EINVAL;
  if (new_mtu == dev->mtu)
    return 0;
  priv = netdev_priv(dev);
  if (!priv || !priv->bridge)
    return -EINVAL;

  printk(KERN_INFO "%s: Changing MTU of %s to %u...\n", __FUNCTION__, dev->name, new_mtu);

  // Stop the net queue
  netif_stop_queue(dev);
  
  // TODO: re-introduce locking here

  // Reset the fragmentation system
  // TODO

  // Set the MTU on the net device
  dev->mtu = new_mtu;

  // Initialize the fragmentation system
  // TODO

  // Start the net queue
  netif_wake_queue(dev);
  return 0;
}

static int n_xbee_netdev_ioctl(struct net_device* dev, struct ifreq* rq, int cmd) {
  // Default is not implemented
  return -ENOSYS;
}

static int n_xbee_netdev_xmit(struct sk_buff* skb, struct net_device* dev) {
  struct xbee_serial_bridge* bridge;
  struct ethhdr* mh;
  int i, err, nbcast = 0;
  wpan_envelope_t envelope;
  struct xbee_remote_node* rnod;
  ENSURE_MODULE_RET(0);
  bridge = n_xbee_find_bridge_byndev(dev);
  if (!bridge)
    return NETDEV_TX_OK;
  memset(&envelope, 0, sizeof(envelope));
  // try to find the equiv ether addr
  mh = eth_hdr(skb);
  // unsigned char mh->h_dest[ETH_ALEN]
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: request to transmit to %pM.\n", __FUNCTION__, mh->h_dest);
#endif
  // set initial values
  envelope.dev = &bridge->xbee_dev->wpan_dev;
  envelope.profile_id = WPAN_PROFILE_DIGI;
  envelope.cluster_id = N_XBEE_CLUSTER_ID;
  envelope.dest_endpoint = envelope.source_endpoint = N_XBEE_ENDPOINT;
  envelope.network_address = WPAN_NET_ADDR_UNDEFINED;
  // check if broadcast addr
  for (i = 0; i < ETH_ALEN; i++) {
    if (mh->h_dest[i] == 0xFF)
      continue;
    nbcast = 1;
    break;
  }
  // destination is broadcast
  if (!nbcast) {
    envelope.ieee_address = *WPAN_IEEE_ADDR_BROADCAST;
    envelope.options |= WPAN_ENVELOPE_BROADCAST_ADDR;
  }
  else {
    rnod = n_xbee_node_find_eth(mh->h_dest, ETH_ALEN);
    if (!rnod) {
#ifdef N_XBEE_VERBOSE
      printk(KERN_INFO "%s: unable to transmit to %pM, can't find in lookup table.\n", __FUNCTION__, mh->h_dest);
#endif
      return NETDEV_TX_OK;
    }
    memcpy(&envelope.ieee_address, &rnod->node_addr, 8);
  }
  envelope.payload = skb->data;
  envelope.length = skb->len;
  if ((err=wpan_envelope_send(&envelope)) != 0) {
#ifdef N_XBEE_VERBOSE
      printk(KERN_ALERT "%s: unable to transmit to %pM, error %d.\n", __FUNCTION__, mh->h_dest, err);
#endif
      return NETDEV_TX_OK;
  }
  return NETDEV_TX_OK;
}

static const struct net_device_ops n_xbee_netdev_ops = {
  .ndo_init = n_xbee_netdev_init_late,
  .ndo_open = n_xbee_netdev_open,
  .ndo_stop = n_xbee_netdev_release,
  .ndo_change_mtu = n_xbee_netdev_change_mtu,
  .ndo_do_ioctl = n_xbee_netdev_ioctl,
  .ndo_get_stats = n_xbee_netdev_stats,
  .ndo_start_xmit = n_xbee_netdev_xmit,
};

int n_xbee_netdev_rx(const wpan_envelope_t FAR *envelope, void FAR* context) {
  struct xbee_serial_bridge* bridge;
  struct xbee_remote_node* remnode;
  ENSURE_MODULE_RET(0);
  bridge = n_xbee_find_bridge_bywpan(envelope->dev);
  if (!bridge)
    return 0;
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: %s handling packet of length %d\n", __FUNCTION__, bridge->name, envelope->length);
#endif
  remnode = n_xbee_node_find_or_insert(&envelope->ieee_address);
  if (!bridge->netdev_up)
    return 0;
  // TODO: implement reception here
  return 0;
}

static void n_xbee_netdev_init_early(struct net_device* dev) {
  struct xbee_netdev_priv* priv = netdev_priv(dev);

  ether_setup(dev);
  dev->netdev_ops = &n_xbee_netdev_ops;
  // dev->flags |= IFF_NOARP;
#ifdef XBEE_USE_ACTUAL_MTU
  dev->mtu    = N_XBEE_DATA_MTU;
#else
  dev->mtu = 1500;
#endif
  // set priv flags and features and mtu
  memset(priv, 0, sizeof(*priv));
}

int n_xbee_init_netdev(xbee_serial_bridge* bridge) {
  struct net_device* ndev;
  struct xbee_netdev_priv* priv;
  int err;
  if (!bridge || !bridge->name) return -1;
  if (bridge->netdevInitialized) {
    printk(KERN_ALERT "%s: Net bridge %s already inited!\n", __FUNCTION__, bridge->netdevName);
    return -1;
  }

  printk(KERN_INFO "%s: Initializing net bridge %s as %s...\n", __FUNCTION__, bridge->name, bridge->netdevName);
  ndev = bridge->netdev = alloc_netdev(sizeof(xbee_netdev_priv), bridge->netdevName, NET_NAME_UNKNOWN, n_xbee_netdev_init_early);
  if (!ndev) {
    printk(KERN_ALERT "%s: Failed to init netdev %s...\n", __FUNCTION__, bridge->netdevName);
    return -1;
  }

  bridge->netdevInitialized = 1;
  priv = netdev_priv(ndev);
  priv->bridge = bridge;

  // set the mac address
  if (ndev->dev_addr) {
#ifdef N_XBEE_VERBOSE
    printk(KERN_INFO "%s: re-allocating dev_addr\n", __FUNCTION__);
#endif
    kfree(ndev->dev_addr);
  }

  ndev->addr_len = ETH_ALEN;
  ndev->dev_addr = kmalloc(ndev->addr_len, GFP_KERNEL);
  ndev->addr_assign_type = NET_ADDR_PERM;
  // skip the first 2 bytes
  memcpy(ndev->dev_addr, bridge->xbee_dev->wpan_dev.address.ieee.b + 2, ndev->addr_len);

  if ((err = register_netdev(ndev)) != 0) {
    printk(KERN_ALERT "%s: Failed to register netdev %s with error %i...", __FUNCTION__, bridge->netdevName, err);
    bridge->netdevInitialized = 0;
    free_netdev(ndev);
    bridge->netdev = NULL;
    return err;
  }

  return 0;
}

void n_xbee_free_netdev(xbee_serial_bridge* n) {
  struct xbee_netdev_priv* priv;
  if (!n) return;
  if (n->netdevInitialized)
    unregister_netdev(n->netdev);
  n->netdevInitialized = 0;
  if (!n->netdev) return;
  printk(KERN_INFO "%s: Shutting down net bridge %s...\n", __FUNCTION__, n->netdevName);
  priv = netdev_priv(n->netdev);
  priv->bridge = NULL;
  free_netdev(n->netdev);
  n->netdev = NULL;
}

// ticks the xbee
// since we register all the callbacks in the xbee code
// we can just call tick.
inline void n_xbee_handle_runtime_frames(xbee_serial_bridge* bridge) {
  unsigned long flags;
  spin_lock_irqsave(&bridge->tick_state->tick_lock, flags);
  xbee_dev_tick(bridge->xbee_dev);
  spin_unlock_irqrestore(&bridge->tick_state->tick_lock, flags);
}

/* = XBEE Detection and Setup  =
 *
 * This driver relies on user-space code to
 * prompt it to attach/scan for an xbee on a serial
 * interface. This is done through a user-space daemon
 * which sets a line discipline on the serial line and then
 * holds it open. When a new serial line opens up this driver
 * is called, which results in a new network device being created
 * to handle the serial line.
 */
// Called when the userspace closes the tty.
static void n_xbee_serial_close(struct tty_struct* tty) {
  xbee_serial_bridge* bridge;
  // Make sure our module is still loaded
  ENSURE_MODULE_NORET;

  printk(KERN_INFO "%s: TTY %s detached.\n", __FUNCTION__, tty->name);
  bridge = n_xbee_find_bridge_byname((const char*) tty->name);
  if (!bridge) {
    printk(KERN_ALERT "%s: TTY %s not found/previously initialized...\n", __FUNCTION__, tty->name);
    return;
  }
  n_xbee_remove_bridge(bridge);
  n_xbee_free_bridge(bridge);
}

// if return anything but zero, call n_xbee_remove_bridge and n_xbee_free_bridge if NOT noFreeBridge
int n_xbee_resolve_pending_dev(xbee_pending_dev* dev) {
  if (dev->cancel)
    return -1;

  if (n_xbee_check_tty(dev->bridge, dev) != 0) {
    printk(KERN_ALERT "%s: Couldn't contact xbee on %s, make sure it's a valid xbee and the baud is correct.\n", __FUNCTION__, dev->bridge->tty->name);
    return -ENODEV;
  }

  if (dev->cancel)
    return -1;

  if (n_xbee_init_netdev(dev->bridge) != 0) {
    printk(KERN_ALERT "%s: %s n_xbee_init_netdev indicated failure, aborting.\n", __FUNCTION__, dev->bridge->tty->name);
    return -ENODEV;
  }

  if (dev->cancel)
    return -1;

  n_xbee_init_tickthread(dev->bridge);

  dev->bridge->pend_dev = NULL;
  return 0;
}

int n_xbee_resolve_pending_dev_thread(void* data) {
  xbee_pending_dev* dev = (xbee_pending_dev*) data;
  if (n_xbee_resolve_pending_dev(dev) != 0) {
    if (!dev->noFreeBridge) {
      n_xbee_remove_bridge(dev->bridge);
      n_xbee_free_bridge(dev->bridge);
    }
  }
  kfree(dev);
  return 0;
}

void n_xbee_node_discovered(xbee_dev_t* xbee, const xbee_node_id_t *rec) {
  xbee_serial_bridge* bridge;
  char addr64_buf[ADDR64_STRING_LENGTH];
  ENSURE_MODULE_NORET;
  bridge = n_xbee_find_bridge_byxbee(xbee);
  if (!bridge)
    return;
  printk(KERN_INFO "%s: %s discovered remote node %s.\n", __FUNCTION__, bridge->name, addr64_format(addr64_buf, &rec->ieee_addr_be));
  n_xbee_node_find_or_insert(&rec->ieee_addr_be);
}

// The receive data function will call tick on its own
// However, there might be some other xbee code internals
// that would want to query the xbee on a timed basis.
int n_xbee_serial_tick_thread(void* data) {
  unsigned long flags;
  int ourId, iterSinceDiscover = 0;
  xbee_tick_threadstate* tstate = (xbee_tick_threadstate*) data;
  if (!data || !tstate || !tstate->bridge || tstate->should_exit || !tstate->bridge->name)
    return 0;
  ourId = ++xbee_tick_thread_counter;
  printk(KERN_INFO "%s: starting tick thread [%d] for %s\n", __FUNCTION__, ourId, tstate->bridge->name);
  while (!tstate->should_exit) {
    spin_lock_irqsave(&tstate->tick_lock, flags);
    // do work
    xbee_dev_tick(tstate->bridge->xbee_dev);
    spin_unlock_irqrestore(&tstate->tick_lock, flags);
    if (iterSinceDiscover > (int)(N_XBEE_DISCOVER_INTERVAL / N_XBEE_TICK_INTERVAL)) {
      iterSinceDiscover = 0;
      xbee_disc_discover_nodes(tstate->bridge->xbee_dev, NULL);
    }
    msleep(N_XBEE_TICK_INTERVAL);
    iterSinceDiscover++;
  }
  printk(KERN_INFO "%s: exiting tick thread [%d]\n", __FUNCTION__, ourId);
  kfree(data);
  return 0;
}


/*
 * Called when the user-space daemon attaches to a
 * serial line. We should allocate the network device
 * in this function.
 *
 * We actually need to check if there is a valid xbee on the
 * other end, and if not, bail out with an error.
 */
static int n_xbee_serial_open(struct tty_struct* tty) {
  xbee_serial_bridge* bridge;
  int i;
  int nlen;
  int ndevnlen;
  char* rttyname;

  // Make sure our module is still loaded
  ENSURE_MODULE;

  // Find the existing allocated netdev for this (shouldn't happen)
  // .. or make a new one.
  printk(KERN_INFO "%s: TTY %s attached.\n", __FUNCTION__, tty->name);
  bridge = n_xbee_find_bridge_byname((const char*) tty->name);
  if (bridge) {
    printk(KERN_ALERT "%s: TTY %s was previously attached, shutting it down first...\n", __FUNCTION__, tty->name);
    n_xbee_remove_bridge(bridge);
    n_xbee_free_bridge(bridge);
  }

  // remove the tty
  rttyname = strncmp("tty", tty->name, 3) == 0 ? (tty->name + 3) : tty->name;
  nlen = strlen(rttyname);

  bridge = (xbee_serial_bridge*)kmalloc(sizeof(xbee_serial_bridge), GFP_KERNEL);
  spin_lock_init(&bridge->write_lock);
  bridge->tty = tty;
  bridge->name = (char*)kmalloc(sizeof(char) * (strlen(tty->name) + 1), GFP_KERNEL);
  bridge->name[strlen(tty->name)] = '\0';
  for (i = 0; i < strlen(tty->name); i++)
    bridge->name[i] = tty->name[i];
  bridge->next = 0;
  bridge->netdevInitialized = 0;
  bridge->netdev = 0;
  bridge->recvbuf = n_xbee_alloc_buffer(N_XBEE_BUFFER_SIZE);
  bridge->tty->receive_room = bridge->recvbuf->size;

  // we have our own xbee device init here, the other doesn't work
  bridge->xbee_dev = (xbee_dev_t*) kmalloc(sizeof(xbee_dev_t), GFP_KERNEL);
  memset(bridge->xbee_dev, 0, sizeof(xbee_dev_t));
  bridge->xbee_dev->serport.baudrate = tty_get_baud_rate(tty);
  bridge->xbee_dev->serport.tty = tty;
  bridge->xbee_dev->serport.ldisc_ops = &n_xbee_ldisc;
  bridge->xbee_dev->guard_time = 1000;
  bridge->xbee_dev->escape_char = '+';
  bridge->xbee_dev->idle_timeout = 100;
  // Don't use flow control TODO
  bridge->xbee_dev->flags &= ~XBEE_DEV_FLAG_USE_FLOWCONTROL;
  bridge->xbee_dev->wpan_dev.extra_ptr = bridge;

  ndevnlen = strlen(XBEE_NETDEV_PREFIX) + nlen;
  bridge->netdevName = (char*)kmalloc(sizeof(char) * (ndevnlen + 1), GFP_KERNEL);
  bridge->netdevName[ndevnlen] = '\0';
  strcpy(bridge->netdevName, XBEE_NETDEV_PREFIX);
  strcpy(bridge->netdevName + strlen(XBEE_NETDEV_PREFIX), rttyname);

  // verify the xbee exists and load its information
  // NOTE: moved, we cannot do this here. let's init the netdev elsewhere too.
  // instead, let's register a pending dev
  bridge->pend_dev = kmalloc(sizeof(xbee_pending_dev), GFP_KERNEL);
  bridge->pend_dev->bridge = bridge;
  bridge->pend_dev->cancel = 0;
  bridge->pend_dev->noFreeBridge = 0;
  bridge->tick_state = 0;

#ifndef N_XBEE_NO_USE_DISC_DATA
  tty->disc_data = bridge;
#endif
  n_xbee_insert_bridge(bridge);
  kthread_run(n_xbee_resolve_pending_dev_thread, (void*)bridge->pend_dev, "xbee_penddev");
  return 0;
}

static ssize_t n_xbee_chars_in_buffer(struct tty_struct* tty) {
  struct xbee_serial_bridge* bridge;
  ENSURE_MODULE;
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return -ENODEV;
  return bridge->recvbuf->pos;
}

static void n_xbee_flush_buffer(struct tty_struct* tty) {
  unsigned long flags;
  struct xbee_serial_bridge* bridge;
  ENSURE_MODULE_NORET;

  // Get the pointer to the bridge
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge || !bridge->recvbuf || !bridge->recvbuf->pos)
    return;
  // Acquire buf lock
  printk(KERN_INFO "%s: %s flushing buffer by kernel request.\n", __FUNCTION__, tty->name);
  spin_lock_irqsave(&bridge->recvbuf->lock, flags);
  bridge->recvbuf->pos = 0;
  spin_unlock_irqrestore(&bridge->recvbuf->lock, flags);
}

// Userspace requests a read from a tty
static ssize_t n_xbee_read(struct tty_struct* tty, struct file* file, unsigned char __user *buf, size_t nr) {
  unsigned long flags;
  struct xbee_serial_bridge* bridge;
  int nleft;
  int i;
  int ntread;
  int cpres;

  ENSURE_MODULE_RET(-1);
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return -ENODEV;

  if (!bridge->recvbuf->pos)
    return 0;
    // return -EAGAIN;

  // acquire read lock
  spin_lock_irqsave(&bridge->recvbuf->lock, flags);

  // read as much as we can
  ntread = bridge->recvbuf->pos;
  if (nr < ntread)
    ntread = nr;

  // Potential optimization: don't always shift back here.
  // copy out the data

  // if file == NULL we are in kernel space memory
  if (!file)
    memcpy(buf, bridge->recvbuf->buffer, ntread);
  else
    if ((cpres = copy_to_user(buf, bridge->recvbuf->buffer, ntread)) != 0)
      printk(KERN_ALERT "%s: copy_to_user returned %d.\n", __FUNCTION__, cpres);

  // nleft = amount of bytes left in the buffer
  nleft = bridge->recvbuf->pos - ntread;
  // printk(KERN_INFO "%s: there will be %d left in buff after read of %d\n", __FUNCTION__, nleft, ntread);
  if (nleft <= 0)
    bridge->recvbuf->pos = 0;
  else {
    //index of first byte we need to store is nread.
    for (i = 0; i < nleft; i++)
      bridge->recvbuf->buffer[i] = bridge->recvbuf->buffer[i + ntread];
    bridge->recvbuf->pos = nleft;
  }

  spin_unlock_irqrestore(&bridge->recvbuf->lock, flags);
  return ntread;
}

static ssize_t n_xbee_write(struct tty_struct* tty, struct file* file, const unsigned char* buf, size_t nr) {
  unsigned long flags;
  struct xbee_serial_bridge* bridge;
  int result;

#if defined(N_XBEE_VERBOSE) && defined(XBEE_SERIAL_VERBOSE)
  int i;
#endif

  ENSURE_MODULE;
  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return -ENODEV;

  // acquire write lock
  spin_lock_irqsave(&bridge->write_lock, flags);
  tty->flags |= (1 << TTY_DO_WRITE_WAKEUP);
  result = tty->driver->ops->write(tty, buf, nr);
  spin_unlock_irqrestore(&bridge->write_lock, flags);
#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: to %s, size %d, written %d\n", __FUNCTION__, tty->name, (int)nr, result);
#if defined(XBEE_SERIAL_VERBOSE)
  {
    printk(KERN_INFO "%s: send [", __FUNCTION__);
    for (i = 0; i < nr; i++) {
      if (buf[i] < 33 || buf[i] > 126)
        printk("(0x%x)", (int)buf[i]);
      else
        printk("(%c, 0x%x)", (char)buf[i], (int)buf[i]);
    }
    printk("]\n");
  }
#endif
#endif

  return result;
}

static int n_xbee_serial_ioctl_chars_in_buffer(struct tty_struct* tty, struct file* file, unsigned int cmd, unsigned long arg) {
  int cpres, blen;
  ENSURE_MODULE_RET(0);
  blen = (int) n_xbee_chars_in_buffer(tty);
  if (blen < 0)
    return blen;
  if (arg) {
    int* mot = (int*) arg;
    if (!file)
      *mot = (int)blen;
    else
      if ((cpres = copy_to_user(mot, &blen, sizeof(int))) != 0)
        printk(KERN_ALERT "%s: copy_to_user returned %d.\n", __FUNCTION__, cpres);
  }
  return 0;
}

static int n_xbee_serial_ioctl(struct tty_struct* tty, struct file* file, unsigned int cmd, unsigned long arg) {
  // handle read chars in buffer
  if (cmd ==
#ifdef FIONREAD
      FIONREAD
#else
      TIOCINQ
#endif
     ) {
    return n_xbee_serial_ioctl_chars_in_buffer(tty, file, cmd, arg);
  }

  // Default is not implemented
  return -EOPNOTSUPP;
}

static void n_xbee_receive_buf(struct tty_struct* tty, const unsigned char* cp, char* fp, int count) {
  unsigned long flags;
  struct xbee_serial_bridge* bridge;
  struct xbee_data_buffer* dbuf;
  int finlen;

#ifdef N_XBEE_VERBOSE
  printk(KERN_INFO "%s: from %s with size %d\n", __FUNCTION__, tty->name, count);
#endif

  ENSURE_MODULE_NORET;

  bridge = n_xbee_find_bridge_bytty(tty);
  if (!bridge)
    return;
  dbuf = bridge->recvbuf;

  spin_lock_irqsave(&dbuf->lock, flags);
  if (count > dbuf->size) {
    printk(KERN_ALERT "%s reading %d bytes which is more than the entire receive buffer size %d\n", tty->name, count, dbuf->size);
    count = dbuf->size;
  }

  finlen = dbuf->pos + count;
  if (finlen > dbuf->size) {
    printk(KERN_ALERT "%s reading %d bytes would overflow recvbuf with %d/%d bytes, dropping existing buffer\n", tty->name, count, dbuf->pos, dbuf->size);
    dbuf->pos = 0;
    finlen = count;
  }
  memcpy((dbuf->buffer + dbuf->pos), cp, count);
  dbuf->pos += count;
  spin_unlock_irqrestore(&dbuf->lock, flags);

  // If we're not pending device setup
  if (!bridge->pend_dev && bridge->netdevInitialized)
    n_xbee_handle_runtime_frames(bridge);
}

static void n_xbee_write_wakeup(struct tty_struct* tty) {
  tty->flags &= ~(1 << TTY_DO_WRITE_WAKEUP);
}

// Line discipline, allows us to assign ownership
// of a serial device to this driver.
// NOTE: make sure to call ENSURE_MODULE FIRST!!
// this ldisc remains after the module is unloaded.
// this means if the userspace tries to use the ldisc it will segfault
// assuimng the module had been removed.
struct tty_ldisc_ops n_xbee_ldisc = {
  .owner = THIS_MODULE,
  .magic = TTY_LDISC_MAGIC,
  .name  = "n_xbee",
  .open  = n_xbee_serial_open,
  .close = n_xbee_serial_close,
  .read  = n_xbee_read,
  .write = n_xbee_write,
  .flush_buffer = n_xbee_flush_buffer,
  .chars_in_buffer = n_xbee_chars_in_buffer,
  .ioctl = n_xbee_serial_ioctl,
  .receive_buf = n_xbee_receive_buf,
  .write_wakeup = n_xbee_write_wakeup
};

//marking both with __init or __exit causes errors ??

static int __init n_xbee_init(void) {
  int result;
  printk(KERN_INFO "%s: xbee-net initializing...\n", __FUNCTION__);

  n_xbee_node_table = NULL;
  n_xbee_init_bridge_ll();
  xbee_tick_thread_counter = 0;
  result = tty_register_ldisc(N_XBEE_LISC, &n_xbee_ldisc);
  if (result) {
    printk(KERN_ALERT "%s: Registering line discipline failed: %d\n", __FUNCTION__, result);
    return result;
  }

  return result;
}

static void __exit n_xbee_cleanup(void) {
  printk(KERN_INFO "%s: xbee-net shutting down...\n", __FUNCTION__);
  // Only unregister if there are NO open ttys on this ldisc.
  if (!n_xbee_serial_bridges)
    tty_unregister_ldisc(N_XBEE_LISC);
  n_xbee_free_all_bridges();
  n_xbee_free_remote_nodetable();
}

module_init(n_xbee_init);
module_exit(n_xbee_cleanup);
