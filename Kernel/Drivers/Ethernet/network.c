#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "e1000.h"
#include "network.h"
#include "rtl810x.h"
#include "serial.h"
#include "timer.h"
#include "vfs.h"
#include "virtio_net.h"

#define ETHERNET_MAX_FRAME 1518
#define ETHERNET_TYPE_IPV4 0x0800
#define ETHERNET_TYPE_ARP 0x0806
#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_UDP 17
#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DHCP_OPTIONS_OFFSET 240
#define ARP_CACHE_SIZE 8
#define NETWORK_POLL_LIMIT 16
#define DNS_PORT 53
#define DNS_TIMEOUT_MS 5000
#define DNS_PACKET_MAX 512

enum dhcp_state
{
    DHCP_DISABLED,
    DHCP_DISCOVERING,
    DHCP_REQUESTING,
    DHCP_CONFIGURED
};

typedef struct
{
    uint8_t address[4];
    uint8_t mac[6];
    uint64_t updated_at;
    bool valid;
} arp_entry_t;

static uint8_t local_address[4];
static uint8_t netmask[4];
static uint8_t gateway[4];
static uint8_t offered_address[4];
static uint8_t dhcp_server[4];
static uint8_t offered_dns[2][4];
static uint8_t dns_servers[2][4];
static uint8_t configured_dns[2][4];
static uint32_t dhcp_transaction;
static uint32_t ping_sequence;
static uint16_t ping_identifier;
static uint16_t active_ping_sequence;
static uint8_t active_ping_address[4];
static uint64_t ping_reply_time;
static bool ping_reply_received;
static uint16_t dns_query_sequence;
static uint16_t active_dns_query;
static bool dns_reply_received;
static uint8_t dns_reply_address[4];
static uint64_t last_dhcp_attempt;
static enum dhcp_state dhcp_status;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static uint32_t arp_next;
static bool initialized;
static bool polling;
static bool request_busy;
static bool using_virtio_net;
static bool using_realtek_net;

static bool lookup_arp(const uint8_t address[4], uint8_t mac[6]);
static void send_arp_request(const uint8_t target[4]);

static const uint8_t *network_mac_address(void)
{
    if (using_virtio_net)
        return virtio_net_mac_address();
    if (using_realtek_net)
        return realtek_mac_address();
    return e1000_mac_address();
}

static bool network_link_is_up(void)
{
    if (using_virtio_net)
        return virtio_net_link_is_up();
    if (using_realtek_net)
        return realtek_link_is_up();
    return e1000_link_is_up();
}

static bool network_send(const void *packet, size_t length)
{
    if (using_virtio_net)
        return virtio_net_send(packet, length);
    if (using_realtek_net)
        return realtek_send(packet, length);
    return e1000_send(packet, length);
}

static size_t network_receive(void *packet, size_t capacity)
{
    if (using_virtio_net)
        return virtio_net_receive(packet, capacity);
    if (using_realtek_net)
        return realtek_receive(packet, capacity);
    return e1000_receive(packet, capacity);
}

static void memory_zero(void *destination, size_t length)
{
    volatile uint8_t *bytes = destination;
    for (size_t i = 0; i < length; i++)
        bytes[i] = 0;
}

static bool mac_equal(const uint8_t *left, const uint8_t *right)
{
    for (uint32_t i = 0; i < 6; i++)
    {
        if (left[i] != right[i])
            return false;
    }
    return true;
}

static void memory_copy(void *destination, const void *source, size_t length)
{
    volatile uint8_t *output = destination;
    const volatile uint8_t *input = source;
    for (size_t i = 0; i < length; i++)
        output[i] = input[i];
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) |
        ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) |
        data[3];
}

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static bool ipv4_equal(const uint8_t *left, const uint8_t *right)
{
    return left[0] == right[0] && left[1] == right[1] &&
        left[2] == right[2] && left[3] == right[3];
}

static uint16_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0;
    while (length >= 2)
    {
        sum += read_be16(data);
        data += 2;
        length -= 2;
    }
    if (length)
        sum += (uint16_t)data[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static bool ipv4_is_zero(const uint8_t address[4])
{
    return !(address[0] | address[1] | address[2] | address[3]);
}

static bool udp_checksum_valid(
    const uint8_t source[4],
    const uint8_t destination[4],
    const uint8_t *datagram,
    size_t length
)
{
    if (length < 8 || length > ETHERNET_MAX_FRAME - 14 - 20)
        return false;
    if (!read_be16(datagram + 6))
        return true;

    uint8_t packet[12 + ETHERNET_MAX_FRAME - 14 - 20];
    memory_copy(packet, source, 4);
    memory_copy(packet + 4, destination, 4);
    packet[8] = 0;
    packet[9] = IP_PROTOCOL_UDP;
    write_be16(packet + 10, (uint16_t)length);
    memory_copy(packet + 12, datagram, length);
    return checksum(packet, 12 + length) == 0;
}

static bool parse_ipv4_text(const char *text, uint8_t address[4])
{
    for (uint32_t octet = 0; octet < 4; octet++)
    {
        if (*text < '0' || *text > '9')
            return false;
        uint32_t value = 0;
        uint32_t digits = 0;
        while (*text >= '0' && *text <= '9')
        {
            value = value * 10 + (uint32_t)(*text++ - '0');
            if (value > 255 || ++digits > 3)
                return false;
        }
        address[octet] = (uint8_t)value;
        if (octet < 3)
        {
            if (*text++ != '.')
                return false;
        }
        else if (*text)
            return false;
    }
    return true;
}

static void load_resolver_config(void)
{
    vfs_file_t file;
    if (vfs_open("/etc/resolv.conf", &file) < 0)
        return;

    char contents[512];
    int64_t count = vfs_read(&file, contents, sizeof(contents) - 1);
    if (count <= 0)
        return;
    contents[count] = '\0';

    uint32_t server_count = 0;
    char *line = contents;
    while (*line && server_count < 2)
    {
        char *end = line;
        while (*end && *end != '\n')
            end++;
        if (*end)
            *end++ = '\0';
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        static const char directive[] = "nameserver";
        size_t line_length = 0;
        while (line[line_length])
            line_length++;
        bool matches = line_length >= sizeof(directive);
        for (size_t i = 0; matches && i < sizeof(directive) - 1; i++)
        {
            if (line[i] != directive[i])
            {
                matches = false;
                break;
            }
        }
        if (matches &&
            (line[sizeof(directive) - 1] == ' ' ||
             line[sizeof(directive) - 1] == '\t'))
        {
            char *value = line + sizeof(directive) - 1;
            while (*value == ' ' || *value == '\t')
                value++;
            char *tail = value;
            while (*tail && *tail != ' ' && *tail != '\t' && *tail != '\r')
                tail++;
            *tail = '\0';
            uint8_t address[4];
            if (parse_ipv4_text(value, address) &&
                !ipv4_is_zero(address))
            {
                memory_copy(configured_dns[server_count], address, 4);
                server_count++;
            }
        }
        line = end;
    }
}

static bool begin_network_request(uint64_t *interrupt_flags)
{
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(*interrupt_flags)
        :
        : "memory"
    );
    if (request_busy)
    {
        if (*interrupt_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
        return false;
    }
    request_busy = true;
    /* Syscall interrupt gates clear IF, but network timeouts need PIT ticks. */
    __asm__ volatile ("sti" ::: "memory");
    return true;
}

static void end_network_request(uint64_t interrupt_flags)
{
    __asm__ volatile ("cli" ::: "memory");
    request_busy = false;
    if (interrupt_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

static bool send_frame(const uint8_t destination[6], uint16_t type,
    const uint8_t *payload, size_t payload_size)
{
    uint8_t frame[ETHERNET_MAX_FRAME];
    if (payload_size > sizeof(frame) - 14)
        return false;
    memory_copy(frame, destination, 6);
    memory_copy(frame + 6, network_mac_address(), 6);
    write_be16(frame + 12, type);
    memory_copy(frame + 14, payload, payload_size);
    return network_send(frame, 14 + payload_size);
}

static bool send_ipv4(const uint8_t destination_mac[6],
    const uint8_t source[4], const uint8_t destination[4],
    uint8_t protocol, const uint8_t *payload, size_t payload_size)
{
    uint8_t packet[ETHERNET_MAX_FRAME - 14];
    if (payload_size > sizeof(packet) - 20)
        return false;
    memory_zero(packet, 20);
    packet[0] = 0x45;
    write_be16(packet + 2, (uint16_t)(20 + payload_size));
    write_be16(packet + 4, (uint16_t)++ping_sequence);
    write_be16(packet + 6, 0x4000);
    packet[8] = 64;
    packet[9] = protocol;
    memory_copy(packet + 12, source, 4);
    memory_copy(packet + 16, destination, 4);
    write_be16(packet + 10, checksum(packet, 20));
    memory_copy(packet + 20, payload, payload_size);
    return send_frame(
        destination_mac,
        ETHERNET_TYPE_IPV4,
        packet,
        20 + payload_size);
}

static bool send_udp(const uint8_t destination_mac[6],
    const uint8_t source_address[4], const uint8_t destination_address[4],
    uint16_t source_port, uint16_t destination_port,
    const uint8_t *payload, size_t payload_size)
{
    uint8_t datagram[ETHERNET_MAX_FRAME - 14 - 20];
    if (payload_size > sizeof(datagram) - 8)
        return false;
    memory_zero(datagram, 8);
    write_be16(datagram, source_port);
    write_be16(datagram + 2, destination_port);
    write_be16(datagram + 4, (uint16_t)(8 + payload_size));
    memory_copy(datagram + 8, payload, payload_size);
    return send_ipv4(
        destination_mac,
        source_address,
        destination_address,
        IP_PROTOCOL_UDP,
        datagram,
        8 + payload_size);
}

static bool wait_for_arp(
    const uint8_t address[4],
    uint8_t mac[6],
    uint64_t timeout_ms
)
{
    if (lookup_arp(address, mac))
        return true;
    send_arp_request(address);
    uint64_t started = timer_get_milliseconds();
    while (timer_get_milliseconds() - started < timeout_ms)
    {
        network_poll();
        if (lookup_arp(address, mac))
            return true;
        __asm__ volatile ("pause");
    }
    return lookup_arp(address, mac);
}

static void route_next_hop(
    const uint8_t destination[4],
    uint8_t next_hop[4]
)
{
    bool local = true;
    for (uint32_t i = 0; i < 4; i++)
    {
        if ((destination[i] & netmask[i]) !=
            (local_address[i] & netmask[i]))
        {
            local = false;
            break;
        }
    }
    memory_copy(next_hop, local ? destination : gateway, 4);
}

static void send_dhcp_message(uint8_t message_type)
{
    uint8_t bootp[300];
    memory_zero(bootp, sizeof(bootp));
    bootp[0] = 1;
    bootp[1] = 1;
    bootp[2] = 6;
    write_be32(bootp + 4, dhcp_transaction);
    write_be16(bootp + 10, 0x8000);
    memory_copy(bootp + 28, network_mac_address(), 6);
    bootp[236] = 99;
    bootp[237] = 130;
    bootp[238] = 83;
    bootp[239] = 99;
    size_t option = DHCP_OPTIONS_OFFSET;
    bootp[option++] = 53;
    bootp[option++] = 1;
    bootp[option++] = message_type;
    if (message_type == 1)
    {
        bootp[option++] = 55;
        bootp[option++] = 4;
        bootp[option++] = 1;
        bootp[option++] = 3;
        bootp[option++] = 6;
        bootp[option++] = 51;
    }
    else
    {
        bootp[option++] = 50;
        bootp[option++] = 4;
        memory_copy(bootp + option, offered_address, 4);
        option += 4;
        bootp[option++] = 54;
        bootp[option++] = 4;
        memory_copy(bootp + option, dhcp_server, 4);
        option += 4;
    }
    bootp[option] = 255;

    static const uint8_t broadcast_mac[6] =
        { 255, 255, 255, 255, 255, 255 };
    static const uint8_t broadcast_ip[4] =
        { 255, 255, 255, 255 };
    static const uint8_t zero_ip[4] = { 0, 0, 0, 0 };
    (void)send_udp(
        broadcast_mac,
        zero_ip,
        broadcast_ip,
        DHCP_CLIENT_PORT,
        DHCP_SERVER_PORT,
        bootp,
        option + 1);
    last_dhcp_attempt = timer_get_milliseconds();
}

static void cache_arp(const uint8_t address[4], const uint8_t mac[6])
{
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (arp_cache[i].valid &&
            ipv4_equal(arp_cache[i].address, address))
        {
            memory_copy(arp_cache[i].mac, mac, 6);
            arp_cache[i].updated_at = timer_get_milliseconds();
            return;
        }
    }
    arp_entry_t *entry = &arp_cache[arp_next++ % ARP_CACHE_SIZE];
    memory_copy(entry->address, address, 4);
    memory_copy(entry->mac, mac, 6);
    entry->updated_at = timer_get_milliseconds();
    entry->valid = true;
}

static bool lookup_arp(const uint8_t address[4], uint8_t mac[6])
{
    uint64_t now = timer_get_milliseconds();
    for (uint32_t i = 0; i < ARP_CACHE_SIZE; i++)
    {
        if (arp_cache[i].valid &&
            ipv4_equal(arp_cache[i].address, address))
        {
            if (now - arp_cache[i].updated_at < 60000)
            {
                memory_copy(mac, arp_cache[i].mac, 6);
                return true;
            }
            arp_cache[i].valid = false;
        }
    }
    return false;
}

static void send_arp_request(const uint8_t target[4])
{
    uint8_t arp[28] = {0};
    write_be16(arp, 1);
    write_be16(arp + 2, ETHERNET_TYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, 1);
    memory_copy(arp + 8, network_mac_address(), 6);
    memory_copy(arp + 14, local_address, 4);
    memory_copy(arp + 24, target, 4);
    static const uint8_t broadcast_mac[6] =
        { 255, 255, 255, 255, 255, 255 };
    (void)send_frame(broadcast_mac, ETHERNET_TYPE_ARP, arp, sizeof(arp));
}

static void send_arp_reply(const uint8_t destination_mac[6],
    const uint8_t destination_address[4])
{
    uint8_t arp[28] = {0};
    write_be16(arp, 1);
    write_be16(arp + 2, ETHERNET_TYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    write_be16(arp + 6, 2);
    memory_copy(arp + 8, network_mac_address(), 6);
    memory_copy(arp + 14, local_address, 4);
    memory_copy(arp + 18, destination_mac, 6);
    memory_copy(arp + 24, destination_address, 4);
    (void)send_frame(
        destination_mac,
        ETHERNET_TYPE_ARP,
        arp,
        sizeof(arp));
}

static void parse_dhcp(const uint8_t *packet, size_t length)
{
    if (length < DHCP_OPTIONS_OFFSET || packet[0] != 2 ||
        packet[1] != 1 || packet[2] != 6 ||
        read_be32(packet + 4) != dhcp_transaction ||
        !mac_equal(packet + 28, network_mac_address()) ||
        packet[236] != 99 || packet[237] != 130 ||
        packet[238] != 83 || packet[239] != 99)
        return;

    uint8_t message_type = 0;
    uint8_t subnet[4] = { 255, 255, 255, 0 };
    uint8_t router[4] = {0};
    uint8_t server[4] = {0};
    uint8_t dns[2][4] = {{0}};
    for (size_t i = DHCP_OPTIONS_OFFSET; i < length;)
    {
        uint8_t code = packet[i++];
        if (code == 0)
            continue;
        if (code == 255 || i >= length)
            break;
        uint8_t option_length = packet[i++];
        if (option_length > length - i)
            break;
        if (code == 53 && option_length == 1)
            message_type = packet[i];
        else if (code == 1 && option_length == 4)
            memory_copy(subnet, packet + i, 4);
        else if (code == 3 && option_length >= 4)
            memory_copy(router, packet + i, 4);
        else if (code == 6 && option_length >= 4)
        {
            uint32_t count = option_length / 4;
            if (count > 2)
                count = 2;
            memory_copy(dns, packet + i, count * 4);
        }
        else if (code == 54 && option_length == 4)
            memory_copy(server, packet + i, 4);
        i += option_length;
    }

    if (message_type == 2 && dhcp_status == DHCP_DISCOVERING)
    {
        memory_copy(offered_address, packet + 16, 4);
        memory_copy(dhcp_server, server, 4);
        memory_copy(offered_dns, dns, sizeof(offered_dns));
        dhcp_status = DHCP_REQUESTING;
        send_dhcp_message(3);
    }
    else if (message_type == 5 && dhcp_status == DHCP_REQUESTING)
    {
        memory_copy(local_address, packet + 16, 4);
        memory_copy(netmask, subnet, 4);
        memory_copy(gateway, router, 4);
        if (!ipv4_is_zero(dns[0]))
            memory_copy(dns_servers, dns, sizeof(dns_servers));
        else if (!ipv4_is_zero(offered_dns[0]))
            memory_copy(dns_servers, offered_dns, sizeof(dns_servers));
        if (gateway[0] == 0)
            memory_copy(gateway, dhcp_server, 4);
        dhcp_status = DHCP_CONFIGURED;
        serial_write("Ethernet DHCP address: ");
        serial_write_dec(local_address[0]);
        serial_putc('.');
        serial_write_dec(local_address[1]);
        serial_putc('.');
        serial_write_dec(local_address[2]);
        serial_putc('.');
        serial_write_dec(local_address[3]);
        serial_putc('\n');
    }
}

static bool dns_skip_name(
    const uint8_t *packet,
    size_t packet_length,
    size_t *offset
)
{
    size_t cursor = *offset;
    size_t steps = 0;
    while (cursor < packet_length && steps++ < packet_length)
    {
        uint8_t label = packet[cursor];
        if ((label & 0xC0) == 0xC0)
        {
            if (cursor + 1 >= packet_length)
                return false;
            *offset = cursor + 2;
            return true;
        }
        if (label & 0xC0)
            return false;
        cursor++;
        if (!label)
        {
            *offset = cursor;
            return true;
        }
        if (label > 63 || label > packet_length - cursor)
            return false;
        cursor += label;
    }
    return false;
}

static void parse_dns(
    const uint8_t *packet,
    size_t length,
    const uint8_t source_address[4]
)
{
    if (length < 12 ||
        read_be16(packet) != active_dns_query ||
        !(packet[2] & 0x80) ||
        (packet[2] & 0x78) != 0 ||
        (!ipv4_equal(source_address, dns_servers[0]) &&
         !ipv4_equal(source_address, dns_servers[1])))
        return;

    dns_reply_received = true;
    if ((packet[3] & 0x0F) != 0)
        return;
    uint16_t questions = read_be16(packet + 4);
    uint16_t answers = read_be16(packet + 6);
    size_t offset = 12;
    for (uint16_t i = 0; i < questions; i++)
    {
        if (!dns_skip_name(packet, length, &offset) ||
            offset > length || length - offset < 4)
            return;
        offset += 4;
    }

    for (uint16_t i = 0; i < answers; i++)
    {
        if (!dns_skip_name(packet, length, &offset) ||
            offset > length || length - offset < 10)
            return;
        uint16_t type = read_be16(packet + offset);
        uint16_t class_code = read_be16(packet + offset + 2);
        uint16_t data_length = read_be16(packet + offset + 8);
        offset += 10;
        if (data_length > length - offset)
            return;
        if (type == 1 && class_code == 1 && data_length == 4 &&
            !ipv4_is_zero(packet + offset))
        {
            memory_copy(dns_reply_address, packet + offset, 4);
            dns_reply_received = true;
            return;
        }
        offset += data_length;
    }
}

static void parse_ipv4(const uint8_t *frame, size_t length)
{
    if (length < 34)
        return;
    const uint8_t *ip = frame + 14;
    size_t header_size = (ip[0] & 0x0F) * 4U;
    uint16_t total_size = read_be16(ip + 2);
    if ((ip[0] >> 4) != 4 || header_size < 20 ||
        total_size < header_size ||
        total_size > length - 14 ||
        checksum(ip, header_size) != 0 ||
        (read_be16(ip + 6) & 0x3FFF) != 0)
        return;

    const uint8_t *source_ip = ip + 12;
    const uint8_t *destination_ip = ip + 16;
    const uint8_t *payload = ip + header_size;
    size_t payload_length = total_size - header_size;
    if (ip[9] == IP_PROTOCOL_UDP && payload_length >= 8 &&
        read_be16(payload) == DHCP_SERVER_PORT &&
        read_be16(payload + 2) == DHCP_CLIENT_PORT)
    {
        uint16_t udp_length = read_be16(payload + 4);
        if (udp_length >= 8 && udp_length <= payload_length &&
            udp_checksum_valid(
                source_ip,
                destination_ip,
                payload,
                udp_length))
            parse_dhcp(payload + 8, udp_length - 8);
    }
    else if (ip[9] == IP_PROTOCOL_UDP && payload_length >= 8 &&
        read_be16(payload) == DNS_PORT)
    {
        uint16_t udp_length = read_be16(payload + 4);
        if (udp_length >= 8 && udp_length <= payload_length &&
            udp_checksum_valid(
                source_ip,
                destination_ip,
                payload,
                udp_length))
        {
            uint16_t source_port = read_be16(payload);
            uint16_t destination_port = read_be16(payload + 2);
            if (source_port == DNS_PORT &&
                destination_port == (uint16_t)(
                    49152U | (active_dns_query & 0x3FFFU)) &&
                ipv4_equal(destination_ip, local_address))
                parse_dns(payload + 8, udp_length - 8, source_ip);
        }
    }
    else if (ip[9] == IP_PROTOCOL_ICMP && payload_length >= 8 &&
        checksum(payload, payload_length) == 0)
    {
        if (payload[0] == 8 && payload[1] == 0 &&
            dhcp_status == DHCP_CONFIGURED &&
            ipv4_equal(destination_ip, local_address) &&
            !ipv4_is_zero(source_ip) && source_ip[0] < 224)
        {
            uint8_t reply[ETHERNET_MAX_FRAME - 14 - 20];
            memory_copy(reply, payload, payload_length);
            reply[0] = 0;
            write_be16(reply + 2, 0);
            write_be16(reply + 2, checksum(reply, payload_length));
            (void)send_ipv4(
                frame + 6,
                local_address,
                source_ip,
                IP_PROTOCOL_ICMP,
                reply,
                payload_length);
        }
        else if (payload[0] == 0 && payload[1] == 0 &&
            read_be16(payload + 4) == ping_identifier &&
            read_be16(payload + 6) == active_ping_sequence &&
            ipv4_equal(source_ip, active_ping_address) &&
            ipv4_equal(destination_ip, local_address))
        {
            ping_reply_time = timer_get_milliseconds();
            ping_reply_received = true;
        }
    }
}

static void parse_arp(const uint8_t *frame, size_t length)
{
    if (length < 42)
        return;
    const uint8_t *arp = frame + 14;
    if (read_be16(arp) != 1 ||
        read_be16(arp + 2) != ETHERNET_TYPE_IPV4 ||
        arp[4] != 6 || arp[5] != 4)
        return;
    uint16_t operation = read_be16(arp + 6);
    const uint8_t *sender_mac = arp + 8;
    const uint8_t *sender_ip = arp + 14;
    const uint8_t *target_ip = arp + 24;
    if ((operation != 1 && operation != 2) ||
        (sender_mac[0] & 1) ||
        !mac_equal(sender_mac, frame + 6) ||
        (operation == 2 &&
         !ipv4_equal(target_ip, local_address)))
        return;
    if (sender_ip[0] || sender_ip[1] || sender_ip[2] || sender_ip[3])
        cache_arp(sender_ip, sender_mac);
    if (operation == 1 && dhcp_status == DHCP_CONFIGURED &&
        ipv4_equal(target_ip, local_address))
        send_arp_reply(sender_mac, sender_ip);
}

static void process_frame(const uint8_t *frame, size_t length)
{
    if (length < 14)
        return;
    uint16_t type = read_be16(frame + 12);
    if (type == ETHERNET_TYPE_ARP)
        parse_arp(frame, length);
    else if (type == ETHERNET_TYPE_IPV4)
        parse_ipv4(frame, length);
}

bool network_init(void)
{
    if (initialized)
        return true;
    initialized = false;
    using_virtio_net = virtio_net_is_initialized();
    using_realtek_net =
        !using_virtio_net && realtek_is_initialized();
    if (!using_virtio_net && !using_realtek_net &&
        !e1000_is_initialized())
        return false;
    memory_zero(local_address, sizeof(local_address));
    memory_zero(netmask, sizeof(netmask));
    memory_zero(gateway, sizeof(gateway));
    memory_zero(offered_dns, sizeof(offered_dns));
    memory_zero(configured_dns, sizeof(configured_dns));
    memory_zero(dns_servers, sizeof(dns_servers));
    memory_zero(arp_cache, sizeof(arp_cache));
    arp_next = 0;
    request_busy = false;
    load_resolver_config();
    memory_copy(dns_servers, configured_dns, sizeof(dns_servers));
    dhcp_transaction = 0x5652534FU ^ (uint32_t)timer_get_milliseconds();
    ping_identifier = (uint16_t)(dhcp_transaction >> 16);
    dns_query_sequence = (uint16_t)(dhcp_transaction ^ 0xA531U);
    dhcp_status = DHCP_DISCOVERING;
    initialized = true;
    send_dhcp_message(1);
    return true;
}

void network_poll(void)
{
    if (!initialized)
        return;
    uint64_t interrupt_flags;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(interrupt_flags)
        :
        : "memory"
    );
    if (polling)
    {
        if (interrupt_flags & (1ULL << 9))
            __asm__ volatile ("sti" ::: "memory");
        return;
    }
    polling = true;
    if (interrupt_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");

    uint8_t frame[ETHERNET_MAX_FRAME];
    for (uint32_t i = 0; i < NETWORK_POLL_LIMIT; i++)
    {
        size_t length = network_receive(frame, sizeof(frame));
        if (!length)
            break;
        process_frame(frame, length);
    }
    if (dhcp_status != DHCP_CONFIGURED &&
        timer_get_milliseconds() - last_dhcp_attempt >= 5000)
    {
        if (dhcp_status == DHCP_REQUESTING)
            dhcp_status = DHCP_DISCOVERING;
        send_dhcp_message(1);
    }

    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(interrupt_flags)
        :
        : "memory"
    );
    polling = false;
    if (interrupt_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

void network_get_info(network_info_t *info)
{
    if (!info)
        return;
    network_poll();
    uint64_t interrupt_flags;
    __asm__ volatile (
        "pushfq\n\t"
        "popq %0\n\t"
        "cli"
        : "=r"(interrupt_flags)
        :
        : "memory"
    );
    memory_zero(info, sizeof(*info));
    if (initialized)
    {
        memory_copy(info->mac, network_mac_address(), 6);
        memory_copy(info->address, local_address, 4);
        memory_copy(info->netmask, netmask, 4);
        memory_copy(info->gateway, gateway, 4);
        memory_copy(info->dns, dns_servers, sizeof(info->dns));
        info->link_up = network_link_is_up();
        info->configured = dhcp_status == DHCP_CONFIGURED;
    }
    if (interrupt_flags & (1ULL << 9))
        __asm__ volatile ("sti" ::: "memory");
}

bool network_ping(const uint8_t address[4], uint64_t *round_trip_ms)
{
    if (!initialized || !address || dhcp_status != DHCP_CONFIGURED ||
        !network_link_is_up())
        return false;

    uint64_t interrupt_flags;
    if (!begin_network_request(&interrupt_flags))
        return false;
    bool result = false;

    uint8_t next_hop[4];
    route_next_hop(address, next_hop);

    uint8_t destination_mac[6];
    if (!wait_for_arp(next_hop, destination_mac, 2000))
        goto out;

    uint8_t packet[16] = {0};
    packet[0] = 8;
    write_be16(packet + 4, ping_identifier);
    active_ping_sequence++;
    write_be16(packet + 6, active_ping_sequence);
    write_be16(packet + 2, checksum(packet, sizeof(packet)));
    ping_reply_received = false;
    memory_copy(active_ping_address, address, 4);
    uint64_t started = timer_get_milliseconds();
    if (!send_ipv4(
            destination_mac,
            local_address,
            address,
            IP_PROTOCOL_ICMP,
            packet,
            sizeof(packet)))
        goto out;

    while (timer_get_milliseconds() - started < 3000)
    {
        network_poll();
        if (ping_reply_received)
        {
            if (round_trip_ms)
                *round_trip_ms = ping_reply_time - started;
            result = true;
            break;
        }
        __asm__ volatile ("pause");
    }

out:
    end_network_request(interrupt_flags);
    return result;
}

bool network_resolve(const char *hostname, uint8_t address[4])
{
    if (!initialized || !hostname || !address ||
        dhcp_status != DHCP_CONFIGURED || !network_link_is_up())
        return false;

    size_t hostname_length = 0;
    while (hostname_length <= NETWORK_HOSTNAME_MAX &&
        hostname[hostname_length])
        hostname_length++;
    if (!hostname_length || hostname_length > NETWORK_HOSTNAME_MAX)
        return false;

    uint8_t server_index = ipv4_is_zero(dns_servers[0]) ? 1 : 0;
    const uint8_t *server = dns_servers[server_index];
    if (ipv4_is_zero(server))
        return false;

    uint8_t query[DNS_PACKET_MAX] = {0};
    uint16_t query_id = ++dns_query_sequence;
    write_be16(query, query_id);
    write_be16(query + 2, 0x0100);
    write_be16(query + 4, 1);
    size_t query_length = 12;

    size_t label_start = 0;
    for (size_t i = 0; i <= hostname_length; i++)
    {
        if (i < hostname_length && hostname[i] != '.')
        {
            unsigned char character = (unsigned char)hostname[i];
            if (character < 0x21 || character > 0x7E)
                return false;
            continue;
        }

        size_t label_length = i - label_start;
        if (!label_length)
        {
            if (i == hostname_length && i > 0 && hostname[i - 1] == '.')
                break;
            return false;
        }
        if (label_length > 63 ||
            query_length + label_length + 1 + 5 > sizeof(query))
            return false;
        query[query_length++] = (uint8_t)label_length;
        for (size_t j = label_start; j < i; j++)
        {
            char character = hostname[j];
            if (character >= 'A' && character <= 'Z')
                character = (char)(character + ('a' - 'A'));
            query[query_length++] = (uint8_t)character;
        }
        label_start = i + 1;
    }

    query[query_length++] = 0;
    write_be16(query + query_length, 1);
    write_be16(query + query_length + 2, 1);
    query_length += 4;

    uint64_t interrupt_flags;
    if (!begin_network_request(&interrupt_flags))
        return false;

    bool result = false;
    uint8_t next_hop[4];
    uint8_t destination_mac[6];
    route_next_hop(server, next_hop);
    if (!wait_for_arp(next_hop, destination_mac, 2000))
        goto resolve_out;

    active_dns_query = query_id;
    dns_reply_received = false;
    memory_zero(dns_reply_address, sizeof(dns_reply_address));
    if (!send_udp(
            destination_mac,
            local_address,
            server,
            (uint16_t)(49152U | (query_id & 0x3FFFU)),
            DNS_PORT,
            query,
            query_length))
        goto resolve_out;

    uint64_t started = timer_get_milliseconds();
    while (timer_get_milliseconds() - started < DNS_TIMEOUT_MS)
    {
        network_poll();
        if (dns_reply_received)
        {
            memory_copy(address, dns_reply_address, 4);
            result = !ipv4_is_zero(dns_reply_address);
            break;
        }
        __asm__ volatile ("pause");
    }

resolve_out:
    end_network_request(interrupt_flags);
    return result;
}
