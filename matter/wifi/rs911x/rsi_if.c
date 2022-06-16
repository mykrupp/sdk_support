#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "em_bus.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_ldma.h"
#include "em_usart.h"

#include "sl_status.h"

#include "FreeRTOS.h"
#include "event_groups.h"
#include "task.h"

#include "wfx_host_events.h"

#include "rsi_driver.h"
#include "rsi_wlan_non_rom.h"

#include "rsi_wlan_config.h"
#include "rsi_data_types.h"
#include "rsi_common_apis.h"
#include "rsi_wlan_apis.h"
#include "rsi_wlan.h"
#include "rsi_utils.h"
#include "rsi_socket.h"
#include "rsi_nwk.h"
//#include "rsi_wlan_non_rom.h"
#include "rsi_bootup_config.h"
#include "rsi_error.h"

#include "wfx_host_events.h"
#include "wfx_rsi.h"
#include "dhcp_client.h"
#include <CHIPDevicePlatformConfig.h>

//#include "rsi_wlan_config.h"

bool hasNotifiedIPV6 = false;
#if (CHIP_DEVICE_CONFIG_ENABLE_IPV4)
bool hasNotifiedIPV4 = false;
#endif /* CHIP_DEVICE_CONFIG_ENABLE_IPV4 */
bool hasNotifiedWifiConnectivity = false;

/*
 * This file implements the interface to the RSI SAPIs
 */
static uint8_t wfx_rsi_drv_buf[WFX_RSI_BUF_SZ];
static void wfx_rsi_join_cb(uint16_t status, const uint8_t *buf, const uint16_t len)
{
  WFX_RSI_LOG("%s: status: %d", __func__, status);
  wfx_rsi.dev_state &= ~WFX_RSI_ST_STA_CONNECTING;
  if (status != RSI_SUCCESS) {
    /*
     * We should enable retry.. (Need config variable for this)
     */
    WFX_RSI_LOG("%s: failed. retry: %d", __func__, wfx_rsi.join_retries);
#if (WFX_RSI_CONFIG_MAX_JOIN != 0)
    if (++wfx_rsi.join_retries < WFX_RSI_CONFIG_MAX_JOIN)
#endif
    {
      xEventGroupSetBits(wfx_rsi.events, WFX_EVT_STA_START_JOIN);
    }
  } else {
    /*
     * Join was complete - Do the DHCP
     */
    WFX_RSI_LOG("%s: join completed.", __func__);
#ifdef RS911X_SOCKETS
    xEventGroupSetBits(wfx_rsi.events, WFX_EVT_STA_DO_DHCP);
#else
    xEventGroupSetBits(wfx_rsi.events, WFX_EVT_STA_CONN);
#endif
  }
}
static void wfx_rsi_join_fail_cb(uint16_t status, uint8_t *buf, uint32_t len)
{
  WFX_RSI_LOG("%s: error: failed status: %d", __func__, status);
}
#ifdef RS911X_SOCKETS
/*
 * DHCP should end up here.
 */
static void wfx_rsi_ipchange_cb(uint16_t status, uint8_t *buf, uint32_t len)
{
  WFX_RSI_LOG("%s: status: %d", __func__, status);
  if (status != RSI_SUCCESS) {
    /* Restart DHCP? */
    xEventGroupSetBits(wfx_rsi.events, WFX_EVT_STA_DO_DHCP);
  } else {
    wfx_rsi.dev_state |= WFX_RSI_ST_STA_DHCP_DONE;
    xEventGroupSetBits(wfx_rsi.events, WFX_EVT_STA_DHCP_DONE);
  }
}
#else
/*
 * Got RAW WLAN data pkt
 */
static void wfx_rsi_wlan_pkt_cb(uint16_t status, uint8_t *buf, uint32_t len)
{
  // WFX_RSI_LOG("%s: status=%d, len=%d", __func__, status, len);
  if (status != RSI_SUCCESS) {
    return;
  }
  wfx_host_received_sta_frame_cb(buf, len);
}
#endif /* !Socket support */
static int32_t wfx_rsi_init(void)
{
  int32_t status;
  uint8_t buf[128];
  extern void rsi_hal_board_init(void);

  /* 
   * Get the GPIOs/PINs set-up
   */
  //rsi_hal_board_init ();
  WFX_RSI_LOG("%s: starting(HEAP_SZ = %d)", __func__, SL_HEAP_SIZE);
  //! Driver initialization
  status = rsi_driver_init(wfx_rsi_drv_buf, WFX_RSI_BUF_SZ);
  if ((status < 0) || (status > WFX_RSI_BUF_SZ)) {
    WFX_RSI_LOG("%s: error: RSI drv init failed with status: %d", __func__, status);
    return status;
  }

  WFX_RSI_LOG("%s: rsi_device_init", __func__);
  //! Redpine module intialisation
  if ((status = rsi_device_init(LOAD_NWP_FW)) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: error: rsi_device_init failed with status: %d", __func__, status);
    return status;
  }
  WFX_RSI_LOG("%s: start wireless drv task", __func__);
  /*
   * Create the driver task
   */
  if (xTaskCreate((TaskFunction_t)rsi_wireless_driver_task, "rsi_drv", WFX_RSI_WLAN_TASK_SZ, NULL, 1, &wfx_rsi.drv_task)
      != pdPASS) {
    WFX_RSI_LOG("%s: error: rsi_wireless_driver_task failed", __func__);
    return RSI_ERROR_INVALID_PARAM;
  }

  WFX_RSI_LOG("%s: rsi_wireless_init", __func__);
  if ((status = rsi_wireless_init(0, 0)) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: error: rsi_wireless_init failed with status: %d", __func__, status);
    return status;
  }
  WFX_RSI_LOG("%s: get FW version..", __func__);
  /*
   * Get the MAC and other info to let the user know about it.
   */
  if (rsi_wlan_get(RSI_FW_VERSION, buf, sizeof(buf)) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: error: rsi_wlan_get(RSI_FW_VERSION) failed with status: %d", __func__, status);
    return status;
  }
  buf[sizeof(buf) - 1] = 0;
  WFX_RSI_LOG("%s: RSI firmware version: %s", __func__, buf);
  //! Send feature frame
  if ((status = rsi_send_feature_frame()) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: error: rsi_send_feature_frame failed with status: %d", __func__, status);
    return status;
  }
  WFX_RSI_LOG("%s: sent rsi_send_feature_frame", __func__);
  (void)rsi_wlan_radio_init(); /* Required so we can get MAC address */
  if ((status = rsi_wlan_get(RSI_MAC_ADDRESS, &wfx_rsi.sta_mac.octet[0], 6)) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: error: rsi_wlan_get failed with status: %d", __func__, status);
    return status;
  }
  WFX_RSI_LOG("%s: WLAN: MAC %02x:%02x:%02x %02x:%02x:%02x",
              __func__,
              wfx_rsi.sta_mac.octet[0],
              wfx_rsi.sta_mac.octet[1],
              wfx_rsi.sta_mac.octet[2],
              wfx_rsi.sta_mac.octet[3],
              wfx_rsi.sta_mac.octet[4],
              wfx_rsi.sta_mac.octet[5]);
  wfx_rsi.events = xEventGroupCreate();
  /*
   * Register callbacks - We are only interested in the connectivity CBs
   */
#if 0  /* missing in sapi library */
        if ((status = rsi_wlan_register_callbacks (RSI_WLAN_JOIN_RESPONSE_HANDLER, wfx_rsi_join_cb)) != RSI_SUCCESS) {
                WFX_RSI_LOG ("*ERR*RSI CB register join cb");
                return status;
        }
#endif /* missing in sapi */
  if ((status = rsi_wlan_register_callbacks(RSI_JOIN_FAIL_CB, wfx_rsi_join_fail_cb)) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: RSI callback register join failed with status: %d", __func__, status);
    return status;
  }
#ifdef RS911X_SOCKETS
  (void)rsi_wlan_register_callbacks(RSI_IP_CHANGE_NOTIFY_CB, wfx_rsi_ipchange_cb);
#else
  if ((status = rsi_wlan_register_callbacks(RSI_WLAN_DATA_RECEIVE_NOTIFY_CB, wfx_rsi_wlan_pkt_cb)) != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: RSI callback register data-notify failed with status: %d", __func__, status);
    return status;
  }
#endif
  wfx_rsi.dev_state |= WFX_RSI_ST_DEV_READY;
  WFX_RSI_LOG("%s: RSI: OK", __func__);
  return RSI_SUCCESS;
}
void wfx_show_err(char *msg)
{
  WFX_RSI_LOG("%s: message: %d", __func__, msg);
}
/*
 * Start an async Join command
 */
static void wfx_rsi_do_join(void)
{
  int32_t status;

  if (wfx_rsi.dev_state & (WFX_RSI_ST_STA_CONNECTING | WFX_RSI_ST_STA_CONNECTED)) {
    WFX_RSI_LOG("%s: not joining - already in progress", __func__);
  } else {
    WFX_RSI_LOG("%s: WLAN: connecting to %s==%s, sec=%d",
                __func__,
                &wfx_rsi.sec.ssid[0],
                &wfx_rsi.sec.passkey[0],
                wfx_rsi.sec.security);
    /*
     * Join the network
     */
    /* TODO - make the WFX_SECURITY_xxx - same as RSI_xxx
     * Right now it's done by hand - we need something better
     */
    wfx_rsi.dev_state |= WFX_RSI_ST_STA_CONNECTING;
    if ((status = rsi_wlan_connect_async((int8_t *)&wfx_rsi.sec.ssid[0],
                                         (rsi_security_mode_t)wfx_rsi.sec.security,
                                         &wfx_rsi.sec.passkey[0],
                                         wfx_rsi_join_cb))
        != RSI_SUCCESS) {
      wfx_rsi.dev_state &= ~WFX_RSI_ST_STA_CONNECTING;
      WFX_RSI_LOG("%s: rsi_wlan_connect_async failed with status: %d", __func__, status);
      /* TODO - Start a timer.. to retry */
    } else {
      WFX_RSI_LOG("%s: starting JOIN to %s \n", __func__, (char *)&wfx_rsi.sec.ssid[0]);
    }
  }
}
#ifdef SL_WFX_CONFIG_SCAN
static void wfx_scan_cb(uint16_t status, const uint8_t *buffer, const uint16_t length)
{
  int x;
  wfx_wifi_scan_result_t ap;
  rsi_scan_info_t *scan;
  rsi_rsp_scan_t *rsp;

  WFX_RSI_LOG("%s: status: %d, len: %d", __func__, (int)status, (int)length);

  if (status) {
    /*
     * Scan is done - failed
     */
  } else
    for (x = 0; x < rsp->scan_count[0]; x++) {
      scan = &rsp->scan_info[x];
      strcpy(&ap.ssid[0], (char *)&scan->ssid[0]);
      ap.security = scan->security_mode;
      ap.rssi     = (int)scan->rssi_val;
      memcpy(&ap.bssid[0], &scan->bssid[0], 6);
      (*wfx_rsi.scan_cb)(&ap);
    }
  wfx_rsi.dev_state &= ~WFX_RSI_ST_SCANSTARTED;
  /* Terminate with end of scan which is no ap sent back */
  (*wfx_rsi.scan_cb)((wfx_wifi_scan_result_t *)0);
  wfx_rsi.scan_cb = (void (*)(wfx_wifi_scan_result_t *))0;
}
#endif /* SL_WFX_CONFIG_SCAN */
/*
 * The main WLAN task - started by wfx_wifi_start () that interfaces with RSI.
 * The rest of RSI stuff come in call-backs.
 * The initialization has been already done.
 */
/* ARGSUSED */
void wfx_rsi_task(void *arg)
{
  EventBits_t flags;
#ifndef RS911X_SOCKETS
  TickType_t last_dhcp_poll, now;
  struct netif *sta_netif;
#endif
  (void)arg;
  uint32_t rsi_status = wfx_rsi_init();
  if (rsi_status != RSI_SUCCESS) {
    WFX_RSI_LOG("%s: error: wfx_rsi_init with status: %d", __func__, rsi_status);
    return;
  }
#ifndef RS911X_SOCKETS
  wfx_lwip_start();
  last_dhcp_poll = xTaskGetTickCount();
  sta_netif      = wfx_get_netif(SL_WFX_STA_INTERFACE);
#endif
  wfx_started_notify();

  WFX_RSI_LOG("%s: starting event wait", __func__);
  for (;;) {
    /*
     * This is the main job of this task.
     * Wait for commands from the ConnectivityManager
     * Make state changes (based on call backs)
     */
    flags = xEventGroupWaitBits(wfx_rsi.events,
                                WFX_EVT_STA_CONN | WFX_EVT_STA_DISCONN | WFX_EVT_STA_START_JOIN
#ifdef RS911X_SOCKETS
                                  | WFX_EVT_STA_DO_DHCP | WFX_EVT_STA_DHCP_DONE
#endif /* RS911X_SOCKETS */
#ifdef SL_WFX_CONFIG_SOFTAP
                                  | WFX_EVT_AP_START | WFX_EVT_AP_STOP
#endif /* SL_WFX_CONFIG_SOFTAP */
#ifdef SL_WFX_CONFIG_SCAN
                                  | WFX_EVT_SCAN
#endif /* SL_WFX_CONFIG_SCAN */
                                  | 0,
                                pdTRUE,  /* Clear the bits */
                                pdFALSE, /* Wait for any bit */
                                pdMS_TO_TICKS(250));

    if (flags) {
      WFX_RSI_LOG("%s: wait event encountered: %x", __func__, flags);
    }
#ifdef RS911X_SOCKETS
    if (flags & WFX_EVT_STA_DO_DHCP) {
      /*
       * Do DHCP -
       */
      if ((status = rsi_config_ipaddress(RSI_IP_VERSION_4,
                                         RSI_DHCP | RSI_DHCP_UNICAST_OFFER,
                                         NULL,
                                         NULL,
                                         NULL,
                                         &wfx_rsi.ip4_addr[0],
                                         4,
                                         0))
          != RSI_SUCCESS) {
        /* We should try this again.. (perhaps sleep) */
        /* TODO - Figure out what to do here */
      }
    }
#else /* !RS911X_SOCKET - using LWIP */
    /*
     * Let's handle DHCP polling here
     */
    if (wfx_rsi.dev_state & WFX_RSI_ST_STA_CONNECTED) {
      if ((now = xTaskGetTickCount()) > (last_dhcp_poll + pdMS_TO_TICKS(250))) {
#if (CHIP_DEVICE_CONFIG_ENABLE_IPV4)
        uint8_t dhcp_state = dhcpclient_poll(sta_netif);
        if (dhcp_state == DHCP_ADDRESS_ASSIGNED && !hasNotifiedIPV4) {
          if (!hasNotifiedWifiConnectivity) {
            wfx_connected_notify(0, &wfx_rsi.ap_mac);
            hasNotifiedWifiConnectivity = true;
          }
          wfx_dhcp_got_ipv4((uint32_t)sta_netif->ip_addr.u_addr.ip4.addr);
          hasNotifiedIPV4 = true;
        } else if (dhcp_state == DHCP_OFF) {
          wfx_ip_changed_notify(0);
          hasNotifiedIPV4 = false;
        }
#endif /* CHIP_DEVICE_CONFIG_ENABLE_IPV4 */
        if ((ip6_addr_ispreferred(netif_ip6_addr_state(sta_netif, 0))) && !hasNotifiedIPV6) {
          if (!hasNotifiedWifiConnectivity) {
            wfx_connected_notify(0, &wfx_rsi.ap_mac);
            hasNotifiedWifiConnectivity = true;
          }
          wfx_ipv6_notify(1);
          hasNotifiedIPV6 = true;
        }
        last_dhcp_poll = now;
      }
    }
#endif /* RS911X_SOCKETS */
    if (flags & WFX_EVT_STA_START_JOIN) {
      wfx_rsi_do_join();
    }
    if (flags & WFX_EVT_STA_CONN) {
      /*
       * Initiate the Join command (assuming we have been provisioned)
       */
      WFX_RSI_LOG("%s: starting LwIP STA", __func__);
      wfx_rsi.dev_state |= WFX_RSI_ST_STA_CONNECTED;
#ifndef RS911X_SOCKETS
      hasNotifiedWifiConnectivity = false;
#if (CHIP_DEVICE_CONFIG_ENABLE_IPV4)
      hasNotifiedIPV4 = false;
#endif // CHIP_DEVICE_CONFIG_ENABLE_IPV4
      hasNotifiedIPV6 = false;
      wfx_lwip_set_sta_link_up();
#endif /* !RS911X_SOCKETS */
      /* We need to get AP Mac - TODO */
      // Uncomment once the hook into MATTER is moved to IP connectivty instead of AP connectivity.
      // wfx_connected_notify(0, &wfx_rsi.ap_mac); // This is independant of IP connectivity.
    }
    if (flags & WFX_EVT_STA_DISCONN) {
      wfx_rsi.dev_state &=
        ~(WFX_RSI_ST_STA_READY | WFX_RSI_ST_STA_CONNECTING | WFX_RSI_ST_STA_CONNECTED | WFX_RSI_ST_STA_DHCP_DONE);
      WFX_RSI_LOG("%s: disconnect notify", __func__);
      /* TODO: Implement disconnect notify */
#ifndef RS911X_SOCKETS
      wfx_lwip_set_sta_link_down(); // Internally dhcpclient_poll(netif) -> wfx_ip_changed_notify(0) for IPV4
#if (CHIP_DEVICE_CONFIG_ENABLE_IPV4)
      wfx_ip_changed_notify(0);
      hasNotifiedIPV4 = false;
#endif /* CHIP_DEVICE_CONFIG_ENABLE_IPV4 */
      wfx_ipv6_notify(0);
      hasNotifiedIPV6             = false;
      hasNotifiedWifiConnectivity = false;
#endif /* !RS911X_SOCKETS */
    }
#ifdef SL_WFX_CONFIG_SCAN
    if (flags & WFX_EVT_SCAN) {
      if (!(wfx_rsi.dev_state & WFX_RSI_ST_SCANSTARTED)) {
        WFX_RSI_LOG("%s: start SSID scan", __func__);
        rsi_wlan_scan_async((int8_t *)wfx_rsi.scan_ssid, 0, wfx_scan_cb);
        if (wfx_rsi.scan_ssid) {
          vPortFree(wfx_rsi.scan_ssid);
          wfx_rsi.scan_ssid = (char *)0;
        }
        wfx_rsi.dev_state |= WFX_RSI_ST_SCANSTARTED;
      }
    }
#endif /* SL_WFX_CONFIG_SCAN */
#ifdef SL_WFX_CONFIG_SOFTAP
    /* TODO */
    if (flags & WFX_EVT_AP_START) {
    }
    if (flags & WFX_EVT_AP_STOP) {
    }
#endif /* SL_WFX_CONFIG_SOFTAP */
  }
}
void wfx_dhcp_got_ipv4(uint32_t ip)
{
  /*
   * Acquire the new IP address
   */
  wfx_rsi.ip4_addr[0] = (ip)&0xff;
  wfx_rsi.ip4_addr[1] = (ip >> 8) & 0xff;
  wfx_rsi.ip4_addr[2] = (ip >> 16) & 0xff;
  wfx_rsi.ip4_addr[3] = (ip >> 24) & 0xff;
  WFX_RSI_LOG("%s: DHCP OK: IP=%d.%d.%d.%d",
              __func__,
              wfx_rsi.ip4_addr[0],
              wfx_rsi.ip4_addr[1],
              wfx_rsi.ip4_addr[2],
              wfx_rsi.ip4_addr[3]);
  /* Notify the Connectivity Manager - via the app */
  wfx_ip_changed_notify(1);
  wfx_rsi.dev_state |= WFX_RSI_ST_STA_READY;
}
/*
 * WARNING - Taken from RSI and broken up
 * This is my own RSI stuff for not copying code and allocating an extra
 * level of indirection - when using LWIP buffers
 * see also: int32_t rsi_wlan_send_data_xx(uint8_t *buffer, uint32_t length)
 */
void *wfx_rsi_alloc_pkt()
{
  rsi_pkt_t *pkt;
  // Allocate packet to send data
  if ((pkt = rsi_pkt_alloc(&rsi_driver_cb->wlan_cb->wlan_tx_pool)) == NULL) {
    return (void *)0;
  }

  return (void *)pkt;
}
void wfx_rsi_pkt_add_data(void *p, uint8_t *buf, uint16_t len, uint16_t off)
{
  rsi_pkt_t *pkt;

  pkt = (rsi_pkt_t *)p;
  memcpy(((char *)pkt->data) + off, buf, len);
}
int32_t wfx_rsi_send_data(void *p, uint16_t len)
{
  int32_t status;
  register uint8_t *host_desc;
  rsi_pkt_t *pkt;

  pkt       = (rsi_pkt_t *)p;
  host_desc = pkt->desc;
  memset(host_desc, 0, RSI_HOST_DESC_LENGTH);
  rsi_uint16_to_2bytes(host_desc, (len & 0xFFF));

  // Fill packet type
  host_desc[1] |= (RSI_WLAN_DATA_Q << 4);
  host_desc[2] |= 0x01;

  rsi_enqueue_pkt(&rsi_driver_cb->wlan_tx_q, pkt);

#ifndef RSI_SEND_SEM_BITMAP
  rsi_driver_cb_non_rom->send_wait_bitmap |= BIT(0);
#endif
  // Set TX packet pending event
  rsi_set_event(RSI_TX_EVENT);

  if (rsi_wait_on_wlan_semaphore(&rsi_driver_cb_non_rom->send_data_sem, RSI_SEND_DATA_RESPONSE_WAIT_TIME)
      != RSI_ERROR_NONE) {
    return RSI_ERROR_RESPONSE_TIMEOUT;
  }
  status = rsi_wlan_get_status();

  return status;
}
struct wfx_rsi wfx_rsi;
