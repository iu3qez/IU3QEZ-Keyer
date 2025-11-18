/**
 * @file dns_server.cpp
 * @brief Implementation of minimal DNS server for captive portal.
 */

#include "captive_portal/dns_server.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstring>
#include <unordered_map>

namespace captive_portal {

namespace {
constexpr const char* kTag = "DNSServer";

// DNS header structure (12 bytes, RFC 1035)
struct DnsHeader {
  uint16_t transaction_id;  // Transaction ID from query
  uint16_t flags;           // Flags (QR, Opcode, AA, TC, RD, RA, Z, RCODE)
  uint16_t questions;       // Number of questions
  uint16_t answers;         // Number of answer RRs
  uint16_t authority;       // Number of authority RRs
  uint16_t additional;      // Number of additional RRs
} __attribute__((packed));

// DNS query types
constexpr uint16_t kDnsTypeA = 1;     // IPv4 address
constexpr uint16_t kDnsTypeAAAA = 28; // IPv6 address

// DNS response codes
constexpr uint16_t kDnsFlagResponse = 0x8000;   // QR bit: 1 = response
constexpr uint16_t kDnsFlagAuthoritative = 0x0400; // AA bit: 1 = authoritative answer
constexpr uint16_t kDnsRcodeSuccess = 0x0000;   // RCODE: No error
constexpr uint16_t kDnsRcodeNXDomain = 0x0003;  // RCODE: Name error (domain doesn't exist)

// Rate limiting: track query count per client IP
std::unordered_map<uint32_t, uint32_t> g_rate_limit_map;
uint32_t g_last_rate_limit_reset = 0;  // Timestamp of last rate limit reset (milliseconds)

}  // namespace

DnsServer::DnsServer()
    : socket_fd_(-1),
      target_ip_(kDefaultTargetIP),
      running_(false),
      task_handle_(nullptr) {
  ESP_LOGI(kTag, "DnsServer constructed");
}

DnsServer::~DnsServer() {
  Stop();
  ESP_LOGI(kTag, "DnsServer destroyed");
}

bool DnsServer::Start() {
  if (running_) {
    ESP_LOGW(kTag, "DNS server already running");
    return true;
  }

  // Create UDP socket
  socket_fd_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd_ < 0) {
    ESP_LOGE(kTag, "Failed to create UDP socket: errno %d", errno);
    return false;
  }

  // Bind to port 53 on all interfaces (0.0.0.0:53)
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(kDnsPort);

  if (lwip_bind(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
    ESP_LOGE(kTag, "Failed to bind to port %d: errno %d", kDnsPort, errno);
    lwip_close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  // Set socket to non-blocking mode
  int flags = lwip_fcntl(socket_fd_, F_GETFL, 0);
  lwip_fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

  // Create FreeRTOS task for server loop
  running_ = true;
  BaseType_t result = xTaskCreate(ServerTask, "dns_server", 4096, this, 5, (TaskHandle_t*)&task_handle_);
  if (result != pdPASS) {
    ESP_LOGE(kTag, "Failed to create DNS server task");
    running_ = false;
    lwip_close(socket_fd_);
    socket_fd_ = -1;
    return false;
  }

  ESP_LOGI(kTag, "DNS server started on port %d (target IP: %d.%d.%d.%d)",
           kDnsPort,
           (target_ip_ >> 24) & 0xFF, (target_ip_ >> 16) & 0xFF,
           (target_ip_ >> 8) & 0xFF, target_ip_ & 0xFF);
  return true;
}

bool DnsServer::Stop() {
  if (!running_) {
    return true;
  }

  running_ = false;

  // Close socket (this will unblock recvfrom in ServerLoop)
  if (socket_fd_ >= 0) {
    lwip_close(socket_fd_);
    socket_fd_ = -1;
  }

  // Wait for task to terminate
  if (task_handle_ != nullptr) {
    vTaskDelete((TaskHandle_t)task_handle_);
    task_handle_ = nullptr;
  }

  ESP_LOGI(kTag, "DNS server stopped");
  return true;
}

bool DnsServer::IsRunning() const {
  return running_;
}

void DnsServer::SetTargetIP(uint32_t ip_addr) {
  target_ip_ = ip_addr;
  ESP_LOGI(kTag, "Target IP set to: %d.%d.%d.%d",
           (ip_addr >> 24) & 0xFF, (ip_addr >> 16) & 0xFF,
           (ip_addr >> 8) & 0xFF, ip_addr & 0xFF);
}

void DnsServer::ServerTask(void* arg) {
  DnsServer* server = static_cast<DnsServer*>(arg);
  server->ServerLoop();
  vTaskDelete(nullptr);  // Self-delete when loop exits
}

void DnsServer::ServerLoop() {
  uint8_t query_packet[kMaxDnsPacketSize];
  uint8_t response_packet[kMaxDnsPacketSize];
  struct sockaddr_in client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  ESP_LOGI(kTag, "DNS server loop started");

  while (running_) {
    // Receive DNS query from client
    ssize_t recv_len = lwip_recvfrom(socket_fd_, query_packet, sizeof(query_packet), 0,
                                     (struct sockaddr*)&client_addr, &client_addr_len);

    if (recv_len < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No data available, sleep to reduce CPU usage
        vTaskDelay(pdMS_TO_TICKS(100));  // 100ms = 10 polls/second (reduced from 100 polls/sec)
        continue;
      }
      ESP_LOGE(kTag, "recvfrom failed: errno %d", errno);
      break;
    }

    if (recv_len < sizeof(DnsHeader)) {
      ESP_LOGW(kTag, "Received packet too small (%d bytes), ignoring", recv_len);
      continue;
    }

    // Check rate limit for this client
    uint32_t client_ip = ntohl(client_addr.sin_addr.s_addr);
    if (!CheckRateLimit(client_ip)) {
      ESP_LOGW(kTag, "Rate limit exceeded for client %d.%d.%d.%d",
               (client_ip >> 24) & 0xFF, (client_ip >> 16) & 0xFF,
               (client_ip >> 8) & 0xFF, client_ip & 0xFF);
      continue;
    }

    // Parse DNS query
    char domain[256];
    uint16_t query_type = 0;
    if (!ParseDnsQuery(query_packet, recv_len, domain, &query_type)) {
      ESP_LOGW(kTag, "Failed to parse DNS query, ignoring");
      continue;
    }

    ESP_LOGD(kTag, "DNS query from %d.%d.%d.%d: %s (type %d)",
             (client_ip >> 24) & 0xFF, (client_ip >> 16) & 0xFF,
             (client_ip >> 8) & 0xFF, client_ip & 0xFF,
             domain, query_type);

    // Build DNS response
    size_t response_len = BuildDnsResponse(query_packet, recv_len, response_packet, sizeof(response_packet));
    if (response_len == 0) {
      ESP_LOGW(kTag, "Failed to build DNS response");
      continue;
    }

    // Send DNS response back to client
    ssize_t sent_len = lwip_sendto(socket_fd_, response_packet, response_len, 0,
                                   (struct sockaddr*)&client_addr, client_addr_len);
    if (sent_len < 0) {
      ESP_LOGE(kTag, "sendto failed: errno %d", errno);
    } else {
      ESP_LOGD(kTag, "DNS response sent (%d bytes)", sent_len);
    }
  }

  ESP_LOGI(kTag, "DNS server loop exited");
}

bool DnsServer::ParseDnsQuery(const uint8_t* packet, size_t length, char* out_domain, uint16_t* out_query_type) {
  if (length < sizeof(DnsHeader)) {
    return false;
  }

  const DnsHeader* header = reinterpret_cast<const DnsHeader*>(packet);
  uint16_t questions = ntohs(header->questions);
  if (questions == 0) {
    return false;  // No questions in query
  }

  // Skip DNS header, parse question section
  const uint8_t* ptr = packet + sizeof(DnsHeader);
  const uint8_t* end = packet + length;

  // Parse domain name (QNAME)
  size_t domain_len = 0;
  while (ptr < end && *ptr != 0) {
    uint8_t label_len = *ptr++;
    if (label_len > 63 || ptr + label_len > end) {
      return false;  // Invalid label length
    }

    // Copy label to output (add dot separator if not first label)
    if (domain_len > 0 && domain_len < 255) {
      out_domain[domain_len++] = '.';
    }
    for (uint8_t i = 0; i < label_len && domain_len < 255; i++) {
      out_domain[domain_len++] = *ptr++;
    }
  }

  if (ptr >= end) {
    return false;  // Unexpected end of packet
  }

  out_domain[domain_len] = '\0';  // Null-terminate domain string
  ptr++;  // Skip null terminator

  // Parse query type (QTYPE) and class (QCLASS)
  if (ptr + 4 > end) {
    return false;  // Not enough bytes for QTYPE and QCLASS
  }

  *out_query_type = ntohs(*reinterpret_cast<const uint16_t*>(ptr));
  return true;
}

size_t DnsServer::BuildDnsResponse(const uint8_t* query_packet, size_t query_length,
                                    uint8_t* response_packet, size_t max_response_len) {
  if (query_length < sizeof(DnsHeader) || max_response_len < kMaxDnsPacketSize) {
    return 0;
  }

  // Copy query packet to response (preserves transaction ID and question section)
  memcpy(response_packet, query_packet, query_length);
  DnsHeader* header = reinterpret_cast<DnsHeader*>(response_packet);

  // Parse query type from question section
  char domain[256];
  uint16_t query_type = 0;
  if (!ParseDnsQuery(query_packet, query_length, domain, &query_type)) {
    return 0;
  }

  // Set response flags
  if (query_type == kDnsTypeA) {
    // A record query: respond with target IP address
    header->flags = htons(kDnsFlagResponse | kDnsFlagAuthoritative | kDnsRcodeSuccess);
    header->answers = htons(1);  // One answer RR
    header->authority = 0;
    header->additional = 0;

    // Append answer RR (NAME + TYPE + CLASS + TTL + RDLENGTH + RDATA)
    uint8_t* ptr = response_packet + query_length;

    // NAME: pointer to question name (DNS compression, 2 bytes)
    *ptr++ = 0xC0;  // Compression pointer flag
    *ptr++ = 0x0C;  // Offset to question name (12 bytes after header start)

    // TYPE: A (1)
    *reinterpret_cast<uint16_t*>(ptr) = htons(kDnsTypeA);
    ptr += 2;

    // CLASS: IN (1)
    *reinterpret_cast<uint16_t*>(ptr) = htons(1);
    ptr += 2;

    // TTL: 60 seconds
    *reinterpret_cast<uint32_t*>(ptr) = htonl(60);
    ptr += 4;

    // RDLENGTH: 4 bytes (IPv4 address)
    *reinterpret_cast<uint16_t*>(ptr) = htons(4);
    ptr += 2;

    // RDATA: IPv4 address (target_ip_ in network byte order)
    *reinterpret_cast<uint32_t*>(ptr) = htonl(target_ip_);
    ptr += 4;

    return ptr - response_packet;

  } else if (query_type == kDnsTypeAAAA) {
    // AAAA record query (IPv6): respond with NXDOMAIN (not supported)
    header->flags = htons(kDnsFlagResponse | kDnsFlagAuthoritative | kDnsRcodeNXDomain);
    header->answers = 0;
    header->authority = 0;
    header->additional = 0;
    return query_length;  // Return query as-is with NXDOMAIN flag

  } else {
    // Other query types: respond with NXDOMAIN
    header->flags = htons(kDnsFlagResponse | kDnsFlagAuthoritative | kDnsRcodeNXDomain);
    header->answers = 0;
    header->authority = 0;
    header->additional = 0;
    return query_length;
  }
}

bool DnsServer::CheckRateLimit(uint32_t client_ip) {
  // Reset rate limit counters every second
  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (now_ms - g_last_rate_limit_reset > 1000) {
    g_rate_limit_map.clear();
    g_last_rate_limit_reset = now_ms;
  }

  // Increment query count for this client
  uint32_t& count = g_rate_limit_map[client_ip];
  count++;

  // Check if rate limit exceeded
  return count <= kMaxQueriesPerSecond;
}

}  // namespace captive_portal
