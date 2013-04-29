/* Martin Vit support@voipmonitor.org
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2.
*/

/*
This unit reads and parse packets from network interface or file 
and insert them into Call class. 

*/

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
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <syslog.h>
#include <semaphore.h>
#include <algorithm>

#include "ipaccount.h"
#include "flags.h"
#include "codecs.h"
#include "calltable.h"
#include "sniff.h"
#include "voipmonitor.h"
#include "filter_mysql.h"
#include "hash.h"
#include "rtp.h"
#include "rtcp.h"
#include "md5.h"
#include "tools.h"
#include "mirrorip.h"
#include "sql_db.h"


using namespace std;

extern int verbosity;

extern char *ipaccountportmatrix;

extern int opt_ipacc_interval;
extern bool opt_ipacc_sniffer_agregate;
extern bool opt_ipacc_agregate_only_customers_on_main_side;
extern bool opt_ipacc_agregate_only_customers_on_any_side;
extern bool opt_ipacc_multithread_save;
extern char get_customer_by_ip_sql_driver[256];
extern char get_customer_by_ip_odbc_dsn[256];
extern char get_customer_by_ip_odbc_user[256];
extern char get_customer_by_ip_odbc_password[256];
extern char get_customer_by_ip_odbc_driver[256];
extern char get_customer_by_ip_query[1024];
extern char get_customers_ip_query[1024];
extern char get_customers_radius_name_query[1024];
extern char get_customer_by_pn_sql_driver[256];
extern char get_customer_by_pn_odbc_dsn[256];
extern char get_customer_by_pn_odbc_user[256];
extern char get_customer_by_pn_odbc_password[256];
extern char get_customer_by_pn_odbc_driver[256];
extern char get_customers_pn_query[1024];
extern char get_radius_ip_driver[256];
extern char get_radius_ip_host[256];
extern char get_radius_ip_db[256];
extern char get_radius_ip_user[256];
extern char get_radius_ip_password[256];
extern char get_radius_ip_query[1024];
extern char get_radius_ip_query_where[1024];
extern int get_customer_by_ip_flush_period;
extern vector<string> opt_national_prefix;

extern char mysql_host[256];
extern char mysql_database[256];
extern char mysql_user[256];
extern char mysql_password[256];

extern SqlDb *sqlDb;
extern MySqlStore *sqlStore;

extern queue<string> mysqlquery;
extern pthread_mutex_t mysqlquery_lock;

typedef map<unsigned int, octects_live_t*> t_ipacc_live;
t_ipacc_live ipacc_live;

typedef map<string, octects_t*> t_ipacc_buffer; 
static t_ipacc_buffer ipacc_buffer[2];
static volatile int sync_save_ipacc_buffer[2];

static unsigned int last_flush_interval_time = 0;
CustIpCache *custIpCache = NULL;
static NextIpCache *nextIpCache = NULL;
CustPhoneNumberCache *custPnCache = NULL;


void ipacc_save(int indexIpaccBuffer, unsigned int interval_time_limit = 0) {
	if(custIpCache) {
		custIpCache->flush();
	}
	if(nextIpCache) {
		nextIpCache->flush();
	}
	if(custIpCache) {
		custIpCache->flush();
	}
	
	octects_t *ipacc_data;
	char keycb[64], 
	     *keyc, *tmp;
	unsigned int saddr,
		     src_id_customer,
		     daddr,
		     dst_id_customer,
		     port,
		     proto;
	bool src_ip_next,
	     dst_ip_next;
	map<unsigned int,IpaccAgreg*> agreg;
	map<unsigned int, IpaccAgreg*>::iterator agregIter;
	char insertQueryBuff[1000];
	if(opt_ipacc_multithread_save) {
		sqlStore->lock(STORE_PROC_ID_IPACC_1);
		sqlStore->lock(STORE_PROC_ID_IPACC_2);
		sqlStore->lock(STORE_PROC_ID_IPACC_3);
	} else {
		pthread_mutex_lock(&mysqlquery_lock);
	}
	int _counter  = 0;
	bool enableClear = true;
	t_ipacc_buffer::iterator iter;
	for (iter = ipacc_buffer[indexIpaccBuffer].begin(); iter != ipacc_buffer[indexIpaccBuffer].end(); iter++) {
			
		ipacc_data = iter->second;
		if(ipacc_data->octects == 0) {
			ipacc_data->erase = true;
		} else if(!interval_time_limit ||  ipacc_data->interval_time <= interval_time_limit) {
			
			strcpy(keycb, iter->first.c_str());
			keyc = keycb;
			
			tmp = strchr(keyc, 'D');
			*tmp = '\0';
			saddr = atol(keyc);
			src_id_customer = custIpCache ? custIpCache->getCustByIp(htonl(saddr)) : 0;
			src_ip_next = nextIpCache ? nextIpCache->isIn(saddr) : false;

			keyc = tmp + 1;
			tmp = strchr(keyc, 'E');
			*tmp = '\0';
			daddr = atol(keyc);
			dst_id_customer = custIpCache ? custIpCache->getCustByIp(htonl(daddr)) : 0;
			dst_ip_next = nextIpCache ? nextIpCache->isIn(daddr) : false;

			keyc = tmp + 1;
			tmp = strchr(keyc, 'P');
			*tmp = '\0';
			port = atoi(keyc);

			keyc = tmp + 1;
			proto = atoi(keyc);

			if(!custIpCache || 
			   !opt_ipacc_agregate_only_customers_on_any_side ||
  			   src_id_customer || dst_id_customer ||
			   src_ip_next || dst_ip_next) {
				if(isTypeDb("mysql")) {
					sprintf(insertQueryBuff,
						"insert into ipacc ("
							"interval_time, saddr, src_id_customer, daddr, dst_id_customer, proto, port, "
							"octects, numpackets, voip, do_agr_trigger"
						") values ("
							"'%s', %u, %u, %u, %u, %u, %u, %u, %u, %u, %u)",
						sqlDateTimeString(ipacc_data->interval_time).c_str(),
						saddr,
						src_id_customer,
						daddr,
						dst_id_customer,
						proto,
						port,
						ipacc_data->octects,
						ipacc_data->numpackets,
						ipacc_data->voippacket,
						opt_ipacc_sniffer_agregate ? 0 : 1);
					if(opt_ipacc_multithread_save) {
						sqlStore->query(insertQueryBuff, STORE_PROC_ID_IPACC_1 + (_counter % 3));
					} else {
						mysqlquery.push(insertQueryBuff);
					}
					//sqlDb->query(insertQueryBuff);////
				} else {
					SqlDb_row row;
					row.add(sqlDateTimeString(ipacc_data->interval_time).c_str(), "interval_time");
					row.add(saddr, "saddr");
					if(src_id_customer) {
						row.add(src_id_customer, "src_id_customer");
					}
					row.add(daddr, "daddr");
					if(dst_id_customer) {
						row.add(dst_id_customer, "dst_id_customer");
					}
					row.add(proto, "proto");
					row.add(port, "port");
					row.add(ipacc_data->octects, "octects");
					row.add(ipacc_data->numpackets, "numpackets");
					row.add(ipacc_data->voippacket, "voip");
					row.add(opt_ipacc_sniffer_agregate ? 0 : 1, "do_agr_trigger");
					sqlDb->insert("ipacc", row);
				}
				++_counter;
				
				if(opt_ipacc_sniffer_agregate) {
					agregIter = agreg.find(ipacc_data->interval_time);
					if(agregIter == agreg.end()) {
						agreg[ipacc_data->interval_time] = new IpaccAgreg;
						agregIter = agreg.find(ipacc_data->interval_time);
					}
					agregIter->second->add(
						saddr, daddr, 
						src_id_customer, dst_id_customer, 
						src_ip_next, dst_ip_next,
						proto, port,
						ipacc_data->octects, ipacc_data->numpackets, ipacc_data->voippacket);
				}
			}
			ipacc_data->erase = true;
		} else {
			enableClear = false;
		}
	}
	for (iter = ipacc_buffer[indexIpaccBuffer].begin(); iter != ipacc_buffer[indexIpaccBuffer].end();) {
		if(iter->second->erase) {
			delete iter->second;
			if(!enableClear) {
				ipacc_buffer[indexIpaccBuffer].erase(iter++);
				continue;
			}
		}
		iter++;
	}
	if(enableClear) {
		ipacc_buffer[indexIpaccBuffer].clear();
	}
	if(opt_ipacc_multithread_save) {
		sqlStore->unlock(STORE_PROC_ID_IPACC_1);
		sqlStore->unlock(STORE_PROC_ID_IPACC_2);
		sqlStore->unlock(STORE_PROC_ID_IPACC_3);
	}
	if(opt_ipacc_sniffer_agregate) {
		for(agregIter = agreg.begin(); agregIter != agreg.end(); ++agregIter) {
			agregIter->second->save(agregIter->first);
			delete agregIter->second;
		}
	}
	if(!opt_ipacc_multithread_save) {
		pthread_mutex_unlock(&mysqlquery_lock);
	}
	
	__sync_sub_and_fetch(&sync_save_ipacc_buffer[indexIpaccBuffer], 1);
	
	//printf("flush\n");
}

void ipacc_add_octets(time_t timestamp, unsigned int saddr, unsigned int daddr, int port, int proto, int packetlen, int voippacket) {
	string key;
	char buf[100];
	octects_t *octects_data;
	unsigned int cur_interval_time = timestamp / opt_ipacc_interval * opt_ipacc_interval;
	int indexIpaccBuffer = (cur_interval_time / opt_ipacc_interval) % 2;
	
	sprintf(buf, "%uD%uE%dP%d", htonl(saddr), htonl(daddr), port, proto);
	key = buf;

	if(last_flush_interval_time != cur_interval_time &&
	   (time(NULL) - cur_interval_time) > opt_ipacc_interval / 5) {
		int saveIndexIpaccBuffer = indexIpaccBuffer == 0 ? 1 : 0;
		if(!__sync_fetch_and_add(&sync_save_ipacc_buffer[saveIndexIpaccBuffer], 1)) {
			last_flush_interval_time = cur_interval_time;
			ipacc_save(saveIndexIpaccBuffer, last_flush_interval_time);
		}
	}
	
	t_ipacc_buffer::iterator iter;
	iter = ipacc_buffer[indexIpaccBuffer].find(key);
	if(iter == ipacc_buffer[indexIpaccBuffer].end()) {
		// not found;
		octects_data = new octects_t;
		octects_data->octects += packetlen;
		octects_data->numpackets++;
		octects_data->interval_time = cur_interval_time;
		octects_data->voippacket = voippacket;
		ipacc_buffer[indexIpaccBuffer][key] = octects_data;
//		printf("key: %s\n", buf);
	} else {
		//found
		octects_t *tmp = iter->second;
		tmp->octects += packetlen;
		tmp->numpackets++;
		tmp->interval_time = cur_interval_time;
		tmp->voippacket = voippacket;
//		printf("key[%s] %u\n", key.c_str(), tmp->octects);
	}

	t_ipacc_live::iterator it;
	octects_live_t *data;
	for(it = ipacc_live.begin(); it != ipacc_live.end();) {
		data = it->second;
		if(!data) {
			it++;
			continue;
		}
		
		if((time(NULL) - data->fetch_timestamp) > 120) {
			if(verbosity > 0) {
				cout << "FORCE STOP LIVE IPACC id: " << it->first << endl; 
			}
			free(it->second);
			ipacc_live.erase(it++);
			continue;
		} else if(data->all) {
			data->all_octects += packetlen;
			data->all_numpackets++;
			if(voippacket) {
				data->voipall_octects += packetlen;
				data->voipall_numpackets++;
			}
		} else if(data->isIpInFilter(saddr)) {
			data->src_octects += packetlen;
			data->src_numpackets++;
			if(voippacket) {
				data->voipsrc_octects += packetlen;
				data->voipsrc_numpackets++;
			}
		} else if(data->isIpInFilter(daddr)) {
			data->dst_octects += packetlen;
			data->dst_numpackets++;
			if(voippacket) {
				data->voipdst_octects += packetlen;
				data->voipdst_numpackets++;
			}
		}
		it++;
		//cout << saddr << "  " << daddr << "  " << port << "  " << proto << "   " << packetlen << endl;
	}
}

void ipaccount(time_t timestamp, struct iphdr *header_ip, int packetlen, int voippacket){
	struct udphdr2 *header_udp;
	struct tcphdr *header_tcp;

	if (header_ip->protocol == IPPROTO_UDP) {
		// prepare packet pointers 
		header_udp = (struct udphdr2 *) ((char *) header_ip + sizeof(*header_ip));

		if(ipaccountportmatrix[htons(header_udp->source)]) {
			ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, htons(header_udp->source), IPPROTO_TCP, packetlen, voippacket);
		} else if (ipaccountportmatrix[htons(header_udp->dest)]) {
			ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, htons(header_udp->dest), IPPROTO_TCP, packetlen, voippacket);
		} else {
			ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, 0, IPPROTO_TCP, packetlen, voippacket);
		}
	} else if (header_ip->protocol == IPPROTO_TCP) {
		header_tcp = (struct tcphdr *) ((char *) header_ip + sizeof(*header_ip));

		if(ipaccountportmatrix[htons(header_tcp->source)]) {
			ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, htons(header_tcp->source), IPPROTO_TCP, packetlen, voippacket);
		} else if (ipaccountportmatrix[htons(header_tcp->dest)]) {
			ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, htons(header_tcp->dest), IPPROTO_TCP, packetlen, voippacket);
		} else {
			ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, 0, IPPROTO_TCP, packetlen, voippacket);
		}
	} else {
		ipacc_add_octets(timestamp, header_ip->saddr, header_ip->daddr, 0, header_ip->protocol, packetlen, voippacket);
	}

}

IpaccAgreg::~IpaccAgreg() {
	map<AgregIP, AgregData*>::iterator iter1;
	for(iter1 = this->map1.begin(); iter1 != this->map1.end(); ++iter1) {
		delete iter1->second;
	}
	map<AgregIP2, AgregData*>::iterator iter2;
	for(iter2 = this->map2.begin(); iter2 != this->map2.end(); ++iter2) {
		delete iter2->second;
	}
}

void IpaccAgreg::add(unsigned int src, unsigned int dst,
		     unsigned int src_id_customer, unsigned int dst_id_customer,
		     bool src_ip_next, bool dst_ip_next,
		     unsigned int proto, unsigned int port,
		     unsigned int traffic, unsigned int packets, bool voip) {
	AgregIP srcA(src, proto, port), dstA(dst, proto, port);
	map<AgregIP, AgregData*>::iterator iter1;
	if(src_id_customer || src_ip_next || !opt_ipacc_agregate_only_customers_on_main_side) {
		iter1 = this->map1.find(srcA);
		if(iter1 == this->map1.end()) {
			AgregData *agregData = new AgregData;
			agregData->id_customer = src_id_customer;
			agregData->addOut(traffic, packets, voip);
			this->map1[srcA] = agregData;
		} else {
			iter1->second->addOut(traffic, packets, voip);
		}
	}
	if(dst_id_customer || dst_ip_next || !opt_ipacc_agregate_only_customers_on_main_side) {
		iter1 = this->map1.find(dstA);
		if(iter1 == this->map1.end()) {
			AgregData *agregData = new AgregData;
			agregData->id_customer = dst_id_customer;
			agregData->addIn(traffic, packets, voip);
			this->map1[dstA] = agregData;
		} else {
			iter1->second->addIn(traffic, packets, voip);
		}
	}
	AgregIP2 srcDstA(src, dst, proto, port), dstSrcA(dst, src, proto, port);
	map<AgregIP2, AgregData*>::iterator iter2;
	if(src_id_customer || src_ip_next || !opt_ipacc_agregate_only_customers_on_main_side) {
		iter2 = this->map2.find(srcDstA);
		if(iter2 == this->map2.end()) {
			AgregData *agregData = new AgregData;
			agregData->id_customer = src_id_customer;
			agregData->id_customer2 = dst_id_customer;
			agregData->addOut(traffic, packets, voip);
			this->map2[srcDstA] = agregData;
		} else {
			iter2->second->addOut(traffic, packets, voip);
		}
	}
	if(dst_id_customer || dst_ip_next || !opt_ipacc_agregate_only_customers_on_main_side) {
		iter2 = this->map2.find(dstSrcA);
		if(iter2 == this->map2.end()) {
			AgregData *agregData = new AgregData;
			agregData->id_customer = dst_id_customer;
			agregData->id_customer2 = src_id_customer;
			agregData->addIn(traffic, packets, voip);
			this->map2[dstSrcA] = agregData;
		} else {
			iter2->second->addIn(traffic, packets, voip);
		}
	}
}

void IpaccAgreg::save(unsigned int time_interval) {
	char insertQueryBuff[10000];
	const char *agreg_table;
	const char *agreg_time_field;
	char agreg_time[100];
	
	map<AgregIP, AgregData*>::iterator iter1;
	for(int i = 0; i < 3; i++) {
		agreg_table = 
			i == 0 ? "ipacc_agr_interval" :
			(i == 1 ? "ipacc_agr_hour" : "ipacc_agr_day");
		agreg_time_field = 
			i == 0 ? "interval_time" :
			(i == 1 ? "time_hour" : "date_day");
		strcpy(agreg_time,
			i == 0 ?
				sqlDateTimeString(time_interval).c_str() :
			(i == 1 ?
				sqlDateTimeString(time_interval / 3600 * 3600).c_str() :
				sqlDateString(time_interval).c_str()));
	if(opt_ipacc_multithread_save) {
		sqlStore->lock(
			i == 0 ? STORE_PROC_ID_IPACC_AGR_INTERVAL :
			(i == 1 ? STORE_PROC_ID_IPACC_AGR_HOUR : STORE_PROC_ID_IPACC_AGR_DAY));
	}
	for(iter1 = this->map1.begin(); iter1 != this->map1.end(); iter1++) {
		sprintf(insertQueryBuff,
			"update %s set "
				"traffic_in = traffic_in + %lu, "
				"traffic_out = traffic_out + %lu, "
				"traffic_sum = traffic_sum + %lu, "
				"packets_in = packets_in + %lu, "
				"packets_out = packets_out + %lu, "
				"packets_sum = packets_sum + %lu, "
				"traffic_voip_in = traffic_voip_in + %lu, "
				"traffic_voip_out = traffic_voip_out + %lu, "
				"traffic_voip_sum = traffic_voip_sum + %lu, "
				"packets_voip_in = packets_voip_in + %lu, "
				"packets_voip_out = packets_voip_out + %lu, "
				"packets_voip_sum = packets_voip_sum + %lu "
			"where %s = '%s' and "
				"addr = %u and customer_id = %u and "
				"proto = %u and port = %u; "
			"if(row_count() <= 0) then "
				"insert into %s ("
						"%s, addr, customer_id, proto, port, "
						"traffic_in, traffic_out, traffic_sum, "
						"packets_in, packets_out, packets_sum, "
						"traffic_voip_in, traffic_voip_out, traffic_voip_sum, "
						"packets_voip_in, packets_voip_out, packets_voip_sum"
					") values ("
						"'%s', %u, %u, %u, %u, "
						"%lu, %lu, %lu, "
						"%lu, %lu, %lu, "
						"%lu, %lu, %lu, "
						"%lu, %lu, %lu);"
			"end if",
			agreg_table,
			iter1->second->traffic_in,
			iter1->second->traffic_out,
			iter1->second->traffic_in + iter1->second->traffic_out,
			iter1->second->packets_in,
			iter1->second->packets_out,
			iter1->second->packets_in + iter1->second->packets_out,
			iter1->second->traffic_voip_in,
			iter1->second->traffic_voip_out,
			iter1->second->traffic_voip_in + iter1->second->traffic_voip_out,
			iter1->second->packets_voip_in,
			iter1->second->packets_voip_out,
			iter1->second->packets_voip_in + iter1->second->packets_voip_out,
			agreg_time_field,
			agreg_time,
			iter1->first.ip,
			iter1->second->id_customer,
			iter1->first.proto,
			iter1->first.port,
			agreg_table,
			agreg_time_field,
			agreg_time,
			iter1->first.ip,
			iter1->second->id_customer,
			iter1->first.proto,
			iter1->first.port,
			iter1->second->traffic_in,
			iter1->second->traffic_out,
			iter1->second->traffic_in + iter1->second->traffic_out,
			iter1->second->packets_in,
			iter1->second->packets_out,
			iter1->second->packets_in + iter1->second->packets_out,
			iter1->second->traffic_voip_in,
			iter1->second->traffic_voip_out,
			iter1->second->traffic_voip_in + iter1->second->traffic_voip_out,
			iter1->second->packets_voip_in,
			iter1->second->packets_voip_out,
			iter1->second->packets_voip_in + iter1->second->packets_voip_out);
		if(opt_ipacc_multithread_save) {
			sqlStore->query(insertQueryBuff,
					i == 0 ? STORE_PROC_ID_IPACC_AGR_INTERVAL :
					(i == 1 ? STORE_PROC_ID_IPACC_AGR_HOUR : STORE_PROC_ID_IPACC_AGR_DAY));
		} else {
			mysqlquery.push(insertQueryBuff);
		}
	}
	if(opt_ipacc_multithread_save) {
		sqlStore->unlock(
			i == 0 ? STORE_PROC_ID_IPACC_AGR_INTERVAL :
			(i == 1 ? STORE_PROC_ID_IPACC_AGR_HOUR : STORE_PROC_ID_IPACC_AGR_DAY));
	}
	}
	
	map<AgregIP2, AgregData*>::iterator iter2;
	for(int i = 0; i < 1; i++) {
		agreg_table = i == 0 ? "ipacc_agr2_hour" : "ipacc_agr2_day";
		agreg_time_field = i == 0 ? "time_hour" : "date_day";
		strcpy(agreg_time,
		       i == 0 ?
				sqlDateTimeString(time_interval / 3600 * 3600).c_str() :
				sqlDateString(time_interval).c_str());
	if(opt_ipacc_multithread_save) {
		if(i == 0) {
			sqlStore->lock(STORE_PROC_ID_IPACC_AGR2_HOUR_1);
			sqlStore->lock(STORE_PROC_ID_IPACC_AGR2_HOUR_2);
			sqlStore->lock(STORE_PROC_ID_IPACC_AGR2_HOUR_3);
		} else {
			sqlStore->lock(STORE_PROC_ID_IPACC_AGR2_DAY_1);
			sqlStore->lock(STORE_PROC_ID_IPACC_AGR2_DAY_2);
			sqlStore->lock(STORE_PROC_ID_IPACC_AGR2_DAY_3);
		}
	}
	int _counter = 0;
	for(iter2 = this->map2.begin(); iter2 != this->map2.end(); iter2++) {
		sprintf(insertQueryBuff,
			"update %s set "
				"traffic_in = traffic_in + %lu, "
				"traffic_out = traffic_out + %lu, "
				"traffic_sum = traffic_sum + %lu, "
				"packets_in = packets_in + %lu, "
				"packets_out = packets_out + %lu, "
				"packets_sum = packets_sum + %lu, "
				"traffic_voip_in = traffic_voip_in + %lu, "
				"traffic_voip_out = traffic_voip_out + %lu, "
				"traffic_voip_sum = traffic_voip_sum + %lu, "
				"packets_voip_in = packets_voip_in + %lu, "
				"packets_voip_out = packets_voip_out + %lu, "
				"packets_voip_sum = packets_voip_sum + %lu "
			"where %s = '%s' and "
				"addr = %u and addr2 = %u  and customer_id = %u and "
				"proto = %u and port = %u; "
			"if(row_count() <= 0) then "
				"insert into %s ("
						"%s, addr, addr2, customer_id, proto, port, "
						"traffic_in, traffic_out, traffic_sum, "
						"packets_in, packets_out, packets_sum, "
						"traffic_voip_in, traffic_voip_out, traffic_voip_sum, "
						"packets_voip_in, packets_voip_out, packets_voip_sum"
					") values ("
						"'%s', %u, %u, %u, %u, %u, "
						"%lu, %lu, %lu, "
						"%lu, %lu, %lu, "
						"%lu, %lu, %lu, "
						"%lu, %lu, %lu);"
			"end if",
			agreg_table,
			iter2->second->traffic_in,
			iter2->second->traffic_out,
			iter2->second->traffic_in + iter2->second->traffic_out,
			iter2->second->packets_in,
			iter2->second->packets_out,
			iter2->second->packets_in + iter2->second->packets_out,
			iter2->second->traffic_voip_in,
			iter2->second->traffic_voip_out,
			iter2->second->traffic_voip_in + iter2->second->traffic_voip_out,
			iter2->second->packets_voip_in,
			iter2->second->packets_voip_out,
			iter2->second->packets_voip_in + iter2->second->packets_voip_out,
			agreg_time_field,
			agreg_time,
			iter2->first.ip1,
			iter2->first.ip2,
			iter2->second->id_customer,
			iter2->first.proto,
			iter2->first.port,
			agreg_table,
			agreg_time_field,
			agreg_time,
			iter2->first.ip1,
			iter2->first.ip2,
			iter2->second->id_customer,
			iter2->first.proto,
			iter2->first.port,
			iter2->second->traffic_in,
			iter2->second->traffic_out,
			iter2->second->traffic_in + iter2->second->traffic_out,
			iter2->second->packets_in,
			iter2->second->packets_out,
			iter2->second->packets_in + iter2->second->packets_out,
			iter2->second->traffic_voip_in,
			iter2->second->traffic_voip_out,
			iter2->second->traffic_voip_in + iter2->second->traffic_voip_out,
			iter2->second->packets_voip_in,
			iter2->second->packets_voip_out,
			iter2->second->packets_voip_in + iter2->second->packets_voip_out);
		if(opt_ipacc_multithread_save) {
			sqlStore->query(insertQueryBuff,
					(i == 0 ? STORE_PROC_ID_IPACC_AGR2_HOUR_1 : STORE_PROC_ID_IPACC_AGR2_DAY_1) +
					(_counter % 3));
		} else {
			mysqlquery.push(insertQueryBuff);
		}
		++_counter;
	}
	if(opt_ipacc_multithread_save) {
		if(i == 0) {
			sqlStore->unlock(STORE_PROC_ID_IPACC_AGR2_HOUR_1);
			sqlStore->unlock(STORE_PROC_ID_IPACC_AGR2_HOUR_2);
			sqlStore->unlock(STORE_PROC_ID_IPACC_AGR2_HOUR_3);
		} else {
			sqlStore->unlock(STORE_PROC_ID_IPACC_AGR2_DAY_1);
			sqlStore->unlock(STORE_PROC_ID_IPACC_AGR2_DAY_2);
			sqlStore->unlock(STORE_PROC_ID_IPACC_AGR2_DAY_3);
		}
	}
	}
}

CustIpCache::CustIpCache() {
	this->sqlDb = NULL;
	this->sqlDbRadius = NULL;
	this->flushCounter = 0;
	this->doFlushVect = false;
}

CustIpCache::~CustIpCache() {
	if(this->sqlDb) {
		delete this->sqlDb;
	}
	if(this->sqlDbRadius) {
		delete this->sqlDbRadius;
	}
}

void CustIpCache::setConnectParams(const char *sqlDriver, const char *odbcDsn, const char *odbcUser, const char *odbcPassword, const char *odbcDriver) {
	if(sqlDriver) 		this->sqlDriver = sqlDriver;
	if(odbcDsn) 		this->odbcDsn = odbcDsn;
	if(odbcUser) 		this->odbcUser = odbcUser;
	if(odbcPassword)	this->odbcPassword = odbcPassword;
	if(odbcDriver) 		this->odbcDriver = odbcDriver;
}

void CustIpCache::setConnectParamsRadius(const char *radiusSqlDriver, const char *radiusHost, const char *radiusDb,const char *radiusUser, const char *radiusPassword) {
	if(radiusSqlDriver)	this->radiusSqlDriver = radiusSqlDriver;
	if(radiusHost)		this->radiusHost = radiusHost;
	if(radiusDb)		this->radiusDb = radiusDb;
	if(radiusUser)		this->radiusUser = radiusUser;
	if(radiusPassword)	this->radiusPassword = radiusPassword;
}

void CustIpCache::setQueryes(const char *getIp, const char *fetchAllIp) {
	if(getIp)		this->query_getIp = getIp;
	if(fetchAllIp)		this->query_fetchAllIp = fetchAllIp;
}

void CustIpCache::setQueryesRadius(const char *fetchAllRadiusNames, const char *fetchAllRadiusIp, const char *fetchAllRadiusIpWhere) {
	if(fetchAllRadiusNames)		this->query_fetchAllRadiusNames = fetchAllRadiusNames;
	if(fetchAllRadiusIp)		this->query_fetchAllRadiusIp = fetchAllRadiusIp;
	if(fetchAllRadiusIpWhere)	this->query_fetchAllRadiusIpWhere = fetchAllRadiusIpWhere;
}

int CustIpCache::connect() {
	if(!this->okParams()) {
		return(0);
	}
	if(!this->sqlDb) {
		SqlDb_odbc *sqlDb_odbc = new SqlDb_odbc();
		sqlDb_odbc->setOdbcVersion(SQL_OV_ODBC3);
		sqlDb_odbc->setSubtypeDb(this->odbcDriver);
		this->sqlDb = sqlDb_odbc;
		this->sqlDb->setConnectParameters(this->odbcDsn, this->odbcUser, this->odbcPassword);
		this->sqlDb->enableSysLog();
	}
	if(!this->sqlDbRadius && this->radiusHost.length()) {
		SqlDb_mysql *sqlDb_mysql = new SqlDb_mysql();
		this->sqlDbRadius = sqlDb_mysql;
		this->sqlDbRadius->setConnectParameters(this->radiusHost, this->radiusUser, this->radiusPassword, this->radiusDb);
		this->sqlDbRadius->enableSysLog();
	}
	return(this->sqlDb->connect() && 
	       (this->sqlDbRadius ? this->sqlDbRadius->connect() : true));
}

bool CustIpCache::okParams() {
	return(this->sqlDriver.length() &&
	       this->odbcDsn.length() &&
	       this->odbcUser.length() &&
	       this->odbcDriver.length() &&
	       (this->query_getIp.length() ||
	        this->query_fetchAllIp.length()));
}

int CustIpCache::getCustByIp(unsigned int ip) {
	if(!this->okParams()) {
		return(0);
	}
	if(!this->sqlDb) {
		this->connect();
	}
	if(this->query_fetchAllIp.length()) {
		if(this->doFlushVect) {
			this->fetchAllIpQueryFromDb();
			this->doFlushVect = false;
		}
		if(!this->custCacheVect.size()) {
			return(0);
		}
		return(this->getCustByIpFromCacheVect(ip));
	} else if(this->query_getIp.length()) {
		int cust_id = 0;
		cust_id = this->getCustByIpFromCacheMap(ip);
		if(cust_id < 0) {
			cust_id = this->getCustByIpFromDb(ip, true);
		}
		return(cust_id);
	}
	return(0);
}

int CustIpCache::getCustByIpFromDb(unsigned int ip, bool saveToCache) {
	if(!this->query_getIp.length()) {
		return(-1);
	}
	string query_str = this->query_getIp;
	size_t query_pos_ip = query_str.find("_IP_");
	if(query_pos_ip != std::string::npos) {
		int cust_id = 0;
		char ip_str[18];
		in_addr ips;
		ips.s_addr = ip;
		strcpy(ip_str, inet_ntoa(ips));
		query_str.replace(query_pos_ip, 4, ip_str);
		this->sqlDb->query(query_str);
		SqlDb_row row = sqlDb->fetchRow();
		if(row) {
			const char *cust_id_str = row["ID"].c_str();
			if(cust_id_str && cust_id_str[0]) {
				cust_id = atol(cust_id_str);
			}
		}
		if(saveToCache) {
			cust_cache_item cache_rec;
			cache_rec.cust_id = cust_id;
			cache_rec.add_timestamp = time(NULL);
			this->custCacheMap[ip] = cache_rec;
		}
		return(cust_id);
	} else {
		return(-1);
	}
}

int CustIpCache::fetchAllIpQueryFromDb() {
	if(!this->query_fetchAllIp.length()) {
		return(-1);
	}
	int _start_time = time(NULL);
	if(this->sqlDb->query(this->query_fetchAllIp)) {
		this->custCacheVect.clear();
		SqlDb_row row;
		while(row = this->sqlDb->fetchRow()) {
			cust_cache_rec rec;
			in_addr ips;
			inet_aton(row["IP"].c_str(), &ips);
			rec.ip = ips.s_addr;
			rec.cust_id = atol(row["ID"].c_str());
			this->custCacheVect.push_back(rec);
		}
		if(this->sqlDbRadius && this->sqlDb->query(this->query_fetchAllRadiusNames)) {
			map<string, unsigned int> radiusUsers;
			string condRadiusUsers;
			SqlDb_row row;
			while(row = this->sqlDb->fetchRow()) {
				radiusUsers[row["radius_username"]] = atol(row["ID"].c_str());
				if(condRadiusUsers.length()) {
					condRadiusUsers += ",";
				}
				condRadiusUsers += string("'") + row["radius_username"] + "'";
			}
			if(radiusUsers.size() &&
			   this->sqlDbRadius->query(
					this->query_fetchAllRadiusIp + " " +
					this->query_fetchAllRadiusIpWhere + "(" + condRadiusUsers + ")")) {
				SqlDb_row row;
				while(row = this->sqlDbRadius->fetchRow()) {
					cust_cache_rec rec;
					in_addr ips;
					inet_aton(row["IP"].c_str(), &ips);
					rec.ip = ips.s_addr;
					rec.cust_id = radiusUsers[row["radius_username"]];
					this->custCacheVect.push_back(rec);
				}
			}
		}
		if(this->custCacheVect.size()) {
			std::sort(this->custCacheVect.begin(), this->custCacheVect.end());
		}
		if(verbosity > 0) {
			int _diff_time = time(NULL) - _start_time;
			cout << "IPACC load customers " << _diff_time << " s" << endl;
		}
	}
	return(this->custCacheVect.size());
}

int CustIpCache::getCustByIpFromCacheMap(unsigned int ip) {
	cust_cache_item cache_rec = this->custCacheMap[ip];
	if((cache_rec.cust_id || cache_rec.add_timestamp) &&
	   (time(NULL) - cache_rec.add_timestamp) < 3600) {
		return(cache_rec.cust_id);
	}
	return(-1);
}

int CustIpCache::getCustByIpFromCacheVect(unsigned int ip) {
  	vector<cust_cache_rec>::iterator findRecIt;
  	findRecIt = std::lower_bound(this->custCacheVect.begin(), this->custCacheVect.end(), ip);
  	if(findRecIt != this->custCacheVect.end() && (*findRecIt).ip == ip) {
  		return((*findRecIt).cust_id);
  	}
	return(0);
}

void CustIpCache::flush() {
	if(get_customer_by_ip_flush_period > 0 && this->flushCounter > 0 &&
	   !(this->flushCounter % get_customer_by_ip_flush_period)) {
		this->custCacheMap.clear();
		this->doFlushVect = true;
	}
	++this->flushCounter;
}

NextIpCache::NextIpCache() {
	this->flushCounter = 0;
	this->doFlush = false;
}

NextIpCache::~NextIpCache() {
	if(this->sqlDb) {
		delete this->sqlDb;
	}
}

int NextIpCache::connect() {
	if(isSqlDriver("mysql")) {
		this->sqlDb = new SqlDb_mysql();
		sqlDb->setConnectParameters(mysql_host, mysql_user, mysql_password, mysql_database);
		this->sqlDb->enableSysLog();
		return(this->sqlDb->connect());
	}
	return(0);
}

bool NextIpCache::isIn(unsigned int ip) {
	if(this->doFlush) {
		this->fetch();
		this->doFlush = false;
	}
	if(!this->nextCache.size()) {
		return(false);
	}
	next_cache_rec rec;
	vector<next_cache_rec>::iterator findRecIt;
	for(unsigned int mask = 32; mask >= 16; --mask) {
		rec.ip = ip;
		rec.mask = mask;
		if(!rec.mask) {
			rec.mask = 32;
		}
		if(rec.mask < 32) {
			rec.ip = rec.ip >> (32 - rec.mask) << (32 - rec.mask);
		}
		findRecIt = std::lower_bound(this->nextCache.begin(), this->nextCache.end(), rec);
		if(findRecIt != this->nextCache.end() && (*findRecIt).ip == rec.ip) {
			//cout << endl << (*findRecIt).ip << "/" << mask << endl;
			return(true);
		}
	}
	return(false);
}

void NextIpCache::fetch() {
	if(this->sqlDb->query("select ip, mask from ipacc_capt_ip where enable")) {
		this->nextCache.clear();
		SqlDb_row row;
		while(row = this->sqlDb->fetchRow()) {
			next_cache_rec rec;
			rec.ip = atol(row["ip"].c_str());
			rec.mask = atol(row["mask"].c_str());
			if(!rec.mask) {
				rec.mask = 32;
			}
			if(rec.mask < 32) {
				rec.ip = rec.ip >> (32 - rec.mask) << (32 - rec.mask);
			}
			this->nextCache.push_back(rec);
		}
		if(this->nextCache.size()) {
			std::sort(this->nextCache.begin(), this->nextCache.end());
		}
		if(verbosity > 0) {
			cout << "IPACC load next IP" << endl;
		}
	}
}

void NextIpCache::flush() {
	if(get_customer_by_ip_flush_period > 0 && this->flushCounter > 0 &&
	   !(this->flushCounter % get_customer_by_ip_flush_period)) {
		this->doFlush = true;
	}
	++this->flushCounter;
}

CustPhoneNumberCache::CustPhoneNumberCache() {
	this->sqlDb = NULL;
	this->flushCounter = 0;
	this->doFlush = false;
}

CustPhoneNumberCache::~CustPhoneNumberCache() {
	if(this->sqlDb) {
		delete this->sqlDb;
	}
}

void CustPhoneNumberCache::setConnectParams(const char* sqlDriver, const char* odbcDsn, const char* odbcUser, const char* odbcPassword, const char* odbcDriver) {
	if(sqlDriver) 		this->sqlDriver = sqlDriver;
	if(odbcDsn) 		this->odbcDsn = odbcDsn;
	if(odbcUser) 		this->odbcUser = odbcUser;
	if(odbcPassword)	this->odbcPassword = odbcPassword;
	if(odbcDriver) 		this->odbcDriver = odbcDriver;
}

void CustPhoneNumberCache::setQueryes(const char* fetchPhoneNumbers) {
	this->query_fetchPhoneNumbers = fetchPhoneNumbers;
}

int CustPhoneNumberCache::connect() {
	if(!this->okParams()) {
		return(0);
	}
	if(!this->sqlDb) {
		SqlDb_odbc *sqlDb_odbc = new SqlDb_odbc();
		sqlDb_odbc->setOdbcVersion(SQL_OV_ODBC3);
		sqlDb_odbc->setSubtypeDb(this->odbcDriver);
		this->sqlDb = sqlDb_odbc;
		this->sqlDb->setConnectParameters(this->odbcDsn, this->odbcUser, this->odbcPassword);
		this->sqlDb->enableSysLog();
	}
	return(this->sqlDb->connect());
}

bool CustPhoneNumberCache::okParams() {
	return(this->sqlDriver.length() &&
	       this->odbcDsn.length() &&
	       this->odbcUser.length() &&
	       this->odbcDriver.length() &&
	       this->query_fetchPhoneNumbers.length());
}

cust_reseller CustPhoneNumberCache::getCustomerByPhoneNumber(char* number) {
	cust_reseller rslt;
	if(!this->okParams()) {
		return(rslt);
	}
	if(!this->sqlDb) {
		this->connect();
	}
	if(this->query_fetchPhoneNumbers.length()) {
		if(this->doFlush) {
			this->fetchPhoneNumbersFromDb();
			this->doFlush = false;
		}
		if(!this->custCache.size()) {
			return(rslt);
		}
		for(uint i = 0; i < opt_national_prefix.size() + 1; i++) {
			string findNumber = number;
			if(i > 0 && opt_national_prefix[i - 1] == findNumber.substr(0, opt_national_prefix[i - 1].length())) {
				findNumber = findNumber.substr(opt_national_prefix[i - 1].length());
			}
			vector<cust_pn_cache_rec>::iterator findRecIt;
			findRecIt = std::lower_bound(this->custCache.begin(), this->custCache.end(), findNumber);
			if(findRecIt != this->custCache.end() && (*findRecIt).cust_id) {
				if(findRecIt != this->custCache.begin() && (*findRecIt).numberFrom > findNumber) {
					--findRecIt;
				}
				if((*findRecIt).cust_id && (*findRecIt).numberFrom <= findNumber && 
				   (*findRecIt).numberTo >= findNumber.substr(0, (*findRecIt).numberTo.length())) {
					rslt.cust_id = (*findRecIt).cust_id;
					rslt.reseller_id = (*findRecIt).reseller_id;
					break;
				}
			}
		}
	}
	return(rslt);
}

int CustPhoneNumberCache::fetchPhoneNumbersFromDb() {
	if(!this->query_fetchPhoneNumbers.length()) {
		return(-1);
	}
	int _start_time = time(NULL);
	if(this->sqlDb->query(this->query_fetchPhoneNumbers)) {
		this->custCache.clear();
		SqlDb_row row;
		while(row = this->sqlDb->fetchRow()) {
			cust_pn_cache_rec rec;
			rec.numberFrom = row["numberFrom"];
			rec.numberTo = row["numberTo"];
			rec.cust_id = atol(row["cust_id"].c_str());
			rec.reseller_id = row["reseller_id"];
			this->custCache.push_back(rec);
		}
		if(this->custCache.size()) {
			std::sort(this->custCache.begin(), this->custCache.end());
		}
		if(verbosity > 0) {
			int _diff_time = time(NULL) - _start_time;
			cout << "CDR load customers phone numbers " << _diff_time << " s" << endl;
		}
	}
	return(this->custCache.size());
}

void CustPhoneNumberCache::flush() {
	if(get_customer_by_ip_flush_period > 0 && this->flushCounter > 0 &&
	   !(this->flushCounter % get_customer_by_ip_flush_period)) {
		this->doFlush = true;
	}
	++this->flushCounter;
}

void octects_live_t::setFilter(const char *ipfilter) {
	string temp_ipfilter = ipfilter;
	char *ip = (char*)temp_ipfilter.c_str();
	while(ip && *ip) {
		char *separator = strchr(ip, ',');
		if(separator) {
			*separator = '\0';
		}
		this->ipfilter.push_back(inet_addr(ip));
		if(separator) {
			ip = separator + 1;
		} else {
			ip = NULL;
		}
	}
	std::sort(this->ipfilter.begin(), this->ipfilter.end());
}

unsigned int lengthIpaccBuffer() {
	return(ipacc_buffer[0].size() + ipacc_buffer[1].size());
}

void initIpacc() {
	if(get_customer_by_ip_sql_driver[0]) {
		custIpCache = new CustIpCache();
		custIpCache->setConnectParams(
			get_customer_by_ip_sql_driver, 
			get_customer_by_ip_odbc_dsn, 
			get_customer_by_ip_odbc_user, 
			get_customer_by_ip_odbc_password, 
			get_customer_by_ip_odbc_driver);
		custIpCache->setConnectParamsRadius(
			get_radius_ip_driver,
			get_radius_ip_host,
			get_radius_ip_db,
			get_radius_ip_user,
			get_radius_ip_password);
		custIpCache->setQueryes(
			get_customer_by_ip_query, 
			get_customers_ip_query);
		custIpCache->setQueryesRadius(
			get_customers_radius_name_query, 
			get_radius_ip_query,
			get_radius_ip_query_where);
		custIpCache->connect();
		if(get_customers_ip_query[0]) {
			custIpCache->fetchAllIpQueryFromDb();
			custIpCache->setMaxQueryPass(2);
		}
	}
	if(isSqlDriver("mysql")) {
		nextIpCache = new NextIpCache();
		nextIpCache->connect();
		nextIpCache->fetch();
	}
	if(get_customer_by_pn_sql_driver[0]) {
		custPnCache = new CustPhoneNumberCache();
		custPnCache->setConnectParams(
			get_customer_by_pn_sql_driver, 
			get_customer_by_pn_odbc_dsn, 
			get_customer_by_pn_odbc_user, 
			get_customer_by_pn_odbc_password, 
			get_customer_by_pn_odbc_driver);
		custPnCache->setQueryes(get_customers_pn_query);
		custPnCache->connect();
		if(get_customers_pn_query[0]) {
			custPnCache->fetchPhoneNumbersFromDb();
			custPnCache->setMaxQueryPass(2);
		}
	}
}

void freeMemIpacc() {
	if(custIpCache) {
		delete custIpCache;
	}
	if(nextIpCache) {
		delete nextIpCache;
	}
	if(custPnCache) {
		delete custPnCache;
	}
	t_ipacc_buffer::iterator iter;
	for(int i = 0; i < 2; i++) {
		for(iter = ipacc_buffer[i].begin(); iter != ipacc_buffer[i].end(); ++iter) {
			delete iter->second;
		}
	}
}
