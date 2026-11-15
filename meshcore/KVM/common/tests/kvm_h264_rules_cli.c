/* Command line of kvm_h264_rules.c for streams from files (Linux, test tool): kvm_h264_rules_cli <file.h264> [codedW codedH [viewerMaxLevel]]
   Prints one line per access unit ("au <n> <offset> <length> idr=<0|1> broke=<rules>") and the table of the rules. Exit code 0 = no rule failed, 1 = a rule failed, 2 = usage / file. */
#define _POSIX_C_SOURCE 200809L

#include "../kvm_h264_rules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	FILE *f;
	uint8_t *d;
	long len;
	size_t pos = 0, n;
	unsigned w = 0, h = 0, idx = 0;
	int maxLevel = 0, i, fails;
	struct kvmh_check c;
	if (argc < 2) { fprintf(stderr, "usage: kvm_h264_rules_cli file.h264 [codedW codedH [viewerMaxLevel]]\n"); return 2; }
	if (argc >= 4) { w = (unsigned)atoi(argv[2]); h = (unsigned)atoi(argv[3]); }
	if (argc >= 5) { maxLevel = atoi(argv[4]); }
	f = fopen(argv[1], "rb");
	if (f == NULL) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
	fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 256L * 1024 * 1024) { fclose(f); fprintf(stderr, "bad size\n"); return 2; }
	d = (uint8_t *)malloc((size_t)len);
	if (d == NULL || fread(d, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(d); return 2; }
	fclose(f);
	n = (size_t)len;
	kvmh_init(&c, w, h, maxLevel);
	while (pos < n)
	{
		size_t e = kvmh_next_au(d, n, pos);
		struct kvmh_au_info in;
		int broke;
		if (e <= pos) { e = n; }
		kvmh_au_info(d + pos, e - pos, &in);
		broke = kvmh_au(&c, d + pos, e - pos, -1);
		printf("au %u %lu %lu idr=%d broke=%d\n", idx++, (unsigned long)pos, (unsigned long)(e - pos), in.idr, broke);
		pos = e;
	}
	fails = kvmh_finish(&c);
	for (i = 0; i < KVMH_NRULES; i++)
	{
		printf("rule %-10s %-5s checked %u failed %u %s\n", kvmh_rule_id(i), kvmh_status_name(c.r[i].status), c.r[i].checks, c.r[i].fails, c.r[i].first);
	}
	free(d);
	return fails ? 1 : 0;
}
