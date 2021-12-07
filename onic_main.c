#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/etherdevice.h>
#include <linux/netdevice.h>
#include <net/busy_poll.h>

#include "onic.h"

char onic_drv_name[] = "onic";

#define DRV_VER "1.00"
char onic_drv_ver[] = DRV_VER;

/* PCI id for devices */
static const struct pci_device_id onic_pci_ids[] = {
	{ PCI_DEVICE(0x10ee, 0x903f) },
	{ PCI_DEVICE(0x10ee, 0x913f) },
	{ PCI_DEVICE(0x10ee, 0x923f) },
	{ PCI_DEVICE(0x10ee, 0x933f) },
	{0}
};
MODULE_DEVICE_TABLE(pci, onic_pci_ids);

static int onic_stats_alloc(struct onic_priv *xpriv)
{
	if (!xpriv) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}
	if (!xpriv->netdev) {
		pr_err("%s: xpriv->netdev is NULL\n", __func__);
		return -EINVAL;
	}

	xpriv->tx_qstats = kcalloc(1,
				   (xpriv->netdev->real_num_tx_queues *
				    sizeof(struct rtnl_link_stats64)) +
				   (xpriv->netdev->real_num_rx_queues *
				    sizeof(struct rtnl_link_stats64))
				   , GFP_KERNEL);

	if (!xpriv->tx_qstats) {
		netdev_err(xpriv->netdev, "%s: Memory allocation failure for stats\n",
			   __func__);
		return -ENOMEM;
	}

	xpriv->rx_qstats = xpriv->tx_qstats + xpriv->netdev->real_num_tx_queues;

	return 0;
}

/* This function creates skb and moves data from dma request to network domain */
static int onic_rx_deliver(struct onic_priv *xpriv, u32 q_no, unsigned int len,
			   unsigned int sgcnt, struct qdma_sw_sg *sgl, void *udd)
{
	struct net_device *netdev = xpriv->netdev;
	struct sk_buff *skb = NULL;
	struct qdma_sw_sg *c2h_sgl = sgl;

	if (!sgcnt) {
		netdev_err(netdev, "%s: SG Count is NULL\n", __func__);
		return -EINVAL;
	}
	if (!sgl) {
		netdev_err(netdev, "%s: SG List is NULL\n", __func__);
		return -EINVAL;
	}

	if (len <= ONIC_RX_COPY_THRES || !(netdev->features & NETIF_F_SG)) {
		skb = napi_alloc_skb(&xpriv->napi[q_no], len);
		if (unlikely(!skb)) {
			netdev_err(netdev, "%s: napi_alloc_skb() failed\n",
				   __func__);
			return -ENOMEM;
		}

		skb_copy_to_linear_data(skb, page_address(c2h_sgl->pg) +
					c2h_sgl->offset, len);
		__skb_put(skb, len);
		put_page(c2h_sgl->pg);
	} else {
		unsigned int nr_frags = 0;
		unsigned int frag_len;
		unsigned int frag_offset;

		/* Main body length for sk_buffs used for Rx Ethernet packets
		 * with fragments. Should be >= ONIC_RX_PULL_LEN
		 */
		skb = napi_alloc_skb(&xpriv->napi[q_no], ONIC_RX_PULL_LEN);
		if (unlikely(!skb)) {
			netdev_err(netdev, "%s: napi_alloc_skb() failed\n",
				   __func__);
			return -ENOMEM;
		}

		skb_copy_to_linear_data(skb,
					page_address(c2h_sgl->pg) + c2h_sgl->offset,
					ONIC_RX_PULL_LEN);
		__skb_put(skb, ONIC_RX_PULL_LEN);

		c2h_sgl->offset += ONIC_RX_PULL_LEN;
		c2h_sgl->len -= ONIC_RX_PULL_LEN;

		while (sgcnt && c2h_sgl) {
			frag_len = c2h_sgl->len;
			frag_offset = c2h_sgl->offset;
			skb_fill_page_desc(skb, nr_frags, c2h_sgl->pg, 
					   frag_offset, frag_len);

			sgcnt--;
			c2h_sgl = c2h_sgl->next;
			nr_frags++;
		}

		skb->len = len;
		skb->data_len = len - ONIC_RX_PULL_LEN;
		skb->truesize += skb->data_len;
	}

	skb->protocol = eth_type_trans(skb, netdev);
	skb->ip_summed = CHECKSUM_NONE;
	skb_record_rx_queue(skb, q_no);

	skb_mark_napi_id(skb, &xpriv->napi[q_no]);
	netif_receive_skb(skb);

	return 0;
}

/* This function process RX dma request */
static int onic_rx_pkt_process(unsigned long qhndl, unsigned long quld,
			       unsigned int len, unsigned int sgcnt,
			       struct qdma_sw_sg *sgl, void *udd)
{
	u32 q_no;
	int ret = 0;
	struct qdma_sw_sg *l_sgl = sgl;
	struct onic_priv *xpriv = (struct onic_priv *)quld;
	struct net_device *netdev = xpriv->netdev;

	if (!netdev) {
		pr_err("%s: netdev is NULL\n", __func__);
		return -EINVAL;
	}

	if (!xpriv) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}

	if (!sgcnt) {
		netdev_err(netdev, "%s: SG Count is zero\n", __func__);
		return -EINVAL;
	}

	q_no = (qhndl - xpriv->base_rx_q_handle);
	ret = onic_rx_deliver(xpriv, q_no, len, sgcnt, sgl, udd);
	if (ret < 0) {
		while (sgcnt) {
			put_page(l_sgl->pg);
			l_sgl = l_sgl->next;
			sgcnt--;
		}
	}

	xpriv->rx_qstats[q_no].rx_packets++;
	xpriv->rx_qstats[q_no].rx_bytes += len;

	netdev_dbg(xpriv->netdev,
		   "%s: q_no = %u, qhndl = %lu, len = %d processed\n", __func__,
		   q_no, qhndl, len);

	return ret;
}

/* This is deffered NAPI task for processing incoming Rx packet from DMA queue
 * This function will from sk_buff from Rx queue data and
 * pass it to above networking layers for processing
 */
static int onic_rx_poll(struct napi_struct *napi, int quota)
{
	int queue_id;
	unsigned long q_handle;
	unsigned int udd_cnt = 0, pkt_cnt = 0, data_len = 0;
	struct onic_priv *xpriv;
	struct net_device *netdev;
	int ret;

	if (unlikely(!napi)) {
		pr_err("%s: Invalid NAPI\n", __func__);
		return -EINVAL;
	}

	netdev = napi->dev;
	if (unlikely(!netdev)) {
		pr_err("%s: netdev is NULL\n", __func__);
		return -EINVAL;
	}

	xpriv = netdev_priv(netdev);
	if (unlikely(!xpriv)) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}

	queue_id = (int)(napi - xpriv->napi);
	q_handle = (xpriv->base_rx_q_handle + queue_id);

	/* Call queue service for QDMA Core to service queue */
	ret = qdma_queue_service(xpriv->dev_handle, q_handle, quota, true);
	/* Indicate napi_complete irrespective of ret */
	napi_complete(napi);
	if (!xpriv->pinfo->poll_mode && ret < 0) {
		netdev_dbg(netdev, "%s: qdma_queue_service for queue=%d returned status=%d\n",
			   __func__, queue_id, ret);
		return ret;
	}

	if (xpriv->qdma_dev_conf.intr_moderation)
		qdma_queue_c2h_peek(xpriv->dev_handle, q_handle, &udd_cnt,
				    &pkt_cnt, &data_len);

	qdma_queue_update_pointers(xpriv->dev_handle, q_handle);

	if (xpriv->pinfo->poll_mode || (pkt_cnt >= quota))
		napi_reschedule(napi);

	return 0;
}

/* This function is RX interrupt handler (TOP half) */
static void onic_isr_rx_tophalf(unsigned long qhndl, unsigned long uld)
{
	u32 q_no;
	struct onic_priv *xpriv = (struct onic_priv *)uld;

	/* ISR is for Rx queue */
	q_no = (qhndl - xpriv->base_rx_q_handle);

	napi_schedule_irqoff(&xpriv->napi[q_no]);

	netdev_dbg(xpriv->netdev, "%s: Rx interrupt called. Mapped queue no = %d\n",
		   __func__, q_no);
}

/* Add a RX queue to QDMA */
static int onic_qdma_rx_queue_add(struct onic_priv *xpriv, u32 q_no,
				  u8 timer_idx, u8 cnt_th_idx)
{
	int ret = 0;
	char error_str[ONIC_ERROR_STR_BUF_LEN] = { '0' };
	unsigned long q_handle = 0;
	struct qdma_queue_conf qconf;

	memset(&qconf, 0, sizeof(struct qdma_queue_conf));
	qconf.st = 1;
	qconf.q_type = Q_C2H;
	qconf.irq_en = 0;
	qconf.pfetch_en = 1;
	qconf.fetch_credit = 1;
	qconf.cmpl_stat_en = 1;
	qconf.cmpl_desc_sz = DESC_SZ_8B;
	qconf.cmpl_rng_sz_idx = xpriv->cmpl_rng_sz_idx;
	qconf.desc_rng_sz_idx = xpriv->rx_desc_rng_sz_idx;
	qconf.c2h_buf_sz_idx = xpriv->rx_buf_sz_idx;
	qconf.cmpl_timer_idx = timer_idx;
	qconf.cmpl_cnt_th_idx = cnt_th_idx;
	qconf.cmpl_trig_mode = TRIG_MODE_COMBO;
	qconf.cmpl_en_intr = (xpriv->pinfo->poll_mode == 0);
	qconf.quld = (unsigned long)xpriv;
	qconf.fp_descq_isr_top = onic_isr_rx_tophalf;
	qconf.fp_descq_c2h_packet = onic_rx_pkt_process;

	netdev_dbg(xpriv->netdev,
		   "%s: c2h_rng_sz_idx = %d, desc_rng_sz_idx = %d, c2h_buf_sz_idx = %d, c2h_timer_idx = %d, c2h_cnt_th_idx = %d\n",
		   __func__, qconf.cmpl_rng_sz_idx, qconf.desc_rng_sz_idx,
		   qconf.c2h_buf_sz_idx, qconf.cmpl_timer_idx,
		   qconf.cmpl_cnt_th_idx);

	qconf.qidx = q_no;
	ret = qdma_queue_add(xpriv->dev_handle, &qconf, &q_handle, error_str,
			     ONIC_ERROR_STR_BUF_LEN);
	if (ret != 0) {
		netdev_err(xpriv->netdev,
			   "%s: qdma_queue_add() failed for queue %d with status %d(%s)\n",
			   __func__, qconf.qidx, ret, error_str);
		return ret;
	}

	/* Get base q_handle */
	if (q_no == 0)
		xpriv->base_rx_q_handle = q_handle;

	return 0;
}

/* This function releases Rx queues */
static void onic_qdma_rx_queue_release(struct onic_priv *xpriv, int num_queues)
{
	int ret = 0, q_no = 0;
	char error_str[ONIC_ERROR_STR_BUF_LEN] = { '0' };

	for (q_no = 0; q_no < num_queues; q_no++) {
		ret = qdma_queue_remove(xpriv->dev_handle,
					(xpriv->base_rx_q_handle + q_no),
					error_str, ONIC_ERROR_STR_BUF_LEN);
		if (ret != 0) {
			netdev_err(xpriv->netdev,
				   "%s: qdma_queue remove() failed for queue %d with status %d(%s)\n",
				   __func__, q_no, ret, error_str);
		}
		netif_napi_del(&xpriv->napi[q_no]);
	}

	kfree(xpriv->napi);
}

/* This function sets up RX queues */
static int onic_qdma_rx_queue_setup(struct onic_priv *xpriv)
{
	int ret = 0, q_no = 0;

	xpriv->napi = kcalloc(xpriv->netdev->real_num_rx_queues,
			      sizeof(struct napi_struct), GFP_KERNEL);
	if (!xpriv->napi) 
		return -ENOMEM;

	for (q_no = 0; q_no < xpriv->netdev->real_num_rx_queues; q_no++) {
		ret = onic_qdma_rx_queue_add(xpriv, q_no, xpriv->rx_timer_idx,
					     xpriv->rx_cnt_th_idx);
		if (ret != 0) {
			netdev_err(xpriv->netdev,
				   "%s: onic_qdma_rx_queue_add() failed for queue %d with status %d\n",
				   __func__, q_no, ret);
			goto release_rx_q;
		}
		netif_napi_add(xpriv->netdev, &xpriv->napi[q_no], onic_rx_poll,
			       ONIC_NAPI_WEIGHT);
	}

	return 0;

release_rx_q:
	onic_qdma_rx_queue_release(xpriv, q_no);
	return ret;
}

/* This function is interrupt handler (TOP half).
 * once packet is written in QDMA queue, relevant interrupt wille be generated.
 * The generated interrupt gets served from this handler
 */
static void onic_isr_tx_tophalf(unsigned long qhndl, unsigned long uld)
{
	u32 q_no;
	struct onic_priv *xpriv = (struct onic_priv *)uld;
	struct xlnx_dma_dev *xdev = (struct xlnx_dma_dev *)xpriv->dev_handle;
	struct qdma_descq *descq = qdma_device_get_descq_by_id(xdev, qhndl,
							       NULL, 0, 0);

	schedule_work(&descq->work);

	/* If ISR is for Tx queue */
	q_no = (qhndl - xpriv->base_tx_q_handle);
	netdev_dbg(xpriv->netdev, "%s: Tx interrupt called, Mapped queue no = %d\n",
		   __func__, q_no);
}

/* This function release Tx queues */
static void onic_qdma_tx_queue_release(struct onic_priv *xpriv, int num_queues)
{
	int ret = 0, q_no = 0;
	char error_str[ONIC_ERROR_STR_BUF_LEN] = { '0' };

	for (q_no = 0; q_no < num_queues; q_no++) {
		ret = qdma_queue_remove(xpriv->dev_handle,
					(xpriv->base_tx_q_handle + q_no),
					error_str, ONIC_ERROR_STR_BUF_LEN);
		if (ret != 0) {
			netdev_err(xpriv->netdev,
				   "%s: qdma_queue_remove() failed for queue %d with status %d(%s)\n",
				   __func__, q_no, ret, error_str);
		}

	}
}

/* This function sets up Tx queues */
static int onic_qdma_tx_queue_setup(struct onic_priv *xpriv)
{
	int ret = 0, q_no = 0;
	char error_str[ONIC_ERROR_STR_BUF_LEN] = { '0' };
	unsigned long q_handle = 0;
	struct qdma_queue_conf qconf;

	for (q_no = 0; q_no < xpriv->netdev->real_num_tx_queues; q_no++) {
		memset(&qconf, 0, sizeof(struct qdma_queue_conf));
		qconf.st = 1;
		qconf.q_type = Q_H2C;
		qconf.irq_en = (xpriv->pinfo->poll_mode == 0);
		qconf.wb_status_en = 1;
		qconf.cmpl_stat_en = 1;
		qconf.cmpl_status_acc_en = 1;
		qconf.cmpl_status_pend_chk = 1;
		qconf.desc_rng_sz_idx = xpriv->tx_desc_rng_sz_idx;
		qconf.fp_descq_isr_top = onic_isr_tx_tophalf;
		qconf.quld = (unsigned long)xpriv;
		qconf.qidx = q_no;

		ret = qdma_queue_add(xpriv->dev_handle, &qconf, &q_handle,
				     error_str, ONIC_ERROR_STR_BUF_LEN);

		if (ret != 0) {
			netdev_err(xpriv->netdev, 
				   "%s: qdma_queue_add() failed for queue %d with status %d(%s)\n",
				   __func__, qconf.qidx, ret, error_str);
			goto cleanup_tx_q;
		}
		if (q_no == 0)
			xpriv->base_tx_q_handle = q_handle;

	}
	return 0;

cleanup_tx_q:
	onic_qdma_tx_queue_release(xpriv, q_no);
	return ret;
}


/* This function stops Tx and Rx queues operations */
static int onic_qdma_stop(struct onic_priv *xpriv, unsigned short int txq,
			  unsigned short int rxq)
{
	int ret = 0, q = 0, err = 0;
	char error_str[ONIC_ERROR_STR_BUF_LEN] = { '0' };

	for (q = 0; q < rxq; q++) {
		ret = qdma_queue_stop(xpriv->dev_handle,
				      xpriv->base_rx_q_handle + q, error_str,
				      ONIC_ERROR_STR_BUF_LEN);
		if (ret < 0) {
			netdev_err(xpriv->netdev,
				   "%s: qdma_queue_stop() failed for Rx queue %d with status %d msg: %s\n",
				   __func__, q, ret, error_str);
			err = -EINVAL;
		}
	}

	for (q = 0; q < txq; q++) {
		ret = qdma_queue_stop(xpriv->dev_handle,
				      xpriv->base_tx_q_handle + q, error_str,
				      ONIC_ERROR_STR_BUF_LEN);
		if (ret < 0) {
			netdev_err(xpriv->netdev,
				   "%s: qdma_queue_stop() failed for Tx queue %d with status %d msg: %s\n",
				   __func__, q, ret, error_str);
			err = -EINVAL;
		}
	}

	return err;
}

/* This function starts Rx and Tx queues operations */
static int onic_qdma_start(struct onic_priv *xpriv)
{
	int ret, q_no;
	char error_str[ONIC_ERROR_STR_BUF_LEN] = { '0' };


	for (q_no = 0; q_no < xpriv->netdev->real_num_rx_queues; q_no++) {
		ret = qdma_queue_start(xpriv->dev_handle,
				       xpriv->base_rx_q_handle + q_no,
				       error_str, ONIC_ERROR_STR_BUF_LEN);
		if (ret != 0) {
			netdev_err(xpriv->netdev,
				   "%s: qdma_queue_start() failed for Rx queue %d with status %d(%s)\n",
				   __func__, q_no, ret, error_str);
			onic_qdma_stop(xpriv, 0, q_no);
			return ret;
		}
	}

	for (q_no = 0; q_no < xpriv->netdev->real_num_tx_queues; q_no++) {
		ret = qdma_queue_start(xpriv->dev_handle,
				       xpriv->base_tx_q_handle + q_no,
				       error_str, ONIC_ERROR_STR_BUF_LEN);
		if (ret != 0) {
			netdev_err(xpriv->netdev,
				   "%s: qdma_queue_start() failed for Tx queue %d with status %d(%s)\n",
				   __func__, q_no, ret, error_str);
			onic_qdma_stop(xpriv, q_no, xpriv->netdev->real_num_rx_queues);
			return ret;
		}
	}

	return 0;
}

/* This function gets called when interface gets 'UP' request via 'ifconfig up'
 * In this function, Rx and Tx queues are setup and send/receive operations
 * are started
 */
int onic_open(struct net_device *netdev)
{
	int ret = 0, q_no = 0;
	struct onic_priv * xpriv;

	if (!netdev) {
		pr_err("%s: netdev is NULL\n", __func__);
		return -EINVAL;
	}

	xpriv = netdev_priv(netdev);
	if (!xpriv) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}

	ret = onic_stats_alloc(xpriv);
	if (ret != 0) {
		netdev_err(netdev,
			   "%s: onic_stats_alloc() failed with status %d\n",
			   __func__, ret);
		return ret;
	}

	ret = onic_qdma_rx_queue_setup(xpriv);
	if (ret != 0) {
		netdev_err(netdev, "%s: onic_qdmx_rx_queue_setup() failed with status %d\n",
			   __func__, ret);
		kfree(xpriv->tx_qstats);
		return ret;
	}

	ret = onic_qdma_tx_queue_setup(xpriv);
	if (ret != 0) {
		netdev_err(netdev, "%s: onic_qdmx_tx_queue_setup() failed with status %d\n",
			   __func__, ret);
		goto release_rx_queues;
	}

	ret = onic_qdma_start(xpriv);
	if (ret != 0) {
		netdev_err(netdev, "%s: onic_qdma_start() failed with status %d\n",
			   __func__, ret);
		goto release_queues;
	}

	for (q_no = 0; q_no < xpriv->netdev->real_num_rx_queues; q_no++)
		napi_enable(&xpriv->napi[q_no]);

	if (xpriv->pinfo->poll_mode)
		for (q_no = 0; q_no < xpriv->netdev->real_num_rx_queues; q_no++)
			napi_schedule(&xpriv->napi[q_no]);

	netif_tx_start_all_queues(netdev);
	netif_carrier_on(netdev);

	netdev_info(netdev, "%s: device open done\n", __func__);

	return 0;

release_queues:
	onic_qdma_tx_queue_release(xpriv, xpriv->netdev->real_num_tx_queues);
release_rx_queues:
	onic_qdma_rx_queue_release(xpriv, xpriv->netdev->real_num_rx_queues);
	return ret;
}

/* This function gets called when there is a interface down request.
 * In this function, Tx/Rx operations on the queues are stopped
 */
static int onic_stop(struct net_device *netdev)
{
	int ret = 0, q_no = 0;
	struct onic_priv *xpriv;

	if (!netdev) {
		pr_err("%s: netdev is NULL\n", __func__);
		return -EINVAL;
	}

	xpriv = netdev_priv(netdev);
	if (!xpriv) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}

	netif_tx_stop_all_queues(netdev);
	netif_carrier_off(netdev);

	for (q_no = 0; q_no < xpriv->netdev->real_num_rx_queues; q_no++)
		napi_disable(&xpriv->napi[q_no]);

	ret = onic_qdma_stop(xpriv, netdev->real_num_tx_queues,
			     netdev->real_num_rx_queues);
	if (ret != 0)
		netdev_err(netdev, "%s: onic_qdma_stop() failed with status %d\n",
			   __func__, ret);

	onic_qdma_rx_queue_release(xpriv, netdev->real_num_rx_queues);
	onic_qdma_tx_queue_release(xpriv, netdev->real_num_tx_queues);

	kfree(xpriv->tx_qstats);

	netdev_info(netdev, "%s: device close done\n", __func__);
	return ret;
}

/* This function free skb allocated memory */
static int onic_unmap_free_pkt_data(struct qdma_request *req)
{
	u16 nb_frags = 0, frag_index = 0;
	struct qdma_sw_sg *qdma_sgl;
	struct onic_dma_request *onic_req;
	struct net_device *netdev;
	struct sk_buff *skb;
	struct onic_priv *xpriv;
	skb_frag_t *frag;

	if (unlikely(!req)) {
		pr_err("%s: req is NULL\n", __func__);
		return -EINVAL;
	}

	onic_req = (struct onic_dma_request *)req->uld_data;
	if (unlikely(!onic_req)) {
		pr_err("%s: onic_req is NULL\n", __func__);
		return -EINVAL;
	}

	netdev = onic_req->netdev;
	if (unlikely(!netdev)) {
		pr_err("%s: netdev is NULL\n", __func__);
		return -EINVAL;
	}

	xpriv = netdev_priv(netdev);
	if (unlikely(!xpriv)) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}

	skb = onic_req->skb;
	if (unlikely(!skb)) {
		netdev_err(netdev, "%s: skb is NULL\n", __func__);
		return -EINVAL;
	}

	nb_frags = skb_shinfo(skb)->nr_frags;
	qdma_sgl = req->sgl;
	dma_unmap_single(netdev->dev.parent, qdma_sgl->dma_addr,
			 skb_headlen(skb), DMA_TO_DEVICE);

	netdev_dbg(netdev,
		   "%s: skb->len = %d skb_headlen(skb) = %d, dma_addr = %llx nb_frags:%d\n",
		   __func__, skb->len, skb_headlen(skb), qdma_sgl->dma_addr,
		   nb_frags);

	qdma_sgl = qdma_sgl->next;
	while (qdma_sgl && (frag_index < nb_frags)) {
		frag = &skb_shinfo(skb)->frags[frag_index];
		qdma_sgl->len = skb_frag_size(frag);
		dma_unmap_single(netdev->dev.parent, qdma_sgl->dma_addr,
				 qdma_sgl->len, DMA_TO_DEVICE);
		qdma_sgl = qdma_sgl->next;
		frag_index++;
	}

	dev_consume_skb_irq(skb);
	kmem_cache_free(xpriv->dma_req, onic_req);

	return 0;
}

/* This function is called by QDMA core when one or multiple packet
 * transmission is completed.
 * This function frees skb associated with the transmitted packets.
 */
static int onic_tx_done(struct qdma_request *req, unsigned int bytes_done,
			int err)
{
	int ret = 0;

	ret = onic_unmap_free_pkt_data(req);
	if (ret != 0)
		pr_err("%s: onic_unmap_free_pkt_data() failed\n", __func__);

	pr_debug("%s: bytes_done = %d, error = %d\n",
		 __func__, bytes_done, err);

	return 0;
}

/* This function is called from networking stack in order to send packet */
static int onic_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	u16 q_id = 0, nb_frags = 0, frag_index = 0;
	int ret = 0, count = 0;
	unsigned long q_handle;
	struct onic_priv *xpriv;
	struct onic_dma_request *onic_req;
	struct qdma_request *qdma_req;
	struct qdma_sw_sg *qdma_sgl;
	skb_frag_t *frag;

	xpriv = netdev_priv(netdev);
	if (unlikely(!xpriv)) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return -EINVAL;
	}

	ret = netif_carrier_ok(netdev);
	if (unlikely(!ret)) {
		netdev_err(netdev, "%s: Packet sent when carrier is down\n",
			   __func__);
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}

	if (unlikely(skb_is_gso(skb))) {
		netdev_err(netdev, "%s: Received GSO SKB\n", __func__);
		return -EINVAL;
	}

	/* minimum Ethernet packet length is 60 */
	ret = skb_put_padto(skb, ETH_ZLEN);
	if (unlikely(ret != 0)) {
		netdev_err(netdev, "%s: skb_put_padto failed with status %d\n", __func__, ret);
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}

	q_id = skb_get_queue_mapping(skb);
	if (unlikely(q_id >= netdev->real_num_tx_queues)) {
		netdev_err(netdev, "%s: Invalid queue mapping. q_id = %d\n",
			   __func__, q_id);
		dev_kfree_skb_any(skb);
		return -EINVAL;
	}

	q_handle = xpriv->base_tx_q_handle + q_id;

	onic_req = kmem_cache_zalloc(xpriv->dma_req, GFP_ATOMIC);
	if (unlikely(!onic_req)) {
		netdev_err(netdev, "%s: onic_req allocation failed\n",
			   __func__);
		return -ENOMEM;
	}
	qdma_req = &onic_req->qdma;
	qdma_sgl = &onic_req->sgl[0];

	onic_req->skb = skb;
	onic_req->netdev = netdev;

	qdma_req->sgl = qdma_sgl;

	qdma_sgl->len = skb_headlen(skb);
	qdma_req->count = qdma_sgl->len;

	qdma_sgl->dma_addr = dma_map_single(netdev->dev.parent, skb->data,
					    skb_headlen(skb), DMA_TO_DEVICE);
	ret = dma_mapping_error(netdev->dev.parent, qdma_sgl->dma_addr);
	if (unlikely(ret)) {
		netdev_err(netdev, "%s: dma_map_single() failed\n", __func__);
		ret = -EFAULT;
		goto free_packet_data;
	}

	qdma_sgl->next = NULL;
	qdma_req->sgcnt++;
	nb_frags = skb_shinfo(skb)->nr_frags;
	/* DMA mapping for fragments data */
	for (frag_index = 0; frag_index < nb_frags; frag_index++) {
		qdma_sgl->next = (qdma_sgl + 1);
		qdma_sgl = qdma_sgl->next;
		qdma_sgl->next = NULL;

		frag = &skb_shinfo(skb)->frags[frag_index];
		qdma_sgl->len = skb_frag_size(frag);
		qdma_req->count += qdma_sgl->len;

		netdev_dbg(netdev, "%s: frag no = %d, skb_frag_size = %d\n",
			   __func__, frag_index, qdma_sgl->len);

		qdma_sgl->dma_addr = (unsigned long)skb_frag_dma_map(
								     netdev->dev.parent, frag, 0, skb_frag_size(frag),
								     DMA_TO_DEVICE);
		ret = dma_mapping_error(netdev->dev.parent, qdma_sgl->dma_addr);
		if (unlikely(ret)) {
			netdev_err(netdev, "%s: dma_map_single failed\n",
				   __func__);
			ret = -EFAULT;
			goto free_packet_data;
		}
		qdma_req->sgcnt++;
	}

	qdma_req->dma_mapped = 1;
	qdma_req->check_qstate_disabled = 1;
	qdma_req->fp_done = onic_tx_done;
	qdma_req->uld_data = (unsigned long)onic_req;

	count = qdma_queue_packet_write(xpriv->dev_handle, q_handle, qdma_req);
	if (unlikely(count < 0)) {
		netdev_err(netdev,
			   "%s: qdma_queue_packet_write() failed, err = %d\n",
			   __func__, count);
		ret = count;
		goto free_packet_data;
	}

	xpriv->tx_qstats[q_id].tx_packets++;
	xpriv->tx_qstats[q_id].tx_bytes += skb->len;

	return NETDEV_TX_OK;

free_packet_data:
	if (onic_unmap_free_pkt_data(qdma_req) != 0) {
		netdev_err(netdev, "%s: onic_unmap_free_pkt_data() failed\n",
			   __func__);
		kmem_cache_free(xpriv->dma_req, onic_req);
	}
	xpriv->tx_qstats[q_id].tx_dropped++;
	return ret;
}

static int onic_set_mac_address(struct net_device *dev, void *addr)
{
	struct sockaddr *saddr = addr;
	u8 *dev_addr = saddr->sa_data;
	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	netdev_info(dev, "Set MAC address to %x:%x:%x:%x:%x:%x",
		    dev_addr[0], dev_addr[1], dev_addr[2],
		    dev_addr[3], dev_addr[4], dev_addr[5]);
	memcpy(dev->dev_addr, dev_addr, dev->addr_len);
	return 0;
};

static int onic_do_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
	return 0;
}

static int onic_change_mtu(struct net_device *netdev, int mtu)
{
	netdev_info(netdev, "Requestd MTU = %d", mtu);
	return 0;
}

static void onic_get_stats64(struct net_device *netdev,
			     struct rtnl_link_stats64 *stats)
{
	u32 q_num = 0;
	struct onic_priv *xpriv;

	if (!netdev) {
		pr_err("%s: netdev is NULL\n", __func__);
		return;
	}

	xpriv = netdev_priv(netdev);
	if (!xpriv) {
		pr_err("%s: xpriv is NULL\n", __func__);
		return;
	}

	if ((xpriv->tx_qstats != NULL) && (xpriv->rx_qstats != NULL)) {
		for (q_num = 0; q_num < netdev->real_num_tx_queues; q_num++) {
			stats->tx_bytes += xpriv->tx_qstats[q_num].tx_bytes;
			stats->tx_packets += xpriv->tx_qstats[q_num].tx_packets;
		}

		for (q_num = 0; q_num < netdev->real_num_rx_queues; q_num++) {
			stats->rx_bytes += xpriv->rx_qstats[q_num].rx_bytes;
			stats->rx_packets += xpriv->rx_qstats[q_num].rx_packets;
		}
	}
}

/* Network Device Operations */
static const struct net_device_ops onic_netdev_ops = {
	.ndo_open = onic_open,
	.ndo_stop = onic_stop,
	.ndo_start_xmit = onic_start_xmit,
	.ndo_set_mac_address = onic_set_mac_address,
	.ndo_do_ioctl = onic_do_ioctl,
	.ndo_change_mtu = onic_change_mtu,
	.ndo_get_stats64 = onic_get_stats64
};

static int onic_set_num_queue(struct onic_priv *xpriv)
{
	unsigned short int nb_queues;
	int num_msix;

	num_msix = pci_msix_vec_count(xpriv->pcidev);
	if (num_msix <= 0) {
		pr_err("%s:No MSIX cpability %d\n", __func__, num_msix);
		return num_msix;
	}

	nb_queues = num_msix;
	nb_queues -= xpriv->pinfo->pci_msix_user_cnt;
	if (xpriv->pinfo->pci_master_pf)
		nb_queues--;

	xpriv->num_msix = num_msix;
	xpriv->nb_queues = nb_queues;

	return 0;
}

static int onic_arr_find(unsigned int *arr, int n, int element)
{
	int i = 0;

	for (i = 0; i < n; i++) {
		if (*(arr + i) == element)
			return i;
	}
	return -1;
}

/* This function gets global CSR and sets the default indexes of the
 * xpriv structure. If the default value set in the driver doesn't match with
 * any of the values supported, index 0 is returned by default.
 */
static int onic_qdma_csr_index_setup(struct onic_priv *xpriv)
{
	int ret = 0, index = 0;
	struct global_csr_conf csr_conf;

	ret = qdma_global_csr_get(xpriv->dev_handle, 0,
				  QDMA_GLOBAL_CSR_ARRAY_SZ, &csr_conf);
	if (ret != 0) {
		dev_err(&xpriv->pcidev->dev, 
			"%s: qdma_global_csr_get() failed with status %d\n",
			__func__, ret);
		return -EINVAL;
	}

	index = onic_arr_find(csr_conf.ring_sz, QDMA_GLOBAL_CSR_ARRAY_SZ,
			      xpriv->pinfo->ring_sz);
	if (index < 0) {
		dev_err(&xpriv->pcidev->dev, 
			"%s: Expected ring size %d not found\n", __func__, ret);
		return index;
	}
	xpriv->tx_desc_rng_sz_idx = index;
	xpriv->rx_desc_rng_sz_idx = index;
	xpriv->cmpl_rng_sz_idx = index;

	index = onic_arr_find(csr_conf.c2h_timer_cnt, QDMA_GLOBAL_CSR_ARRAY_SZ,
			      xpriv->pinfo->c2h_tmr_cnt);
	if (index < 0) {
		dev_err(&xpriv->pcidev->dev,
			"%s: Expected default C2H Timer count %d not found\n",
			__func__, xpriv->pinfo->c2h_tmr_cnt);
		return index;
	}
	xpriv->rx_timer_idx = index;

	index = onic_arr_find(csr_conf.c2h_cnt_th, QDMA_GLOBAL_CSR_ARRAY_SZ,
			      xpriv->pinfo->c2h_cnt_thr);
	if (index < 0) {
		dev_err(&xpriv->pcidev->dev,
			"%s: Expected default C2H count threshold count %d not found\n",
			__func__, xpriv->pinfo->c2h_cnt_thr);
		return index;
	}
	xpriv->rx_cnt_th_idx = index;

	index = onic_arr_find(csr_conf.c2h_buf_sz, QDMA_GLOBAL_CSR_ARRAY_SZ,
			      xpriv->pinfo->c2h_buf_sz);
	if (index < 0) {
		dev_err(&xpriv->pcidev->dev,
			"%s: Expected default C2H Buffer size %d not found",
			__func__, xpriv->pinfo->c2h_buf_sz);
		return index;
	}
	xpriv->rx_buf_sz_idx = index;
	return 0;
}

/* Configure QDMA Device, Global CSR Registers */
static int onic_qdma_setup(struct onic_priv *xpriv)
{
	int ret;

	memset(&xpriv->qdma_dev_conf, 0, sizeof(struct qdma_dev_conf));

	xpriv->qdma_dev_conf.bar_num_config = xpriv->pinfo->qdma_bar;
	xpriv->qdma_dev_conf.msix_qvec_max = xpriv->num_msix;
	xpriv->qdma_dev_conf.data_msix_qvec_max = xpriv->nb_queues;
	xpriv->qdma_dev_conf.user_msix_qvec_max = xpriv->pinfo->pci_msix_user_cnt;
	xpriv->qdma_dev_conf.master_pf = xpriv->pinfo->pci_master_pf;
	if (!xpriv->pinfo->poll_mode)
		xpriv->qdma_dev_conf.intr_moderation =
			xpriv->pinfo->intr_mod_en;
	xpriv->qdma_dev_conf.qsets_max = xpriv->pinfo->queue_max;
	xpriv->qdma_dev_conf.qsets_base = xpriv->pinfo->queue_base;
	xpriv->qdma_dev_conf.pdev = xpriv->pcidev;
	if (xpriv->pinfo->poll_mode)
		xpriv->qdma_dev_conf.qdma_drv_mode = POLL_MODE;
	else
		xpriv->qdma_dev_conf.qdma_drv_mode = DIRECT_INTR_MODE;

	ret = qdma_device_open(onic_drv_name, &xpriv->qdma_dev_conf, &xpriv->dev_handle);
	if (ret != 0) {
		dev_err(&xpriv->pcidev->dev, 
			"%s: qdma_device_open() failed: Error Code: %d\n",
			__func__, ret);
		return -EINVAL;
	}

	ret = onic_qdma_csr_index_setup(xpriv);
	if (ret != 0) {
		dev_err(&xpriv->pcidev->dev,
			"%s: onic_qdma_csr_index_setup() failed width status %d\n",
			__func__, ret);
		ret = -EINVAL;
		goto close_qdma;
	}

	return 0;

close_qdma:
	qdma_device_close(xpriv->pcidev, xpriv->dev_handle);
	return ret;
}

static bool onic_rx_lane_aligned(struct onic_priv *xpriv, u8 cmac_id)
{
	u32 offset = CMAC_OFFSET_STAT_RX_STATUS(cmac_id);
	u32 val;

	/* read twice to flush any previously latched value */
	val = readl(xpriv->bar_base + offset);
	val = readl(xpriv->bar_base + offset);
	return (val == 0x3);
}

static void onic_disable_cmac(struct onic_priv *xpriv)
{
	u8 cmac_id = xpriv->pinfo->port_id;

	writel(0x0, xpriv->bar_base + CMAC_OFFSET_CONF_TX_1(cmac_id));
	writel(0x0, xpriv->bar_base + CMAC_OFFSET_CONF_RX_1(cmac_id));
}

static int onic_enable_cmac(struct onic_priv *xpriv)
{
	u8 cmac_id = xpriv->pinfo->port_id;

	if (xpriv->pinfo->rsfec_en) {
		/* Enable RS-FEC for CMACs with RS-FEC implemented */
		writel(0x3, xpriv->bar_base + CMAC_OFFSET_RSFEC_CONF_ENABLE(cmac_id));
		writel(0x7, xpriv->bar_base + CMAC_OFFSET_RSFEC_CONF_IND_CORRECTION(cmac_id));
	}

	if (cmac_id == 0) {
		writel(0x10, xpriv->bar_base + SYSCFG_OFFSET_SHELL_RESET);
		while ((readl(xpriv->bar_base + SYSCFG_OFFSET_SHELL_STATUS) & 0x10) != 0x10)
			mdelay(1);
	} else {
		writel(0x100, xpriv->bar_base + SYSCFG_OFFSET_SHELL_RESET);
		while ((readl(xpriv->bar_base + SYSCFG_OFFSET_SHELL_STATUS) & 0x100) != 0x100)
			mdelay(1);
	}

	writel(0x1, xpriv->bar_base + CMAC_OFFSET_CONF_RX_1(cmac_id));
	writel(0x10, xpriv->bar_base + CMAC_OFFSET_CONF_TX_1(cmac_id));

	/* wait for lane alignment */
	if (!onic_rx_lane_aligned(xpriv, cmac_id)) {
		mdelay(100);
		if (!onic_rx_lane_aligned(xpriv, cmac_id))
			goto rx_not_aligned;
	}

	writel(0x1, xpriv->bar_base + CMAC_OFFSET_CONF_TX_1(cmac_id));

	/* RX flow control */
	writel(0x00003DFF, xpriv->bar_base + CMAC_OFFSET_CONF_RX_FC_CTRL_1(cmac_id));
	writel(0x0001C631, xpriv->bar_base + CMAC_OFFSET_CONF_RX_FC_CTRL_2(cmac_id));

	/* TX flow control */
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_QNTA_1(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_QNTA_2(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_QNTA_3(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_QNTA_4(cmac_id));
	writel(0x0000FFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_QNTA_5(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_RFRH_1(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_RFRH_2(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_RFRH_3(cmac_id));
	writel(0xFFFFFFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_RFRH_4(cmac_id));
	writel(0x0000FFFF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_RFRH_5(cmac_id));
	writel(0x000001FF, xpriv->bar_base + CMAC_OFFSET_CONF_TX_FC_CTRL_1(cmac_id));

	return 0;

rx_not_aligned:
	onic_disable_cmac(xpriv);
	return -EBUSY;
}

static void onic_init_reta(struct onic_priv *xpriv)
{
	u32 val;
	int i;

	/* inform shell about the function map */
	val = (FIELD_SET(QDMA_FUNC_QCONF_QBASE_MASK, xpriv->pinfo->queue_base) |
	       FIELD_SET(QDMA_FUNC_QCONF_NUMQ_MASK,
			 xpriv->netdev->real_num_rx_queues));
	writel(val, xpriv->bar_base +
	       QDMA_FUNC_OFFSET_QCONF(xpriv->pinfo->port_id)); 

	/* initialize indirection table */
	for (i = 0; i < 128; i++) {
		u32 val = (i % xpriv->netdev->real_num_rx_queues) & 0x0000FFFF;
		u32 offset = QDMA_FUNC_OFFSET_INDIR_TABLE(xpriv->pinfo->port_id, i);
		writel(val, xpriv->bar_base + offset);
	}

}

static int onic_get_pinfo(struct pci_dev *pdev, struct onic_platform_info
			  **pinfo_ref)
{
	int ret;
	struct onic_platform_info *pinfo;
	char file_name[50] = {0};

	pinfo = kzalloc(sizeof(struct onic_platform_info), GFP_KERNEL);
	if (!pinfo)
		return -ENOMEM;

	sprintf(file_name, "onic_%4x.json", pdev->device);
	ret = onic_get_platform_info(file_name, pinfo);

	if (ret) {
		pr_err("%s: onic_get_platform_info failed\n", __func__);
		kfree(pinfo);
		return ret;
	}

	*pinfo_ref = pinfo;

	return 0;
}

extern void onic_set_ethtool_ops(struct net_device *netdev);

/* This is probe function which is called when Linux kernel detects PCIe device
 * with Vendor ID and Device ID listed in the in onic_pci_ids table.
 * From this function netdevice and device initialization are done
 */
static int onic_pci_probe(struct pci_dev *pdev, 
			  const struct pci_device_id *pci_dev_id)
{
	struct onic_platform_info *pinfo = NULL;
	struct net_device *netdev;
	struct onic_priv *xpriv;
	struct sockaddr saddr;
	char dev_name[IFNAMSIZ];
	int ret;
	u64 bar_start;
	u64 bar_len;

	ret = onic_get_pinfo(pdev, &pinfo);
	if (ret) {
		pr_err("%s: onic_get_pinfo() failed with status %d\n", __func__,
		       ret);
		return ret;
	}

	netdev = alloc_etherdev_mq(sizeof(struct onic_priv),
				   pinfo->queue_max);
	if (!netdev) {
		dev_err(&pdev->dev, "%s: alloc_etherdev_mq() failed\n", __func__);
		return -ENODEV;
	}

	SET_NETDEV_DEV(netdev, &pdev->dev);
	pci_set_drvdata(pdev, netdev);
	netdev->netdev_ops = &onic_netdev_ops;
	onic_set_ethtool_ops(netdev);

	snprintf(dev_name, IFNAMSIZ, "onic%ds%df%d",
		 pdev->bus->number,
		 PCI_SLOT(pdev->devfn),
		 PCI_FUNC(pdev->devfn));
	strlcpy(netdev->name, dev_name, sizeof(netdev->name));

	/* Initialize driver private data */
	xpriv = netdev_priv(netdev);
	memset(xpriv, 0, sizeof(struct onic_priv));
	xpriv->netdev = netdev;
	xpriv->pcidev = pdev;
	xpriv->pinfo = pinfo;

	memset(&saddr, 0, sizeof(struct sockaddr));
	memcpy(saddr.sa_data, pinfo->mac_addr, 6);
	onic_set_mac_address(netdev, (void *)&saddr);

	if (pinfo->pci_master_pf) {
		dev_info(&pdev->dev, "device is master PF");
	}

	ret = onic_set_num_queue(xpriv);
	if (ret) {
		goto exit;
	}

	if (xpriv->pinfo->used_queues > 0 ) {
		netif_set_real_num_tx_queues(xpriv->netdev,
					     xpriv->pinfo->used_queues);
		netif_set_real_num_rx_queues(xpriv->netdev,
					     xpriv->pinfo->used_queues);
	} else {
		netif_set_real_num_tx_queues(xpriv->netdev, xpriv->nb_queues);
		netif_set_real_num_rx_queues(xpriv->netdev, xpriv->nb_queues);
	}

	xpriv->dma_req = KMEM_CACHE(onic_dma_request, 0);
	if (!xpriv->dma_req) {
		dev_err(&pdev->dev, "%s: Cache alloc failed", __func__);
		ret = -ENOMEM;
		goto exit;
	}

	ret = onic_qdma_setup(xpriv);
	if (ret != 0) {
		dev_err(&pdev->dev, "%s: onic_qdma_setup() failed with status %d\n",
			__func__, ret);
		goto destroy_kmem_cache;
	}

	/* Map the User BAR */
	bar_start = pci_resource_start(pdev, pinfo->user_bar);
	bar_len = pci_resource_len(pdev, pinfo->user_bar);
	xpriv->bar_base = ioremap(bar_start, bar_len);
	if (!xpriv->bar_base) {
		dev_err(&pdev->dev, "%s: ioremap() failed for BAR%d\n", 
			__func__, pinfo->user_bar);
		ret = -EIO;
		goto close_qdma_device;
	}

	ret = onic_enable_cmac(xpriv);
	if (ret != 0) {
		dev_err(&pdev->dev, "%s: onic_enable_cmac() failed with status %d\n", __func__, ret);
		goto iounmap_bar;
	}

	onic_init_reta(xpriv);

	ret = register_netdev(netdev);
	if (ret != 0) {
		dev_err(&pdev->dev, "%s: Failed to register network driver\n",
			__func__);
		goto disable_cmac;
	}

	netif_carrier_off(netdev);

	return 0;

disable_cmac:
	onic_disable_cmac(xpriv);
iounmap_bar:
	iounmap(xpriv->bar_base);
close_qdma_device:
	qdma_device_close(pdev, xpriv->dev_handle);
destroy_kmem_cache:
	kmem_cache_destroy(xpriv->dma_req);
exit:
	kfree(xpriv->pinfo);
	free_netdev(netdev);
	return ret;
}

/* This function gets called when PCIe device has been removed from the bus
 * This function cleans up all initialized components of the driver
 */
static void onic_pci_remove(struct pci_dev *pdev)
{
	struct net_device *netdev = pci_get_drvdata(pdev);
	struct onic_priv *xpriv;

	if (!netdev) {
		dev_err(&pdev->dev, "%s: netdev is NULL\n", __func__);
		return;
	}

	xpriv = netdev_priv(netdev);
	if (!xpriv) {
		dev_err(&pdev->dev, "%s: xpriv is NULL\n", __func__);
		return;
	}

	pci_set_drvdata(pdev, NULL);
	unregister_netdev(netdev);

	onic_disable_cmac(xpriv);
	if (xpriv->bar_base)
		iounmap(xpriv->bar_base);
	qdma_device_close(pdev, xpriv->dev_handle);
	kmem_cache_destroy(xpriv->dma_req);
	kfree(xpriv->pinfo);
	free_netdev(netdev);
}

/* PCIe Driver */
static struct pci_driver onic_pci_driver = {
	.name = onic_drv_name,
	.id_table = onic_pci_ids,
	.probe = onic_pci_probe,
	.remove = onic_pci_remove,
};

/* This is the entry point of the NIC driver.
 * This function will get called on insert of the driver into the kernel
 */
static int __init onic_module_init(void)
{
	int err = 0;

	/* Initialize QDMA Library */
	err = libqdma_init(0, NULL);
	if (err != 0) {
		pr_err("%s: libqdma_init() failed\n", __func__);
		return err;
	}

	err = pci_register_driver(&onic_pci_driver);
	if (err < 0) {
		pr_err("%s: PCI registration failed with status %d\n",
		       __func__, err);
	}

	return err;
}

/* This is the exit point of the NIC driver.
 * This function will get called on remove of the driver from the kernel
 */
static void __exit onic_module_exit(void)
{
	pci_unregister_driver(&onic_pci_driver);
	libqdma_exit();
}

module_init(onic_module_init);
module_exit(onic_module_exit);

MODULE_AUTHOR("Hyunok Kim");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION(DRV_VER);
MODULE_VERSION(DRV_VER);
