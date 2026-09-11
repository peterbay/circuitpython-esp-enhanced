// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-License-Identifier: MIT

// CIRCUITPY-CHANGE: the espressif end of the USB network interface. The class
// in shared-module/usb_net carries Ethernet frames over USB; this hangs an
// esp_netif on them, gives it the addresses boot.py asked for, and runs the
// IDF's DHCP server and a DNS responder of its own on it, so a host that
// plugs the board in gets an address and can reach the board by name.

#include "shared-module/usb_net/__init__.h"

#if CIRCUITPY_USB_NET

#include <string.h>

#include "apps/dhcpserver/dhcpserver.h"
#include "device/usbd_pvt.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "tusb.h"

#include "supervisor/port_heap.h"

static const char *TAG = "usb_net";

// Frames on their way to the host. The network stack hands them over from the
// TCP/IP task, but TinyUSB's class may only be called from the task that runs
// tud_task(), so that task is asked to take them. It cannot be left to notice
// on its own: tud_task() waits for USB events and returns only after sixteen
// of them.
#define USB_NET_TX_QUEUE_LEN (8)
// While the class has no room, the flush is retried every tick. A host that
// takes nothing in this many flushes is not reading the link, and what is
// queued is dropped, as an Ethernet driver drops frames on a link that is down.
#define USB_NET_TX_STALL_LIMIT (100)

typedef struct {
    uint16_t len;
    uint8_t *frame;
} usb_net_tx_item_t;

typedef struct {
    esp_netif_driver_base_t base;
} usb_net_driver_t;

static esp_netif_t *usb_netif;
static usb_net_driver_t usb_net_driver;
static QueueHandle_t tx_queue;
static TimerHandle_t tx_retry_timer;
static bool tx_flush_requested;
static uint32_t tx_stalled_flushes;

static void usb_net_flush(void *param);

// Any task. However many frames arrive before the flush runs, it is asked for
// once, so the network cannot fill TinyUSB's event queue, which USB's own
// interrupts need.
static void usb_net_request_flush(void) {
    if (!__atomic_exchange_n(&tx_flush_requested, true, __ATOMIC_SEQ_CST)) {
        usbd_defer_func(usb_net_flush, NULL, false);
    }
}

static void usb_net_retry_flush(TimerHandle_t timer) {
    (void)timer;
    usb_net_request_flush();
}

// Runs in the task that runs tud_task(), through usbd_defer_func().
static void usb_net_flush(void *param) {
    (void)param;
    __atomic_store_n(&tx_flush_requested, false, __ATOMIC_SEQ_CST);
    bool sent = false;
    usb_net_tx_item_t item;
    while (xQueuePeek(tx_queue, &item, 0) == pdTRUE && usb_net_send(item.frame, item.len)) {
        xQueueReceive(tx_queue, &item, 0);
        port_free(item.frame);
        sent = true;
    }
    if (uxQueueMessagesWaiting(tx_queue) == 0) {
        tx_stalled_flushes = 0;
        return;
    }
    tx_stalled_flushes = sent ? 0 : tx_stalled_flushes + 1;
    if (tx_stalled_flushes > USB_NET_TX_STALL_LIMIT) {
        while (xQueueReceive(tx_queue, &item, 0) == pdTRUE) {
            port_free(item.frame);
        }
        tx_stalled_flushes = 0;
        return;
    }
    // The class takes the next frame once the host has read the one before,
    // and says nothing when it has.
    xTimerStart(tx_retry_timer, 0);
}

// Called from the TCP/IP task. The frame belongs to lwip and is gone when
// this returns, so it is copied into the queue.
static esp_err_t usb_net_transmit(void *h, void *buffer, size_t len) {
    (void)h;
    if (tx_queue == NULL || len == 0 || len > 1514) {
        return ESP_FAIL;
    }
    usb_net_tx_item_t item = { .len = (uint16_t)len, .frame = port_malloc(len, false) };
    if (item.frame == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(item.frame, buffer, len);
    if (xQueueSend(tx_queue, &item, 0) != pdTRUE) {
        // The host is not draining the link; drop, as an Ethernet driver would.
        port_free(item.frame);
        return ESP_ERR_NO_MEM;
    }
    usb_net_request_flush();
    return ESP_OK;
}

// esp_netif hands the receive buffer on to lwip without copying it and frees
// it through this when the stack is done.
static void usb_net_free_rx_buffer(void *h, void *buffer) {
    (void)h;
    if (buffer != NULL) {
        port_free(buffer);
    }
}

static esp_err_t usb_net_post_attach(esp_netif_t *netif, void *args) {
    usb_net_driver_t *driver = args;
    driver->base.netif = netif;

    const esp_netif_driver_ifconfig_t ifconfig = {
        .handle = driver,
        .transmit = usb_net_transmit,
        .driver_free_rx_buffer = usb_net_free_rx_buffer,
    };
    return esp_netif_set_driver_config(netif, &ifconfig);
}

void usb_net_port_receive(const uint8_t *frame, size_t len) {
    if (usb_netif == NULL) {
        return;
    }
    // ethernetif_input() wraps this buffer in a pbuf rather than copying it,
    // and the frame belongs to TinyUSB only until we return, so hand over a
    // copy and let the free callback above release it. esp_netif frees the
    // copy itself on every failure path it takes.
    uint8_t *copy = port_malloc(len, false);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, frame, len);
    esp_netif_receive(usb_netif, copy, len, NULL);
}

// DNS, just enough of RFC 1035 to answer for one name. TinyUSB's responder in
// lib/networking answers a query of any type with an A record and ignores
// names it does not know, which leaves the host waiting out a timeout; this
// one says at once that there is no such record, or that it will not answer.
#define DNS_PORT (53)
#define DNS_HEADER_LEN (12)
#define DNS_FLAG_QR (0x8000)
#define DNS_FLAG_OPCODE (0x7800)
#define DNS_FLAG_AA (0x0400)
#define DNS_FLAG_RD (0x0100)
#define DNS_RCODE_REFUSED (5)
#define DNS_TYPE_A (1)
#define DNS_CLASS_IN (1)
#define DNS_TTL_SECONDS (60)
#define DNS_ANSWER_LEN (16)
// A header and a question for the longest name there can be.
#define DNS_MAX_QUERY (DNS_HEADER_LEN + 256 + 4)

static char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Walks the question's name, which starts at offset. Returns the offset just
// past it, or 0 if it runs off the end or is compressed, which a question
// never needs to be, and says whether it spells hostname.
static size_t dns_read_name(const uint8_t *msg, size_t len, size_t offset,
    const char *hostname, bool *is_hostname) {
    size_t matched = 0;
    bool matching = hostname[0] != '\0';
    for (;;) {
        if (offset >= len) {
            return 0;
        }
        uint8_t label_len = msg[offset++];
        if (label_len == 0) {
            break;
        }
        if ((label_len & 0xC0) != 0 || offset + label_len > len) {
            return 0;
        }
        if (matching && matched > 0) {
            matching = hostname[matched] == '.';
            matched++;
        }
        for (uint8_t i = 0; matching && i < label_len; i++, matched++) {
            matching = hostname[matched] != '\0' &&
                ascii_lower(hostname[matched]) == ascii_lower((char)msg[offset + i]);
        }
        offset += label_len;
    }
    *is_hostname = matching && hostname[matched] == '\0';
    return offset;
}

// Runs in the TCP/IP task.
static void dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    const usb_net_config_t *config = arg;
    uint8_t msg[DNS_MAX_QUERY + DNS_ANSWER_LEN];
    size_t len = pbuf_copy_partial(p, msg, DNS_MAX_QUERY, 0);
    pbuf_free(p);

    if (len < DNS_HEADER_LEN) {
        return;
    }
    const uint16_t flags = (msg[2] << 8) | msg[3];
    const uint16_t question_count = (msg[4] << 8) | msg[5];
    // A standard query with one question is all a stub resolver sends.
    if ((flags & (DNS_FLAG_QR | DNS_FLAG_OPCODE)) != 0 || question_count != 1) {
        return;
    }

    bool is_hostname = false;
    size_t end = dns_read_name(msg, len, DNS_HEADER_LEN, config->hostname, &is_hostname);
    if (end == 0 || end + 4 > len) {
        return;
    }
    const uint16_t type = (msg[end] << 8) | msg[end + 1];
    const uint16_t class = (msg[end + 2] << 8) | msg[end + 3];
    end += 4;

    // The reply is the query sent back with its flags and counts rewritten,
    // cut off after the question, which drops any EDNS record it carried.
    uint16_t reply_flags = DNS_FLAG_QR | (flags & DNS_FLAG_RD);
    uint8_t answer_count = 0;
    if (!is_hostname) {
        reply_flags |= DNS_RCODE_REFUSED;
    } else {
        // Any other type for our name gets no answer and no error, which
        // says the name exists but has no such record.
        reply_flags |= DNS_FLAG_AA;
        answer_count = (type == DNS_TYPE_A && class == DNS_CLASS_IN) ? 1 : 0;
    }
    msg[2] = reply_flags >> 8;
    msg[3] = reply_flags & 0xff;
    msg[6] = 0;
    msg[7] = answer_count;
    memset(msg + 8, 0, 4);

    if (answer_count > 0) {
        uint8_t *answer = msg + end;
        // The name, as a pointer back to the question's.
        answer[0] = 0xC0;
        answer[1] = DNS_HEADER_LEN;
        answer[2] = 0;
        answer[3] = DNS_TYPE_A;
        answer[4] = 0;
        answer[5] = DNS_CLASS_IN;
        answer[6] = (DNS_TTL_SECONDS >> 24) & 0xff;
        answer[7] = (DNS_TTL_SECONDS >> 16) & 0xff;
        answer[8] = (DNS_TTL_SECONDS >> 8) & 0xff;
        answer[9] = DNS_TTL_SECONDS & 0xff;
        answer[10] = 0;
        answer[11] = 4;
        memcpy(answer + 12, config->ipv4_address, 4);
        end += DNS_ANSWER_LEN;
    }

    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, end, PBUF_RAM);
    if (reply == NULL) {
        return;
    }
    memcpy(reply->payload, msg, end);
    udp_sendto(pcb, reply, addr, port);
    pbuf_free(reply);
}

// Run in the TCP/IP task, where lwip's raw API has to be used.
static esp_err_t dns_start(void *ctx) {
    const usb_net_config_t *config = ctx;
    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // Bound to the board's address on the link, so it answers nobody on wifi.
    ip_addr_t bind_address;
    IP_ADDR4(&bind_address, config->ipv4_address[0], config->ipv4_address[1],
        config->ipv4_address[2], config->ipv4_address[3]);
    if (udp_bind(pcb, &bind_address, DNS_PORT) != ERR_OK) {
        udp_remove(pcb);
        return ESP_FAIL;
    }
    udp_recv(pcb, dns_recv, (void *)config);
    return ESP_OK;
}

void usb_net_port_init(void) {
    if (usb_netif != NULL) {
        return;
    }
    const usb_net_config_t *config = usb_net_config();

    // Either of these may already have been done by the wifi code.
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %d", err);
        return;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop failed: %d", err);
        return;
    }

    tx_queue = xQueueCreate(USB_NET_TX_QUEUE_LEN, sizeof(usb_net_tx_item_t));
    tx_retry_timer = xTimerCreate("usb_net", 1, pdFALSE, NULL, usb_net_retry_flush);
    if (tx_queue == NULL || tx_retry_timer == NULL) {
        ESP_LOGE(TAG, "no memory for the transmit queue");
        return;
    }

    // A gateway of zero keeps the router option out of the DHCP offer.
    esp_netif_ip_info_t ip_info;
    memcpy(&ip_info.ip.addr, config->ipv4_address, 4);
    memcpy(&ip_info.netmask.addr, config->netmask, 4);
    memcpy(&ip_info.gw.addr, config->gateway, 4);

    esp_netif_inherent_config_t base_config = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base_config.if_key = "USB_NET";
    base_config.if_desc = "usb";
    base_config.flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP;
    base_config.ip_info = &ip_info;
    // Prefer wifi for anything that is not on this link.
    base_config.route_prio = 10;

    const esp_netif_config_t netif_config = {
        .base = &base_config,
        .driver = NULL,
        .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };

    usb_netif = esp_netif_new(&netif_config);
    if (usb_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return;
    }

    usb_net_driver.base.post_attach = usb_net_post_attach;
    err = esp_netif_attach(usb_netif, &usb_net_driver);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach failed: %d", err);
        return;
    }

    err = esp_netif_set_mac(usb_netif, (uint8_t *)config->mac_address);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_mac failed: %d", err);
    }

    // The DHCP server must be stopped while the address and its options are
    // set. With no DNS server set it offers its own address as the host's,
    // which is what the responder below is there for.
    esp_netif_dhcps_stop(usb_netif);
    err = esp_netif_set_ip_info(usb_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info failed: %d", err);
    }
    // One address to give away: there is only ever one host on the link.
    dhcps_lease_t lease = { .enable = true };
    memcpy(&lease.start_ip.addr, config->host_ipv4_address, 4);
    lease.end_ip = lease.start_ip;
    err = esp_netif_dhcps_option(usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS,
        &lease, sizeof(lease));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP lease range failed: %d", err);
    }
    dhcps_offer_t offer_router = OFFER_ROUTER;
    err = esp_netif_dhcps_option(usb_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS,
        &offer_router, sizeof(offer_router));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP router option failed: %d", err);
    }
    err = esp_netif_dhcps_start(usb_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DHCP server failed: %d", err);
    }

    esp_netif_action_start(usb_netif, NULL, 0, NULL);
    esp_netif_action_connected(usb_netif, NULL, 0, NULL);

    err = esp_netif_tcpip_exec(dns_start, (void *)config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DNS responder failed: %d", err);
    }

    ESP_LOGI(TAG, "USB network interface up on " IPSTR, IP2STR(&ip_info.ip));
}

#endif // CIRCUITPY_USB_NET
