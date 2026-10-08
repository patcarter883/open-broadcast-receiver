/* librist. Copyright © 2020 SipRadius LLC. All right reserved.
 * Author: Gijs Peskens
 * Author: Sergio Ammirata, Ph.D. <sergio@ammirata.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* rist2rist receive simple profile rist and expose it as main profile */

#include <librist/librist.h>
#include "librist/version.h"
#include "risturlhelp.h"
#include "config.h"
#if HAVE_SRP_SUPPORT
#include "librist/librist_srp.h"
#include "srp_shared.h"
#endif
#if HAVE_PROMETHEUS_SUPPORT
#include "prometheus-exporter.h"
#endif
#include "vcs_version.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "getopt-shim.h"
#include <assert.h>
#include <signal.h>
#ifdef __unix
#include <unistd.h>
#endif
#include "oob_shared.h"
#include "string-shim.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define RIST2RIST_VERSION "31"

#define RTT_HYSTERESIS_MS 15
#define MAX_SENDER_PEERS  8

struct __attribute__((__packed__)) wan_telemetry_t {
	uint8_t  link_quality;     // 0..100
	uint32_t worst_case_rtt;   // ms, network byte order
};

struct sender_peer_stat {
	uint32_t peer_id;
	int      in_use;
	double   quality;
	uint32_t rtt;
	size_t   bandwidth;
};

/* All telemetry globals are touched only from the librist worker thread
 * (cb_auth_connect, cb_auth_disconnect, cb_stats), so no locking needed. */
static struct sender_peer_stat g_sender_peers[MAX_SENDER_PEERS];
static uint8_t  g_last_link_quality = 0xFFu;       /* sentinel: forces first emit */
static uint32_t g_last_rtt          = 0xFFFFFFFFu; /* sentinel: forces first emit */
static struct rist_ctx  *g_lan_receiver_ctx  = NULL;
static struct rist_peer *g_lan_receiver_peer = NULL;
/* Quality of the encoder->rist2rist (input) leg, from RIST_STATS_RECEIVER_FLOW.
 * The sender-peer quality only covers the rist2rist->downstream legs, so loss on
 * the input leg is invisible there; we report the worst of the two. */
static double   g_lan_recv_quality  = 100.0;

struct rist_sender_args {
	char* cname;
	char* shared_secret;
	char* outputurl;
	uint16_t dst_port;
	enum rist_log_level loglevel;
	int encryption_type;
	uint32_t flow_id;
	int statsinterval;
	int npd_enabled;
	enum rist_profile profile;
};

struct rist_cb_arg {
	uint16_t src_port;
	uint16_t dst_port;
	struct rist_ctx *sender_ctx;
	struct rist_sender_args *client_args;
};

static int keep_running = 1;
static struct rist_logging_settings logging_settings = LOGGING_SETTINGS_INITIALIZER;

#if HAVE_PROMETHEUS_SUPPORT
static struct rist_prometheus_stats *prom_stats_ctx = NULL;
static bool prometheus_httpd = false;
static bool enable_prometheus = false;
static char *prometheus_tags = NULL;
static uint16_t prometheus_port = 9100;
static char *prometheus_ip = NULL;
#endif

static struct option long_options[] = {
{ "inurl",           required_argument, NULL, 'i' },
{ "outurl",          required_argument, NULL, 'o' },
{ "secret",          required_argument, NULL, 's' },
{ "encryption-type", required_argument, NULL, 'e' },
{ "cname",           required_argument, NULL, 'N' },
{ "statsinterval",   required_argument, NULL, 'S' },
{ "verbose-level",   required_argument, NULL, 'v' },
{ "remote-logging",  required_argument, NULL, 'r' },
{ "profile",         required_argument, NULL, 'p' },
{ "out-profile",     required_argument, NULL, 'P' },
{ "npd",             no_argument,       NULL, 'n' },
#if HAVE_SRP_SUPPORT
{ "srpfile",         required_argument, NULL, 'F' },
#endif
#if HAVE_PROMETHEUS_SUPPORT
{ "enable-metrics",  no_argument,       NULL, 'M' },
{ "metrics-tags",    required_argument, NULL, 1 },
#if HAVE_LIBMICROHTTPD
{ "metrics-http",    no_argument,       NULL, 4 },
{ "metrics-port",    required_argument, NULL, 2 },
{ "metrics-ip",      required_argument, NULL, 3 },
#endif
#endif
{ "help",            no_argument,       NULL, 'h' },
{ 0, 0, 0, 0 },
};

const char help_str[] = "Where OPTIONS are:\n"
"       -i | --inurl  ADDRESS:PORT[,ADDR:PORT]* * | Input URL (comma-separated list for source bonding)      |\n"
"       -o | --outurl ADDRESS:PORT[,ADDR:PORT]* * | Output URL (comma-separated list for destination fanout) |\n"
"       -s | --secret PWD                         | Pre-shared encryption secret                             |\n"
"       -e | --encryption-type TYPE               | Encryption type (0 = none, 1 = AES-128, 2 = AES-256)     |\n"
"       -S | --statsinterval value (ms)           | Interval at which stats get printed, 0 to disable        |\n"
"       -N | --cname identifier                   | Manually configured identifier                           |\n"
"       -v | --verbose-level value                | To disable logging: -1, log levels match syslog levels   |\n"
"       -r | --remote-logging IP:PORT             | Send logs and stats to this IP:PORT using udp messages   |\n"
"       -p | --profile number                     | Rist receive profile (0 = simple, 1 = main, 2 = advanced)|\n"
"       -P | --out-profile number                 | Rist output profile  (0 = simple, 1 = main, 2 = advanced)|\n"
"       -n | --npd                                | Enable Null Packet Deletion on output                    |\n"
#if HAVE_SRP_SUPPORT
"       -F | --srpfile filepath                   | When in listening mode, use this file to hold the list   |\n"
"                                                 | of usernames and passwords to validate against. Use the  |\n"
"                                                 | ristsrppasswd tool to create the line entries.           |\n"
#endif
#if HAVE_PROMETHEUS_SUPPORT
"       -M | --enable-metrics                     | Enable OpenMetrics/Prometheus compatible metrics         |\n"
"          | --metrics-tags                       | Additional tags to add to the metrics                    |\n"
#if HAVE_LIBMICROHTTPD
"          | --metrics-http                       | Start HTTP server to expose metrics                      |\n"
"          | --metrics-port                       | Port for metrics HTTP server (default: 9100)             |\n"
"          | --metrics-ip                         | IP for metrics HTTP server (default: 0.0.0.0)            |\n"
#endif
#endif
"       -h | --help                               | Show this help                                           |\n"
"       -u | --help-url                           | Show all the possible url options                        |\n"
"   * == mandatory value \n"
"Default values:\n"
"       --profile 0               \\\n"
"       --statsinterval 1000      \\\n"
"       --verbose-level 6         \n";

#if HAVE_SRP_SUPPORT
	char *srpfile = NULL;
#endif

static void usage(char *cmd)
{
	rist_log(&logging_settings, RIST_LOG_INFO,
		"rist2rist version %s libRIST library: %s API version: %s\n"
		"Usage: %s [OPTIONS]\n%s",
		RIST2RIST_VERSION, librist_version(), librist_api_version(),
		cmd, help_str);
	exit(1);
}

static int cb_auth_connect(void *arg, const char* connecting_ip, uint16_t connecting_port, const char* local_ip, uint16_t local_port, struct rist_peer *peer)
{
	struct rist_ctx *receiver_ctx = (struct rist_ctx *)arg;
	uint16_t buffer[250];
	char message[200];
	int message_len = snprintf(message, 200, "auth,%s:%d,%s:%d", connecting_ip, connecting_port, local_ip, local_port);
	// To be compliant with the spec, the message must have an ipv4 header
	int ret = oob_build_api_payload(buffer, (char *)connecting_ip, (char *)local_ip, message, message_len);
	rist_log(&logging_settings, RIST_LOG_INFO,"Peer has been authenticated, sending oob/api message: %s\n", message);
	struct rist_oob_block oob_block;
	oob_block.peer = peer;
	oob_block.payload = buffer;
	oob_block.payload_len = ret;
	rist_oob_write(receiver_ctx, &oob_block);
	/* Track the LAN-side peer so we can write WAN telemetry back over its OOB channel. */
	if (arg == g_lan_receiver_ctx) {
		g_lan_receiver_peer = peer;
	}
	return 0;
}

static int cb_auth_disconnect(void *arg, struct rist_peer *peer)
{
	if (arg == g_lan_receiver_ctx && peer == g_lan_receiver_peer) {
		g_lan_receiver_peer = NULL;
	}
	return 0;
}

static int cb_recv_oob(void *arg, const struct rist_oob_block *oob_block)
{
	struct rist_ctx *ctx = (struct rist_ctx *)arg;
	(void)ctx;
	int message_len = 0;
	char *message = oob_process_api_message((int)oob_block->payload_len, (char *)oob_block->payload, &message_len);
	if (message) {
		rist_log(&logging_settings, RIST_LOG_INFO,"Out-of-band api data received: %.*s\n", message_len, message);
	}
	return 0;
}

static void telemetry_emit(void)
{
	if (!g_lan_receiver_ctx || !g_lan_receiver_peer) {
		return;
	}

	/* Aggregate quality across all known sender (downstream/WAN) peers. */
	double   weighted_q_sum = 0.0;
	double   q_sum          = 0.0;
	size_t   total_bw       = 0;
	uint32_t max_rtt        = 0;
	int      n_peers        = 0;
	for (int i = 0; i < MAX_SENDER_PEERS; i++) {
		if (!g_sender_peers[i].in_use) continue;
		n_peers++;
		weighted_q_sum += g_sender_peers[i].quality * (double)g_sender_peers[i].bandwidth;
		q_sum          += g_sender_peers[i].quality;
		total_bw       += g_sender_peers[i].bandwidth;
		if (g_sender_peers[i].rtt > max_rtt) {
			max_rtt = g_sender_peers[i].rtt;
		}
	}

	/* Output-leg quality (100 until a downstream peer reports in). */
	double q_out = 100.0;
	if (n_peers > 0) {
		q_out = (total_bw > 0) ? (weighted_q_sum / (double)total_bw)
		                       : (q_sum / (double)n_peers);
	}

	/* Report the worst of the input (encoder->rist2rist) and output legs, so
	 * loss on EITHER leg pulls the encoder's WAN quality down. */
	double q_final = (q_out < g_lan_recv_quality) ? q_out : g_lan_recv_quality;
	if (q_final < 0.0)   q_final = 0.0;
	if (q_final > 100.0) q_final = 100.0;
	uint8_t  link_quality = (uint8_t)(q_final + 0.5);
	uint32_t rtt          = max_rtt;

	int quality_changed = (link_quality != g_last_link_quality);
	int rtt_changed     = (abs((int)rtt - (int)g_last_rtt) > RTT_HYSTERESIS_MS);
	if (!quality_changed && !rtt_changed) {
		return;
	}

	struct wan_telemetry_t telemetry;
	telemetry.link_quality   = link_quality;
	telemetry.worst_case_rtt = htonl(rtt);

	/* The encoder expects the RAW 5-byte wan_telemetry struct on the wire — it
	 * rejects any OOB payload whose size != sizeof(wan_telemetry) (see
	 * open-broadcast-encoder source/lib/lib.h / source/main.cpp rist_oob_cb).
	 * Do NOT wrap it in the IP/API envelope used for auth messages. */
	struct rist_oob_block oob_block = {
		.peer        = g_lan_receiver_peer,
		.payload     = &telemetry,
		.payload_len = sizeof(telemetry),
		.ts_ntp      = 0,
	};
	rist_oob_write(g_lan_receiver_ctx, &oob_block);

	g_last_link_quality = link_quality;
	g_last_rtt          = rtt;

	rist_log(&logging_settings, RIST_LOG_DEBUG,
		"telemetry: q=%u (out=%.1f in=%.1f) rtt=%u ms peers=%d\n",
		link_quality, q_out, g_lan_recv_quality, rtt, n_peers);
}

static void wan_telemetry_update(const struct rist_stats_sender_peer *sp)
{
	/* Find or insert this peer in the per-peer cache. */
	int slot = -1;
	int first_free = -1;
	for (int i = 0; i < MAX_SENDER_PEERS; i++) {
		if (g_sender_peers[i].in_use && g_sender_peers[i].peer_id == sp->peer_id) {
			slot = i;
			break;
		}
		if (!g_sender_peers[i].in_use && first_free < 0) {
			first_free = i;
		}
	}
	if (slot < 0) {
		if (first_free < 0) {
			return; /* table full — drop silently */
		}
		slot = first_free;
		g_sender_peers[slot].in_use = 1;
		g_sender_peers[slot].peer_id = sp->peer_id;
	}
	g_sender_peers[slot].quality   = sp->quality;
	g_sender_peers[slot].rtt       = sp->rtt;
	g_sender_peers[slot].bandwidth = sp->bandwidth;

	telemetry_emit();
}

static int cb_stats(void *arg, const struct rist_stats *stats_container) {
	rist_log(&logging_settings, RIST_LOG_INFO, "%s\n\n", stats_container->stats_json);
#if HAVE_PROMETHEUS_SUPPORT
	if (prom_stats_ctx) {
		rist_prometheus_parse_stats(prom_stats_ctx, stats_container, (uintptr_t)arg);
	}
#else
	(void)arg;
#endif
	if (stats_container->stats_type == RIST_STATS_SENDER_PEER) {
		wan_telemetry_update(&stats_container->stats.sender_peer);
	} else if (stats_container->stats_type == RIST_STATS_RECEIVER_FLOW) {
		/* Quality of the encoder->rist2rist (input) leg. Captures loss the
		 * downstream sender-peer stats can't see; folded into the telemetry. */
		double q = stats_container->stats.receiver_flow.quality;
		if (q < 0.0)   q = 0.0;
		if (q > 100.0) q = 100.0;
		g_lan_recv_quality = q;
		telemetry_emit();
	}
	rist_stats_free(stats_container);
	return 0;
}

static struct rist_ctx* setup_rist_sender(struct rist_sender_args *setup) {
	struct rist_ctx *ctx;
	printf("CName: %s\n", setup->cname);
	printf("Outurl: %s\n", setup->outputurl);
	int rist;
	if (rist_sender_create(&ctx, setup->profile, setup->flow_id, &logging_settings) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create rist sender context\n");
		exit(1);
	}

	rist = rist_auth_handler_set(ctx, cb_auth_connect, cb_auth_disconnect, ctx);
	if (rist < 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not initialize rist auth handler\n");
		exit(1);
	}

	if (rist_oob_callback_set(ctx, cb_recv_oob, ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add enable out-of-band data\n");
		exit(1);
	}

	if (setup->statsinterval) {
		/* Pass ctx pointer as user data so the Prometheus id is unique per
		 * context (receiver vs. sender) when both are active in the same
		 * process. */
		rist_stats_callback_set(ctx, setup->statsinterval, cb_stats, ctx);
	}

	if (setup->npd_enabled) {
		if (rist_sender_npd_enable(ctx) != 0) {
			rist_log(&logging_settings, RIST_LOG_WARN, "Could not enable null-packet-deletion on sender\n");
		} else {
			rist_log(&logging_settings, RIST_LOG_INFO, "Null-packet-deletion enabled on sender\n");
		}
	}

	int keysize = setup->encryption_type * 128;

	/* Tokenize the output URL list on ',' and add one sender peer per token.
	 * Matches the convention used by ristsender/ristreceiver. */
	char *saveptr = NULL;
	char *outtoken = strtok_r(setup->outputurl, ",", &saveptr);
	int peer_count = 0;
	while (outtoken) {
		/* Applications defaults reset per peer (URL parsing mutates config). */
		struct rist_peer_config app_peer_config = {
			.version = RIST_PEER_CONFIG_VERSION,
			.virt_dst_port = setup->dst_port,
			.recovery_mode = RIST_DEFAULT_RECOVERY_MODE,
			.recovery_maxbitrate = RIST_DEFAULT_RECOVERY_MAXBITRATE,
			.recovery_maxbitrate_return = RIST_DEFAULT_RECOVERY_MAXBITRATE_RETURN,
			.recovery_length_min = RIST_DEFAULT_RECOVERY_LENGTH_MIN,
			.recovery_length_max = RIST_DEFAULT_RECOVERY_LENGTH_MAX,
			.recovery_reorder_buffer = RIST_DEFAULT_RECOVERY_REORDER_BUFFER,
			.recovery_rtt_min = RIST_DEFAULT_RECOVERY_RTT_MIN,
			.recovery_rtt_max = RIST_DEFAULT_RECOVERY_RTT_MAX,
			.weight = 5,
			.congestion_control_mode = RIST_DEFAULT_CONGESTION_CONTROL_MODE,
			.min_retries = RIST_DEFAULT_MIN_RETRIES,
			.max_retries = RIST_DEFAULT_MAX_RETRIES,
			.key_size = keysize,
		};

		if (setup->shared_secret != NULL) {
			strncpy(app_peer_config.secret, setup->shared_secret, RIST_MAX_STRING_SHORT -1);
		}

		if (setup->cname != NULL) {
			strncpy(app_peer_config.cname, setup->cname, RIST_MAX_STRING_SHORT -1);
		}

		/* URL overrides (also cleans up the URL) */
		struct rist_peer_config *peer_config = &app_peer_config;
		if (rist_parse_address2(outtoken, &peer_config)) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse peer options for sender: %s\n", outtoken);
			exit(1);
		}

		struct rist_peer *peer;
		if (rist_peer_create(ctx, &peer, peer_config) == -1) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add peer connector to sender: %s\n", outtoken);
			exit(1);
		}

#if HAVE_SRP_SUPPORT
		int srp_error = 0;
		if (strlen(peer_config->srp_username) > 0 && strlen(peer_config->srp_password) > 0)
		{
			srp_error = rist_enable_eap_srp_2(peer, peer_config->srp_username, peer_config->srp_password, NULL, NULL);
			if (srp_error)
				rist_log(&logging_settings, RIST_LOG_WARN, "Error %d trying to enable SRP for peer\n", srp_error);
		}
		if (srpfile)
		{
			srp_error = rist_enable_eap_srp_2(peer, NULL, NULL, user_verifier_lookup, srpfile);
			if (srp_error)
				rist_log(&logging_settings, RIST_LOG_WARN, "Error %d trying to enable SRP global authenticator, file %s\n", srp_error, srpfile);
		}
#endif
		peer_count++;
		outtoken = strtok_r(NULL, ",", &saveptr);
	}

	if (peer_count == 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "No valid output peer URLs provided\n");
		exit(1);
	}

	if (rist_start(ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start rist sender\n");
		exit(1);
	}
	return ctx;
}

static int cb_recv(void *arg, struct rist_data_block *b)
{
	struct rist_cb_arg *cb_arg = (struct rist_cb_arg *) arg;
	struct rist_data_block *block = (struct rist_data_block*)b;
	if (cb_arg->client_args->flow_id != b->flow_id) {
		printf("Flow ID %ud\n",b->flow_id);
		cb_arg->client_args->flow_id = b->flow_id;
		assert(cb_arg->sender_ctx != NULL);
		rist_sender_flow_id_set(cb_arg->sender_ctx, b->flow_id);
	}
	b->virt_src_port = cb_arg->src_port;
	b->virt_dst_port = cb_arg->dst_port;
	block->flags = RIST_DATA_FLAGS_USE_SEQ;//We only need this flag set, this way we don't have to null it beforehand.
	int ret = rist_sender_data_write(cb_arg->sender_ctx, b);
	rist_receiver_data_block_free2(&b);
	return ret;
}

static void intHandler(int signal) {
	rist_log(&logging_settings, RIST_LOG_NOTICE, "Signal %d received\n", signal);
	keep_running = 0;
}

int main (int argc, char **argv) {
	char *inputurl = NULL;
	char *cname = NULL;
	char *outputurl = NULL;
	struct rist_cb_arg cb_arg;
	struct rist_sender_args client_args;
	cb_arg.client_args = &client_args;
	cb_arg.src_port = 1971;
	cb_arg.dst_port = 1968;
	client_args.dst_port = 1968;
	client_args.encryption_type = 0;
	client_args.shared_secret = NULL;
	client_args.flow_id = 0;
	client_args.npd_enabled = 0;
	int statsinterval = 1000;
	enum rist_log_level loglevel = RIST_LOG_INFO;
	char *remote_log_address = NULL;
	int exitcode = 0;
	enum rist_profile profile = RIST_PROFILE_SIMPLE;
	// Output (downstream) profile, independent of the input receive profile.
	// Default to advanced so it matches an advanced-profile downstream receiver.
	enum rist_profile out_profile = RIST_PROFILE_ADVANCED;
#ifdef _WIN32
#define STDERR_FILENO 2
	signal(SIGINT, intHandler);
	signal(SIGTERM, intHandler);
	signal(SIGABRT, intHandler);
#else
	struct sigaction act = { {0} };
	act.sa_handler = intHandler;
	act.sa_flags = 0;
	sigaction(SIGINT, &act, NULL);
#endif

	// Default log settings
	struct rist_logging_settings *log_ptr = &logging_settings;
	if (rist_logging_set(&log_ptr, loglevel, NULL, NULL, NULL, stderr) != 0) {
		fprintf(stderr,"Failed to setup default logging!\n");
		exit(1);
	}

	rist_log(&logging_settings, RIST_LOG_INFO, "Starting rist2rist version: %s libRIST library: %s API version: %s\n", RIST2RIST_VERSION, librist_version(), librist_api_version());

	int option_index;
	int c;
	/* Short-option string. Note: 'n' is flag-only (no argument), while 'M'
	 * (enable-metrics) is also flag-only. Long-only options use numeric
	 * return codes (see long_options). */
	const char *short_opts =
#if HAVE_SRP_SUPPORT
		"r:i:o:s:e:N:v:S:p:P:F:nMhu";
#else
		"r:i:o:s:e:N:v:S:p:P:nMhu";
#endif
	while ((c = getopt_long(argc, argv, short_opts, long_options, &option_index)) != -1) {
		switch (c) {
		case 'i':
			if (inputurl != NULL)
				goto usage;
			inputurl = strdup(optarg);
			break;
		case 'o':
			if (outputurl != NULL)
				goto usage;
			outputurl = strdup(optarg);
			break;
		case 's':
			if (client_args.shared_secret != NULL)
				goto usage;
			client_args.shared_secret = strdup(optarg);
			break;
		case 'e':
			client_args.encryption_type =atoi(optarg);
			break;
		case 'N':
			if (cname != NULL)
				goto usage;
			cname = strdup(optarg);
			break;
		case 'p':
			profile = atoi(optarg);
			break;
		case 'P':
			out_profile = atoi(optarg);
			break;
		case 'n':
			client_args.npd_enabled = 1;
			break;
		case 'v':
			loglevel = (enum rist_log_level) atoi(optarg);
			break;
		case 'r':
			if (remote_log_address != NULL)
				goto usage;
			remote_log_address = strdup(optarg);
		break;
#if HAVE_SRP_SUPPORT
		case 'F': {
			FILE* f = fopen(optarg, "r");
			if (!f) {
				rist_log(&logging_settings, RIST_LOG_ERROR, "Could not open srp file %s\n", optarg);
				return 1;
			}
			srpfile = strdup(optarg);
		}
		break;
#endif
#if HAVE_PROMETHEUS_SUPPORT
		case 'M':
			enable_prometheus = true;
			break;
		case 1:
			if (prometheus_tags)
				free(prometheus_tags);
			prometheus_tags = strdup(optarg);
			break;
#if HAVE_LIBMICROHTTPD
		case 2:
			prometheus_port = (uint16_t)atoi(optarg);
			break;
		case 3:
			if (prometheus_ip)
				free(prometheus_ip);
			prometheus_ip = strdup(optarg);
			break;
		case 4:
			prometheus_httpd = true;
			break;
#endif
#endif
		case 'S':
			statsinterval = atoi(optarg);
			break;
		case 'u':
			rist_log(&logging_settings, RIST_LOG_INFO, "%s", help_urlstr);
			exit(1);
		case 'h':
			//
		default:
usage:
			usage(argv[0]);
			break;
		}
	}
	client_args.cname = cname;
	client_args.loglevel = loglevel;
	client_args.outputurl = outputurl;
	client_args.statsinterval = statsinterval;

	if (inputurl == NULL || outputurl == NULL) {
		usage(argv[0]);
	}

	if (argc < 2) {
		usage(argv[0]);
	}

	if (rist_logging_set(&log_ptr, loglevel, NULL, NULL, remote_log_address, stderr) != 0) {
		fprintf(stderr, "Failed to setup logging!\n");
		exitcode = 1;
		goto out;;
	}

#if HAVE_PROMETHEUS_SUPPORT
	if (enable_prometheus) {
		struct prometheus_httpd_options httpd_opt = {
			.enabled = prometheus_httpd,
			.port = prometheus_port,
			.bind_sockaddr = false,
			.ip = prometheus_ip,
		};
		prom_stats_ctx = rist_setup_prometheus_stats(log_ptr,
			prometheus_tags,
			/* multiple_metric_datapoints */ false,
			/* skipcreated */ false,
			&httpd_opt,
			/* unix_socket */ NULL);
		if (prom_stats_ctx == NULL) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Failed to initialise Prometheus metrics exporter\n");
			exitcode = 1;
			goto out;
		}
	}
#endif

	struct rist_ctx *receiver_ctx;

	if (rist_receiver_create(&receiver_ctx, profile, &logging_settings) != 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not create rist receiver context\n");
		exitcode = 1;
		goto out;
	}

	/* Publish receiver ctx for the WAN-telemetry path before auth can fire. */
	g_lan_receiver_ctx = receiver_ctx;
	memset(g_sender_peers, 0, sizeof(g_sender_peers));

	if (rist_auth_handler_set(receiver_ctx, cb_auth_connect, cb_auth_disconnect, receiver_ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not init rist auth handler\n");
		exitcode = 1;
		goto out;
	}

	/* Enable OOB on the receiver (encoder-facing) context. Without this,
	 * rist_oob_write() back to the encoder — the auth ack in cb_auth_connect and
	 * the WAN telemetry in wan_telemetry_update — fails with "OOB not enabled". */
	if (rist_oob_callback_set(receiver_ctx, cb_recv_oob, receiver_ctx) == -1) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not enable out-of-band data on receiver\n");
		exitcode = 1;
		goto out;
	}

	struct rist_peer_config app_peer_config = {
		.version = RIST_PEER_CONFIG_VERSION,
		.virt_dst_port = RIST_DEFAULT_VIRT_DST_PORT,
		.recovery_mode = RIST_DEFAULT_RECOVERY_MODE,
		.recovery_maxbitrate = RIST_DEFAULT_RECOVERY_MAXBITRATE,
		.recovery_maxbitrate_return = RIST_DEFAULT_RECOVERY_MAXBITRATE_RETURN,
		.recovery_length_min = RIST_DEFAULT_RECOVERY_LENGTH_MIN,
		.recovery_length_max = RIST_DEFAULT_RECOVERY_LENGTH_MAX,
		.recovery_reorder_buffer = RIST_DEFAULT_RECOVERY_REORDER_BUFFER,
		.recovery_rtt_min = RIST_DEFAULT_RECOVERY_RTT_MIN,
		.recovery_rtt_max = RIST_DEFAULT_RECOVERY_RTT_MAX,
		.weight = 5,
		.congestion_control_mode = RIST_DEFAULT_CONGESTION_CONTROL_MODE,
		.min_retries = RIST_DEFAULT_MIN_RETRIES,
		.max_retries = RIST_DEFAULT_MAX_RETRIES,
		.key_size = 0
	};

	if (cname != NULL) {
		strncpy(app_peer_config.cname, cname, RIST_MAX_STRING_SHORT -1);
	}

	if (statsinterval) {
		rist_stats_callback_set(receiver_ctx, statsinterval, cb_stats, receiver_ctx);
	}

	/* Tokenize the input URL list on ',' and add one receiver peer per token.
	 * Matches ristreceiver's convention for multi-peer/bonded inputs. */
	char *rx_saveptr = NULL;
	char *intoken = strtok_r(inputurl, ",", &rx_saveptr);
	int rx_peer_count = 0;
	while (intoken) {
		struct rist_peer_config per_peer_config = app_peer_config;
		struct rist_peer_config *peer_config = &per_peer_config;
		if (rist_parse_address2(intoken, &peer_config)) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not parse peer options for receiver: %s\n", intoken);
			exitcode = 1;
			goto out;
		}

		struct rist_peer *peer;
		if (rist_peer_create(receiver_ctx, &peer, peer_config) == -1) {
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not add peer connector to receiver: %s\n", intoken);
			exitcode = 1;
			goto out;
		}
		rx_peer_count++;
		intoken = strtok_r(NULL, ",", &rx_saveptr);
	}

	if (rx_peer_count == 0) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "No valid input peer URLs provided\n");
		exitcode = 1;
		goto out;
	}


	// callback is best unless you are using the timestamps passed with the buffer
	int enable_data_callback = 0;

	if (enable_data_callback == 1) {
		if (rist_receiver_data_callback_set2(receiver_ctx, cb_recv, &cb_arg))
		{
			rist_log(&logging_settings, RIST_LOG_ERROR, "Could not set data_callback pointer");
			exitcode = 1;
			goto out;
		}
	}
	client_args.profile = out_profile;
	cb_arg.sender_ctx = setup_rist_sender(&client_args);
	if (rist_start(receiver_ctx)) {
		rist_log(&logging_settings, RIST_LOG_ERROR, "Could not start rist receiver\n");
		exitcode = 1;
		goto out;
	}
	/* Start the rist protocol thread */
	if (enable_data_callback == 1) {
#ifdef _WIN32
		system("pause");
#else
		pause();
#endif
	}
	else {
		// Master loop
		while (keep_running)
		{
			struct rist_data_block *b;
			int ret = rist_receiver_data_read2(receiver_ctx, &b, 5);
			if (ret && b && b->payload) cb_recv(&cb_arg, b);
		}
	}

	rist_destroy(receiver_ctx);
	rist_destroy(cb_arg.sender_ctx);
out:
#if HAVE_PROMETHEUS_SUPPORT
	if (prom_stats_ctx) {
		rist_prometheus_stats_destroy(prom_stats_ctx);
		prom_stats_ctx = NULL;
	}
	if (prometheus_tags)
		free(prometheus_tags);
	if (prometheus_ip)
		free(prometheus_ip);
#endif
	rist_logging_unset_global();
	if (client_args.shared_secret)
		free(client_args.shared_secret);
	if (cname)
		free(cname);
	if (inputurl)
		free(inputurl);
	if (outputurl)
		free(outputurl);
	if (remote_log_address)
		free(remote_log_address);

	return exitcode;
}
