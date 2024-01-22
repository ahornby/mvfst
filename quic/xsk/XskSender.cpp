/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#if defined(__linux__)

#include <folly/Benchmark.h>
#include <folly/String.h>
#include <quic/xsk/XskSender.h>
#include <quic/xsk/packet_utils.h>
#include <unistd.h>
#include <exception>
#include <stdexcept>

namespace facebook::xdpsocket {

const static int kDefaultTos = 0;
const static int kDefaultTtl = 64;

XskSender::~XskSender() {
  if (xskFd_ >= 0) {
    close_xsk(xskFd_);
  }

  if (umemArea_) {
    free_umem(umemArea_, numFrames_, frameSize_);
  }

  if (txMap_) {
    unmap_tx_ring(txMap_, &xskOffsets_, numFrames_);
  }

  if (cxMap_) {
    unmap_completion_ring(cxMap_, &xskOffsets_, numFrames_);
  }
}

folly::Optional<XskBuffer> XskSender::getXskBuffer(bool isIpV6) {
  std::lock_guard<std::mutex> guard(m_);

  uint32_t numFreeFrames = freeUmemIndices_.size();
  if (numFreeFrames <= (numFrames_ / 2)) {
    getFreeUmemFrames();
  }

  auto maybeFreeUmemLoc = getFreeUmemIndex();

  if (!maybeFreeUmemLoc.hasValue()) {
    return folly::none;
  }

  XskBuffer xskBuffer;
  char* buffer = (char*)umemArea_ + size_t(*maybeFreeUmemLoc * frameSize_) +
      sizeof(udphdr) + (isIpV6 ? sizeof(ipv6hdr) : sizeof(iphdr)) +
      sizeof(ethhdr);
  xskBuffer.buffer = buffer;
  xskBuffer.frameIndex = *maybeFreeUmemLoc;
  xskBuffer.payloadLength = 0;

  return xskBuffer;
}

void XskSender::writeXskBuffer(
    const XskBuffer& xskBuffer,
    const folly::SocketAddress& peer,
    const folly::SocketAddress& src) {
  bool isIpV6 = peer.getIPAddress().isV6();

  char* buffer = (char*)umemArea_ + size_t(xskBuffer.frameIndex * frameSize_);
  writeUdpPacketScaffoldingToBuffer(buffer, peer, src, xskBuffer.payloadLength);

  std::lock_guard<std::mutex> guard(m_);
  xdp_desc* descriptor = getTxDescriptor();
  descriptor->addr = __u64(xskBuffer.frameIndex * frameSize_);
  descriptor->len = xskBuffer.payloadLength + sizeof(udphdr) +
      (isIpV6 ? sizeof(ipv6hdr) : sizeof(iphdr)) + sizeof(ethhdr);
  descriptor->options = 0;

  numPacketsSentInBatch_++;
  if (numPacketsSentInBatch_ >= batchSize_) {
    numPacketsSentInBatch_ = 0;
    flush();
  }
}

void XskSender::returnBuffer(const XskBuffer& xskBuffer) {
  std::lock_guard<std::mutex> guard(m_);
  freeUmemIndices_.push(xskBuffer.frameIndex);
}

void XskSender::writeUdpPacketScaffoldingToBuffer(
    char* buffer,
    const folly::SocketAddress& peer,
    const folly::SocketAddress& src,
    uint16_t payloadLength) {
  char* bufferCopy = buffer;

  // Write the MAC header
  auto ethhdrCopy = ethhdr_;
  if (!peer.getIPAddress().isV6()) {
    ethhdrCopy.h_proto = htons(ETH_P_IP);
  }
  writeMacHeader(&ethhdrCopy, buffer);

  uint16_t ipPayloadLen = payloadLength + sizeof(udphdr);

  // Write the IP header
  if (peer.getIPAddress().isV6()) {
    writeIpHeader(
        peer.getIPAddress(),
        src.getIPAddress(),
        &ipv6hdr_,
        ipPayloadLen,
        buffer);
  } else {
    writeIpHeader(
        peer.getIPAddress(), src.getIPAddress(), &iphdr_, ipPayloadLen, buffer);
  }

  // Write the UDP header
  writeUdpHeader(
      src.getPort(), peer.getPort(), 0 /* checksum */, ipPayloadLen, buffer);

  writeChecksum(
      peer.getIPAddress(), src.getIPAddress(), bufferCopy, ipPayloadLen);
}

SendResult XskSender::writeUdpPacket(
    const folly::SocketAddress& peer,
    const folly::SocketAddress& src,
    const void* data,
    uint16_t len) {
  bool isV6 = src.getIPAddress().isV6();
  uint32_t numFreeFrames = freeUmemIndices_.size();
  if (numFreeFrames <= (numFrames_ / 2)) {
    getFreeUmemFrames();
  }

  auto freeUmemLoc = getFreeUmemIndex();

  if (!freeUmemLoc.hasValue()) {
    return SendResult::NO_FREE_DESCRIPTORS;
  }

  // Just write to the first slot in the umem for the time being
  char* buffer = (char*)umemArea_ + size_t(*freeUmemLoc * frameSize_);

  writeUdpPacketToBuffer(buffer, peer, src, (const char*)data, len);

  xdp_desc* descriptor = getTxDescriptor();
  descriptor->addr = __u64(*freeUmemLoc * frameSize_);
  descriptor->len = len + sizeof(udphdr) +
      (isV6 ? sizeof(ipv6hdr) : sizeof(iphdr)) + sizeof(ethhdr);
  descriptor->options = 0;

  folly::doNotOptimizeAway(descriptor->addr);
  folly::doNotOptimizeAway(descriptor->len);
  folly::doNotOptimizeAway(descriptor->options);
  folly::doNotOptimizeAway(descriptor);

  numPacketsSentInBatch_++;
  if (numPacketsSentInBatch_ >= batchSize_) {
    numPacketsSentInBatch_ = 0;
    flush();
  }

  return SendResult::SUCCESS;
}

SendResult XskSender::writeUdpPacket(
    const folly::SocketAddress& peer,
    const folly::SocketAddress& src,
    std::unique_ptr<folly::IOBuf>& data,
    uint16_t len) {
  return writeUdpPacket(peer, src, data->data(), len);
}

folly::Expected<folly::Unit, std::runtime_error> XskSender::init(
    const folly::MacAddress& localMac,
    const folly::MacAddress& gatewayMac) {
  auto xdpSocketInitResult = initXdpSocket();
  if (xdpSocketInitResult.hasError()) {
    return folly::makeUnexpected(xdpSocketInitResult.error());
  }

  initAddresses(localMac, gatewayMac);

  return folly::Unit();
}

folly::Expected<folly::Unit, std::runtime_error> XskSender::bind(int queueId) {
  int bind_result = bind_xsk(xskFd_, queueId);
  if (bind_result < 0) {
    std::string errorMsg = folly::to<std::string>(
        "Failed to bind xdp socket: ", folly::errnoStr(errno));
    return folly::makeUnexpected(std::runtime_error(errorMsg));
  }

  return folly::Unit();
}

FlushResult XskSender::flush() {
  auto* producerValPtr = (uint32_t*)((char*)txMap_ + xskOffsets_.tx.producer);
  __atomic_store_n(producerValPtr, txProducerIndex_, __ATOMIC_RELEASE);

  auto* txFlagsPtr = (uint32_t*)((char*)txMap_ + xskOffsets_.tx.flags);
  uint32_t flags = __atomic_load_n(txFlagsPtr, __ATOMIC_ACQUIRE);
  if ((flags & XDP_RING_NEED_WAKEUP) == 0) {
    return FlushResult::SUCCESS;
  }

  int ret = sendto(xskFd_, nullptr, 0, MSG_DONTWAIT, nullptr, 0);
  if (ret < 0) {
    return FlushResult::FAILED_SENDTO;
  }

  return FlushResult::SUCCESS;
}

void XskSender::writeUdpPacketToBuffer(
    char* buffer,
    const folly::SocketAddress& peer,
    const folly::SocketAddress& src,
    const void* data,
    uint16_t len) {
  char* bufferCopy = buffer;

  // Write the MAC header
  writeMacHeader(&ethhdr_, buffer);

  uint16_t ipPayloadLen = len + sizeof(udphdr);

  // Write the IP header
  if (src.getIPAddress().isV6()) {
    writeIpHeader(
        peer.getIPAddress(),
        src.getIPAddress(),
        &ipv6hdr_,
        ipPayloadLen,
        buffer);
  } else {
    writeIpHeader(
        peer.getIPAddress(), src.getIPAddress(), &iphdr_, ipPayloadLen, buffer);
  }

  // Write the UDP header
  writeUdpHeader(
      src.getPort(), peer.getPort(), 0 /* checksum */, ipPayloadLen, buffer);

  // Write the payload
  writeUdpPayload((const char*)data, len, buffer);

  writeChecksum(
      peer.getIPAddress(),
      src.getIPAddress(),
      bufferCopy,
      len + sizeof(udphdr));
}

folly::Expected<folly::Unit, std::runtime_error> XskSender::initXdpSocket() {
  // Create xdp socket
  xskFd_ = create_xsk();
  if (xskFd_ < 0) {
    return folly::makeUnexpected(
        std::runtime_error("Failed to create xdp socket"));
  }

  // Create umem
  umemArea_ = create_umem(xskFd_, numFrames_, frameSize_);

  // The guard takes care of cleanup in case something goes wrong during
  // initialization. We disarm the guard at the end if initaliztion is
  // successful.
  auto g = folly::makeGuard([&]() {
    close_xsk(xskFd_);
    if (umemArea_) {
      free_umem(umemArea_, numFrames_, frameSize_);
    }
    if (cxMap_) {
      unmap_completion_ring(cxMap_, &xskOffsets_, numFrames_);
    }
  });
  if (!umemArea_) {
    return folly::makeUnexpected(std::runtime_error("Failed to create umem"));
  }

  // Set completion ring
  int completion_ring_set_result = set_completion_ring(xskFd_, numFrames_);
  if (completion_ring_set_result < 0) {
    return folly::makeUnexpected(
        std::runtime_error("Failed to set completion ring"));
  }

  // Set fill ring
  int fill_ring_set_result = set_fill_ring(xskFd_);
  if (fill_ring_set_result < 0) {
    return folly::makeUnexpected(std::runtime_error("Failed to set fill ring"));
  }

  // Set tx ring
  int tx_ring_set_result = set_tx_ring(xskFd_, numFrames_);
  if (tx_ring_set_result < 0) {
    return folly::makeUnexpected(std::runtime_error("Failed to set tx ring"));
  }

  // Get mmap offsets
  int xsk_map_offsets_get_result = xsk_get_mmap_offsets(xskFd_, &xskOffsets_);
  if (xsk_map_offsets_get_result < 0) {
    return folly::makeUnexpected(
        std::runtime_error("Failed to get mmap offsets"));
  }

  // Map completion ring
  cxMap_ = map_completion_ring(xskFd_, &xskOffsets_, numFrames_);
  if (!cxMap_) {
    return folly::makeUnexpected(
        std::runtime_error("Failed to map completion ring"));
  }

  // Map tx ring
  txMap_ = map_tx_ring(xskFd_, &xskOffsets_, numFrames_);
  if (!txMap_) {
    return folly::makeUnexpected(std::runtime_error("Failed to map tx ring"));
  }

  g.dismiss();
  return folly::Unit();
}

void XskSender::initAddresses(
    const folly::MacAddress& localMac,
    const folly::MacAddress& gatewayMac) {
  // Set the ethhdr based on the local and gateway addresses obtained
  // previously.
  folly::doNotOptimizeAway(
      memcpy(ethhdr_.h_dest, gatewayMac.bytes(), ETH_ALEN));
  folly::doNotOptimizeAway(
      memcpy(ethhdr_.h_source, localMac.bytes(), ETH_ALEN));
  ethhdr_.h_proto = htons(ETH_P_IPV6);

  // Set the ipv6hdr based on the local address. The daddr and payload_len
  // fields are not modified. Those fields will be filled in when the
  // packet is being written to the shared buffer.
  ipv6hdr_.version = 6;
  folly::doNotOptimizeAway(
      memset(ipv6hdr_.flow_lbl, 0, sizeof(ipv6hdr_.flow_lbl)));
  ipv6hdr_.priority = kDefaultTos;
  ipv6hdr_.nexthdr = IPPROTO_UDP;
  ipv6hdr_.hop_limit = kDefaultTtl;

  iphdr_.version = 4;
  iphdr_.ihl = 5;
  iphdr_.tos = kDefaultTos;
  iphdr_.protocol = IPPROTO_UDP;
  iphdr_.ttl = kDefaultTtl;
  iphdr_.frag_off = 0x40;
}

xdp_desc* XskSender::getTxDescriptor() {
  auto* base = (xdp_desc*)((char*)txMap_ + xskOffsets_.tx.desc);
  xdp_desc* result = base + (txProducerIndex_ % numFrames_);
  txProducerIndex_++;
  return result;
}

folly::Optional<uint32_t> XskSender::getFreeUmemIndex() {
  if (freeUmemIndices_.empty()) {
    return folly::none;
  }
  uint32_t freeLoc = freeUmemIndices_.front();
  freeUmemIndices_.pop();
  return freeLoc;
}

void XskSender::getFreeUmemFrames() {
  auto* producerPtr = (uint32_t*)((char*)cxMap_ + xskOffsets_.cr.producer);
  uint32_t crProducerIndex = __atomic_load_n(producerPtr, __ATOMIC_ACQUIRE);
  folly::doNotOptimizeAway(crProducerIndex);
  auto* baseDesc = (uint64_t*)((char*)cxMap_ + xskOffsets_.cr.desc);
  uint32_t numEntries = crProducerIndex - crConsumerIndex_;

  for (uint32_t i = 0; i < numEntries; i++) {
    uint64_t* desc = baseDesc + (crConsumerIndex_ % numFrames_);
    uint32_t frameIndex = *desc / frameSize_;
    freeUmemIndices_.push(frameIndex);
    crConsumerIndex_++;
  }

  auto* consumerPtr = (uint32_t*)((char*)cxMap_ + xskOffsets_.cr.consumer);
  __atomic_store_n(consumerPtr, crConsumerIndex_, __ATOMIC_RELEASE);
}

} // namespace facebook::xdpsocket

#endif
