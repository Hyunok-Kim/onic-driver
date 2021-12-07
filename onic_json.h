#ifndef ONIC_JSON_H
#define ONIC_JSON_H

struct onic_platform_info {
	u8 qdma_bar;
	u8 user_bar;
	u16 queue_base;
	u16 queue_max;
	u16 used_queues;
	u8 pci_msix_user_cnt;
	bool pci_master_pf;
	bool poll_mode;
	bool intr_mod_en;
	int ring_sz;
	int c2h_tmr_cnt;
	int c2h_cnt_thr;
	int c2h_buf_sz;
	bool rsfec_en;
	u8 port_id;
	u8 mac_addr[6];
};

int onic_get_platform_info(char *fname, struct onic_platform_info *pinfo);

#endif
