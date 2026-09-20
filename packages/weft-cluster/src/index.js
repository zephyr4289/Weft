// index.js — @weft/cluster public surface.
export { ClusterClient, Subscription } from './client.js';
export { ClusterMesh } from './mesh.js';
export { ShmFabric, LoopbackShmTransport, ShmRing } from './transport/shm.js';
export { UdpTransport, CTL_SUB, CTL_UNSUB } from './transport/udp.js';
export {
  MembershipTable, GossipEngine, NodeHandle,
  NODE_FLAG_ALIVE, NODE_FLAG_LEAVING, ipToU32,
} from './topology.js';
export { ShardRouter } from './router.js';
export { WC, wcName, WeftClusterError, hasCode } from './errors.js';
export { crc32, crc32Range } from './crc32.js';
export {
  fnv1a64, u64ToBigInt, FrameHandle,
  encodeWcn1Into, decodeWcn1Into, verifyFrameCrc, parseWcn1,
  encodeWgs1Into, writeGossipEntryInto, decodeWgs1Into, readGossipEntryInto,
  WCN1_MAGIC, WGS1_MAGIC, WCN1_HEADER_SIZE, WGS1_HEADER_SIZE, WGS1_ENTRY_SIZE,
  WC_FLAG_INLINE_PAYLOAD, WC_FLAG_RDMA_REF, WC_FLAG_CRC_PRESENT, WC_FLAG_CONTROL,
} from './wire.js';
