/* Martin Vit support@voipmonitor.org
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2.
*/

#ifndef VOIPMONITOR_H

#define RTPSENSOR_VERSION "4.19"
#define NAT

#define FORMAT_WAV	1
#define FORMAT_OGG	2

/* choose what method wil be used to synchronize threads. NONBLOCK is the fastest. Do not enable both at once */
#define QUEUE_NONBLOCK 
//#define QUEUE_MUTEX 

/* if you want to see all new calls in syslog enable DEBUG_INVITE */
//#define DEBUG_INVITE

void reload_config();

#define VOIPMONITOR_H
#endif

