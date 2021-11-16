#ifndef ONIC_H
#define ONIC_H

#include <linux/netdevice.h>
#include <linux/cpumask.h>
#include "libqdma_export.h"
#include "onic_register.h"
#include "qdma_access/qdma_access_common.h"
#include "qdma_descq.h"

#define ONIC_MAX_USER_MSIX                  (1)
#define ONIC_MAX_QUEUES                     (64)
#define ONIC_FLAG_MASTER_PF                 (0)
#define ONIC_INTERRUPT_MODERATION_EN        (1)
#define ONIC_USER_BAR                       (2)
#define ONIC_ERROR_STR_BUF_LEN              (512)

#define ONIC_DEFAULT_RING_SIZE              (1024)
#define ONIC_DEFAULT_C2H_TIMER_COUNT        (5)
#define ONIC_DEFAULT_C2H_COUNT_THRESHOLD    (64)
#define ONIC_DEFAULT_C2H_BUFFER_SIZE        (4096)

#define ONIC_RX_COPY_THRES                  (256)
#define ONIC_RX_PULL_LEN                    (128)
#define ONIC_NAPI_WEIGHT                    (64)


struct onic_dma_request {
	struct sk_buff *skb;
	struct net_device *netdev;
	struct qdma_request qdma;
	struct qdma_sw_sg sgl[MAX_SKB_FRAGS];
};

/* ONIC Net device private structure */
struct onic_priv {
	u8 rx_desc_rng_sz_idx;
	u8 tx_desc_rng_sz_idx;
	u8 rx_buf_sz_idx;
	u8 rx_timer_idx;
	u8 rx_cnt_th_idx;
	u8 cmpl_rng_sz_idx;

	struct net_device *netdev;
	struct pci_dev *pcidev;

	u16 num_msix;
	u16 nb_queues;
	DECLARE_BITMAP(flags, 32);
	int func_id;
	int RS_FEC;

	struct kmem_cache *dma_req;
	struct qdma_dev_conf qdma_dev_conf;
	unsigned long dev_handle;
	void __iomem *bar_base;

	unsigned long base_tx_q_handle, base_rx_q_handle;
	struct napi_struct *napi;
	struct rtnl_link_stats64 *tx_qstats, *rx_qstats;

};

#endif /* ONIC_H */
