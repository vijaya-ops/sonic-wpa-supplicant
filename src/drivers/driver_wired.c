/*
 * Wired Ethernet driver interface
 * Copyright (c) 2005-2009, Jouni Malinen <j@w1.fi>
 * Copyright (c) 2004, Gunter Burchardt <tira@isx.de>
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "includes.h"

#include "common.h"
#include "eloop.h"
#include "driver.h"
#include "driver_wired_common.h"
#include "common/eapol_common.h"
#ifdef CONFIG_SONIC_HOSTAPD
#ifdef HOSTAPD
#include "ap/hostapd.h"
#include "ap/sta_info.h"
#endif
#endif

#include <sys/ioctl.h>
#undef IFNAMSIZ
#include <net/if.h>
#ifdef __linux__
#include <netpacket/packet.h>
#include <net/if_arp.h>
#endif /* __linux__ */
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__FreeBSD_kernel__)
#include <net/if_dl.h>
#include <net/if_media.h>
#endif /* defined(__FreeBSD__) || defined(__DragonFly__) || defined(__FreeBSD_kernel__) */
#ifdef __sun__
#include <sys/sockio.h>
#endif /* __sun__ */

#ifdef _MSC_VER
#pragma pack(push, 1)
#endif /* _MSC_VER */

#ifdef _MSC_VER
#pragma pack(pop)
#endif /* _MSC_VER */

#ifdef HOSTAPD
extern int wired_driver_auth_resp_send(char *intf, u8 *addr, char *status, void *param);
#endif

struct wpa_driver_wired_data {
	struct driver_wired_common_data common;

#ifndef CONFIG_SONIC_HOSTAPD
	int dhcp_sock; /* socket for dhcp packets */
#endif
	int use_pae_group_addr;
};


/* TODO: detecting new devices should eventually be changed from using DHCP
 * snooping to trigger on any packet from a new layer 2 MAC address, e.g.,
 * based on ebtables, etc. */

struct dhcp_message {
	u_int8_t op;
	u_int8_t htype;
	u_int8_t hlen;
	u_int8_t hops;
	u_int32_t xid;
	u_int16_t secs;
	u_int16_t flags;
	u_int32_t ciaddr;
	u_int32_t yiaddr;
	u_int32_t siaddr;
	u_int32_t giaddr;
	u_int8_t chaddr[16];
	u_int8_t sname[64];
	u_int8_t file[128];
	u_int32_t cookie;
	u_int8_t options[308]; /* 312 - cookie */
};


#ifdef __linux__
static void handle_data(void *ctx, unsigned char *buf, size_t len)
{
#ifdef HOSTAPD
	struct ieee8023_hdr *hdr;
	u8 *pos, *sa;
	size_t left;
	union wpa_event_data event;
	struct sta_info *sta = NULL;
	struct hostapd_data *hapd = ctx;
	struct ieee802_1x_hdr *hdr_802_1x = NULL;

	/* must contain at least ieee8023_hdr 6 byte source, 6 byte dest,
	 * 2 byte ethertype */
	if (len < 14) {
		wpa_printf(MSG_MSGDUMP, "handle_data: too short (%lu)",
			   (unsigned long) len);
		return;
	}

	hdr = (struct ieee8023_hdr *) buf;

	switch (ntohs(hdr->ethertype)) {
	case ETH_P_PAE:
		wpa_printf(MSG_MSGDUMP, "Received EAPOL packet");

    if (hapd->driver->auth_resp_send)
    {
               hostapd_logger(hapd, hdr->src, HOSTAPD_MODULE_IEEE8021X,
                   HOSTAPD_LEVEL_DEBUG,
                   "Received EAPOL packet. Get the associated station");

      sta = ap_get_sta(hapd, hdr->src);

      pos = (u8 *) (hdr + 1);
      hdr_802_1x = (struct ieee802_1x_hdr *)pos;

      if (!sta){
        if (IEEE802_1X_TYPE_EAPOL_START == hdr_802_1x->type) {

          /* Inform PAC */
               hostapd_logger(hapd, hdr->src, HOSTAPD_MODULE_IEEE8021X,
                   HOSTAPD_LEVEL_DEBUG,
                   "Received EAPOL START packet. Informing PAC");
          hapd->driver->auth_resp_send(hapd->conf->iface, hdr->src, "new_client", NULL);
        }
        else if (IEEE802_1X_TYPE_EAPOL_LOGOFF == hdr_802_1x->type)
        {
          /* Inform PAC */
               hostapd_logger(hapd, hdr->src, HOSTAPD_MODULE_IEEE8021X,
                   HOSTAPD_LEVEL_DEBUG,
                   "Received EAPOL LOGOFF packet. Informing PAC");

          hapd->driver->auth_resp_send(hapd->conf->iface, hdr->src, "client_disconnected", NULL);
        }
        return;
      }

      /* check if the client is getting de-authenticated.
         if yes, then ignore any triggers during this state */

	  if (sta->flags & WLAN_STA_PENDING_DEAUTH_CB)
      {
        hostapd_logger(hapd, hdr->src, HOSTAPD_MODULE_IEEE8021X,
            HOSTAPD_LEVEL_DEBUG,
            "Received EAPOL packet of type %d while client is getting de-authenticated. Ignoring EAPOL packet", hdr_802_1x->type);
        return;
      }
    }

		sa = hdr->src;
		os_memset(&event, 0, sizeof(event));
		event.new_sta.addr = sa;
		wpa_supplicant_event(ctx, EVENT_NEW_STA, &event);

		pos = (u8 *) (hdr + 1);
		left = len - sizeof(*hdr);
		drv_event_eapol_rx(ctx, sa, pos, left);
		break;

	default:
		wpa_printf(MSG_DEBUG, "Unknown ethertype 0x%04x in data frame",
			   ntohs(hdr->ethertype));
		break;
	}
#endif /* HOSTAPD */
}


static void handle_read(int sock, void *eloop_ctx, void *sock_ctx)
{
	int len;
	unsigned char buf[3000];

	len = recv(sock, buf, sizeof(buf), 0);
	if (len < 0) {
		wpa_printf(MSG_ERROR, "recv: %s", strerror(errno));
		return;
	}

	handle_data(eloop_ctx, buf, len);
}

#ifndef CONFIG_SONIC_HOSTAPD
static void handle_dhcp(int sock, void *eloop_ctx, void *sock_ctx)
{
	int len;
	unsigned char buf[3000];
	struct dhcp_message *msg;
	u8 *mac_address;
	union wpa_event_data event;

	len = recv(sock, buf, sizeof(buf), 0);
	if (len < 0) {
		wpa_printf(MSG_ERROR, "recv: %s", strerror(errno));
		return;
	}

	/* must contain at least dhcp_message->chaddr */
	if (len < 44) {
		wpa_printf(MSG_MSGDUMP, "handle_dhcp: too short (%d)", len);
		return;
	}

	msg = (struct dhcp_message *) buf;
	mac_address = (u8 *) &(msg->chaddr);

	wpa_printf(MSG_MSGDUMP, "Got DHCP broadcast packet from " MACSTR,
		   MAC2STR(mac_address));

	os_memset(&event, 0, sizeof(event));
	event.new_sta.addr = mac_address;
	wpa_supplicant_event(eloop_ctx, EVENT_NEW_STA, &event);
}
#endif
#endif /* __linux__ */


static int wired_init_sockets(struct wpa_driver_wired_data *drv, u8 *own_addr)
{
#ifdef __linux__
	struct ifreq ifr;
	struct sockaddr_ll addr;

#ifndef CONFIG_SONIC_HOSTAPD
	struct sockaddr_in addr2;
	int n = 1;
#endif

#define SOCK_RCV_BUF_LEN  (1024 * 1024)

    int recv_buf_len = SOCK_RCV_BUF_LEN;

	drv->common.sock = socket(PF_PACKET, SOCK_RAW, htons(ETH_P_PAE));
	if (drv->common.sock < 0) {
		wpa_printf(MSG_ERROR, "socket[PF_PACKET,SOCK_RAW]: %s",
			   strerror(errno));
		return -1;
	}

    if (setsockopt(drv->common.sock, SOL_SOCKET, SO_RCVBUF, &recv_buf_len, sizeof(recv_buf_len)) == -1)
    {
		wpa_printf(MSG_INFO, "Could not set the read socket buffer size");
        close(drv->common.sock);
        return -1;
    }

	if (eloop_register_read_sock(drv->common.sock, handle_read,
				     drv->common.ctx, NULL)) {
		wpa_printf(MSG_INFO, "Could not register read socket");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "Getting the ifindex for interface  %s",
		   drv->common.ifname);
	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, drv->common.ifname, sizeof(ifr.ifr_name));
	if (ioctl(drv->common.sock, SIOCGIFINDEX, &ifr) != 0) {
		wpa_printf(MSG_ERROR, "ioctl(SIOCGIFINDEX): %s",
			   strerror(errno));
		return -1;
	}

	os_memset(&addr, 0, sizeof(addr));
	addr.sll_family = AF_PACKET;
	addr.sll_ifindex = ifr.ifr_ifindex;
	wpa_printf(MSG_DEBUG, "Opening raw packet socket for ifindex %d",
		   addr.sll_ifindex);

	if (bind(drv->common.sock, (struct sockaddr *) &addr, sizeof(addr)) < 0)
	{
		wpa_printf(MSG_ERROR, "bind: %s", strerror(errno));
		return -1;
	}

	/* filter multicast address */
	if (wired_multicast_membership(drv->common.sock, ifr.ifr_ifindex,
				       pae_group_addr, 1) < 0) {
		wpa_printf(MSG_ERROR, "wired: Failed to add multicast group "
			   "membership");
		return -1;
	}

	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_name, drv->common.ifname, sizeof(ifr.ifr_name));
	if (ioctl(drv->common.sock, SIOCGIFHWADDR, &ifr) != 0) {
		wpa_printf(MSG_ERROR, "ioctl(SIOCGIFHWADDR): %s",
			   strerror(errno));
		return -1;
	}

	if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
		wpa_printf(MSG_INFO, "Invalid HW-addr family 0x%04x",
			   ifr.ifr_hwaddr.sa_family);
		return -1;
	}
	os_memcpy(own_addr, ifr.ifr_hwaddr.sa_data, ETH_ALEN);

#ifndef CONFIG_SONIC_HOSTAPD
	/* setup dhcp listen socket for sta detection */
	if ((drv->dhcp_sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
		wpa_printf(MSG_ERROR, "socket call failed for dhcp: %s",
			   strerror(errno));
		return -1;
	}

	if (eloop_register_read_sock(drv->dhcp_sock, handle_dhcp,
				     drv->common.ctx, NULL)) {
		wpa_printf(MSG_INFO, "Could not register read socket");
		return -1;
	}

	os_memset(&addr2, 0, sizeof(addr2));
	addr2.sin_family = AF_INET;
	addr2.sin_port = htons(67);
	addr2.sin_addr.s_addr = INADDR_ANY;

	if (setsockopt(drv->dhcp_sock, SOL_SOCKET, SO_REUSEADDR, (char *) &n,
		       sizeof(n)) == -1) {
		wpa_printf(MSG_ERROR, "setsockopt[SOL_SOCKET,SO_REUSEADDR]: %s",
			   strerror(errno));
		return -1;
	}
	if (setsockopt(drv->dhcp_sock, SOL_SOCKET, SO_BROADCAST, (char *) &n,
		       sizeof(n)) == -1) {
		wpa_printf(MSG_ERROR, "setsockopt[SOL_SOCKET,SO_BROADCAST]: %s",
			   strerror(errno));
		return -1;
	}

	os_memset(&ifr, 0, sizeof(ifr));
	os_strlcpy(ifr.ifr_ifrn.ifrn_name, drv->common.ifname, IFNAMSIZ);
	if (setsockopt(drv->dhcp_sock, SOL_SOCKET, SO_BINDTODEVICE,
		       (char *) &ifr, sizeof(ifr)) < 0) {
		wpa_printf(MSG_ERROR,
			   "setsockopt[SOL_SOCKET,SO_BINDTODEVICE]: %s",
			   strerror(errno));
		return -1;
	}

	if (bind(drv->dhcp_sock, (struct sockaddr *) &addr2,
		 sizeof(struct sockaddr)) == -1) {
		wpa_printf(MSG_ERROR, "bind: %s", strerror(errno));
		return -1;
	}
#endif
	return 0;
#else /* __linux__ */
	return -1;
#endif /* __linux__ */
}


static int wired_send_eapol(void *priv, const u8 *addr,
			    const u8 *data, size_t data_len, int encrypt,
			    const u8 *own_addr, u32 flags)
{
	struct wpa_driver_wired_data *drv = priv;
	struct ieee8023_hdr *hdr;
	size_t len;
	u8 *pos;
	int res;

	len = sizeof(*hdr) + data_len;
	hdr = os_zalloc(len);
	if (hdr == NULL) {
		wpa_printf(MSG_INFO,
			   "malloc() failed for wired_send_eapol(len=%lu)",
			   (unsigned long) len);
		return -1;
	}

	os_memcpy(hdr->dest, drv->use_pae_group_addr ? pae_group_addr : addr,
		  ETH_ALEN);
	os_memcpy(hdr->src, own_addr, ETH_ALEN);
	hdr->ethertype = htons(ETH_P_PAE);

	pos = (u8 *) (hdr + 1);
	os_memcpy(pos, data, data_len);

	res = send(drv->common.sock, (u8 *) hdr, len, 0);
	os_free(hdr);

	if (res < 0) {
		wpa_printf(MSG_ERROR,
			   "wired_send_eapol - packet len: %lu - failed: send: %s",
			   (unsigned long) len, strerror(errno));
	}

	return res;
}

static void * wired_driver_hapd_init(struct hostapd_data *hapd,
				     struct wpa_init_params *params)
{
	struct wpa_driver_wired_data *drv;

	drv = os_zalloc(sizeof(struct wpa_driver_wired_data));
	if (drv == NULL) {
		wpa_printf(MSG_INFO,
			   "Could not allocate memory for wired driver data");
		return NULL;
	}

	drv->common.ctx = hapd;
	os_strlcpy(drv->common.ifname, params->ifname,
		   sizeof(drv->common.ifname));
		wpa_printf(MSG_INFO,
			   "drv->common.ifname received is %s", drv->common.ifname);

	drv->use_pae_group_addr = params->use_pae_group_addr;

	if (wired_init_sockets(drv, params->own_addr)) {
		os_free(drv);
		return NULL;
	}
	
	return drv;
}

static void wired_driver_hapd_deinit(void *priv)
{
	struct wpa_driver_wired_data *drv = priv;

	if (drv->common.sock >= 0) {
		eloop_unregister_read_sock(drv->common.sock);
		close(drv->common.sock);
	}
#ifndef CONFIG_SONIC_HOSTAPD
	if (drv->dhcp_sock >= 0) {
		eloop_unregister_read_sock(drv->dhcp_sock);
		close(drv->dhcp_sock);
	}
#endif
	
	os_free(drv);
}


static void * wpa_driver_wired_init(void *ctx, const char *ifname)
{
	struct wpa_driver_wired_data *drv;

	drv = os_zalloc(sizeof(*drv));
	if (drv == NULL)
		return NULL;

	if (driver_wired_init_common(&drv->common, ifname, ctx) < 0) {
		os_free(drv);
		return NULL;
	}

	return drv;
}


static void wpa_driver_wired_deinit(void *priv)
{
	struct wpa_driver_wired_data *drv = priv;

	driver_wired_deinit_common(&drv->common);
	os_free(drv);
}

const struct wpa_driver_ops wpa_driver_wired_ops = {
	.name = "wired",
	.desc = "Wired Ethernet driver",
	.hapd_init = wired_driver_hapd_init,
	.hapd_deinit = wired_driver_hapd_deinit,
	.hapd_send_eapol = wired_send_eapol,
	.get_ssid = driver_wired_get_ssid,
	.get_bssid = driver_wired_get_bssid,
	.get_capa = driver_wired_get_capa,
#ifdef HOSTAPD
  .auth_resp_send = wired_driver_auth_resp_send,
#endif
  .init = wpa_driver_wired_init,
	.deinit = wpa_driver_wired_deinit,
};
