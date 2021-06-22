/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Eideticom Inc.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Eideticom Inc,  nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "spdk/stdinc.h"

#include "spdk/env.h"
#include "spdk/nvme.h"
#include "spdk/string.h"
#include "spdk/trace.h"

// LTTng tracing
#include "cmb_copy-tp.h"

#define CMB_COPY_DELIM "-"
#define CMB_COPY_READ 0
#define CMB_COPY_WRITE 1

struct nvme_io {
	struct spdk_nvme_ctrlr *ctrlr;
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_qpair	*qpair;
	struct spdk_nvme_ns *ns;
	unsigned nsid;
	unsigned slba;
	unsigned nlbas;
	uint32_t lba_size;
	unsigned done;
};

struct cmb_t {
	struct spdk_nvme_transport_id trid;
	struct spdk_nvme_ctrlr *ctrlr;
};

struct config {
	struct nvme_io read;
	struct nvme_io write;
	struct cmb_t   cmb;
	size_t         copy_size;
};

static struct config g_config;

/* Namespaces index from 1. Return 0 to invoke an error */
static unsigned
get_nsid(const struct spdk_nvme_transport_id *trid)
{
	if (!strcmp(trid->traddr, g_config.read.trid.traddr)) {
		return g_config.read.nsid;
	}
	if (!strcmp(trid->traddr, g_config.write.trid.traddr)) {
		return g_config.write.nsid;
	}
	return 0;
}

static int
get_rw(const struct spdk_nvme_transport_id *trid)
{
	if (!strcmp(trid->traddr, g_config.read.trid.traddr)) {
		return CMB_COPY_READ;
	}
	if (!strcmp(trid->traddr, g_config.write.trid.traddr)) {
		return CMB_COPY_WRITE;
	}
	return -1;
}

static void
check_io(void *arg, const struct spdk_nvme_cpl *completion)
{
	int *rw = (unsigned *)arg;

	if (*rw == CMB_COPY_READ) {
		g_config.read.done = 1;
	} else {
		g_config.write.done = 1;
	}
}

#define SIMPLE_TRACING_ON 1

#if SIMPLE_TRACING_ON
static int tracing_index = 0;
static char* message_buffer[128] = {NULL,};
static uint64_t time_point_buffer[128] = {0,};
#endif

inline
static void simplistic_tracing(const char* message) {
// empty function call will be removed by compiler
#if SIMPLE_TRACING_ON
	time_point_buffer[tracing_index] = spdk_get_ticks();
	message_buffer[tracing_index] = message;
	tracing_index++;
#endif
}

static void print_tracing(void) {
#if SIMPLE_TRACING_ON
	for (int i = 0; i < tracing_index; ++i) {
		if (i) {
			printf("%s, time : %lu [ns], delta : %lu [ns]\n", message_buffer[i] ? message_buffer[i] : "", time_point_buffer[i], time_point_buffer[i]-time_point_buffer[i-1]);
		} else {
			printf("%s, time : %lu [ns]\n", message_buffer[i] ? message_buffer[i] : "", time_point_buffer[i]);
		}
	}
#endif
}

static int
cmb_copy(void)
{
	int rc = 0, rw;
	void *buf;
	size_t sz;

	/* Allocate QPs for the read and write controllers */
	g_config.read.qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_config.read.ctrlr, NULL, 0);
	g_config.write.qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_config.write.ctrlr, NULL, 0);
	if (g_config.read.qpair == NULL || g_config.read.qpair == NULL) {
		printf("ERROR: spdk_nvme_ctrlr_alloc_io_qpair() failed\n");
		return -ENOMEM;
	}

	/* Allocate a buffer from our CMB */
	buf = spdk_nvme_ctrlr_map_cmb(g_config.cmb.ctrlr, &sz);
	if (buf == NULL || sz < g_config.copy_size) {
		printf("ERROR: buffer allocation failed\n");
		printf("Are you sure %s has a valid CMB?\n",
		       g_config.cmb.trid.traddr);
		return -ENOMEM;
	}

	/* Clear the done flags */
	g_config.read.done = 0;
	g_config.write.done = 0;

	rw = CMB_COPY_READ;
	/* Do the read to the CMB IO buffer */
	simplistic_tracing("Sending read request");
	rc = spdk_nvme_ns_cmd_read(g_config.read.ns, g_config.read.qpair, buf,
				   g_config.read.slba, g_config.read.nlbas,
				   check_io, &rw, 0);
	simplistic_tracing("Read request sent");
	if (rc != 0) {
		fprintf(stderr, "starting read I/O failed\n");
		return -EIO;
	}
	while (!g_config.read.done) {
		spdk_nvme_qpair_process_completions(g_config.read.qpair, 0);
	}
	simplistic_tracing("Read request completed");

	/* Do the write from the CMB IO buffer */
	rw = CMB_COPY_WRITE;
	simplistic_tracing("Sending write request");
	rc = spdk_nvme_ns_cmd_write(g_config.write.ns, g_config.write.qpair, buf,
				    g_config.write.slba, g_config.write.nlbas,
				    check_io, &rw, 0);
	simplistic_tracing("Write request sent");
	if (rc != 0) {
		fprintf(stderr, "starting write I/O failed\n");
		return -EIO;
	}
	while (!g_config.write.done) {
		spdk_nvme_qpair_process_completions(g_config.write.qpair, 0);
	}
	simplistic_tracing("Write request completed");

	// Print the tracing
	print_tracing();

	/* Clear the done flags */
	g_config.read.done = 0;
	g_config.write.done = 0;

	/* Free CMB buffer */
	spdk_nvme_ctrlr_unmap_cmb(g_config.cmb.ctrlr);

	/* Free the queues */
	spdk_nvme_ctrlr_free_io_qpair(g_config.read.qpair);
	spdk_nvme_ctrlr_free_io_qpair(g_config.write.qpair);

	return rc;
}

static bool
probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	 struct spdk_nvme_ctrlr_opts *opts)
{
	/* We will only attach to the read or write controller */
	if (strcmp(trid->traddr, g_config.read.trid.traddr) &&
	    strcmp(trid->traddr, g_config.write.trid.traddr)) {
		printf("%s - not probed %s!\n", __func__, trid->traddr);
		return 0;
	}

	opts->use_cmb_sqs = false;

	printf("%s - probed %s!\n", __func__, trid->traddr);
	return 1;
}

static void
attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
	  struct spdk_nvme_ctrlr *ctrlr, const struct spdk_nvme_ctrlr_opts *opts)
{
	struct spdk_nvme_ns *ns;

	ns = spdk_nvme_ctrlr_get_ns(ctrlr, get_nsid(trid));
	if (ns == NULL) {
		fprintf(stderr, "Could not locate namespace %d on controller %s.\n",
			get_nsid(trid), trid->traddr);
		exit(-1);
	}
	if (get_rw(trid) == CMB_COPY_READ) {
		g_config.read.ctrlr    = ctrlr;
		g_config.read.ns       = ns;
		g_config.read.lba_size = spdk_nvme_ns_get_sector_size(ns);
	} else {
		g_config.write.ctrlr    = ctrlr;
		g_config.write.ns       = ns;
		g_config.write.lba_size = spdk_nvme_ns_get_sector_size(ns);
	}
	printf("%s - attached %s!\n", __func__, trid->traddr);

	return;
}

static void
usage(char *program_name)
{
	printf("%s options (all mandatory)", program_name);
	printf("\n");
	printf("\t[-r NVMe read parameters]\n");
	printf("\t[-w NVMe write parameters]\n");
	printf("\t[-c CMB to use for data buffers]\n");
	printf("\n");
	printf("Read/Write params:\n");
	printf("  <pci id>-<namespace>-<start LBA>-<number of LBAs>\n");
}

static void
parse(char *in, struct nvme_io *io)
{
	char *tok = NULL;
	long int val;

	tok = strtok(in, CMB_COPY_DELIM);
	if (tok == NULL) {
		goto err;
	}
	snprintf(&io->trid.traddr[0], SPDK_NVMF_TRADDR_MAX_LEN + 1,
		 "%s", tok);

	tok = strtok(NULL, CMB_COPY_DELIM);
	if (tok == NULL) {
		goto err;
	}
	val = spdk_strtol(tok, 10);
	if (val < 0) {
		goto err;
	}
	io->nsid  = (unsigned)val;

	tok = strtok(NULL, CMB_COPY_DELIM);
	if (tok == NULL) {
		goto err;
	}
	val = spdk_strtol(tok, 10);
	if (val < 0) {
		goto err;
	}
	io->slba  = (unsigned)val;

	tok = strtok(NULL, CMB_COPY_DELIM);
	if (tok == NULL) {
		goto err;
	}
	val = spdk_strtol(tok, 10);
	if (val < 0) {
		goto err;
	}
	io->nlbas = (unsigned)val;

	tok = strtok(NULL, CMB_COPY_DELIM);
	if (tok != NULL) {
		goto err;
	}
	return;

err:
	fprintf(stderr, "%s: error parsing %s\n", __func__, in);
	exit(-1);

}

static int
parse_args(int argc, char **argv)
{
	int op;
	unsigned read = 0, write = 0, cmb = 0;

	while ((op = getopt(argc, argv, "r:w:c:")) != -1) {
		switch (op) {
		case 'r':
			parse(optarg, &g_config.read);
			read = 1;
			break;
		case 'w':
			parse(optarg, &g_config.write);
			write = 1;
			break;
		case 'c':
			snprintf(g_config.cmb.trid.traddr, SPDK_NVMF_TRADDR_MAX_LEN + 1,
				 "%s", optarg);
			cmb = 1;
			break;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if ((!read || !write || !cmb)) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	int rc = 0;
	struct spdk_env_opts opts;

	/*
	 * Parse the input arguments. For now we use the following
	 * format list:
	 *
	 * <pci id>-<namespace>-<start LBA>-<number of LBAs>
	 *
	 */
	rc = parse_args(argc, argv);
	if (rc) {
		fprintf(stderr, "Error in parse_args(): %d\n",
			rc);
		return -1;
	}

	/*
	 * SPDK relies on an abstraction around the local environment
	 * named env that handles memory allocation and PCI device operations.
	 * This library must be initialized first.
	 *
	 */
	spdk_env_opts_init(&opts);
	opts.name = "cmb_copy";
	opts.shm_id = 0;
	if (spdk_env_init(&opts) < 0) {
		fprintf(stderr, "Unable to initialize SPDK env\n");
		return 1;
	}

	// LTTng
    /*
     * A tracepoint() call.
     *
     * Arguments, as defined in cmb_copy-tp.h:
     *
     * 1. Tracepoint provider name   (required)
     * 2. Tracepoint name            (required)
     * 3. my_integer_arg             (first user-defined argument)
     * 4. my_string_arg              (second user-defined argument)
     *
     * Notice the tracepoint provider and tracepoint names are
     * NOT strings: they are in fact parts of variables that the
     * macros in hello-tp.h create.
     */
    tracepoint(cmb_copy, my_first_tracepoint, 42, "The owls are not what they seem...");
	for (int x = 0; x < argc; ++x) {
        tracepoint(cmb_copy, my_first_tracepoint, x, argv[x]);
    }

#if SIMPLE_TRACING_ON
#else
	/*
	 * Setup tracing (this is similar to app_setup_trace from app.c)
	 */
	char		shm_name[64];
	uint64_t	tpoint_group_mask = -1; /* All tracegroups, mask 0xFFF...F */
	if (opts.shm_id >= 0) {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.%d", opts.name, opts.shm_id);
	} else {
		snprintf(shm_name, sizeof(shm_name), "/%s_trace.pid%d", opts.name, (int)getpid());
	}

	/*                        Number of elements in circular buffer */
	if (spdk_trace_init(shm_name, 1000 /*opts->num_entries*/) != 0) {
		fprintf(stderr, "Unable to initialize SPDK trace\n");
		return -1;
	}

	spdk_trace_set_tpoint_group_mask(tpoint_group_mask);
#endif

	/*
	 * CMBs only apply to PCIe attached NVMe controllers so we
	 * only probe the PCIe bus. This is the default when we pass
	 * in NULL for the first argument.
	 */

	rc = spdk_nvme_probe(NULL, NULL, probe_cb, attach_cb, NULL);
	if (rc) {
		fprintf(stderr, "Error in spdk_nvme_probe(): %d\n",
			rc);
		return -1;
	}

	/*
	 * For now enforce that the read and write controller are not
	 * the same. This avoids an internal only DMA.
	 */
	if (!strcmp(g_config.write.trid.traddr, g_config.read.trid.traddr)) {
		fprintf(stderr, "Read and Write controllers must differ!\n");
		return -1;
	}

	/*
	 * Perform a few sanity checks and set the buffer size for the
	 * CMB.
	 */
	if (g_config.read.nlbas * g_config.read.lba_size !=
	    g_config.write.nlbas * g_config.write.lba_size) {
		fprintf(stderr, "Read and write sizes do not match!\n");
		return -1;
	}
	g_config.copy_size = g_config.read.nlbas * g_config.read.lba_size;

	/*
	 * Get the ctrlr pointer for the CMB. For now we assume this
	 * is either the read or write NVMe controller though in
	 * theory that is not a necessary condition.
	 */

	if (!strcmp(g_config.cmb.trid.traddr, g_config.read.trid.traddr)) {
		g_config.cmb.ctrlr = g_config.read.ctrlr;
	}
	if (!strcmp(g_config.cmb.trid.traddr, g_config.write.trid.traddr)) {
		g_config.cmb.ctrlr = g_config.write.ctrlr;
	}

	/*
	 * Call the cmb_copy() function which performs the CMB
	 * based copy or returns an error code if it fails.
	 */
	rc = cmb_copy();
	if (rc) {
		fprintf(stderr, "Error in spdk_cmb_copy(): %d\n",
			rc);
		return -1;
	}

#if SIMPLE_TRACING_ON
#else
	/*
	 * Cleanup
	 */
	spdk_trace_cleanup();
#endif

	return rc;
}
