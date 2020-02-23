/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

/*
 * Sample application demostrating how to do packet I/O in a multi-process
 * environment. The same code can be run as a primary process and as a
 * secondary process, just with a different proc-id parameter in each case
 * (apart from the EAL flag to indicate a secondary process).
 *
 * Each process will read from the same ports, given by the port-mask
 * parameter, which should be the same in each case, just using a different
 * queue per port as determined by the proc-id parameter.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/queue.h>
#include <getopt.h>
#include <signal.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_atomic.h>
#include <rte_branch_prediction.h>
#include <rte_debug.h>
#include <rte_interrupts.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_memcpy.h>
#include <rte_mbuf.h>
#include <rte_string_fns.h>
#include <rte_cycles.h>

#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1

#define NB_MBUFS 64*1024 /* use 64k mbufs */
#define MBUF_CACHE_SIZE 256
#define PKT_BURST 32
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define PARAM_PROC_ID "proc-id"
#define PARAM_NUM_PROCS "num-procs"

// #define RSS_DEBUG 1
#define PERF_DEBUG 1

#ifdef PERF_DEBUG
#include <time.h>
#include <sys/time.h>
#endif

/* for each lcore, record the elements of the ports array to use */
struct lcore_ports{
	unsigned start_port;
	unsigned num_ports;
};

/* structure to record the rx and tx packets. Put two per cache line as ports
 * used in pairs */
struct port_stats{
	unsigned rx;
	unsigned tx;
	unsigned drop;
} __attribute__((aligned(RTE_CACHE_LINE_SIZE / 2)));

static int proc_id = -1;
static unsigned num_procs = 0;

static uint16_t ports[RTE_MAX_ETHPORTS];
static unsigned num_ports = 0;

static struct lcore_ports lcore_ports[RTE_MAX_LCORE];
static struct port_stats pstats[RTE_MAX_ETHPORTS];

/* prints the usage statement and quits with an error message */
static void
smp_usage(const char *prgname, const char *errmsg)
{
	printf("\nError: %s\n",errmsg);
	printf("\n%s [EAL options] -- -p <port mask> "
			"--"PARAM_NUM_PROCS" <n>"
			" --"PARAM_PROC_ID" <id>\n"
			"-p         : a hex bitmask indicating what ports are to be used\n"
			"--num-procs: the number of processes which will be used\n"
			"--proc-id  : the id of the current process (id < num-procs)\n"
			"\n",
			prgname);
	exit(1);
}


/* signal handler configured for SIGTERM and SIGINT to print stats on exit */
static void
print_stats(int signum)
{
	unsigned i;
	printf("\nExiting on signal %d\n\n", signum);
	for (i = 0; i < num_ports; i++){
		const uint8_t p_num = ports[i];
		printf("Port %u: RX - %u, TX - %u, Drop - %u\n", (unsigned)p_num,
				pstats[p_num].rx, pstats[p_num].tx, pstats[p_num].drop);
	}
	exit(0);
}

/* Parse the argument given in the command line of the application */
static int
smp_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	uint16_t i, port_mask = 0;
	char *prgname = argv[0];
	static struct option lgopts[] = {
			{PARAM_NUM_PROCS, 1, 0, 0},
			{PARAM_PROC_ID, 1, 0, 0},
			{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:", \
			lgopts, &option_index)) != EOF) {

		switch (opt) {
		case 'p':
			port_mask = strtoull(optarg, NULL, 16);
			break;
			/* long options */
		case 0:
			if (strncmp(lgopts[option_index].name, PARAM_NUM_PROCS, 8) == 0)
				num_procs = atoi(optarg);
			else if (strncmp(lgopts[option_index].name, PARAM_PROC_ID, 7) == 0)
				proc_id = atoi(optarg);
			break;

		default:
			smp_usage(prgname, "Cannot parse all command-line arguments\n");
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	if (proc_id < 0)
		smp_usage(prgname, "Invalid or missing proc-id parameter\n");
	if (rte_eal_process_type() == RTE_PROC_PRIMARY && num_procs == 0)
		smp_usage(prgname, "Invalid or missing num-procs parameter\n");
	if (port_mask == 0)
		smp_usage(prgname, "Invalid or missing port mask\n");

	/* get the port numbers from the port mask */
	RTE_ETH_FOREACH_DEV(i)
		if(port_mask & (1 << i))
			ports[num_ports++] = (uint8_t)i;

	ret = optind-1;
	optind = 1; /* reset getopt lib */

	return ret;
}

static uint8_t seed[40] = {
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A 
};

int sym_hash_enable(int port_id, uint32_t ftype, enum rte_eth_hash_function function)
{
    struct rte_eth_hash_filter_info info;
    int ret = 0;
    uint32_t idx = 0;
    uint32_t offset = 0;

    memset(&info, 0, sizeof(info));

    ret = rte_eth_dev_filter_supported(port_id, RTE_ETH_FILTER_HASH);
    if (ret < 0) {
        DPDK_ERROR("RTE_ETH_FILTER_HASH not supported on port: %d",
                         port_id);
        return ret;
    }

    info.info_type = RTE_ETH_HASH_FILTER_GLOBAL_CONFIG;
    info.info.global_conf.hash_func = function;

    idx = ftype / UINT64_BIT;
    offset = ftype % UINT64_BIT;
    info.info.global_conf.valid_bit_mask[idx] |= (1ULL << offset);
    info.info.global_conf.sym_hash_enable_mask[idx] |=
                        (1ULL << offset);

    ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_HASH,
                                  RTE_ETH_FILTER_SET, &info);
    if (ret < 0)
    {
        DPDK_ERROR("Cannot set global hash configurations"
                        "on port %u", port_id);
        return ret;
    }

    return 0;
}

int sym_hash_set(int port_id, int enable)
{
    int ret = 0;
    struct rte_eth_hash_filter_info info;

    memset(&info, 0, sizeof(info));

    ret = rte_eth_dev_filter_supported(port_id, RTE_ETH_FILTER_HASH);
    if (ret < 0) {
        DPDK_ERROR("RTE_ETH_FILTER_HASH not supported on port: %d",
                         port_id);
        return ret;
    }

    info.info_type = RTE_ETH_HASH_FILTER_SYM_HASH_ENA_PER_PORT;
    info.info.enable = enable;
    ret = rte_eth_dev_filter_ctrl(port_id, RTE_ETH_FILTER_HASH,
                        RTE_ETH_FILTER_SET, &info);

    if (ret < 0)
    {
        DPDK_ERROR("Cannot set symmetric hash enable per port "
                        "on port %u", port_id);
        return ret;
    }

    return 0;
}

static void
set_xl710(uint16_t port_id)
{
	sym_hash_enable(port_id, RTE_ETH_FLOW_NONFRAG_IPV4_TCP, RTE_ETH_HASH_FUNCTION_TOEPLITZ);
	sym_hash_enable(port_id, RTE_ETH_FLOW_NONFRAG_IPV4_UDP, RTE_ETH_HASH_FUNCTION_TOEPLITZ);
	sym_hash_enable(port_id, RTE_ETH_FLOW_FRAG_IPV4, RTE_ETH_HASH_FUNCTION_TOEPLITZ);
	sym_hash_enable(port_id, RTE_ETH_FLOW_NONFRAG_IPV4_SCTP, RTE_ETH_HASH_FUNCTION_TOEPLITZ);
	sym_hash_enable(port_id, RTE_ETH_FLOW_NONFRAG_IPV4_OTHER, RTE_ETH_HASH_FUNCTION_TOEPLITZ);

	sym_hash_set(port_id, 1);
}

static void 
set_flow_type_mask(struct rte_eth_hash_filter_info *info, uint32_t ftype)
{
    uint32_t idx, offset;

    idx = ftype / (CHAR_BIT * sizeof(uint32_t));
    offset = ftype % (CHAR_BIT * sizeof(uint32_t));
    info->info.global_conf.valid_bit_mask[idx] |= (1UL << offset);
    info->info.global_conf.sym_hash_enable_mask[idx] |= (1UL << offset);
}

static int
set_xl710_nic(uint16_t port)
{
	int ret = 0;
	struct rte_eth_hash_filter_info info;

	ret = rte_eth_dev_filter_supported(port, RTE_ETH_FILTER_HASH);
	if (ret < 0) {
	    printf("RTE_ETH_FILTER_HASH not supported on port: %d\n", port);
	    return ret;
	}

	memset(&info, 0, sizeof(info));
	info.info_type = RTE_ETH_HASH_FILTER_GLOBAL_CONFIG;
	info.info.global_conf.hash_func = RTE_ETH_HASH_FUNCTION_TOEPLITZ;

	// i40e支持的所有flow type见drivers/net/i40e/i40e_ethdev.c, I40E_FLOW_TYPES
	//set_flow_type_mask(&info, RTE_ETH_FLOW_IPV4);
	// set_flow_type_mask(&info, RTE_ETH_FLOW_FRAG_IPV4);
	//set_flow_type_mask(&info, RTE_ETH_FLOW_IPV6);
	// set_flow_type_mask(&info, RTE_ETH_FLOW_FRAG_IPV6);
	// set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV6_OTHER);
	//set_flow_type_mask(&info, RTE_ETH_FLOW_IPV6_EX);
	set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV4_TCP);
	set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV4_UDP);
	set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV4_SCTP);
	set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV4_OTHER);
	// set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV6_TCP);
	//set_flow_type_mask(&info, RTE_ETH_FLOW_IPV6_TCP_EX);
	// set_flow_type_mask(&info, RTE_ETH_FLOW_NONFRAG_IPV6_UDP);
	//set_flow_type_mask(&info, RTE_ETH_FLOW_IPV6_UDP_EX);

	ret = rte_eth_dev_filter_ctrl(port, RTE_ETH_FILTER_HASH, RTE_ETH_FILTER_SET, &info);
	if (ret < 0) {
	    printf("Cannot set global hash configurations on port %u\n", port);
	    return ret;
	}

	memset(&info, 0, sizeof(info));
	info.info_type = RTE_ETH_HASH_FILTER_SYM_HASH_ENA_PER_PORT;
	info.info.enable = 1;
	ret = rte_eth_dev_filter_ctrl(port, RTE_ETH_FILTER_HASH, 
	                              RTE_ETH_FILTER_SET, &info);

	if (ret < 0) {
	    printf("Cannot set symmetric hash enable per port on port %u\n", port);
	    return ret;
	}
	
	return 0;
}

/*
 * Initialises a given port using global settings and with the rx buffers
 * coming from the mbuf_pool passed as parameter
 */
static inline int
smp_port_init(uint16_t port, struct rte_mempool *mbuf_pool,
	       uint16_t num_queues)
{
	struct rte_eth_conf port_conf = {
			.rxmode = {
				.mq_mode	= ETH_MQ_RX_RSS,
				.split_hdr_size = 0,
				.offloads = (DEV_RX_OFFLOAD_CHECKSUM | DEV_RX_OFFLOAD_CRC_STRIP),
			},
			.rx_adv_conf = {
				// .rss_conf = {
				// 	.rss_key = seed,
				// 	.rss_key_len = sizeof(seed),
				// 	// .rss_hf = ETH_RSS_IP | ETH_RSS_UDP | ETH_RSS_TCP,
				// 	.rss_hf = ETH_RSS_NONFRAG_IPV4_TCP | ETH_RSS_NONFRAG_IPV4_UDP |
				// 			  ETH_RSS_NONFRAG_IPV4_SCTP,
				// 	// .rss_hf = ETH_RSS_TCP | ETH_RSS_UDP | ETH_RSS_IP | ETH_RSS_SCTP,
				// },
				.rss_conf = {
				    .rss_hf = ETH_RSS_IP |
				          ETH_RSS_TCP |
				          ETH_RSS_UDP |
				          ETH_RSS_SCTP,
				}

			},
			.txmode = {
				.mq_mode = ETH_MQ_TX_NONE,
			}
	};
	const uint16_t rx_rings = num_queues, tx_rings = num_queues;
	struct rte_eth_dev_info info;
	struct rte_eth_rxconf rxq_conf;
	struct rte_eth_txconf txq_conf;
	int retval;
	uint16_t q;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	uint64_t rss_hf_tmp;

	if (rte_eal_process_type() == RTE_PROC_SECONDARY)
		return 0;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

	printf("# Initialising port %u... ", port);
	fflush(stdout);

	rte_eth_dev_info_get(port, &info);
	info.default_rxconf.rx_drop_en = 1;

	if (info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			DEV_TX_OFFLOAD_MBUF_FAST_FREE;

	rss_hf_tmp = port_conf.rx_adv_conf.rss_conf.rss_hf;
	port_conf.rx_adv_conf.rss_conf.rss_hf &= info.flow_type_rss_offloads;
	if (port_conf.rx_adv_conf.rss_conf.rss_hf != rss_hf_tmp) {
		printf("Port %u modified RSS hash function based on hardware support,"
			"requested:%#"PRIx64" configured:%#"PRIx64"\n",
			port,
			rss_hf_tmp,
			port_conf.rx_adv_conf.rss_conf.rss_hf);
	}


	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval < 0)
		return retval;

	set_xl710(port);
	// retval = set_xl710_nic(port);
	// if (retval < 0)
		// return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval < 0)
		return retval;

	rxq_conf = info.default_rxconf;
	rxq_conf.offloads = port_conf.rxmode.offloads;
	for (q = 0; q < rx_rings; q ++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port),
				&rxq_conf,
				mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txq_conf = info.default_txconf;
	txq_conf.offloads = port_conf.txmode.offloads;
	for (q = 0; q < tx_rings; q ++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port),
				&txq_conf);
		if (retval < 0)
			return retval;
	}

	rte_eth_promiscuous_enable(port);

	retval  = rte_eth_dev_start(port);
	if (retval < 0)
		return retval;

	return 0;
}

/* Goes through each of the lcores and calculates what ports should
 * be used by that core. Fills in the global lcore_ports[] array.
 */
static void
assign_ports_to_cores(void)
{

	const unsigned lcores = rte_eal_get_configuration()->lcore_count;
	const unsigned port_pairs = num_ports / 2;
	const unsigned pairs_per_lcore = port_pairs / lcores;
	unsigned extra_pairs = port_pairs % lcores;
	unsigned ports_assigned = 0;
	unsigned i;

	RTE_LCORE_FOREACH(i) {
		lcore_ports[i].start_port = ports_assigned;
		lcore_ports[i].num_ports = pairs_per_lcore * 2;
		if (extra_pairs > 0) {
			lcore_ports[i].num_ports += 2;
			extra_pairs--;
		}
		ports_assigned += lcore_ports[i].num_ports;
	}
}

unsigned long pkt_id = 0;
unsigned long long pkt_vol = 0;
struct timeval now;
struct timeval milestone;

/* Main function used by the processing threads.
 * Prints out some configuration details for the thread and then begins
 * performing packet RX and TX.
 */
static int
lcore_main(void *arg __rte_unused)
{
	const unsigned id = rte_lcore_id();
	const unsigned start_port = lcore_ports[id].start_port;
	const unsigned end_port = start_port + lcore_ports[id].num_ports;
	const uint16_t q_id = (uint16_t)proc_id;
	unsigned p, i;
	char msgbuf[256];
	int msgbufpos = 0;

	if (start_port == end_port){
		printf("Lcore %u has nothing to do\n", id);
		return 0;
	}

	/* build up message in msgbuf before printing to decrease likelihood
	 * of multi-core message interleaving.
	 */
	msgbufpos += snprintf(msgbuf, sizeof(msgbuf) - msgbufpos,
			"Lcore %u using ports ", id);
	for (p = start_port; p < end_port; p++){
		msgbufpos += snprintf(msgbuf + msgbufpos, sizeof(msgbuf) - msgbufpos,
				"%u ", (unsigned)ports[p]);
	}
	printf("%s\n", msgbuf);
	printf("lcore %u using queue %u of each port\n", id, (unsigned)q_id);

	/* handle packet I/O from the ports, reading and writing to the
	 * queue number corresponding to our process number (not lcore id)
	 */

	for (;;) {
		struct rte_mbuf *buf[PKT_BURST];

		for (p = start_port; p < end_port; p++) {
			const uint8_t src = ports[p];
			const uint8_t dst = ports[p ^ 1]; /* 0 <-> 1, 2 <-> 3 etc */
			const uint16_t rx_c = rte_eth_rx_burst(src, q_id, buf, PKT_BURST);
			if (rx_c == 0)
				continue;
			pstats[src].rx += rx_c;

			/* handle each of the recieved packets */
			uint16_t j;
			for (j = 0 ;j < rx_c; j++) {
				struct rte_mbuf* cur_buf = buf[j];
#ifdef PERF_DEBUG
				pkt_vol += rte_pktmbuf_pkt_len(cur_buf);
				if (pkt_id++ > 5000000) {
					gettimeofday(&now, NULL);
					time_t s = now.tv_sec - milestone.tv_sec;
					suseconds_t u = s * 1000000 + now.tv_usec - milestone.tv_usec;
					double throughput = (double)8000000*pkt_vol/(u*1000*1000*1000);
					// printf("time: %luus, pkt: %lu, vol: %lld, ", u, pkt_id, pkt_vol);
					printf("Lcore %d, Throughput: %lfGbps\n", id, throughput);
					milestone.tv_sec = now.tv_sec;
					milestone.tv_usec = now.tv_usec;
					pkt_id = 0;
					pkt_vol = 0;
				}
#endif
#ifdef RSS_DEBUG
				unsigned char* eth_hdr = rte_pktmbuf_mtod(cur_buf, unsigned char*);
				/* IPv4 only */
				if (eth_hdr[12] != 0x08 || eth_hdr[13] != 0x00) continue;
				printf("\n");

				struct iphdr* ip_hdr = (struct iphdr*)(eth_hdr+14);

				printf("ip hdr len: %u, ", 4 * ip_hdr->ihl);
				unsigned char* tcp_hdr = (unsigned char*)(ip_hdr + 4*ip_hdr->ihl);

				uint16_t sport, dport;
				memcpy(&sport, eth_hdr+14+4*ip_hdr->ihl, 2);
				memcpy(&dport, eth_hdr+14+4*ip_hdr->ihl+2, 2);

				struct in_addr addr1, addr2;
				memcpy(&addr1, &(ip_hdr->saddr), 4);
				memcpy(&addr2, &(ip_hdr->daddr), 4);
				printf("src ip: %s, ", inet_ntoa(addr1));
				printf("src port: %hu, ", ntohs(sport));
				printf("dst ip: %s, ", inet_ntoa(addr2));
				printf("dst port: %hu, ", ntohs(dport));
				printf(" rss hash = %u", cur_buf->hash.rss);
#endif
			}

			const uint16_t tx_c = rte_eth_tx_burst(dst, q_id, buf, rx_c);
			pstats[dst].tx += tx_c;
			if (tx_c != rx_c) {
				pstats[dst].drop += (rx_c - tx_c);
				for (i = tx_c; i < rx_c; i++)
					rte_pktmbuf_free(buf[i]);
			}
		}
	}
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint16_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t port;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (port = 0; port < port_num; port++) {
			if ((port_mask & (1 << port)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(port, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf(
					"Port%d Link Up. Speed %u Mbps - %s\n",
						port, link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n", port);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

/* Main function.
 * Performs initialisation and then calls the lcore_main on each core
 * to do the packet-processing work.
 */
int
main(int argc, char **argv)
{
	static const char *_SMP_MBUF_POOL = "SMP_MBUF_POOL";
	int ret;
	unsigned i;
	enum rte_proc_type_t proc_type;
	struct rte_mempool *mp;

	/* set up signal handlers to print stats on exit */
	signal(SIGINT, print_stats);
	signal(SIGTERM, print_stats);

	/* initialise the EAL for all */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Cannot init EAL\n");
	argc -= ret;
	argv += ret;

	/* determine the NIC devices available */
	if (rte_eth_dev_count_avail() == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	/* parse application arguments (those after the EAL ones) */
	smp_parse_args(argc, argv);

	proc_type = rte_eal_process_type();
	mp = (proc_type == RTE_PROC_SECONDARY) ?
			rte_mempool_lookup(_SMP_MBUF_POOL) :
			rte_pktmbuf_pool_create(_SMP_MBUF_POOL, NB_MBUFS,
				MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
				rte_socket_id());
	if (mp == NULL)
		rte_exit(EXIT_FAILURE, "Cannot get memory pool for buffers\n");

	if (num_ports & 1)
		rte_exit(EXIT_FAILURE, "Application must use an even number of ports\n");
	for(i = 0; i < num_ports; i++){
		if(proc_type == RTE_PROC_PRIMARY)
			if (smp_port_init(ports[i], mp, (uint16_t)num_procs) < 0)
				rte_exit(EXIT_FAILURE, "Error initialising ports\n");
	}

	if (proc_type == RTE_PROC_PRIMARY)
		check_all_ports_link_status((uint8_t)num_ports, (~0x0));

	assign_ports_to_cores();

	RTE_LOG(INFO, APP, "Finished Process Init.\n");

#ifdef PERF_DEBUG
	gettimeofday(&milestone, NULL);
#endif

	rte_eal_mp_remote_launch(lcore_main, NULL, CALL_MASTER);

	return 0;
}
