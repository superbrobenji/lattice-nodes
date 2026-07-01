"""
Integration test: enrollment flow.
Requires: master ESP32 on MASTER_PORT, node ESP32 on NODE_PORT.
Requires: motionSensorServer running at SERVER_URL with ADMIN_KEY set.
"""
import pytest
import os
import time
from harness import Node

MASTER_PORT = os.getenv('MASTER_PORT', '/dev/ttyUSB0')
NODE_PORT   = os.getenv('NODE_PORT',   '/dev/ttyUSB1')
SERVER_URL  = os.getenv('SERVER_URL',  'http://localhost:8080')
ADMIN_KEY   = os.getenv('ADMIN_KEY',   '')


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
    assert pub_key is not None, "Node did not print LATTICE_PUBKEY"
    assert len(pub_key) == 32


@pytest.mark.integration
def test_master_receives_enrollment_request(master, node):
    """Master should relay OP_ENROLLMENT_REQ to server (serial) within 15s."""
    enrolled = master.wait_for_log('Enrollment request complete, relaying to server', timeout=15.0)
    assert enrolled, "Master did not receive enrollment request from node"


@pytest.mark.integration
def test_server_approval_triggers_join_ack(master, node):
    """
    Approve enrollment via server HTTP API.
    Node should receive JOIN_ACK and log 'Enrollment approved'.
    """
    test_mac = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01])
    master.send_enrollment_approve(test_mac, SERVER_URL, ADMIN_KEY)

    approved = node.wait_for_log('Enrollment approved', timeout=5.0)
    assert approved, "Node did not receive JOIN_ACK after server approval"


@pytest.mark.integration
def test_enrolled_node_stops_broadcasting_requests(node):
    """After enrollment, node should not re-broadcast enrollment requests."""
    time.sleep(12.0)
    new_request = node.wait_for_log('Enrollment request sent', timeout=2.0)
    assert not new_request, "Enrolled node is still broadcasting enrollment requests"
