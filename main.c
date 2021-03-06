#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <sodium/core.h>
#include <sodium/randombytes.h>
#ifdef PASSPHRASE
#include <sodium/crypto_pwhash.h>
#endif
#include <sodium/utils.h>

#include "types.h"
#include "vec.h"
#include "base32.h"
#include "cpucount.h"
#include "keccak.h"
#include "ioutil.h"
#include "common.h"
#include "yaml.h"

#include "filters.h"

#include "worker.h"

#ifndef _WIN32
#define FSZ "%zu"
#else
#define FSZ "%Iu"
#endif

// Argon2 hashed passphrase stretching settings
// NOTE: changing these will break compatibility
#define PWHASH_OPSLIMIT 48
#define PWHASH_MEMLIMIT 64 * 1024 * 1024
#define PWHASH_ALG      crypto_pwhash_ALG_ARGON2ID13

///
/* Keep quiet */
static int quietflag = 1;
///
static int verboseflag = 0;
#ifndef PCRE2FILTER
static int wantdedup = 0;
#endif

// 0, direndpos, onionendpos
// printstartpos = either 0 or direndpos
// printlen      = either onionendpos + 1 or ONION_LEN + 1 (additional 1 is for newline)
size_t onionendpos;   // end of .onion within string
size_t direndpos;     // end of dir before .onion within string
size_t printstartpos; // where to start printing from
size_t printlen;      // precalculated, related to printstartpos

pthread_mutex_t fout_mutex;
FILE *fout;

static void termhandler(int sig)
{
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		endwork = 1;
		break;
	}
}

#ifdef STATISTICS
struct tstatstruct {
	u64 numcalc;
	u64 numsuccess;
	u64 numrestart;
	u32 oldnumcalc;
	u32 oldnumsuccess;
	u32 oldnumrestart;
} ;
VEC_STRUCT(tstatsvec,struct tstatstruct);
#endif

static void printhelp(FILE *out,const char *progname)
{
	fprintf(out,
		"Usage: %s filter [filter...] [options]\n"
		"       %s -f filterfile [options]\n"
		"Options:\n"
		"\t-h  - print help to stdout and quit\n"
		"\t-f  - specify filter file which contains filters separated by newlines\n"
		"\t-D  - deduplicate filters\n"
		"\t-q  - do not print diagnostic output to stderr\n"
		"\t-x  - do not print onion names\n"
		"\t-v  - print more diagnostic data\n"
		"\t-o filename  - output onion names to specified file (append)\n"
		"\t-O filename  - output onion names to specified file (overwrite)\n"
		"\t-F  - include directory names in onion names output\n"
		"\t-d dirname  - output directory\n"
		"\t-t numthreads  - specify number of threads to utilise (default - CPU core count or 1)\n"
		"\t-j numthreads  - same as -t\n"
		"\t-n numkeys  - specify number of keys (default - 0 - unlimited)\n"
		"\t-N numwords  - specify number of words per key (default - 1)\n"
		"\t-Z  - use \"slower\" key generation method (initial default)\n"
		"\t-z  - use \"faster\" key generation method (later default)\n"
		"\t-B  - use batching key generation method (>10x faster than -z, current default)\n"
		"\t-s  - print statistics each 10 seconds\n"
		"\t-S t  - print statistics every specified ammount of seconds\n"
		"\t-T  - do not reset statistics counters when printing\n"
		"\t-y  - output generated keys in YAML format instead of dumping them to filesystem\n"
		"\t-Y [filename [host.onion]]  - parse YAML encoded input and extract key(s) to filesystem\n"
		"\t--rawyaml  - raw (unprefixed) public/secret keys for -y/-Y (may be useful for tor controller API)\n"
#ifdef PASSPHRASE
		"\t-p passphrase  - use passphrase to initialize the random seed with\n"
		"\t-P  - same as -p, but takes passphrase from PASSPHRASE environment variable\n"
#endif
		,progname,progname);
	fflush(out);
}

static void e_additional(void)
{
	fprintf(stderr,"additional argument required\n");
	exit(1);
}

#ifndef STATISTICS
static void e_nostatistics(void)
{
	fprintf(stderr,"statistics support not compiled in\n");
	exit(1);
}
#endif

static void setworkdir(const char *wd)
{
	free(workdir);
	size_t l = strlen(wd);
	if (!l) {
		workdir = 0;
		workdirlen = 0;
		if (!quietflag)
			fprintf(stderr,"unset workdir\n");
		return;
	}
	unsigned needslash = 0;
	if (wd[l-1] != '/')
		needslash = 1;
	char *s = (char *) malloc(l + needslash + 1);
	if (!s)
		abort();
	memcpy(s,wd,l);
	if (needslash)
		s[l++] = '/';
	s[l] = 0;

	workdir = s;
	workdirlen = l;
	if (!quietflag)
		fprintf(stderr,"set workdir: %s\n",workdir);
}

#ifdef PASSPHRASE
static void setpassphrase(const char *pass)
{
	static u8 salt[crypto_pwhash_SALTBYTES] = {0};
	fprintf(stderr,"expanding passphrase (may take a while)...");
	if (crypto_pwhash(determseed,sizeof(determseed),
		pass,strlen(pass),salt,
		PWHASH_OPSLIMIT,PWHASH_MEMLIMIT,PWHASH_ALG) != 0)
	{
		fprintf(stderr," out of memory!\n");
		exit(1);
	}
	fprintf(stderr," done.\n");
}
#endif

VEC_STRUCT(threadvec, pthread_t);

#include "filters_inc.inc.h"
#include "filters_main.inc.h"

enum worker_type {
	WT_SLOW,
	WT_FAST,
	WT_BATCH,
};

int main(int argc,char **argv)
{
	const char *outfile = 0;
	const char *infile = 0;
	const char *onehostname = 0;
	const char *arg;
	int ignoreargs = 0;
	int dirnameflag = 0;
	int numthreads = 0;
	enum worker_type wt = WT_BATCH;
	int yamlinput = 0;
#ifdef PASSPHRASE
	int deterministic = 0;
#endif
	int outfileoverwrite = 0;
	struct threadvec threads;
#ifdef STATISTICS
	struct statsvec stats;
	struct tstatsvec tstats;
	u64 reportdelay = 0;
	int realtimestats = 1;
#endif
	int tret;

	if (sodium_init() < 0) {
		fprintf(stderr,"sodium_init() failed\n");
		return 1;
	}
	worker_init();
	filters_init();

	setvbuf(stderr,0,_IONBF,0);
	fout = stdout;

///
	/* Force options : -Z a */

	wt = WT_SLOW; // Use the slow worker
	filters_add("ooo"); // Dummy filter
///

	filters_prepare();

///
	/* Hide useless info */
	//filters_print();
///

#ifdef STATISTICS
	if (!filters_count() && !reportdelay)
#else
	if (!filters_count())
#endif
		return 0;

#ifdef EXPANDMASK
	if (numwords > 1 && flattened)
		fprintf(stderr,"WARNING: -N switch will produce bogus results because we can't know filter width. reconfigure with --enable-besort and recompile.\n");
#endif

	pthread_mutex_init(&keysgenerated_mutex,0);
	pthread_mutex_init(&fout_mutex,0);
#ifdef PASSPHRASE
	pthread_mutex_init(&determseed_mutex,0);
#endif

	if (numthreads <= 0) {
///
		/* Use a single thread */
		numthreads = 1;
		//numthreads = cpucount();
		//if (numthreads <= 0)
		//	numthreads = 1;
///
	}
	if (!quietflag)
		fprintf(stderr,"using %d %s\n",
			numthreads,numthreads == 1 ? "thread" : "threads");

#ifdef PASSPHRASE
	if (!quietflag && deterministic && numneedgenerate != 1)
		fprintf(stderr,"CAUTION: avoid using keys generated with same password for unrelated services, as single leaked key may help attacker to regenerate related keys.\n");
#endif

	signal(SIGTERM,termhandler);
	signal(SIGINT,termhandler);

	VEC_INIT(threads);
	VEC_ADDN(threads,numthreads);
#ifdef STATISTICS
	VEC_INIT(stats);
	VEC_ADDN(stats,numthreads);
	VEC_ZERO(stats);
	VEC_INIT(tstats);
	VEC_ADDN(tstats,numthreads);
	VEC_ZERO(tstats);
#endif

	pthread_attr_t tattr,*tattrp = &tattr;
	tret = pthread_attr_init(tattrp);
	if (tret) {
		perror("pthread_attr_init");
		tattrp = 0;
	}
	else {
		// 256KiB plus whatever batch stuff uses if in batch mode
		size_t ss = 256 << 10;
		if (wt == WT_BATCH)
			ss += worker_batch_memuse();
		// align to 64KiB
		ss = (ss + (64 << 10) - 1) & ~((64 << 10) - 1);
		//printf("stack size: " FSZ "\n",ss);
		tret = pthread_attr_setstacksize(tattrp,ss);
		if (tret)
			perror("pthread_attr_setstacksize");
	}

	for (size_t i = 0;i < VEC_LENGTH(threads);++i) {
		void *tp = 0;
#ifdef STATISTICS
		tp = &VEC_BUF(stats,i);
#endif
		tret = pthread_create(
			&VEC_BUF(threads,i),
			tattrp,
#ifdef PASSPHRASE
			deterministic
				? (wt == WT_BATCH
					? worker_batch_pass
					: worker_fast_pass)
				:
#endif
			wt == WT_BATCH
				? worker_batch
				:
			wt == WT_FAST
				? worker_fast
				: worker_slow,
			tp
		);
		if (tret) {
			fprintf(stderr,"error while making " FSZ "th thread: %s\n",i,strerror(tret));
			exit(1);
		}
	}

	if (tattrp) {
		tret = pthread_attr_destroy(tattrp);
		if (tret)
			perror("pthread_attr_destroy");
	}

#ifdef STATISTICS
	struct timespec nowtime;
	u64 istarttime,inowtime,ireporttime = 0,elapsedoffset = 0;
	if (clock_gettime(CLOCK_MONOTONIC,&nowtime) < 0) {
		perror("failed to get time");
		exit(1);
	}
	istarttime = (1000000 * (u64)nowtime.tv_sec) + ((u64)nowtime.tv_nsec / 1000);
#endif
	struct timespec ts;
	memset(&ts,0,sizeof(ts));
	ts.tv_nsec = 100000000;
	while (!endwork) {
		if (numneedgenerate && keysgenerated >= numneedgenerate) {
			endwork = 1;
			break;
		}
		nanosleep(&ts,0);

#ifdef STATISTICS
		clock_gettime(CLOCK_MONOTONIC,&nowtime);
		inowtime = (1000000 * (u64)nowtime.tv_sec) + ((u64)nowtime.tv_nsec / 1000);
		u64 sumcalc = 0,sumsuccess = 0,sumrestart = 0;
		for (int i = 0;i < numthreads;++i) {
			u32 newt,tdiff;
			// numcalc
			newt = VEC_BUF(stats,i).numcalc.v;
			tdiff = newt - VEC_BUF(tstats,i).oldnumcalc;
			VEC_BUF(tstats,i).oldnumcalc = newt;
			VEC_BUF(tstats,i).numcalc += (u64)tdiff;
			sumcalc += VEC_BUF(tstats,i).numcalc;
			// numsuccess
			newt = VEC_BUF(stats,i).numsuccess.v;
			tdiff = newt - VEC_BUF(tstats,i).oldnumsuccess;
			VEC_BUF(tstats,i).oldnumsuccess = newt;
			VEC_BUF(tstats,i).numsuccess += (u64)tdiff;
			sumsuccess += VEC_BUF(tstats,i).numsuccess;
			// numrestart
			newt = VEC_BUF(stats,i).numrestart.v;
			tdiff = newt - VEC_BUF(tstats,i).oldnumrestart;
			VEC_BUF(tstats,i).oldnumrestart = newt;
			VEC_BUF(tstats,i).numrestart += (u64)tdiff;
			sumrestart += VEC_BUF(tstats,i).numrestart;
		}
		if (reportdelay && (!ireporttime || inowtime - ireporttime >= reportdelay)) {
			if (ireporttime)
				ireporttime += reportdelay;
			else
				ireporttime = inowtime;
			if (!ireporttime)
				ireporttime = 1;

			double calcpersec = (1000000.0 * sumcalc) / (inowtime - istarttime);
			double succpersec = (1000000.0 * sumsuccess) / (inowtime - istarttime);
			double restpersec = (1000000.0 * sumrestart) / (inowtime - istarttime);
			fprintf(stderr,">calc/sec:%8lf, succ/sec:%8lf, rest/sec:%8lf, elapsed:%5.6lfsec\n",
				calcpersec,succpersec,restpersec,
				(inowtime - istarttime + elapsedoffset) / 1000000.0);

			if (realtimestats) {
				for (int i = 0;i < numthreads;++i) {
					VEC_BUF(tstats,i).numcalc = 0;
					VEC_BUF(tstats,i).numsuccess = 0;
					VEC_BUF(tstats,i).numrestart = 0;
				}
				elapsedoffset += inowtime - istarttime;
				istarttime = inowtime;
			}
		}
		if (sumcalc > U64_MAX / 2) {
			for (int i = 0;i < numthreads;++i) {
				VEC_BUF(tstats,i).numcalc /= 2;
				VEC_BUF(tstats,i).numsuccess /= 2;
				VEC_BUF(tstats,i).numrestart /= 2;
			}
			u64 timediff = (inowtime - istarttime + 1) / 2;
			elapsedoffset += timediff;
			istarttime += timediff;
		}
#endif
	}

	if (!quietflag)
		fprintf(stderr,"waiting for threads to finish...");
	for (size_t i = 0;i < VEC_LENGTH(threads);++i)
		pthread_join(VEC_BUF(threads,i),0);
	if (!quietflag)
		fprintf(stderr," done.\n");

	if (yamloutput)
		yamlout_clean();

#ifdef PASSPHRASE
	pthread_mutex_destroy(&determseed_mutex);
#endif
	pthread_mutex_destroy(&fout_mutex);
	pthread_mutex_destroy(&keysgenerated_mutex);

done:
	filters_clean();

	if (outfile)
		fclose(fout);

	return 0;
}
