#include "CompactMessage.h"
#include <cstring>

namespace lattice {
namespace mesh {

void toCompact(const mesh_message& src, CompactMessage& dst) {
  dst.proto_version = src.proto_version;
  dst.message_type = src.message_type;
  dst.data_type = src.data_type;
  memcpy(dst.origin_mac, src.origin_mac_address, 6);
  memcpy(dst.target_mac, src.target_mac_address, 6);
  memcpy(dst.last_hop_mac, src.last_hop_mac_address, 6);
  memcpy(dst.data, src.data, sizeof(dst.data));
  dst.hop_count = src.hop_count;
  dst.epoch_num = src.epoch_num;
  dst.seq_num = src.seq_num;
  memcpy(dst.enrollment_public_key, src.enrollment_public_key, sizeof(dst.enrollment_public_key));
  dst.route_len = src.route_len;
  memcpy(dst.route_path, src.route_path, sizeof(dst.route_path));
  memcpy(dst.auth_tag, src.auth_tag, sizeof(dst.auth_tag));
  memcpy(dst.auth_path, src.auth_path, sizeof(dst.auth_path));
}

void toWire(const CompactMessage& src, mesh_message& dst) {
  memset(&dst, 0, sizeof(dst));
  dst.proto_version = src.proto_version;
  dst.message_type = src.message_type;
  dst.data_type = src.data_type;
  memcpy(dst.origin_mac_address, src.origin_mac, 6);
  memcpy(dst.target_mac_address, src.target_mac, 6);
  memcpy(dst.last_hop_mac_address, src.last_hop_mac, 6);
  memcpy(dst.data, src.data, sizeof(src.data));
  dst.hop_count = src.hop_count;
  dst.epoch_num = src.epoch_num;
  dst.seq_num = src.seq_num;
  memcpy(dst.enrollment_public_key, src.enrollment_public_key, sizeof(src.enrollment_public_key));
  dst.route_len = src.route_len;
  memcpy(dst.route_path, src.route_path, sizeof(src.route_path));
  memcpy(dst.auth_tag, src.auth_tag, sizeof(src.auth_tag));
  memcpy(dst.auth_path, src.auth_path, sizeof(src.auth_path));
  // secondary_master_mac / secondary_public_key: left zeroed by the memset
  // above — CompactMessage does not carry them (see header comment).
}

} // namespace mesh
} // namespace lattice
