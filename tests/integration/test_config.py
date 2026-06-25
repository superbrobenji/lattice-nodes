"""
Integration test: TX power preset + CONFIG_SET propagation.
"""
import pytest
import os
from harness import Node, OP_TX_POWER_SET, OP_CONFIG_SET

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
def test_tx_power_preset_propagates_to_node(master, node):
    """
    Server sends OP_TX_POWER_SET=INDOOR(1) to master.
    Master should broadcast to all nodes.
    Node should log 'TX power preset applied from mesh'.
    """
    master.send_opcode(OP_TX_POWER_SET, bytes([1]))  # INDOOR

    applied = node.wait_for_log('TX power preset applied from mesh', timeout=10.0)
    assert applied, "Node did not apply TX power preset broadcast from master"


@pytest.mark.integration
def test_config_set_from_non_master_rejected(node):
    """
    Node sends CONFIG_SET directly — should be rejected by other nodes (TOFU).
    This simulates a rogue enrolled node trying to reconfigure peers.
    """
    # Send a CONFIG_SET from the node serial port (pretending it's a mesh message)
    # The node's own logging should NOT show it applying the config
    node.send_opcode(OP_CONFIG_SET, bytes([0x03]))  # Try to set SERIAL_ADAPTER

    # Wait briefly — if the node applies the config, it logs "Adapter configured"
    # This test verifies the master/relay rejects it — harder to test directly
    # without a third node. Mark as informational for now.
    pytest.skip("Requires 3-node setup to verify rejection on relay")
