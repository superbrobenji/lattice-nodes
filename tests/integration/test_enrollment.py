"""
Integration test: enrollment flow.
Requires: master ESP32 on MASTER_PORT, node ESP32 on NODE_PORT.
"""
import pytest
import os
import time
from harness import Node

MASTER_PORT = os.getenv('MASTER_PORT', '/dev/ttyUSB0')
NODE_PORT   = os.getenv('NODE_PORT',   '/dev/ttyUSB1')


@pytest.fixture(scope='module')
def master():
    n = Node(MASTER_PORT, 'master')
    yield n
    n.close()


@pytest.fixture(scope='module')
def node():
    n = Node(NODE_PORT, 'node')
    yield n
    n.close()


@pytest.mark.integration
def test_node_prints_public_key_on_boot(node):
    """New node should print its public key to serial for provisioning."""
    pub_key = node.get_public_key(timeout=10.0)
    assert pub_key is not None, "Node did not print PLANETOPIA_PUBKEY"
    assert len(pub_key) == 32


@pytest.mark.integration
def test_master_receives_enrollment_request(master, _node):
    """Master should relay OP_ENROLLMENT_REQ to server (serial) within 15s."""
    # Master should log enrollment request after node broadcasts
    enrolled = master.wait_for_log('Enrollment request complete, relaying to server', timeout=15.0)
    assert enrolled, "Master did not receive enrollment request from node"


@pytest.mark.integration
def test_server_approval_triggers_join_ack(master, node):
    """
    Simulate server approval: send OP_ENROLLMENT_APPROVE to master.
    Node should receive JOIN_ACK and log 'Enrollment approved'.
    """
    # Get node public key from its serial output
    pub_key = node.get_public_key(timeout=5.0)
    assert pub_key is not None

    # Simulate node MAC (read from its log or use known test MAC)
    test_mac = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01])

    # Send approval to master
    master.send_enrollment_approve(test_mac, pub_key)

    # Node should receive JOIN_ACK within 5s
    approved = node.wait_for_log('Enrollment approved', timeout=5.0)
    assert approved, "Node did not receive JOIN_ACK after server approval"


@pytest.mark.integration
def test_enrolled_node_stops_broadcasting_requests(node):
    """After enrollment, node should not re-broadcast enrollment requests."""
    # Wait 12s (longer than 10s retry interval)
    time.sleep(12.0)
    # No new enrollment log should appear
    new_request = node.wait_for_log('Enrollment request sent', timeout=2.0)
    assert not new_request, "Enrolled node is still broadcasting enrollment requests"
