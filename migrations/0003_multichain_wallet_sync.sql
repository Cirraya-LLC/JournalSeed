-- Extend built-in wallet sync from TRON-only to Ethereum, Polygon, and Solana.

ALTER TABLE chain_networks DROP CONSTRAINT IF EXISTS chain_networks_chain_type_check;
ALTER TABLE chain_networks ADD CONSTRAINT chain_networks_chain_type_check
    CHECK (chain_type IN ('tron', 'evm', 'solana'));

ALTER TABLE chain_networks ADD COLUMN chain_id BIGINT;
ALTER TABLE chain_networks ADD COLUMN address_family TEXT;
UPDATE chain_networks
   SET chain_id = NULL,
       address_family = 'tron'
 WHERE code = 'tron-mainnet';
ALTER TABLE chain_networks ALTER COLUMN address_family SET NOT NULL;
ALTER TABLE chain_networks ADD CONSTRAINT chain_networks_address_family_check
    CHECK (address_family IN ('tron', 'evm', 'solana'));

INSERT INTO chain_networks(code, name, chain_type, chain_id, address_family, native_asset_symbol, native_decimals, explorer_tx_url)
VALUES
    ('ethereum-mainnet', 'Ethereum Mainnet', 'evm', 1, 'evm', 'ETH', 18, 'https://etherscan.io/tx/{txHash}'),
    ('polygon-mainnet', 'Polygon PoS Mainnet', 'evm', 137, 'evm', 'POL', 18, 'https://polygonscan.com/tx/{txHash}'),
    ('solana-mainnet', 'Solana Mainnet', 'solana', NULL, 'solana', 'SOL', 9, 'https://solscan.io/tx/{txHash}')
ON CONFLICT (code) DO UPDATE SET
    name = EXCLUDED.name,
    chain_type = EXCLUDED.chain_type,
    chain_id = EXCLUDED.chain_id,
    address_family = EXCLUDED.address_family,
    native_asset_symbol = EXCLUDED.native_asset_symbol,
    native_decimals = EXCLUDED.native_decimals,
    explorer_tx_url = EXCLUDED.explorer_tx_url,
    enabled = TRUE;

ALTER TABLE chain_settings ADD COLUMN etherscan_api_key_ciphertext BYTEA;
ALTER TABLE chain_settings ADD COLUMN etherscan_api_key_nonce BYTEA;
ALTER TABLE chain_settings ADD COLUMN etherscan_api_key_updated_at TIMESTAMPTZ;
ALTER TABLE chain_settings ADD COLUMN ethereum_rpc_url_ciphertext BYTEA;
ALTER TABLE chain_settings ADD COLUMN ethereum_rpc_url_nonce BYTEA;
ALTER TABLE chain_settings ADD COLUMN ethereum_rpc_url_updated_at TIMESTAMPTZ;
ALTER TABLE chain_settings ADD COLUMN polygon_rpc_url_ciphertext BYTEA;
ALTER TABLE chain_settings ADD COLUMN polygon_rpc_url_nonce BYTEA;
ALTER TABLE chain_settings ADD COLUMN polygon_rpc_url_updated_at TIMESTAMPTZ;
ALTER TABLE chain_settings ADD COLUMN solana_rpc_url_ciphertext BYTEA;
ALTER TABLE chain_settings ADD COLUMN solana_rpc_url_nonce BYTEA;
ALTER TABLE chain_settings ADD COLUMN solana_rpc_url_updated_at TIMESTAMPTZ;

ALTER TABLE wallet_sync_states ADD COLUMN checkpoint_signature TEXT;

ALTER TABLE wallet_address_labels
    DROP CONSTRAINT IF EXISTS wallet_address_labels_ledger_id_chain_network_id_address_no_key;
ALTER TABLE wallet_address_labels ALTER COLUMN chain_network_id DROP NOT NULL;
ALTER TABLE wallet_address_labels ADD COLUMN scope TEXT NOT NULL DEFAULT 'chain';
ALTER TABLE wallet_address_labels ADD CONSTRAINT wallet_address_labels_scope_check
    CHECK (scope IN ('chain', 'evm'));
ALTER TABLE wallet_address_labels ADD CONSTRAINT wallet_address_labels_scope_shape_check
    CHECK ((scope = 'chain' AND chain_network_id IS NOT NULL)
        OR (scope = 'evm' AND chain_network_id IS NULL));

CREATE UNIQUE INDEX wallet_address_labels_chain_unique
    ON wallet_address_labels (ledger_id, chain_network_id, address_normalized)
    WHERE scope = 'chain';
CREATE UNIQUE INDEX wallet_address_labels_evm_unique
    ON wallet_address_labels (ledger_id, address_normalized)
    WHERE scope = 'evm';
CREATE INDEX wallet_address_labels_scope_lookup
    ON wallet_address_labels (ledger_id, scope, address_normalized);
