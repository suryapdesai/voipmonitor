/* Martin Vit support@voipmonitor.org
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2.
*/

#include <queue>
#include <climits>
// stevek - it could be smarter if sys/inotyfy.h available then use it otherwise use linux/inotify.h. I will do it later
#include <sys/inotify.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <signal.h>
#include <endian.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <pthread.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <semaphore.h>
#include <dirent.h>
#include <signal.h>
#include <execinfo.h>

#ifdef ISCURL
#include <curl/curl.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <pcap.h>

#include "rtp.h"
#include "calltable.h"
#include "voipmonitor.h"
#include "sniff.h"
#include "simpleini/SimpleIni.h"
#include "manager.h"
#include "filter_mysql.h"
#include "sql_db.h"
#include "tools.h"
#include "mirrorip.h"
#include "ipaccount.h"

#if defined(QUEUE_MUTEX) || defined(QUEUE_NONBLOCK)
extern "C" {
#include "liblfds.6/inc/liblfds.h"
}
#endif

#define BACKTRACE 1

#ifdef BACKTRACE
/* Since kernel version 2.2 the undocumented parameter to the signal handler has been declared
obsolete in adherence with POSIX.1b. A more correct way to retrieve additional information is
to use the SA_SIGINFO option when setting the handler */
#undef USE_SIGCONTEXT

#ifndef USE_SIGCONTEXT
/* get REG_EIP / REG_RIP from ucontext.h */
#include <ucontext.h>

        #ifndef EIP
        #define EIP     14
        #endif

        #if (defined (__x86_64__))
                #ifndef REG_RIP
                #define REG_RIP REG_INDEX(rip) /* seems to be 16 */
                #endif
        #endif

#endif

typedef struct { char name[10]; int id; char description[40]; } signal_def;

signal_def signal_data[] =
{
        { "SIGHUP", SIGHUP, "Hangup (POSIX)" },
        { "SIGINT", SIGINT, "Interrupt (ANSI)" },
        { "SIGQUIT", SIGQUIT, "Quit (POSIX)" },
        { "SIGILL", SIGILL, "Illegal instruction (ANSI)" },
        { "SIGTRAP", SIGTRAP, "Trace trap (POSIX)" },
        { "SIGABRT", SIGABRT, "Abort (ANSI)" },
        { "SIGIOT", SIGIOT, "IOT trap (4.2 BSD)" },
        { "SIGBUS", SIGBUS, "BUS error (4.2 BSD)" },
        { "SIGFPE", SIGFPE, "Floating-point exception (ANSI)" },
        { "SIGKILL", SIGKILL, "Kill, unblockable (POSIX)" },
        { "SIGUSR1", SIGUSR1, "User-defined signal 1 (POSIX)" },
        { "SIGSEGV", SIGSEGV, "Segmentation violation (ANSI)" },
        { "SIGUSR2", SIGUSR2, "User-defined signal 2 (POSIX)" },
        { "SIGPIPE", SIGPIPE, "Broken pipe (POSIX)" },
        { "SIGALRM", SIGALRM, "Alarm clock (POSIX)" },
        { "SIGTERM", SIGTERM, "Termination (ANSI)" },
        { "SIGSTKFLT", SIGSTKFLT, "Stack fault" },
        { "SIGCHLD", SIGCHLD, "Child status has changed (POSIX)" },
        { "SIGCLD", SIGCLD, "Same as SIGCHLD (System V)" },
        { "SIGCONT", SIGCONT, "Continue (POSIX)" },
        { "SIGSTOP", SIGSTOP, "Stop, unblockable (POSIX)" },
        { "SIGTSTP", SIGTSTP, "Keyboard stop (POSIX)" },
        { "SIGTTIN", SIGTTIN, "Background read from tty (POSIX)" },
        { "SIGTTOU", SIGTTOU, "Background write to tty (POSIX)" },
        { "SIGURG", SIGURG, "Urgent condition on socket (4.2 BSD)" },
        { "SIGXCPU", SIGXCPU, "CPU limit exceeded (4.2 BSD)" },
        { "SIGXFSZ", SIGXFSZ, "File size limit exceeded (4.2 BSD)" },
        { "SIGVTALRM", SIGVTALRM, "Virtual alarm clock (4.2 BSD)" },
        { "SIGPROF", SIGPROF, "Profiling alarm clock (4.2 BSD)" },
        { "SIGWINCH", SIGWINCH, "Window size change (4.3 BSD, Sun)" },
        { "SIGIO", SIGIO, "I/O now possible (4.2 BSD)" },
        { "SIGPOLL", SIGPOLL, "Pollable event occurred (System V)" },
        { "SIGPWR", SIGPWR, "Power failure restart (System V)" },
        { "SIGSYS", SIGSYS, "Bad system call" },
};
#endif

using namespace std;

/* global variables */

extern Calltable *calltable;
extern volatile int calls;
unsigned int opt_openfile_max = 65535;
int opt_packetbuffered = 0;	// Make .pcap files writing ‘‘packet-buffered’’ 
				// more slow method, but you can use partitialy 
				// writen file anytime, it will be consistent.
					
int opt_fork = 1;		// fork or run foreground 
int opt_saveSIP = 0;		// save SIP packets to pcap file?
int opt_saveRTP = 0;		// save RTP packets to pcap file?
int opt_onlyRTPheader = 0;	// do not save RTP payload, only RTP header
int opt_saveRTCP = 0;		// save RTCP packets to pcap file?
int opt_saveudptl = 0;		// if = 1 all UDPTL packets will be saved (T.38 fax)
int opt_saveRAW = 0;		// save RTP packets to pcap file?
int opt_saveWAV = 0;		// save RTP packets to pcap file?
int opt_saveGRAPH = 0;		// save GRAPH data to *.graph file? 
int opt_gzipGRAPH = 0;		// compress GRAPH data ? 
int opt_saverfc2833 = 0;
int opt_dbdtmf = 0;
int opt_rtcp = 1;		// pair RTP+1 port to RTCP and save it. 
int opt_nocdr = 0;		// do not save cdr?
int opt_gzipPCAP = 0;		// compress PCAP data ? 
int opt_mos_g729 = 0;		// calculate MOS for G729 codec
int verbosity = 0;		// cebug level
int opt_rtp_firstleg = 0;	// if == 1 then save RTP stream only for first INVITE leg in case you are 
				// sniffing on SIP proxy where voipmonitor see both SIP leg. 
int opt_jitterbuffer_f1 = 1;		// turns off/on jitterbuffer simulator to compute MOS score mos_f1
int opt_jitterbuffer_f2 = 1;		// turns off/on jitterbuffer simulator to compute MOS score mos_f2
int opt_jitterbuffer_adapt = 1;		// turns off/on jitterbuffer simulator to compute MOS score mos_adapt
int opt_sip_register_active_nologbin = 1;
int opt_ringbuffer = 10;	// ring buffer in MB 
int opt_sip_register = 0;	// if == 1 save REGISTER messages
int opt_audio_format = FORMAT_WAV;	// define format for audio writing (if -W option)
int opt_manager_port = 5029;	// manager api TCP port
char opt_manager_ip[32] = "127.0.0.1";	// manager api listen IP address
int opt_pcap_threaded = 0;	// run reading packets from pcap in one thread and process packets in another thread via queue
int opt_norecord_header = 0;	// if = 1 SIP call with X-VoipMonitor-norecord header will be not saved although global configuration says to record. 
int opt_rtpnosip = 0;		// if = 1 RTP stream will be saved into calls regardless on SIP signalizatoin (handy if you need extract RTP without SIP)
int opt_norecord_dtmf = 0;	// if = 1 SIP call with dtmf == *0 sequence (in SIP INFO) will stop recording
int opt_savewav_force = 0;	// if = 1 WAV will be generated no matter on filter rules
int opt_sipoverlap = 1;		
int opt_id_sensor = -1;		
int readend = 0;
int opt_dup_check = 0;
int rtptimeout = 300;
char opt_cdrurl[1024] = "";
int opt_cleanspool_interval = 0; // number of seconds between cleaning spool directory. 0 = disabled
int opt_cleanspool_sizeMB = 0; // number of MB to keep in spooldir
int opt_domainport = 0;
int request_iptelnum_reload = 0;
int opt_mirrorip = 0;
int opt_mirrorall = 0;
int opt_mirroronly = 0;
char opt_mirrorip_src[20];
char opt_mirrorip_dst[20];
int opt_printinsertid = 0;
int opt_ipaccount = 0;
int opt_ipacc_interval = 300;
bool opt_ipacc_sniffer_agregate = false;
bool opt_ipacc_agregate_only_customers_on_main_side = true;
bool opt_ipacc_agregate_only_customers_on_any_side = true;
bool opt_ipacc_multithread_save = true;
int opt_udpfrag = 1;
MirrorIP *mirrorip = NULL;
int opt_cdronlyanswered = 0;
int opt_cdronlyrtp = 0;
int opt_pcap_split = 1;
int opt_newdir = 1;
char opt_clientmanager[1024] = "";
int opt_clientmanagerport = 9999;
int opt_callslimit = 0;
char opt_silencedmtfseq[16] = "";
char opt_keycheck[1024] = "";
char opt_convert_char[64] = "";
int opt_skinny = 0;
int opt_read_from_file = 0;

bool opt_cdr_partition = 1;
vector<dstring> opt_custom_headers;

char configfile[1024] = "";	// config file name

string insert_funcname = "__insert";

char sql_driver[256] = "mysql";
char sql_cdr_table[256] = "cdr";
char sql_cdr_table_last30d[256] = "";
char sql_cdr_table_last7d[256] = "";
char sql_cdr_table_last1d[256] = "";
char sql_cdr_next_table[256] = "cdr_next";
char sql_cdr_ua_table[256] = "cdr_ua";
char sql_cdr_sip_response_table[256] = "cdr_sip_response";

char mysql_host[256] = "localhost";
char mysql_database[256] = "voipmonitor";
char mysql_table[256] = "cdr";
char mysql_user[256] = "root";
char mysql_password[256] = "";
int opt_mysql_port = 0; // 0 menas use standard port 
int opt_skiprtpdata = 0;

char opt_match_header[128] = "";

char odbc_dsn[256] = "voipmonitor";
char odbc_user[256];
char odbc_password[256];
char odbc_driver[256];

char get_customer_by_ip_sql_driver[256] = "odbc";
char get_customer_by_ip_odbc_dsn[256];
char get_customer_by_ip_odbc_user[256];
char get_customer_by_ip_odbc_password[256];
char get_customer_by_ip_odbc_driver[256];
char get_customer_by_ip_query[1024];
char get_customers_ip_query[1024];
char get_customers_radius_name_query[1024];

char get_customer_by_pn_sql_driver[256] = "odbc";
char get_customer_by_pn_odbc_dsn[256];
char get_customer_by_pn_odbc_user[256];
char get_customer_by_pn_odbc_password[256];
char get_customer_by_pn_odbc_driver[256];
char get_customers_pn_query[1024];
vector<string> opt_national_prefix;

char get_radius_ip_driver[256];
char get_radius_ip_host[256];
char get_radius_ip_db[256];
char get_radius_ip_user[256];
char get_radius_ip_password[256];
char get_radius_ip_query[1024];
char get_radius_ip_query_where[1024];
int get_customer_by_ip_flush_period = 1;

char opt_pidfile[4098] = "/var/run/voipmonitor.pid";

char user_filter[2048] = "";
char ifname[1024];	// Specifies the name of the network device to use for 
			// the network lookup, for example, eth0
char opt_scanpcapdir[2048] = "";	// Specifies the name of the network device to use for 
int opt_promisc = 1;	// put interface to promisc mode?
char pcapcommand[4092] = "";

int rtp_threaded = 0; // do not enable this until it will be reworked to be thread safe
int num_threads = 0; // this has to be 1 for now
unsigned int rtpthreadbuffer = 20;	// default 20MB
unsigned int gthread_num = 0;

int opt_pcapdump = 0;

int opt_callend = 1; //if true, cdr.called is saved
char opt_chdir[1024];
char opt_cachedir[1024];

IPfilter *ipfilter = NULL;		// IP filter based on MYSQL 
IPfilter *ipfilter_reload = NULL;	// IP filter based on MYSQL for reload purpose
int ipfilter_reload_do = 0;	// for reload in main thread

TELNUMfilter *telnumfilter = NULL;		// IP filter based on MYSQL 
TELNUMfilter *telnumfilter_reload = NULL;	// IP filter based on MYSQL for reload purpose
int telnumfilter_reload_do = 0;	// for reload in main thread

pthread_t call_thread;		// ID of worker storing CDR thread 
pthread_t readdump_libpcap_thread;
pthread_t manager_thread = 0;	// ID of worker manager thread 
pthread_t manager_client_thread;	// ID of worker manager thread 
pthread_t cachedir_thread;	// ID of worker cachedir thread 
pthread_t cleanspool_thread;	// ID of worker clean thread 
int terminating;		// if set to 1, worker thread will terminate
int terminating2;		// if set to 1, worker thread will terminate
char *sipportmatrix;		// matrix of sip ports to monitor
char *ipaccountportmatrix;

queue<string> mysqlquery;

volatile unsigned int readit = 0;
volatile unsigned int writeit = 0;
int global_livesniffer = 0;
int global_livesniffer_all = 0;
unsigned int qringmax = 12500;
#if defined(QUEUE_MUTEX) || defined(QUEUE_NONBLOCK) || defined(QUEUE_NONBLOCK2)
pcap_packet *qring;
#endif

pcap_t *handle = NULL;		// pcap handler 

read_thread *threads;

int manager_socket_server = 0;

pthread_mutex_t mysqlquery_lock;

pthread_t pcap_read_thread;
#ifdef QUEUE_MUTEX
pthread_mutex_t readpacket_thread_queue_lock;
sem_t readpacket_thread_semaphore;
#endif

#ifdef QUEUE_NONBLOCK
struct queue_state *qs_readpacket_thread_queue = NULL;
#endif

nat_aliases_t nat_aliases;	// net_aliases[local_ip] = extern_ip

SqlDb *sqlDb = NULL;
MySqlStore *sqlStore = NULL;

char mac[32] = "";

void rename_file(const char *src, const char *dst) {
	int read_fd = 0;
	int write_fd = 0;
	struct stat stat_buf;
	off_t offset = 0;

	/* Open the input file. */
	read_fd = open (src, O_RDONLY);
	if(read_fd == -1) {
		syslog(LOG_ERR, "Cannot open file for reading [%s]\n", src);
		return;
	}
		
	/* Stat the input file to obtain its size. */
	fstat (read_fd, &stat_buf);
	/*
As you can see we are calling fdatasync right before calling posix_fadvise, this makes sure that all data associated with the file handle has been committed to disk. This is not done because there is any danger of loosing data. But it makes sure that that the posix_fadvise has an effect. Since the posix_fadvise function is advisory, the OS will simply ignore it, if it can not comply. At least with Linux, the effect of calling posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED) is immediate. This means if you write a file and call posix_fadvise right after writing a chunk of data, it will probably have no effect at all since the data in question has not been committed to disk yet, and therefore can not be released from cache.
	*/
	fdatasync(read_fd);
	posix_fadvise(read_fd, 0, 0, POSIX_FADV_DONTNEED);

	/* Open the output file for writing, with the same permissions as the source file. */
	write_fd = open (dst, O_WRONLY | O_CREAT, stat_buf.st_mode);
	if(write_fd == -1) {
		syslog(LOG_ERR, "Cannot open file for writing [%s] leaving the source file undeleted\n", src);
		close(read_fd);
		return;
	}
	fdatasync(write_fd);
	posix_fadvise(write_fd, 0, 0, POSIX_FADV_DONTNEED);
	/* Blast the bytes from one file to the other. */
	int res = sendfile(write_fd, read_fd, &offset, stat_buf.st_size);
	if(res == -1) {
		// fall back to portable way if sendfile fails 
		char buf[8192];	// if this is 8kb it will stay in L1 cache on most CPUs. Dont know if higher buffer is better for sequential write	
		ssize_t result;
		while (1) {
			result = read(read_fd, &buf[0], sizeof(buf));
			if (!result) break;
			write(write_fd, &buf[0], result);
		}
	}
	
	/* clean */
	close (read_fd);
	close (write_fd);
	unlink(src);
}

void terminate2() {
	terminating = 1;
}

/* handler for INTERRUPT signal */
void sigint_handler(int param)
{
	syslog(LOG_ERR, "SIGINT received, terminating\n");
	terminate2();
}

/* handler for TERMINATE signal */
void sigterm_handler(int param)
{
	syslog(LOG_ERR, "SIGTERM received, terminating\n");
	terminate2();
}

void find_and_replace( string &source, const string find, string replace ) {
 
	size_t j;
	for ( ; (j = source.find( find )) != string::npos ; ) {
		source.replace( j, find.length(), replace );
	}
}

void *clean_spooldir( void *dummy ) {
	char buffer[4092];
	while(!terminating2) {

		char cmd[2048];
		sprintf(cmd, "find \"%s/\" -type f -printf \"%%T@::%%p::%%s\\n\" | sort -rn | awk -v maxbytes=\"$((1024 * 1024 * %d))\" -F \"::\" 'BEGIN { curSize=0; } { curSize += $3; if (curSize > maxbytes) { print $2; } }'", opt_chdir, opt_cleanspool_sizeMB);

		if(verbosity > 0) syslog(LOG_NOTICE, "cleaning spool: [%s]\n", cmd);
		FILE* pipe = popen(cmd, "r");
		if (!pipe) {
			syslog(LOG_ERR, "cannot rum clean command: [%s] error:[%s]", cmd, strerror(errno));
			continue;
		}
		while(!feof(pipe)) {
			if(fgets(buffer, 4092, pipe) != NULL) {
				// remove new line 
				buffer[strlen(buffer) - 1] = '\0';
				unlink(buffer);
			}
		}
		pclose(pipe);
		sleep(opt_cleanspool_interval);
	}
	return NULL;
}

/* cycle files_queue and move it to spool dir */
void *moving_cache( void *dummy ) {
	string file;
	char src_c[1024];
	char dst_c[1024];
	while(1) {
		while (1) {
			calltable->lock_files_queue();
			if(calltable->files_queue.size() == 0) {
				calltable->unlock_files_queue();
				break;
			}
			file = calltable->files_queue.front();
			calltable->files_queue.pop();
			calltable->unlock_files_queue();

			string src;
			src.append(opt_cachedir);
			src.append("/");
			src.append(file);

			string dst;
			dst.append(opt_chdir);
			dst.append("/");
			dst.append(file);

			strncpy(src_c, (char*)src.c_str(), sizeof(src_c));
			strncpy(dst_c, (char*)dst.c_str(), sizeof(dst_c));

			if(verbosity > 2) syslog(LOG_ERR, "rename([%s] -> [%s])\n", src_c, dst_c);
			rename_file(src_c, dst_c);
			//TODO: error handling
			//perror ("The following error occurred");
		}
		if(terminating2) {
			break;
		}
		sleep(1);
	}
	return NULL;
}

/* cycle calls_queue and save it to MySQL */
void *storing_cdr( void *dummy ) {
	Call *call;
	while(1) {
		if(request_iptelnum_reload == 1) { reload_capture_rules(); request_iptelnum_reload = 0;};
#ifdef ISCURL
		string cdrtosend;
#endif
		if(verbosity > 0) { 
			syslog(LOG_ERR, "calls[%d] ipacc_buffer[%u] qring[%d (w%d,r%d)]\n", 
			       calls, lengthIpaccBuffer(), writeit >= readit ? writeit - readit : writeit + qringmax - readit, writeit, readit);
		}
		while (1) {

			if(request_iptelnum_reload == 1) { reload_capture_rules(); request_iptelnum_reload = 0;};

			calltable->lock_calls_queue();
			if(calltable->calls_queue.size() == 0) {
				calltable->unlock_calls_queue();
				break;
			}
			call = calltable->calls_queue.front();
			calltable->calls_queue.pop_front();
			calltable->unlock_calls_queue();
	
			if(!opt_nocdr) {
				if(call->type == INVITE or call->type == SKINNY_NEW) {
					call->saveToDb(1);
				} else if(call->type == REGISTER){
					call->saveRegisterToDb();
				} else if(call->type == MESSAGE){
					call->saveMessageToDb();
				}
			}
#ifdef ISCURL
			if(opt_cdrurl[0] != '\0') {
				cdrtosend += call->getKeyValCDRtext();
				cdrtosend += "##vmdelimiter###\n";
			}
#endif

			call->closeRawFiles();
			if( (opt_savewav_force || (call->flags & FLAG_SAVEWAV)) && (call->type == INVITE || call->type == SKINNY_NEW)) {
				if(verbosity > 0) printf("converting RAW file to WAV Queue[%d]\n", (int)calltable->calls_queue.size());
				call->convertRawToWav();
			}

			/* if pcapcommand is defined, execute command */
			if(strlen(pcapcommand)) {
				string source(pcapcommand);
				string find1 = "%pcap%";
				string find2 = "%basename%";
				string find3 = "%dirname%";
				string replace;
				replace.append("\"");
				replace.append(opt_chdir);
				replace.append("/");
				replace.append(call->dirname());
				replace.append("/");
				replace.append(call->fbasename);
				replace.append(".pcap");
				replace.append("\"");
				find_and_replace(source, find1, replace);
				find_and_replace(source, find2, call->fbasename);
				find_and_replace(source, find3, call->dirname());
				if(verbosity >= 2) printf("command: [%s]\n", source.c_str());
				system(source.c_str());
			};

			/* if we delete call here directly, destructors and another cleaning functions can be
			 * called in the middle of working with call or another structures inside main thread
			 * so put it in deletequeue and delete it in the main thread. Another way can be locking
			 * call structure for every case in main thread but it can slow down thinks for each 
			 * processing packet.
			*/
			calltable->lock_calls_deletequeue();
			calltable->calls_deletequeue.push(call);
			calltable->unlock_calls_deletequeue();
		}

		// process mysql query queue - concatenate queries to N messages
		int size = 0;
		int msgs = 50;
		int _start = time(NULL);
                int _counterIpacc = 0;
		string queryqueue = "";
		int mysqlQuerySize = mysqlquery.size();
		while(1) {
			pthread_mutex_lock(&mysqlquery_lock);
			//if(mysqlQuerySize == 0) {
			if(mysqlquery.size() == 0) {
				pthread_mutex_unlock(&mysqlquery_lock);
				if(queryqueue != "") {
					// send the rest 
					sqlDb->query("drop procedure if exists " + insert_funcname);
					sqlDb->query("create procedure " + insert_funcname + "()\nbegin\n" + queryqueue + "\nend");
					sqlDb->query("call " + insert_funcname + "();");
					//sqlDb->query(queryqueue);
					queryqueue = "";
				}
				break;
			}
			string query = mysqlquery.front();
			mysqlquery.pop();
			--mysqlQuerySize;
			pthread_mutex_unlock(&mysqlquery_lock);
			queryqueue.append(query + "; ");
			if(verbosity > 0) {
				if(query.find("ipacc ") != string::npos) {
					++_counterIpacc;
				}
			}
			if(size < msgs) {
				size++;
			} else {
				sqlDb->query("drop procedure if exists " + insert_funcname);
				sqlDb->query("create procedure " + insert_funcname + "()\nbegin\n" + queryqueue + "\nend");
				sqlDb->query("call " + insert_funcname + "();");
				//sqlDb->query(queryqueue);
				queryqueue = "";
				size = 0;
			}
		}
		if(verbosity > 0 && _counterIpacc > 0) {
			int diffTime = time(NULL) - _start;
			cout << "SAVE IPACC (" << sqlDateTimeString(time(NULL)) << "): " << _counterIpacc << " rec";
			if(diffTime > 0) {
				cout << "  " << diffTime << " s  " << (_counterIpacc/diffTime) << " rec/s";
			}
			cout << endl;
		}
                
#ifdef ISCURL
		if(opt_cdrurl[0] != '\0' && cdrtosend.length() > 0) {
			sendCDR(cdrtosend);
		}
#endif
		if(terminating) {
			break;
		}
	
		sleep(1);
	}
	return NULL;
}

static void daemonize(void)
{
	pid_t pid;

	pid = fork();
	if (pid) {
		// parent
		exit(0);
	} else {
		// child
		FILE* f;
		pid_t vmon_pid;

		setsid();

		// write pid file to opt_pidfile
		vmon_pid = getpid();
		f = fopen(opt_pidfile, "w");
		if (f) {
		       fprintf(f, "%ld\n", (long)vmon_pid);
		       fclose(f);
		} else {
		       syslog(LOG_ERR,"Error occurs while writing pid file to %s\n", opt_pidfile);
		}

		// close std descriptors (otherwise problems detaching ssh)
		close(0); open("/dev/null", O_RDONLY);
		close(1); open("/dev/null", O_WRONLY);
		close(2); open("/dev/null", O_WRONLY);
	}
}

bool FileExists(char *strFilename) { 
	struct stat stFileInfo; 
	int intStat; 

	// Attempt to get the file attributes 
	intStat = stat(strFilename, &stFileInfo); 
	if(intStat == 0) { 
		// We were able to get the file attributes 
		// so the file obviously exists. 
		return true; 
	} else { 
		// We were not able to get the file attributes. 
		// This may mean that we don't have permission to 
		// access the folder which contains this file. If you 
		// need to do that level of checking, lookup the 
		// return values of stat which will give you 
		// more details on why stat failed. 
		return false; 
	} 
}

int yesno(const char *arg) {
	if(arg[0] == 'y' or arg[0] == '1') 
		return 1;
	else
		return 0;
}

int load_config(char *fname) {
	if(!FileExists(fname)) {
		return 1;
	}

	printf("Loading configuration from file %s\n", fname);

	CSimpleIniA ini;
	ini.SetUnicode();
	ini.SetMultiKey(true);
	ini.LoadFile(fname);
	const char *value;
	CSimpleIniA::TNamesDepend values;

	// sip ports
	if (ini.GetAllValues("general", "sipport", values)) {
		CSimpleIni::TNamesDepend::const_iterator i = values.begin();
		// reset default port 
		sipportmatrix[5060] = 0;
		for (; i != values.end(); ++i) {
			sipportmatrix[atoi(i->pItem)] = 1;
		}
	}

	// ipacc ports
	if (ini.GetAllValues("general", "ipaccountport", values)) {
		CSimpleIni::TNamesDepend::const_iterator i = values.begin();
		// reset default port 
		for (; i != values.end(); ++i) {
			if(!ipaccountportmatrix) {
				ipaccountportmatrix = (char*)calloc(1, sizeof(char) * 65537);
			}
			ipaccountportmatrix[atoi(i->pItem)] = 1;
		}
	}

	// nat aliases
	if (ini.GetAllValues("general", "natalias", values)) {
		char local_ip[30], extern_ip[30];
		in_addr_t nlocal_ip, nextern_ip;
		int len, j = 0, i;
		char *s = local_ip;
		CSimpleIni::TNamesDepend::const_iterator it = values.begin();

		for (; it != values.end(); ++it) {
			s = local_ip;
			j = 0;
			for(i = 0; i < 30; i++) {
				local_ip[i] = '\0';
				extern_ip[i] = '\0';
			}

			len = strlen(it->pItem);
			for(int i = 0; i < len; i++) {
				if(it->pItem[i] == ' ' or it->pItem[i] == ':' or it->pItem[i] == '=' or it->pItem[i] == ' ') {
					// moving s to b pointer (write to b ip
					s = extern_ip;
					j = 0;
				} else {
					s[j] = it->pItem[i];
					j++;
				}
			}
			if ((int32_t)(nlocal_ip = inet_addr(local_ip)) != -1 && (int32_t)(nextern_ip = inet_addr(extern_ip)) != -1 ){
				nat_aliases[nlocal_ip] = nextern_ip;
				if(verbosity > 3) printf("adding local_ip[%s][%u] = extern_ip[%s][%u]\n", local_ip, nlocal_ip, extern_ip, nextern_ip);
			}
		}
	}

	if((value = ini.GetValue("general", "interface", NULL))) {
		strncpy(ifname, value, sizeof(ifname));
	}
	if((value = ini.GetValue("general", "cleanspool_interval", NULL))) {
		opt_cleanspool_interval = atoi(value);
	}
	if((value = ini.GetValue("general", "cleanspool_size", NULL))) {
		opt_cleanspool_sizeMB = atoi(value);
	}
	if((value = ini.GetValue("general", "id_sensor", NULL))) {
		opt_id_sensor = atoi(value);
		insert_funcname = "__insert_";
		insert_funcname.append(value);
	}
	if((value = ini.GetValue("general", "pcapcommand", NULL))) {
		strncpy(pcapcommand, value, sizeof(pcapcommand));
	}
	if((value = ini.GetValue("general", "ringbuffer", NULL))) {
		opt_ringbuffer = MIN(atoi(value), 2000);
	}
	if((value = ini.GetValue("general", "rtpthreads", NULL))) {
		num_threads = atoi(value);
	}
	if((value = ini.GetValue("general", "rtptimeout", NULL))) {
		rtptimeout = atoi(value);
	}
	if((value = ini.GetValue("general", "rtpthread-buffer", NULL))) {
		rtpthreadbuffer = atoi(value);
	}
	if((value = ini.GetValue("general", "rtp-firstleg", NULL))) {
		opt_rtp_firstleg = yesno(value);
	}
	if((value = ini.GetValue("general", "sip-register", NULL))) {
		opt_sip_register = yesno(value);
	}
	if((value = ini.GetValue("general", "sip-register-active-nologbin", NULL))) {
		opt_sip_register_active_nologbin = yesno(value);
	}
	if((value = ini.GetValue("general", "deduplicate", NULL))) {
		opt_dup_check = yesno(value);
	}
	if((value = ini.GetValue("general", "mos_g729", NULL))) {
		opt_mos_g729 = yesno(value);
	}
	if((value = ini.GetValue("general", "nocdr", NULL))) {
		opt_nocdr = yesno(value);
	}
	if((value = ini.GetValue("general", "skinny", NULL))) {
		opt_skinny = yesno(value);
	}
	if((value = ini.GetValue("general", "cdr_partition", NULL))) {
		opt_cdr_partition = yesno(value);
	}
	if((value = ini.GetValue("general", "custom_headers", NULL))) {
		char *pos = (char*)value;
		while(pos && *pos) {
			char *posSep = strchr(pos, ';');
			if(posSep) {
				*posSep = 0;
			}
			string custom_header = pos;
			custom_header.erase(custom_header.begin(), std::find_if(custom_header.begin(), custom_header.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
			custom_header.erase(std::find_if(custom_header.rbegin(), custom_header.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), custom_header.end());
			string custom_header_field = "custom_header__" + custom_header;
			std::replace(custom_header_field.begin(), custom_header_field.end(), ' ', '_');
			opt_custom_headers.push_back(dstring(custom_header, custom_header_field));
			pos = posSep ? posSep + 1 : NULL;
		}
	}
	if((value = ini.GetValue("general", "savesip", NULL))) {
		opt_saveSIP = yesno(value);
	}
	if((value = ini.GetValue("general", "savertp", NULL))) {
		switch(value[0]) {
		case 'y':
		case 'Y':
		case '1':
			opt_saveRTP = 1;
			break;
		case 'h':
		case 'H':
			opt_saveRTP = 1;
			opt_onlyRTPheader = 1;
			break;
		}
	}
	if((value = ini.GetValue("general", "saverfc2833", NULL))) {
		opt_saverfc2833 = yesno(value);
	}
	if((value = ini.GetValue("general", "dtmf2db", NULL))) {
		opt_dbdtmf = yesno(value);
	}
	if((value = ini.GetValue("general", "saveudptl", NULL))) {
		opt_saveudptl = yesno(value);
	}
	if((value = ini.GetValue("general", "norecord-header", NULL))) {
		opt_norecord_header = yesno(value);
	}
	if((value = ini.GetValue("general", "norecord-dtmf", NULL))) {
		opt_norecord_dtmf = yesno(value);
	}
	if((value = ini.GetValue("general", "vmbuffer", NULL))) {
		qringmax = (unsigned int)((unsigned int)MIN(atoi(value), 4000) * 1024 * 1024 / (unsigned int)sizeof(pcap_packet));
	}
	if((value = ini.GetValue("general", "matchheader", NULL))) {
		snprintf(opt_match_header, sizeof(opt_match_header), "\n%s:", value);
	}
	//for compatibility 
	if((value = ini.GetValue("general", "match_header", NULL))) {
		snprintf(opt_match_header, sizeof(opt_match_header), "\n%s:", value);
	}
	if((value = ini.GetValue("general", "domainport", NULL))) {
		opt_domainport = atoi(value);
	}
	if((value = ini.GetValue("general", "managerport", NULL))) {
		opt_manager_port = atoi(value);
	}
	if((value = ini.GetValue("general", "managerip", NULL))) {
		strncpy(opt_manager_ip, value, sizeof(opt_manager_ip));
	}
	if((value = ini.GetValue("general", "managerclient", NULL))) {
		strncpy(opt_clientmanager, value, sizeof(opt_clientmanager) - 1);
	}
	if((value = ini.GetValue("general", "managerclientport", NULL))) {
		opt_clientmanagerport = atoi(value);
	}
	if((value = ini.GetValue("general", "savertcp", NULL))) {
		opt_saveRTCP = yesno(value);
	}
	if((value = ini.GetValue("general", "saveaudio", NULL))) {
		switch(value[0]) {
		case 'y':
		case '1':
		case 'w':
			opt_saveWAV = 1;
			opt_audio_format = FORMAT_WAV;
			break;
		case 'o':
			opt_saveWAV = 1;
			opt_audio_format = FORMAT_OGG;
			break;
		}
	}
	if((value = ini.GetValue("general", "savegraph", NULL))) {
		switch(value[0]) {
		case 'y':
		case '1':
		case 'p':
			opt_saveGRAPH = 1;
			break;
		case 'g':
			opt_saveGRAPH = 1;
			opt_gzipGRAPH = 1;
			break;
		}
	}
	if((value = ini.GetValue("general", "filter", NULL))) {
		strncpy(user_filter, value, sizeof(user_filter));
	}
	if((value = ini.GetValue("general", "cachedir", NULL))) {
		strncpy(opt_cachedir, value, sizeof(opt_cachedir));
		mkdir_r(opt_cachedir, 0777);
	}
	if((value = ini.GetValue("general", "spooldir", NULL))) {
		strncpy(opt_chdir, value, sizeof(opt_chdir));
		mkdir_r(opt_chdir, 0777);
	}
	if((value = ini.GetValue("general", "spooldiroldschema", NULL))) {
		opt_newdir = !yesno(value);
	}
	if((value = ini.GetValue("general", "pcapsplit", NULL))) {
		opt_pcap_split = yesno(value);
	}
	if((value = ini.GetValue("general", "scanpcapdir", NULL))) {
		strncpy(opt_scanpcapdir, value, sizeof(opt_scanpcapdir));
	}
	if((value = ini.GetValue("general", "promisc", NULL))) {
		opt_promisc = yesno(value);
	}
	if((value = ini.GetValue("general", "sqldriver", NULL))) {
		strncpy(sql_driver, value, sizeof(sql_driver));
	}
	if((value = ini.GetValue("general", "sqlcdrtable", NULL))) {
		strncpy(sql_cdr_table, value, sizeof(sql_cdr_table));
	}
	if((value = ini.GetValue("general", "sqlcdrtable_last30d", NULL))) {
		strncpy(sql_cdr_table_last30d, value, sizeof(sql_cdr_table_last30d));
	}
	if((value = ini.GetValue("general", "sqlcdrtable_last7d", NULL))) {
		strncpy(sql_cdr_table_last7d, value, sizeof(sql_cdr_table_last1d));
	}
	if((value = ini.GetValue("general", "sqlcdrtable_last1d", NULL))) {
		strncpy(sql_cdr_table_last7d, value, sizeof(sql_cdr_table_last1d));
	}
	if((value = ini.GetValue("general", "sqlcdrnexttable", NULL)) ||
	   (value = ini.GetValue("general", "sqlcdr_next_table", NULL))) {
		strncpy(sql_cdr_next_table, value, sizeof(sql_cdr_next_table));
	}
	if((value = ini.GetValue("general", "sqlcdruatable", NULL)) ||
	   (value = ini.GetValue("general", "sqlcdr_ua_table", NULL))) {
		strncpy(sql_cdr_ua_table, value, sizeof(sql_cdr_ua_table));
	}
	if((value = ini.GetValue("general", "sqlcdrsipresptable", NULL)) ||
	   (value = ini.GetValue("general", "sqlcdr_sipresp_table", NULL))) {
		strncpy(sql_cdr_sip_response_table, value, sizeof(sql_cdr_sip_response_table));
	}
	
	if((value = ini.GetValue("general", "mysqlhost", NULL))) {
		strncpy(mysql_host, value, sizeof(mysql_host));
	}
	if((value = ini.GetValue("general", "mysqlport", NULL))) {
		opt_mysql_port = atoi(value);
	}
	if((value = ini.GetValue("general", "myqslhost", NULL))) {
		printf("You have old version of config file! there were typo in myqslhost instead of mysqlhost! Fix your config! exiting...\n");
		syslog(LOG_ERR, "You have old version of config file! there were typo in myqslhost instead of mysqlhost! Fix your config! exiting...\n");
		exit(1);
	}
	if((value = ini.GetValue("general", "mysqldb", NULL))) {
		strncpy(mysql_database, value, sizeof(mysql_database));
	}
	if((value = ini.GetValue("general", "mysqltable", NULL))) {
		strncpy(mysql_table, value, sizeof(mysql_table));
	}
	if((value = ini.GetValue("general", "mysqlusername", NULL))) {
		strncpy(mysql_user, value, sizeof(mysql_user));
	}
	if((value = ini.GetValue("general", "mysqlpassword", NULL))) {
		strncpy(mysql_password, value, sizeof(mysql_password));
	}
	if((value = ini.GetValue("general", "odbcdsn", NULL))) {
		strncpy(odbc_dsn, value, sizeof(odbc_dsn));
	}
	if((value = ini.GetValue("general", "odbcuser", NULL))) {
		strncpy(odbc_user, value, sizeof(odbc_user));
	}
	if((value = ini.GetValue("general", "odbcpass", NULL))) {
		strncpy(odbc_password, value, sizeof(odbc_password));
	}
	if((value = ini.GetValue("general", "odbcdriver", NULL))) {
		strncpy(odbc_driver, value, sizeof(odbc_driver));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_sql_driver", NULL))) {
		strncpy(get_customer_by_ip_sql_driver, value, sizeof(get_customer_by_ip_sql_driver));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_odbc_dsn", NULL))) {
		strncpy(get_customer_by_ip_odbc_dsn, value, sizeof(get_customer_by_ip_odbc_dsn));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_odbc_user", NULL))) {
		strncpy(get_customer_by_ip_odbc_user, value, sizeof(get_customer_by_ip_odbc_user));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_odbc_password", NULL))) {
		strncpy(get_customer_by_ip_odbc_password, value, sizeof(get_customer_by_ip_odbc_password));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_odbc_driver", NULL))) {
		strncpy(get_customer_by_ip_odbc_driver, value, sizeof(get_customer_by_ip_odbc_driver));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_query", NULL))) {
		strncpy(get_customer_by_ip_query, value, sizeof(get_customer_by_ip_query));
	}
	if((value = ini.GetValue("general", "get_customers_ip_query", NULL))) {
		strncpy(get_customers_ip_query, value, sizeof(get_customers_ip_query));
	}
	if((value = ini.GetValue("general", "get_customers_radius_name_query", NULL))) {
		strncpy(get_customers_radius_name_query, value, sizeof(get_customers_radius_name_query));
	}
	if((value = ini.GetValue("general", "get_customer_by_pn_sql_driver", NULL))) {
		strncpy(get_customer_by_pn_sql_driver, value, sizeof(get_customer_by_pn_sql_driver));
	}
	if((value = ini.GetValue("general", "get_customer_by_pn_odbc_dsn", NULL))) {
		strncpy(get_customer_by_pn_odbc_dsn, value, sizeof(get_customer_by_pn_odbc_dsn));
	}
	if((value = ini.GetValue("general", "get_customer_by_pn_odbc_user", NULL))) {
		strncpy(get_customer_by_pn_odbc_user, value, sizeof(get_customer_by_pn_odbc_user));
	}
	if((value = ini.GetValue("general", "get_customer_by_pn_odbc_password", NULL))) {
		strncpy(get_customer_by_pn_odbc_password, value, sizeof(get_customer_by_pn_odbc_password));
	}
	if((value = ini.GetValue("general", "get_customer_by_pn_odbc_driver", NULL))) {
		strncpy(get_customer_by_pn_odbc_driver, value, sizeof(get_customer_by_pn_odbc_driver));
	}
	if((value = ini.GetValue("general", "get_customers_pn_query", NULL))) {
		strncpy(get_customers_pn_query, value, sizeof(get_customers_pn_query));
	}
	if((value = ini.GetValue("general", "national_prefix", NULL))) {
		char *pos = (char*)value;
		while(pos && *pos) {
			char *posSep = strchr(pos, ';');
			if(posSep) {
				*posSep = 0;
			}
			opt_national_prefix.push_back(pos);
			pos = posSep ? posSep + 1 : NULL;
		}
	}
	if((value = ini.GetValue("general", "get_radius_ip_driver", NULL))) {
		strncpy(get_radius_ip_driver, value, sizeof(get_radius_ip_driver));
	}
	if((value = ini.GetValue("general", "get_radius_ip_host", NULL))) {
		strncpy(get_radius_ip_host, value, sizeof(get_radius_ip_host));
	}
	if((value = ini.GetValue("general", "get_radius_ip_db", NULL))) {
		strncpy(get_radius_ip_db, value, sizeof(get_radius_ip_db));
	}
	if((value = ini.GetValue("general", "get_radius_ip_user", NULL))) {
		strncpy(get_radius_ip_user, value, sizeof(get_radius_ip_user));
	}
	if((value = ini.GetValue("general", "get_radius_ip_password", NULL))) {
		strncpy(get_radius_ip_password, value, sizeof(get_radius_ip_password));
	}
	if((value = ini.GetValue("general", "get_radius_ip_query", NULL))) {
		strncpy(get_radius_ip_query, value, sizeof(get_radius_ip_query));
	}
	if((value = ini.GetValue("general", "get_radius_ip_query_where", NULL))) {
		strncpy(get_radius_ip_query_where, value, sizeof(get_radius_ip_query_where));
	}
	if((value = ini.GetValue("general", "get_customer_by_ip_flush_period", NULL))) {
		get_customer_by_ip_flush_period = atoi(value);
	}
	if((value = ini.GetValue("general", "sipoverlap", NULL))) {
		opt_sipoverlap = yesno(value);
	}
	if((value = ini.GetValue("general", "dumpallpackets", NULL))) {
		opt_pcapdump = yesno(value);
	}
	if((value = ini.GetValue("general", "jitterbuffer_f1", NULL))) {
		switch(value[0]) {
		case 'Y':
		case 'y':
		case '1':
			opt_jitterbuffer_f1 = 1;
			break;
		default: 
			opt_jitterbuffer_f1 = 0;
			break;
		}
	}
	if((value = ini.GetValue("general", "jitterbuffer_f2", NULL))) {
		switch(value[0]) {
		case 'Y':
		case 'y':
		case '1':
			opt_jitterbuffer_f2 = 1;
			break;
		default: 
			opt_jitterbuffer_f2 = 0;
			break;
		}
	}
	if((value = ini.GetValue("general", "jitterbuffer_adapt", NULL))) {
		switch(value[0]) {
		case 'Y':
		case 'y':
		case '1':
			opt_jitterbuffer_adapt = 1;
			break;
		default: 
			opt_jitterbuffer_adapt = 0;
			break;
		}
	}
	if((value = ini.GetValue("general", "sqlcallend", NULL))) {
		opt_callend = yesno(value);
	}
	if((value = ini.GetValue("general", "cdrurl", NULL))) {
		strncpy(opt_cdrurl, value, sizeof(opt_cdrurl) - 1);
	}
	if((value = ini.GetValue("general", "mirrorip", NULL))) {
		opt_mirrorip = yesno(value);
	}
	if((value = ini.GetValue("general", "mirrorall", NULL))) {
		opt_mirrorall = yesno(value);
	}
	if((value = ini.GetValue("general", "mirroronly", NULL))) {
		opt_mirroronly = yesno(value);
	}
	if((value = ini.GetValue("general", "mirroripsrc", NULL))) {
		strncpy(opt_mirrorip_src, value, sizeof(opt_mirrorip_src));
	}
	if((value = ini.GetValue("general", "mirroripdst", NULL))) {
		strncpy(opt_mirrorip_dst, value, sizeof(opt_mirrorip_dst));
	}
	if((value = ini.GetValue("general", "printinsertid", NULL))) {
		opt_printinsertid = yesno(value);
	}
	if((value = ini.GetValue("general", "ipaccount", NULL))) {
		opt_ipaccount = yesno(value);
	}
	if((value = ini.GetValue("general", "ipaccount_interval", NULL))) {
		opt_ipacc_interval = atoi(value);
	}
	if((value = ini.GetValue("general", "ipaccount_sniffer_agregate", NULL))) {
		opt_ipacc_sniffer_agregate = yesno(value);
	}
	if((value = ini.GetValue("general", "ipaccount_agregate_only_customers_on_main_side", NULL))) {
		opt_ipacc_agregate_only_customers_on_main_side = yesno(value);
	}
	if((value = ini.GetValue("general", "ipaccount_agregate_only_customers_on_any_side", NULL))) {
		opt_ipacc_agregate_only_customers_on_any_side = yesno(value);
	}
	if((value = ini.GetValue("general", "ipaccount_multithread_save", NULL))) {
		opt_ipacc_multithread_save = yesno(value);
	}
	if((value = ini.GetValue("general", "cdronlyanswered", NULL))) {
		opt_cdronlyanswered = yesno(value);
	}
	if((value = ini.GetValue("general", "cdronlyrtp", NULL))) {
		opt_cdronlyrtp = yesno(value);
	}
	if((value = ini.GetValue("general", "callslimit", NULL))) {
		opt_callslimit = atoi(value);
	}
	if((value = ini.GetValue("general", "pauserecordingdtmf", NULL))) {
		strncpy(opt_silencedmtfseq, value, 15);
	}
	if((value = ini.GetValue("general", "keycheck", NULL))) {
		strncpy(opt_keycheck, value, 1024);
	}
	if((value = ini.GetValue("general", "convertchar", NULL))) {
		strncpy(opt_convert_char, value, sizeof(opt_convert_char));
	}
	if((value = ini.GetValue("general", "openfile_max", NULL))) {
                opt_openfile_max = atoi(value);
        }
	return 0;
}

void reload_config() {
	load_config(configfile);

	request_iptelnum_reload = 1;
}

void reload_capture_rules() {

	if(ipfilter_reload) {
		delete ipfilter_reload;
	}

	ipfilter_reload = new IPfilter;
	ipfilter_reload->load();
	ipfilter_reload_do = 1;

	if(telnumfilter_reload) {
		delete telnumfilter_reload;
	}

	telnumfilter_reload = new TELNUMfilter;
	telnumfilter_reload->load();
	telnumfilter_reload_do = 1;
}

#ifdef BACKTRACE
#ifndef USE_SIGCONTEXT
void bt_sighandler(int sig, siginfo_t *info, void *secret)
#else
void bt_sighandler(int sig, struct sigcontext ctx)
#endif
{

        void *trace[16];
        char **messages = (char **)NULL;
        int i, trace_size = 0;

        signal_def *d = NULL;
        for (i = 0; i < (int)(sizeof(signal_data) / sizeof(signal_def)); i++)
                if (sig == signal_data[i].id)
                        { d = &signal_data[i]; break; }
        if (d) 
                syslog(LOG_ERR, "Got signal 0x%02X (%s): %s\n", sig, signal_data[i].name, signal_data[i].description);
        else   
                syslog(LOG_ERR, "Got signal 0x%02X\n", sig);

        #ifndef USE_SIGCONTEXT

                void *pnt = NULL;
                #if defined(__x86_64__)
                        ucontext_t* uc = (ucontext_t*) secret;
                        pnt = (void*) uc->uc_mcontext.gregs[REG_RIP] ;
                #elif defined(__hppa__)
                        ucontext_t* uc = (ucontext_t*) secret;
                        pnt = (void*) uc->uc_mcontext.sc_iaoq[0] & ~0×3UL ;
                #elif (defined (__ppc__)) || (defined (__powerpc__))
                        ucontext_t* uc = (ucontext_t*) secret;
                        pnt = (void*) uc->uc_mcontext.regs->nip ;
                #elif defined(__sparc__)
                struct sigcontext* sc = (struct sigcontext*) secret;
                        #if __WORDSIZE == 64
                                pnt = (void*) scp->sigc_regs.tpc ;
                        #else  
                                pnt = (void*) scp->si_regs.pc ;
                        #endif
                #elif defined(__i386__)
                        ucontext_t* uc = (ucontext_t*) secret;
                        pnt = (void*) uc->uc_mcontext.gregs[REG_EIP] ;
                #endif
        /* potentially correct for other archs:
         * alpha: ucp->m_context.sc_pc
         * arm: ucp->m_context.ctx.arm_pc
         * ia64: ucp->m_context.sc_ip & ~0×3UL
         * mips: ucp->m_context.sc_pc
         * s390: ucp->m_context.sregs->regs.psw.addr
         */

        if (sig == SIGSEGV)
                syslog(LOG_ERR,"Faulty address is %p, called from %p\n", info->si_addr, pnt);

        /* The first two entries in the stack frame chain when you
         * get into the signal handler contain, respectively, a
         * return address inside your signal handler and one inside
         * sigaction() in libc. The stack frame of the last function
         * called before the signal (which, in case of fault signals,
         * also is the one that supposedly caused the problem) is lost.
         */

        /* the third parameter to the signal handler points to an
         * ucontext_t structure that contains the values of the CPU
         * registers when the signal was raised.
         */
        trace_size = backtrace(trace, 16);
        /* overwrite sigaction with caller's address */
        trace[1] = pnt;

        #else

        if (sig == SIGSEGV)
                syslog(LOG_ERR("Faulty address is %p, called from %p\n",
                        ctx.cr2, ctx.eip);

        /* An undocumented parameter of type sigcontext that is passed
         * to the signal handler (see the UNDOCUMENTED section in man
         * sigaction) and contains, among other things, the value of EIP
         * when the signal was raised. Declared obsolete in adherence
         * with POSIX.1b since kernel version 2.2
         */

        trace_size = backtrace(trace, 16);
        /* overwrite sigaction with caller's address */
        trace[1] = (void *)ctx.eip;
        #endif

        messages = backtrace_symbols(trace, trace_size);
        /* skip first stack frame (points here) */
        syslog(LOG_ERR, "[bt] Execution path:\n");
        for (i=1; i<trace_size; ++i)
                syslog(LOG_ERR, "[bt] %s\n", messages[i]);

	/* those two lines causes core dump generation */
	signal(sig, SIG_DFL);
	kill(getpid(), sig);
}
#endif

int opt_test = 0;
void *readdump_libpcap_thread_fce(void *handle);
void test();

int main(int argc, char *argv[]) {

#ifdef BACKTRACE

        /* Install our signal handler */
        struct sigaction sa;

        sa.sa_sigaction = bt_sighandler;
        sigemptyset (&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_SIGINFO;

        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
//        sigaction(SIGUSR1, &sa, NULL);
//        sigaction(SIGUSR2, &sa, NULL);
	
#endif

	/* parse arguments */

	char fname[1024] = "";	// pcap file to read on 
	ifname[0] = '\0';
	opt_mirrorip_src[0] = '\0';
	opt_mirrorip_dst[0] = '\0';
	strcpy(opt_chdir, "/var/spool/voipmonitor");
	strcpy(opt_cachedir, "");
	sipportmatrix = (char*)calloc(1, sizeof(char) * 65537);
	// set default SIP port to 5060
	sipportmatrix[5060] = 1;

	pthread_mutex_init(&mysqlquery_lock, NULL);

	// if the system has more than one CPU enable threading
	opt_pcap_threaded = sysconf( _SC_NPROCESSORS_ONLN ) > 1; 
	num_threads = sysconf( _SC_NPROCESSORS_ONLN ) - 1;
	set_mac();

#ifdef ISCURL
	curl_global_init(CURL_GLOBAL_ALL);
#endif

	int option_index = 0;
	static struct option long_options[] = {
	    {"gzip-graph", 0, 0, '1'},
	    {"gzip-pcap", 0, 0, '2'},
	    {"deduplicate", 0, 0, 'L'},
	    {"dump-allpackets", 0, 0, 'M'},
	    {"save-sip", 0, 0, 'S'},
	    {"save-rtp", 0, 0, 'R'},
	    {"skip-rtppayload", 0, 0, 'o'},
	    {"save-udptl", 0, 0, 'D'},
	    {"save-rtcp", 0, 0, '9'},
	    {"save-raw", 0, 0, 'A'},
	    {"save-audio", 0, 0, 'W'},
	    {"no-cdr", 0, 0, 'c'},
	    {"save-graph", 2, 0, 'G'},
	    {"mysql-server", 1, 0, 'h'},
	    {"mysql-port", 1, 0, 'O'},
	    {"mysql-database", 1, 0, 'b'},
	    {"mysql-username", 1, 0, 'u'},
	    {"mysql-password", 1, 0, 'p'},
	    {"mysql-table", 1, 0, 't'},
	    {"pid-file", 1, 0, 'P'},
	    {"rtp-timeout", 1, 0, 'm'},
	    {"rtp-firstleg", 0, 0, '3'},
	    {"sip-register", 0, 0, '4'},
	    {"audio-format", 1, 0, '5'},
	    {"ring-buffer", 1, 0, '6'},
	    {"vm-buffer", 1, 0, 'T'},
	    {"rtp-threads", 1, 0, 'e'},
	    {"rtpthread-buffer", 1, 0, 'E'},
	    {"config-file", 1, 0, '7'},
	    {"manager-port", 1, 0, '8'},
	    {"pcap-command", 1, 0, 'a'},
	    {"norecord-header", 0, 0, 'N'},
	    {"norecord-dtmf", 0, 0, 'K'},
	    {"rtp-nosig", 0, 0, 'I'},
	    {"cachedir", 1, 0, 'C'},
	    {"id-sensor", 1, 0, 's'},
	    {"ipaccount", 0, 0, 'x'},
	    {"pcapscan-dir", 1, 0, '0'},
	    {"keycheck", 1, 0, 'Z'},
	    {"keycheck", 1, 0, 'Z'},
	    {"pcapfilter", 1, 0, 'f'},
	    {"interface", 1, 0, 'i'},
	    {"read", 1, 0, 'r'},
	    {"spooldir", 1, 0, 'd'},
	    {"verbose", 1, 0, 'v'},
	    {"nodaemon", 0, 0, 'k'},
	    {"promisc", 0, 0, 'n'},
	    {"pcapbuffered", 0, 0, 'U'},
	    {"test", 0, 0, 'X'},
	    {"allsipports", 0, 0, 'y'},
	    {"skinny", 0, 0, 200},
	    {0, 0, 0, 0}
	};

	terminating = 0;
	terminating2 = 0;

	umask(0000);

	openlog("voipmonitor", LOG_CONS | LOG_PERROR | LOG_PID, LOG_DAEMON);

	/* command line arguments overrides configuration in voipmonitor.conf file */
	while(1) {
		int c;
		c = getopt_long(argc, argv, "C:f:i:r:d:v:O:h:b:t:u:p:P:s:T:D:e:E:m:LkncUSRoAWGXNIKy4Mx", long_options, &option_index);
		//"i:r:d:v:h:b:u:p:fnU", NULL, NULL);
		if (c == -1)
			break;

		switch (c) {
			/*
			case 0:
				printf ("option %s\n", long_options[option_index].name);
				break;
			*/
			case 200:
				opt_skinny = 1;
				break;
			case 'x':
				opt_ipaccount = 1;
				break;
			case 'y':
				for(int i = 5060; i < 5099; i++) {
					sipportmatrix[i] = 1;
				}
				sipportmatrix[443] = 1;
				sipportmatrix[80] = 1;
				break;
			case 'm':
				rtptimeout = atoi(optarg);
				break;
			case 'M':
				opt_pcapdump = 1;
				break;
			case 'e':
				num_threads = atoi(optarg);
				break;
			case 'E':
				rtpthreadbuffer = atoi(optarg);
				break;
			case 'T':
				qringmax = (unsigned int)((unsigned int)MIN(atoi(optarg), 4000) * 1024 * 1024 / (unsigned int)sizeof(pcap_packet));
				break;
			case 's':
				opt_id_sensor = atoi(optarg);
				insert_funcname = "__insert_";
				insert_funcname.append(optarg);
				break;
			case 'Z':
				strncpy(opt_keycheck, optarg, sizeof(opt_keycheck));
				break;
			case '0':
				strncpy(opt_scanpcapdir, optarg, sizeof(opt_scanpcapdir));
				break;
			case 'a':
				strncpy(pcapcommand, optarg, sizeof(pcapcommand));
				break;
			case 'I':
				opt_rtpnosip = 1;
				break;
			case 'L':
				opt_dup_check = 1;
				break;
			case 'K':
				opt_norecord_dtmf = 1;
				break;
			case 'N':
				opt_norecord_header = 1;
				break;
			case '1':
				opt_gzipGRAPH = 1;
				break;
			case '2':
				opt_gzipPCAP = 1;
				break;
			case '3':
				opt_rtp_firstleg = 1;
				break;
			case '4':
				opt_sip_register = 1;
				break;
			case '5':
				if(optarg[0] == 'o') {
					opt_audio_format = FORMAT_OGG;
				} else {
					opt_audio_format = FORMAT_WAV;
				}
				break;
			case '6':
				opt_ringbuffer = MIN(atoi(optarg), 2000);
				break;
			case '7':
				strncpy(configfile, optarg, sizeof(configfile));
				load_config(configfile);
				break;
			case '8':
				opt_manager_port = atoi(optarg);
				if(char *pointToSeparator = strchr(optarg,'/')) {
					strncpy(opt_manager_ip, pointToSeparator+1, sizeof(opt_manager_ip));
				}
				break;
			case '9':
				opt_saveRTCP = 1;
				break;
			case 'i':
				strncpy(ifname, optarg, sizeof(ifname));
				break;
			case 'v':
				verbosity = atoi(optarg);
				break;
			case 'r':
				strcpy(fname, optarg);
				opt_read_from_file = 1;
				opt_scanpcapdir[0] = '\0';
				//opt_cachedir[0] = '\0';
				break;
			case 'c':
				opt_nocdr = 1;
				break;
			case 'C':
				strncpy(opt_cachedir, optarg, sizeof(opt_cachedir));
				break;
			case 'd':
				strncpy(opt_chdir, optarg, sizeof(opt_chdir));
				mkdir_r(opt_chdir, 0777);
				break;
			case 'k':
				opt_fork = 0;
				break;
			case 'n':
				opt_promisc = 0;
				break;
			case 'U':
				opt_packetbuffered=1;
				break;
			case 'h':
				strncpy(mysql_host, optarg, sizeof(mysql_host));
				break;
			case 'O':
				opt_mysql_port = atoi(optarg);
				break;
			case 'b':
				strncpy(mysql_database, optarg, sizeof(mysql_database));
				break;
			case 't':
				strncpy(mysql_table, optarg, sizeof(mysql_table));
				break;
			case 'u':
				strncpy(mysql_user, optarg, sizeof(mysql_user));
				break;
			case 'p':
				strncpy(mysql_password, optarg, sizeof(mysql_password));
				break;
			case 'P':
				strncpy(opt_pidfile, optarg, sizeof(opt_pidfile));
				break;
			case 'f':
				strncpy(user_filter, optarg, sizeof(user_filter));
				break;
			case 'S':
				opt_saveSIP = 1;
				break;
			case 'R':
				opt_saveRTP = 1;
				break;
			case 'D':
				opt_saveudptl = 1;
				break;
			case 'o':
				opt_onlyRTPheader = 1;
				break;
			case 'A':
				opt_saveRAW = 1;
				break;
			case 'W':
				opt_saveWAV = 1;
				opt_savewav_force = 1;
				break;
			case 'G':
				opt_saveGRAPH = 1;
				if(optarg && optarg[0] == 'g') {
					opt_gzipGRAPH = 1;
				}
				break;
			case 'X':
				opt_test = 1;
				break;
		}
	}
	if(opt_ipaccount) {
		initIpacc();
	}
	if ((fname[0] == '\0') && (ifname[0] == '\0') && opt_scanpcapdir[0] == '\0'){
                        /* Ruler to assist with keeping help description to max. 80 chars wide:
                                  1         2         3         4         5         6         7         8
                         12345678901234567890123456789012345678901234567890123456789012345678901234567890
                        */
                printf("\nvoipmonitor version %s\n"
                        "\nUsage: voipmonitor [OPTIONS]\n"
                        "\n"
                        " -A, --save-raw\n"
                        "      Save RTP payload to RAW format. Default is disabled.\n"
                        "\n"
                        " -b <database>, --mysql-database=<database>\n"
                        "      mysql database, default voipmonitor\n"
                        "\n"
                        " -C <dir>, --cachedir=<dir>\n"
                        "      Store pcap file to <dir> and move it after call ends to spool directory.\n"
                        "      Moving all files are guaranteed to be serialized which solves slow\n"
                        "      random write I/O on magnetic or other media.  Typical cache directory\n"
                        "      is /dev/shm/voipmonitor which is in RAM and grows automatically or\n"
                        "      /mnt/ssd/voipmonitor which is mounted to SSD disk or some very fast\n"
                        "      SAS/SATA disk where spool can be network storage or raid5 etc.\n"
                        "      Wav files are not implemented yet\n"
                        "\n"
                        " -c, --no-cdr\n"
                        "      Do no save CDR to MySQL database.\n"
                        "\n"
                        " -D, --save-udptl\n"
                        "      Save UDPTL packets (T.38).  If savertp = yes the UDPTL packets are saved\n"
                        "      automatically.  If savertp = no and you want to save only udptl packets\n"
                        "      enable saveudptl = yes and savertp = no\n"
                        "\n"
                        " -d <dir>\n"
                        "      Where to store pcap files.  Default is /var/spool/voipmonitor\n"
                        "\n"
                        " -E <n>, --rtpthread-buffer=<n>\n"
                        "      Size of rtp thread ring buffer in MB. Default is 20MB per thread.\n"
                        "\n"
                        " -e <n>, --rtp-threads=<n>\n"
                        "      Number of threads to process RTP packets. If not specified it will be\n"
                        "      number of available CPUs.  If equel to zero RTP threading will be turned\n"
                        "      off.  Each thread allocates default 20MB for buffers.  This buffer can be\n"
                        "      controlled with --rtpthread-buffer.  For < 150 concurrent calls you can\n"
                        "      turn it off.\n"
                        "\n"
                        " -f <filter>\n"
                        "      Pcap filter.  If you will use only UDP, set to udp.  WARNING: If you set\n"
                        "      protocol to 'udp' pcap discards VLAN packets.  Maximum size is 2040\n"
                        "      characters.\n"
                        "\n"
                        " -G [plain|gzip], --save-graph=[plain|gzip]\n"
                        "      Save GRAPH data to graph file.  Default is disabled.  If enabled without\n"
                        "      a value, 'plain' is used.\n"
                        "\n"
                        " -h <hostname>, --mysql-server=<hostname>\n"
                        "      mysql server - default localhost\n"
                        "\n"
                        " -i <interface>\n"
                        "      Interface on which to listen.  Example: eth0\n"
                        "\n"
                        " -k   Do not fork or detach from controlling terminal.\n"
                        "\n"
                        " -L, --deduplicate\n"
                        "      Duplicate check do md5 sum for each packet and if md5 is same as previous\n"
                        "      packet it will discard it.  WARNING: md5 is expensive function (slows\n"
                        "      voipmonitor 3 times) so use it only if you have enough CPU or for pcap\n"
                        "      conversion only.\n"
                        "\n"
                        " -M, --dump-allpackets\n"
                        "      Dump all packets to /tmp/voipmonitor-[UNIX_TIMESTAMP].pcap\n"
                        "\n"
                        " -m <n>, --rtp-timeout=<n>\n"
                        "      rtptimeout is important value which specifies how much seconds from the\n"
                        "      last SIP packet or RTP packet is call closed and writen to database. It\n"
                        "      means that if you need to monitor ONLY SIP you have to set this to at\n"
                        "      least 2 hours = 7200 assuming your calls is not longer than 2 hours. Take\n"
                        "      in mind that seting this to very large value will cause to keep call in\n"
                        "      memory in case the call lost BYE and can consume all memory and slows\n"
                        "      down the sniffer - so do not set it to very high numbers.\n"
                        "      Default is 300 seconds. \n"
                        "\n"
                        " -n   Do not put the interface into promiscuous mode.\n"
                        "\n"
                        " -O <port>, --mysql-port=<port>\n"
                        "      mysql server - default localhost\n"
                        "\n"
                        " -o, --skip-rtppayload\n"
                        "      Skip RTP payload and save only RTP headers.\n"
                        "\n"
                        " -P <pid-file>, --pid-file=<pid-file>\n"
                        "      pid file, default /var/run/voipmonitor.pid\n"
                        "\n"
                        " -p <password>, --mysql-password=<password>\n"
                        "      mysql password, default is empty\n"
                        "\n"
                        " -R, --save-rtp\n"
                        "      Save RTP packets to pcap file. Default is disabled. Whan enabled RTCP\n"
                        "      packets will be saved too.\n"
                        "\n"
                        " -r <pcap-file>\n"
                        "      Read packets from <pcap-file>.\n"
                        "\n"
                        " -S, --save-sip\n"
                        "      Save SIP packets to pcap file.  Default is disabled.\n"
                        "\n"
                        " -s <num>, --id-sensor=<num>\n"
                        "      If set the number is saved to sql cdr.id_sensor.  Used to uniquely\n"
                        "      identify a copy of voipmonitor where many servers with voipmonitor are\n"
                        "      writing to a common database.\n"
                        "\n"
                        " -t <table>, --mysql-table=<table>\n"
                        "      mysql table, default cdr\n"
                        "\n"
                        " -U   Make .pcap files writing packet-buffered - more slow method, but you can\n"
                        "      use partialy writen file anytime, it will be consistent.\n"
                        "\n"
                        " -u <username>, --mysql-username=<username>\n"
                        "      mysql username, default voipmonitor\n"
                        "\n"
                        " -v <level-number>\n"
                        "      Set verbosity level (higher number is more verbose).\n"
                        "\n"
                        " -W, --save-audio\n"
                        "      Save RTP packets and covert it to one WAV file. Default is disabled.\n"
                        "\n"
                        " -y   Listen to SIP protocol on ports 5060 - 5099\n"
                        "\n"
                        " --audio-format=<wav|ogg>\n"
                        "      Save to WAV or OGG audio format. Default is WAV.\n"
                        "\n"
                        " --config-file=<filename>\n"
                        "      Specify configuration file full path.  Suggest /etc/voipmonitor.conf\n"
                        "\n"
                        " --manager-port=<port-number>\n"
                        "      TCP port top which manager interface should bind.  Default is 5029.\n"
                        "\n"
                        " --norecord-header\n"
                        "      If any of SIP message during the call contains header\n"
                        "      X-VoipMonitor-norecord call will be not converted to wav and pcap file\n"
                        "      will be deleted.\n"
                        "\n"
                        " --ring-buffer=<n>\n"
                        "      Set ring buffer in MB (feature of newer >= 2.6.31 kernels and\n"
                        "      libpcap >= 1.0.0).  If you see voipmonitor dropping packets in syslog\n"
                        "      upgrade to newer kernel and increase --ring-buffer to higher MB.\n"
                        "      Ring-buffer is between kernel and pcap library.  The top reason why\n"
                        "      voipmonitor drops packets is waiting for I/O operations or it consumes\n"
                        "      100%% CPU.\n"
                        "\n"
                        " --rtp-firstleg\n"
                        "      This is important option if voipmonitor is sniffing on SIP proxy and see\n"
                        "      both RTP leg of CALL.  In that case use this option.  It will analyze RTP\n"
                        "      only for the first LEG and not each 4 RTP streams which will confuse\n"
                        "      voipmonitor. Drawback of this switch is that voipmonitor will analyze\n"
                        "      SDP only for SIP packets which have the same IP and port of the first\n"
                        "      INVITE source IP and port.  It means it will not work in case where phone\n"
                        "      sends INVITE from a.b.c.d:1024 and SIP proxy replies to a.b.c.d:5060.  If\n"
                        "      you have better idea how to solve this problem better please contact\n"
                        "      support@voipmonitor.org\n"
                        "\n"
                        " --rtp-nosig\n"
                        "      Analyze calls based on RTP only - handy if you want extract call which\n"
                        "      does not have signalization (or H323 calls which voipmonitor does not\n"
                        "      know yet).\n"
                        "\n"
                        " --save-rtcp\n"
                        "      Save RTCP packets to pcap file.  You can enable SIP signalization + only\n"
                        "      RTCP packets and not RTP packets.\n"
                        "\n"
			" --skinny\n"
			"      analyze SKINNY VoIP protocol on TCP port 2000\n"
                        "\n"
                        " --sip-messages\n"
                        "      Save REGISTER messages.\n"
                        "\n"
                        " --sip-register\n"
                        "      Save SIP register requests to cdr.register table and to pcap file.\n"
                        "\n"
                        " --update-schema\n"
                        "      Create or upgrade the database schema, and then exit.  Forces -k option\n"
                        "      and will use 'root' user to perform operations, so supply root's password\n"
                        "      with the -p option.  For safety, this is not compatible with the\n"
                        "      --config-file option.\n"
                        "\n"
                        " --vm-buffer=<n>\n"
                        "      vmbuffer is user space buffers in MB which is used in case there is more\n"
                        "      than 1 CPU and the sniffer run two threads - one for reading data from\n"
                        "      libpcap and writing to vmbuffer and second reads data from vmbuffer and\n"
                        "      processes it.  For very high network loads set this to very high number.\n"
                        "      In case the system is droping packets (which is logged to syslog)\n"
                        "      increase this value.  Default is 20 MB\n"
                        "\n"
                        "One of <-i interface> or <-r pcap-file> must be specified, otherwise you may\n"
                        "set interface in configuration file.\n\n"
                        , RTPSENSOR_VERSION);
                        /*        1         2         3         4         5         6         7         8
                         12345678901234567890123456789012345678901234567890123456789012345678901234567890
                           Ruler to assist with keeping help description to max. 80 chars wide:
                        */

		return 1;
	}

	cout << "SQL DRIVER: " << sql_driver << endl;
	if(isSqlDriver("mysql")) {
		sqlDb = new SqlDb_mysql();
		sqlDb->setConnectParameters(mysql_host, mysql_user, mysql_password, mysql_database);
		sqlStore = new MySqlStore(mysql_host, mysql_user, mysql_password, mysql_database);
	} else if(isSqlDriver("odbc")) {
		SqlDb_odbc *sqlDb_odbc = new SqlDb_odbc();
		sqlDb_odbc->setOdbcVersion(SQL_OV_ODBC3);
		sqlDb_odbc->setSubtypeDb(odbc_driver);
		sqlDb = sqlDb_odbc;
		sqlDb->setConnectParameters(odbc_dsn, odbc_user, odbc_password);
	}
	sqlDb->enableSysLog();
	if(!opt_nocdr) {
		if(sqlDb->connect()) {
			sqlDb->createSchema();
			sqlDb->checkSchema();
		}
	}

	signal(SIGINT,sigint_handler);
	signal(SIGTERM,sigterm_handler);
	
	calltable = new Calltable;

	// preparing pcap reading and pcap filters 
	
	bpf_u_int32 mask;		// Holds the subnet mask associated with device.
	char errbuf[PCAP_ERRBUF_SIZE];	// Returns error text and is only set when the pcap_lookupnet subroutine fails.
	
	if(opt_test) {
		test();
		if(sqlDb) {
			delete sqlDb;
		}
		if(sqlStore) {
			delete sqlStore;
		}
		return(0);
	}
	rtp_threaded = num_threads > 0;

	// check if sniffer will be reading pcap files from dir and if not if it reads from eth interface or read only one file
	if(opt_scanpcapdir[0] == '\0') {
		if (fname[0] == '\0' && ifname[0] != '\0'){
			bpf_u_int32 net;

			printf("Capturing on interface: %s\n", ifname);
			// Find the properties for interface 
			if (pcap_lookupnet(ifname, &net, &mask, errbuf) == -1) {
				// if not available, use default
				mask = PCAP_NETMASK_UNKNOWN;
			}
	/*
			handle = pcap_open_live(ifname, 1600, opt_promisc, 1000, errbuf);
			if (handle == NULL) {
				fprintf(stderr, "Couldn't open inteface '%s': %s\n", ifname, errbuf);
				return(2);
			}
	*/

			/* to set own pcap_set_buffer_size it must be this way and not useing pcap_lookupnet */

			int status = 0;
			if((handle = pcap_create(ifname, errbuf)) == NULL) {
				fprintf(stderr, "pcap_create failed on iface '%s': %s\n", ifname, errbuf);
				return(2);
			}
			if((status = pcap_set_snaplen(handle, 3200)) != 0) {
				fprintf(stderr, "error pcap_set_snaplen\n");
				return(2);
			}
			if((status = pcap_set_promisc(handle, opt_promisc)) != 0) {
				fprintf(stderr, "error pcap_set_promisc\n");
				return(2);
			}
			if((status = pcap_set_timeout(handle, 1000)) != 0) {
				fprintf(stderr, "error pcap_set_timeout\n");
				return(2);
			}

			/* this is not possible for libpcap older than 1.0.0 so now voipmonitor requires libpcap > 1.0.0
				set ring buffer size to 5M to prevent packet drops whan CPU goes high or on very high traffic 
				- default is 2MB for libpcap > 1.0.0
				- for libpcap < 1.0.0 it is controled by /proc/sys/net/core/rmem_default which is very low 
			*/
			if((status = pcap_set_buffer_size(handle, opt_ringbuffer * 1024 * 1024)) != 0) {
				fprintf(stderr, "error pcap_set_buffer_size\n");
				return(2);
			}

			if((status = pcap_activate(handle)) != 0) {
				fprintf(stderr, "libpcap error: [%s]\n", pcap_geterr(handle));
				return(2);
			}
		} else {
			// if reading file
			rtp_threaded = 0;
			opt_mirrorip = 0; // disable mirroring packets when reading pcap files from file
//			opt_cachedir[0] = '\0'; //disabling cache if reading from file 
			opt_pcap_threaded = 0; //disable threading because it is useless while reading packets from file
			opt_cleanspool_interval = 0; // disable cleaning spooldir when reading from file 
			opt_manager_port = 0; // disable cleaning spooldir when reading from file 
			printf("Reading file: %s\n", fname);
			mask = PCAP_NETMASK_UNKNOWN;
			handle = pcap_open_offline(fname, errbuf);
			if(handle == NULL) {
				fprintf(stderr, "Couldn't open pcap file '%s': %s\n", fname, errbuf);
				return(2);
			}
		}

		if(opt_mirrorip) {
			if(opt_mirrorip_dst[0] == '\0') {
				syslog(LOG_ERR, "Mirroring SIP packets disabled because mirroripdst was not set");
				opt_mirrorip = 0;
			} else {
				syslog(LOG_NOTICE, "Starting SIP mirroring [%s]->[%s]", opt_mirrorip_src, opt_mirrorip_dst);
				mirrorip = new MirrorIP(opt_mirrorip_src, opt_mirrorip_dst);
			}
		}

		char filter_exp[2048] = "";		// The filter expression
		struct bpf_program fp;		// The compiled filter 

		if(*user_filter != '\0') {
			snprintf(filter_exp, sizeof(filter_exp), "%s", user_filter);

			// Compile and apply the filter
			if (pcap_compile(handle, &fp, filter_exp, 0, mask) == -1) {
				fprintf(stderr, "Couldn't parse filter %s: %s\n", filter_exp, pcap_geterr(handle));
				return(2);
			}
			if (pcap_setfilter(handle, &fp) == -1) {
				fprintf(stderr, "Couldn't install filter %s: %s\n", filter_exp, pcap_geterr(handle));
				return(2);
			}
		}
	}
	//opt_pcap_threaded = 0; //disable threading because it is useless while reading packets from file

	chdir(opt_chdir);

	// set maximum open files 
	struct rlimit rlp;
        rlp.rlim_cur = opt_openfile_max;
        rlp.rlim_max = opt_openfile_max;
        setrlimit(RLIMIT_NOFILE, &rlp);
        getrlimit(RLIMIT_NOFILE, &rlp);
        if(rlp.rlim_cur < 65535) {
                printf("Warning, max open files is: %d consider raise this to 65535 with ulimit -n 65535 and set it in config file\n", (int)rlp.rlim_cur);
        }
	// set core file dump to unlimited size
	rlp.rlim_cur = UINT_MAX;
	rlp.rlim_max = UINT_MAX;
	setrlimit(RLIMIT_CORE, &rlp);

	ipfilter = new IPfilter;
	if(!opt_nocdr) {
		ipfilter->load();
	}
//	ipfilter->dump();

	if(opt_ipaccount and !ipaccountportmatrix) {
		ipaccountportmatrix = (char*)calloc(1, sizeof(char) * 65537);
	}


	telnumfilter = new TELNUMfilter;
	if(!opt_nocdr) {
		telnumfilter->load();
	}

	// filters are ok, we can daemonize 
	if (opt_fork){
		daemonize();
	}
	
	// start thread processing queued cdr 
	pthread_create(&call_thread, NULL, storing_cdr, NULL);

	if(opt_cachedir[0] != '\0') {
		pthread_create(&cachedir_thread, NULL, moving_cache, NULL);
	}

	if(opt_cleanspool_interval > 0 && opt_cleanspool_sizeMB > 0) {
		if(verbosity > 0) syslog(LOG_NOTICE, "Spawning cleanspool_thread interval[%d]s size[%d]MB", opt_cleanspool_interval, opt_cleanspool_sizeMB);
		pthread_create(&cleanspool_thread, NULL, clean_spooldir, NULL);
	}

	// start manager thread 	
	if(opt_manager_port > 0) {
		pthread_create(&manager_thread, NULL, manager_server, NULL);
		// start reversed manager thread
		if(opt_clientmanager[0] != '\0') {
			pthread_create(&manager_client_thread, NULL, manager_client, NULL);
		}
	};

	// start reading threads
	if(rtp_threaded) {
		threads = (read_thread*)malloc(sizeof(read_thread) * num_threads);
		for(int i = 0; i < num_threads; i++) {
#ifdef QUEUE_MUTEX
			pthread_mutex_init(&(threads[i].qlock), NULL);
			sem_init(&(threads[i].semaphore), 0, 0);
#endif

#ifdef QUEUE_NONBLOCK
			threads[i].pqueue = NULL;
			queue_new(&(threads[i].pqueue), 10000);
#endif

#ifdef QUEUE_NONBLOCK2
			threads[i].vmbuffermax = rtpthreadbuffer * 1024 * 1024 / sizeof(rtp_packet);
			threads[i].writeit = 0;
			threads[i].readit = 0;
			threads[i].vmbuffer = (rtp_packet*)malloc(sizeof(rtp_packet) * (threads[i].vmbuffermax + 1));
			for(int j = 0; j < threads[i].vmbuffermax + 1; j++) {
				threads[i].vmbuffer[j].free = 1;
			}
#endif

			pthread_create(&(threads[i].thread), NULL, rtp_read_thread_func, (void*)&threads[i]);
		}
	}
	if(opt_pcap_threaded) {
#ifdef QUEUE_MUTEX
		pthread_mutex_init(&readpacket_thread_queue_lock, NULL);
		sem_init(&readpacket_thread_semaphore, 0, 0);
#endif

#ifdef QUEUE_NONBLOCK
		queue_new(&qs_readpacket_thread_queue, 100000);
		pthread_create(&pcap_read_thread, NULL, pcap_read_thread_func, NULL);
#endif

#ifdef QUEUE_NONBLOCK2
		qring = (pcap_packet*)malloc((size_t)((unsigned int)sizeof(pcap_packet) * (qringmax + 1)));
		for(unsigned int i = 0; i < qringmax + 1; i++) {
			qring[i].free = 1;
		}
		pthread_create(&pcap_read_thread, NULL, pcap_read_thread_func, NULL);
#endif 
	}

	if(opt_scanpcapdir[0] != '\0') {
		// scan directory opt_scanpcapdir (typically /dev/shm/voipmonitor
		char filename[1024];
		char filter_exp[2048] = "";		// The filter expression
		struct bpf_program fp;		// The compiled filter 
		pcap_t *scanhandle = NULL;		// pcap handler
		struct inotify_event *event;
		char buff[1024];
		int i=0, fd, wd, len=0;
		fd = inotify_init();
		/*checking for error*/
		if(fd < 0) perror( "inotify_init" );
		wd = inotify_add_watch(fd, opt_scanpcapdir, IN_CLOSE_WRITE);
		while(1 and terminating == 0) {
			i = 0;
			len = read(fd, buff, 1024);
			while(i < len) {
				event = (struct inotify_event *) &buff[i];
				if (event->mask & IN_CLOSE_WRITE) { // this will prevent opening files which is still open for writes
				    snprintf(filename, sizeof(filename), "%s/%s", opt_scanpcapdir, event->name);
				    int close = 1;
				    //printf("File [%s]\n", filename);
				    if(!file_exists(filename)) { 
				        i += sizeof(struct inotify_event) + event->len;
					continue;
				    }
				    // if reading file
				    //printf("Reading file: %s\n", filename);
				    mask = PCAP_NETMASK_UNKNOWN;
				    scanhandle = pcap_open_offline(filename, errbuf);
				    if(!handle) {
					    // keep the first handle as global handle and do not change it because it is not threadsafe to close/open it while the other parts are using it
					    handle = scanhandle;
					    close = 0;
				    } else {
					    close = 1;
				    }
				    if(scanhandle == NULL) {
					    syslog(LOG_ERR, "Couldn't open pcap file '%s': %s\n", filename, errbuf);
					    i += sizeof(struct inotify_event) + event->len;
					    continue;
				    }
				    if(*user_filter != '\0') {
					    snprintf(filter_exp, sizeof(filter_exp), "%s", user_filter);

					    // Compile and apply the filter
					    if (pcap_compile(scanhandle, &fp, filter_exp, 0, mask) == -1) {
						    syslog(LOG_ERR, "Couldn't parse filter %s: %s\n", filter_exp, pcap_geterr(scanhandle));
						    inotify_rm_watch(fd, wd);
						    return(2);
					    }
					    if (pcap_setfilter(scanhandle, &fp) == -1) {
						    syslog(LOG_ERR, "Couldn't install filter %s: %s\n", filter_exp, pcap_geterr(scanhandle));
						    inotify_rm_watch(fd, wd);
						    return(2);
					    }
				    }
				    readdump_libpcap(scanhandle);
				    unlink(filename);
				    if(*user_filter != '\0') {
					    pcap_freecode(&fp);
				    }
				    if(close) {
					    pcap_close(scanhandle);
				    }
				}
				i += sizeof(struct inotify_event) + event->len;
			}
			//readend = 1;
		}
		inotify_rm_watch(fd, wd);
		if(handle) pcap_close(handle);
	} else {
		// start reading packets
		//readdump_libnids(handle);

		if(opt_pcap_threaded) {
			pthread_create(&readdump_libpcap_thread, NULL, readdump_libpcap_thread_fce, handle);
			pthread_join(readdump_libpcap_thread, NULL);
		} else {
			readdump_libpcap(handle);
		}
		unlink(fname);
	}

	readend = 1;

	//wait for manager to properly terminate 
	if(opt_manager_port && manager_thread > 0) {
		int res;
		res = shutdown(manager_socket_server, SHUT_RDWR);	// break accept syscall in manager thread
		if(res == -1) {
			// if shutdown failed it can happen when reding very short pcap file and the bind socket was not created in manager
			usleep(10000); 
			res = shutdown(manager_socket_server, SHUT_RDWR);	// break accept syscall in manager thread
		}
		struct timespec ts;
		ts.tv_sec = 1;
		ts.tv_nsec = 0;
		// wait for thread max 1 sec
		pthread_timedjoin_np(manager_thread, NULL, &ts);
	}

#ifdef QUEUE_NONBLOCK2
	if(opt_pcap_threaded) {
		pthread_join(pcap_read_thread, NULL);
	}
#endif

	// wait for RTP threads
	if(rtp_threaded) {
		for(int i = 0; i < num_threads; i++) {
			pthread_join((threads[i].thread), NULL);
		}
	}

	// close handler
	if(opt_scanpcapdir[0] == '\0') {
		pcap_close(handle);
	}

	// flush all queues
	Call *call;
	calltable->cleanup(0);
	terminating = 1;
	pthread_join(call_thread, NULL);
	while(calltable->calls_queue.size() != 0) {
			call = calltable->calls_queue.front();
			calltable->calls_queue.pop_front();
			delete call;
			calls--;
	}
	while(calltable->calls_deletequeue.size() != 0) {
			call = calltable->calls_deletequeue.front();
			calltable->calls_deletequeue.pop();
			delete call;
			calls--;
	}

	free(sipportmatrix);
	if(opt_ipaccount) {
		free(ipaccountportmatrix);
	}

	if(opt_cachedir[0] != '\0') {
		terminating2 = 1;
		pthread_join(cachedir_thread, NULL);
	}
	delete calltable;
	
	if(sqlDb) {
		delete sqlDb;
	}
	if(sqlStore) {
		delete sqlStore;
	}

	if(mirrorip) {
		delete mirrorip;
	}

	if (opt_fork){
		unlink(opt_pidfile);
	}
	pthread_mutex_destroy(&mysqlquery_lock);
	clean_tcpstreams();
	ipfrag_prune(0, 1);
	freeMemIpacc();
}

void *readdump_libpcap_thread_fce(void *handle) {
	readdump_libpcap((pcap_t*)handle);
	return(NULL);
}


void test() {
	
	cout << "db major version: " << sqlDb->getDbMajorVersion() << endl
	     << "db minor version: " << sqlDb->getDbMinorVersion() << endl
	     << "db minor version: " << sqlDb->getDbMinorVersion(1) << endl;
	
	/*
	initIpacc();
	extern CustPhoneNumberCache *custPnCache;
	cust_reseller cr;
	cr = custPnCache->getCustomerByPhoneNumber("0352307212");
	cout << cr.cust_id << " - " << cr.reseller_id << endl;
	*/
	
	/*
	extern CustIpCache *custIpCache;
	custIpCache->fetchAllIpQueryFromDb();
	*/
	
	/*
	for(int i = 1; i <= 10; i++) {
	sqlStore->lock(i);
	sqlStore->query("insert into _test set test = 1", i);
	sqlStore->query("insert into _test set test = 2", i);
	sqlStore->query("insert into _test set test = 3", i);
	sqlStore->query("insert into _test set test = 4", i);
	sqlStore->unlock(i);
	}
	terminating = true;
	//sleep(2);
	*/
	
	/*
	octects_live_t a;
	a.setFilter(string("192.168.1.2,192.168.1.1").c_str());
	cout << (a.isIpInFilter(inet_addr("192.168.1.1")) ? "find" : "----") << endl;
	cout << (a.isIpInFilter(inet_addr("192.168.1.3")) ? "find" : "----") << endl;
	cout << (a.isIpInFilter(inet_addr("192.168.1.2")) ? "find" : "----") << endl;
	cout << (a.isIpInFilter(inet_addr("192.168.1.3")) ? "find" : "----") << endl;
	*/
	
	/*
	extern void ipacc_add_octets(time_t timestamp, unsigned int saddr, unsigned int daddr, int port, int proto, int packetlen, int voippacket);
	extern void ipacc_save(unsigned int interval_time_limit = 0);

	//for(int i = 0; i < 100000; i++) {
	//	ipacc_add_octets(1, rand()%5000, rand()%5000, rand()%4, rand()%3, rand(), rand()%100);
	//}
	
	ipacc_add_octets(1, 1, 2, 3, 4, 5, 6);
	ipacc_add_octets(1, 1, 2, 3, 4, 5, 6);
	
	ipacc_save();
	
	freeMemIpacc();
	*/
	
	/*
	CustIpCache *custIpCache = new CustIpCache;
	custIpCache->setConnectParams(
		get_customer_by_ip_sql_driver, 
		get_customer_by_ip_odbc_dsn, 
		get_customer_by_ip_odbc_user, 
		get_customer_by_ip_odbc_password, 
		get_customer_by_ip_odbc_driver);
	custIpCache->setQueryes(
		get_customer_by_ip_query, 
		get_customers_ip_query);
	
	unsigned int cust_id = custIpCache->getCustByIp(inet_addr("192.168.1.241"));
	cout << cust_id << endl;
	
	return;
	
	cout << endl << endl;
	for(int i = 0; i < 20; i++) {
		cout << "iter:" << (i+1) << endl;
		unsigned int cust_id = custIpCache->getCustByIp(inet_addr("1.2.3.4"));
		cout << cust_id << endl;
		cust_id = custIpCache->getCustByIp(inet_addr("2.3.4.5"));
		cout << cust_id << endl;
		sleep(1);
	}
	
	return;
	*/
	
	/*
	ipfilter = new IPfilter;
	ipfilter->load();
	ipfilter->dump();

	telnumfilter = new TELNUMfilter;
	telnumfilter->load();
	telnumfilter->dump();
	*/
	
	/*
	sqlDb->query("select _LC_[UNIX_TIMESTAMP('1970-01-01') = 0] as eee;");
	SqlDb_row row = sqlDb->fetchRow();
	cout << row["eee"] << endl;
	*/
	
	/*
	// výmaz - příprava
	sqlDb->query("delete from cdr_sip_response where id > 0");
	cout << sqlDb->getLastErrorString() << endl;
	
	// čtení
	SqlDb_row row1;
	sqlDb->query("select * from cdr order by ID DESC");
	while((row1 = sqlDb->fetchRow())) {
		cout << row1["ID"] << " : " << row1["calldate"] << endl;
	}
	cout << sqlDb->getLastErrorString() << endl;
	
	// zápis
	SqlDb_row row2;
	row2.add("122 wrrrrrrrr", "lastSIPresponse");
	cout << sqlDb->insert("cdr_sip_response", row2) << endl;

	// unique zápis
	SqlDb_row row3;
	row3.add("123 wrrrrrrrr", "lastSIPresponse");
	cout << sqlDb->getIdOrInsert("cdr_sip_response", "id", "lastSIPresponse", row3) << endl;
	
	cout << sqlDb->getLastErrorString() << endl;
	cout << endl << "--------------" << endl;
	*/
	
	//exit(0);
}
