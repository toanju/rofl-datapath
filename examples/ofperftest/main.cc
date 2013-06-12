#include <rofl/platform/unix/cunixenv.h>

#include "ofperftest.h"
#include "match_eth_dst.h"

void usage(int argc, char** argv);
void parse_args(int argc, char** argv);

std::string s_testcase;
unsigned int n_entries = 0;

int
main(int argc, char** argv)
{
	parse_args(argc, argv);

	/* update defaults */
	rofl::csyslog::initlog(
			rofl::csyslog::LOGTYPE_STDERR,
			rofl::csyslog::EMERGENCY,
			std::string("ofperftest.log"),
			"an example: ");

	rofl::csyslog::set_debug_level("ciosrv", "emergency");
	rofl::csyslog::set_debug_level("cthread", "emergency");



	ofperftest* perftest;

	if (s_testcase == std::string("match_eth_dst")) {
		perftest = new match_eth_dst(n_entries);
	} else {
		fprintf(stderr, "testcase %s not found, aborting\n", s_testcase.c_str());
		exit(1);
	}

	perftest->rpc_listen_for_dpts(caddress(AF_INET, "0.0.0.0", 6633));
	perftest->rpc_listen_for_dpts(caddress(AF_INET, "0.0.0.0", 6632));

	perftest->run();

	return 0;
}



void
usage(int argc, char** argv)
{
	fprintf(stderr, "testcases for %s:\n", argv[0]);
	fprintf(stderr, "match_eth_dst\n");

	exit(0);
}


void
parse_args(int argc, char** argv)
{

	int option_index = 0;
	static struct option long_options[] = {
	    {"testcase", required_argument, 0, 0},
	    {"entries", required_argument, 0, 0},
	    {0, 0, 0, 0}
	};



	while (1) {
		int c = getopt_long (argc, argv, "t:e:", long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 't':
			s_testcase.assign(optarg);
			break;

		case 'e':
			n_entries = atoi(optarg);
			break;

		case '?':
			break;

		default:
			usage(argc, argv);
		}
	}
}