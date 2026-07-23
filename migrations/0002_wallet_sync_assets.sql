-- Asset-aware accounting and built-in watch-only chain wallet sync.

ALTER TABLE accounts ALTER COLUMN opening_balance TYPE NUMERIC(38, 18);
ALTER TABLE journal_rows ALTER COLUMN amount TYPE NUMERIC(38, 18);
ALTER TABLE postings ALTER COLUMN signed_amount TYPE NUMERIC(38, 18);

CREATE TABLE chain_networks (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    code TEXT NOT NULL UNIQUE CHECK (code ~ '^[a-z0-9_:-]+$'),
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    chain_type TEXT NOT NULL CHECK (chain_type IN ('tron')),
    native_asset_symbol TEXT NOT NULL,
    native_decimals SMALLINT NOT NULL CHECK (native_decimals BETWEEN 0 AND 18),
    explorer_tx_url TEXT,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

INSERT INTO chain_networks(code, name, chain_type, native_asset_symbol, native_decimals, explorer_tx_url)
VALUES ('tron-mainnet', 'TRON Mainnet', 'tron', 'TRX', 6, 'https://tronscan.org/#/transaction/{txHash}')
ON CONFLICT (code) DO NOTHING;

CREATE TABLE assets (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    chain_network_id BIGINT REFERENCES chain_networks(id),
    contract_address TEXT,
    contract_address_normalized TEXT,
    symbol TEXT NOT NULL CHECK (length(btrim(symbol)) BETWEEN 1 AND 24),
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    decimals SMALLINT NOT NULL CHECK (decimals BETWEEN 0 AND 18),
    is_native BOOLEAN NOT NULL DEFAULT FALSE,
    is_default BOOLEAN NOT NULL DEFAULT FALSE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK ((is_default AND chain_network_id IS NULL AND contract_address_normalized IS NULL)
        OR NOT is_default),
    CHECK ((is_native AND chain_network_id IS NOT NULL AND contract_address_normalized IS NULL)
        OR NOT is_native)
);

CREATE UNIQUE INDEX assets_ledger_default_unique ON assets (ledger_id) WHERE is_default;
CREATE UNIQUE INDEX assets_chain_identifier_unique
    ON assets (ledger_id, chain_network_id, COALESCE(contract_address_normalized, 'native:' || lower(symbol)))
    WHERE chain_network_id IS NOT NULL;
CREATE INDEX assets_ledger_symbol ON assets (ledger_id, lower(symbol), id);

INSERT INTO assets(ledger_id, symbol, name, decimals, is_default)
SELECT id, 'DEFAULT', '默认本位资产', 2, TRUE FROM ledgers
ON CONFLICT DO NOTHING;

ALTER TABLE accounts ADD COLUMN asset_id BIGINT;
UPDATE accounts a
   SET asset_id = asset.id
  FROM assets asset
 WHERE asset.ledger_id = a.ledger_id AND asset.is_default;
ALTER TABLE accounts ALTER COLUMN asset_id SET NOT NULL;
ALTER TABLE accounts ADD CONSTRAINT accounts_asset_fk FOREIGN KEY (asset_id) REFERENCES assets(id);
ALTER TABLE accounts DROP CONSTRAINT IF EXISTS accounts_ledger_id_system_code_key;
ALTER TABLE accounts ADD CONSTRAINT accounts_ledger_system_asset_unique
    UNIQUE (ledger_id, system_code, asset_id);
CREATE INDEX accounts_asset_lookup ON accounts (ledger_id, asset_id, id) WHERE deleted_at IS NULL;

ALTER TABLE journal_rows ADD COLUMN asset_id BIGINT;
UPDATE journal_rows r
   SET asset_id = asset.id
  FROM assets asset
 WHERE asset.ledger_id = r.ledger_id AND asset.is_default;
ALTER TABLE journal_rows ALTER COLUMN asset_id SET NOT NULL;
ALTER TABLE journal_rows ADD CONSTRAINT journal_rows_asset_fk FOREIGN KEY (asset_id) REFERENCES assets(id);
CREATE INDEX journal_rows_asset_lookup ON journal_rows (ledger_id, asset_id, occurred_on DESC)
    WHERE deleted_at IS NULL;

ALTER TABLE postings ADD COLUMN asset_id BIGINT;
UPDATE postings p
   SET asset_id = r.asset_id
  FROM journal_rows r
 WHERE r.id = p.row_id;
ALTER TABLE postings ALTER COLUMN asset_id SET NOT NULL;
ALTER TABLE postings ADD CONSTRAINT postings_asset_fk FOREIGN KEY (asset_id) REFERENCES assets(id);
CREATE INDEX postings_asset_balance ON postings (account_id, asset_id, row_id) INCLUDE (signed_amount);

CREATE OR REPLACE FUNCTION check_journal_row_links_asset_ledger() RETURNS TRIGGER
LANGUAGE plpgsql AS $$
DECLARE
    linked_ledger_id BIGINT;
    linked_asset_id BIGINT;
BEGIN
    IF NEW.account_id IS NOT NULL THEN
        SELECT ledger_id, asset_id INTO linked_ledger_id, linked_asset_id
          FROM accounts WHERE id = NEW.account_id;
        IF linked_ledger_id IS DISTINCT FROM NEW.ledger_id THEN
            RAISE EXCEPTION 'row % account must belong to the row ledger', NEW.id;
        ELSIF linked_asset_id IS DISTINCT FROM NEW.asset_id THEN
            RAISE EXCEPTION 'row % account asset must match row asset', NEW.id;
        END IF;
    END IF;

    IF NEW.transfer_account_id IS NOT NULL THEN
        SELECT ledger_id, asset_id INTO linked_ledger_id, linked_asset_id
          FROM accounts WHERE id = NEW.transfer_account_id;
        IF linked_ledger_id IS DISTINCT FROM NEW.ledger_id THEN
            RAISE EXCEPTION 'row % transfer account must belong to the row ledger', NEW.id;
        ELSIF linked_asset_id IS DISTINCT FROM NEW.asset_id THEN
            RAISE EXCEPTION 'row % transfer account asset must match row asset', NEW.id;
        END IF;
    END IF;

    IF NEW.category_id IS NOT NULL THEN
        SELECT ledger_id INTO linked_ledger_id FROM categories WHERE id = NEW.category_id;
        IF linked_ledger_id IS DISTINCT FROM NEW.ledger_id THEN
            RAISE EXCEPTION 'row % category must belong to the row ledger', NEW.id;
        END IF;
    END IF;

    RETURN NEW;
END;
$$;

CREATE TRIGGER journal_rows_link_guard
BEFORE INSERT OR UPDATE OF ledger_id, asset_id, account_id, category_id, transfer_account_id
ON journal_rows
FOR EACH ROW EXECUTE FUNCTION check_journal_row_links_asset_ledger();

CREATE OR REPLACE FUNCTION check_row_postings_balanced() RETURNS TRIGGER
LANGUAGE plpgsql AS $$
DECLARE
    checked_row_id BIGINT := COALESCE(NEW.row_id, OLD.row_id);
    row_kind TEXT;
    row_ledger_id BIGINT;
    row_asset_id BIGINT;
    posting_count INTEGER;
    posting_sum NUMERIC(38, 18);
    posting_asset_count INTEGER;
BEGIN
    SELECT kind, ledger_id, asset_id INTO row_kind, row_ledger_id, row_asset_id
      FROM journal_rows WHERE id = checked_row_id;
    IF row_kind IS NULL THEN
        RETURN NULL;
    END IF;

    SELECT count(*), COALESCE(sum(signed_amount), 0), count(DISTINCT asset_id)
      INTO posting_count, posting_sum, posting_asset_count
      FROM postings WHERE row_id = checked_row_id;

    IF row_kind = 'note' AND posting_count <> 0 THEN
        RAISE EXCEPTION 'note row % must not have postings', checked_row_id;
    ELSIF row_kind <> 'note' AND (posting_count <> 2 OR posting_sum <> 0 OR posting_asset_count <> 1) THEN
        RAISE EXCEPTION 'financial row % must have two balanced postings in one asset', checked_row_id;
    ELSIF row_kind <> 'note' AND EXISTS (
        SELECT 1 FROM postings WHERE row_id = checked_row_id AND asset_id <> row_asset_id
    ) THEN
        RAISE EXCEPTION 'financial row % postings must match row asset', checked_row_id;
    ELSIF row_kind <> 'note' AND EXISTS (
        SELECT 1
          FROM postings p
          JOIN accounts a ON a.id = p.account_id
         WHERE p.row_id = checked_row_id
           AND (a.ledger_id <> row_ledger_id OR a.asset_id <> p.asset_id)
    ) THEN
        RAISE EXCEPTION 'financial row % posting accounts must match row ledger and posting asset', checked_row_id;
    ELSIF row_kind <> 'note' AND EXISTS (
        SELECT 1
          FROM postings p
          JOIN categories c ON c.id = p.category_id
         WHERE p.row_id = checked_row_id
           AND p.category_id IS NOT NULL
           AND c.ledger_id <> row_ledger_id
    ) THEN
        RAISE EXCEPTION 'financial row % posting categories must match row ledger', checked_row_id;
    END IF;
    RETURN NULL;
END;
$$;

CREATE TABLE chain_settings (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (singleton),
    tron_grid_api_key_ciphertext BYTEA,
    tron_grid_api_key_nonce BYTEA,
    tron_grid_api_key_updated_at TIMESTAMPTZ,
    sync_interval_minutes INTEGER NOT NULL DEFAULT 30 CHECK (sync_interval_minutes BETWEEN 5 AND 1440),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

INSERT INTO chain_settings(singleton) VALUES (TRUE)
ON CONFLICT (singleton) DO NOTHING;

CREATE TABLE wallet_accounts (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    chain_network_id BIGINT NOT NULL REFERENCES chain_networks(id),
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    address TEXT NOT NULL,
    address_normalized TEXT NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    auto_sync BOOLEAN NOT NULL DEFAULT TRUE,
    last_synced_at TIMESTAMPTZ,
    last_error TEXT,
    deleted_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE UNIQUE INDEX wallet_accounts_active_address_unique
    ON wallet_accounts (ledger_id, chain_network_id, address_normalized)
    WHERE deleted_at IS NULL;
CREATE INDEX wallet_accounts_ledger_order
    ON wallet_accounts (ledger_id, deleted_at, lower(name), id);

CREATE TABLE wallet_asset_accounts (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    wallet_id BIGINT NOT NULL REFERENCES wallet_accounts(id) ON DELETE CASCADE,
    asset_id BIGINT NOT NULL REFERENCES assets(id) ON DELETE CASCADE,
    account_id BIGINT NOT NULL REFERENCES accounts(id) ON DELETE RESTRICT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (wallet_id, asset_id),
    UNIQUE (account_id)
);

CREATE TABLE chain_transactions (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    chain_network_id BIGINT NOT NULL REFERENCES chain_networks(id),
    tx_hash TEXT NOT NULL,
    block_number BIGINT,
    block_timestamp TIMESTAMPTZ,
    confirmed BOOLEAN NOT NULL DEFAULT TRUE,
    success BOOLEAN NOT NULL DEFAULT TRUE,
    raw_json JSONB NOT NULL DEFAULT '{}'::jsonb,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ledger_id, chain_network_id, tx_hash)
);

CREATE INDEX chain_transactions_ledger_time
    ON chain_transactions (ledger_id, block_timestamp DESC NULLS LAST, id DESC);

CREATE TABLE chain_asset_movements (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    chain_transaction_id BIGINT NOT NULL REFERENCES chain_transactions(id) ON DELETE CASCADE,
    wallet_id BIGINT NOT NULL REFERENCES wallet_accounts(id) ON DELETE CASCADE,
    asset_id BIGINT NOT NULL REFERENCES assets(id) ON DELETE RESTRICT,
    movement_key TEXT NOT NULL,
    direction TEXT NOT NULL CHECK (direction IN ('incoming', 'outgoing', 'internal', 'fee')),
    amount NUMERIC(38, 18) NOT NULL CHECK (amount >= 0),
    from_address TEXT,
    from_address_normalized TEXT,
    to_address TEXT,
    to_address_normalized TEXT,
    occurred_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ledger_id, movement_key)
);

CREATE INDEX chain_asset_movements_ledger_time
    ON chain_asset_movements (ledger_id, occurred_at DESC NULLS LAST, id DESC);
CREATE INDEX chain_asset_movements_address_lookup
    ON chain_asset_movements (ledger_id, from_address_normalized, to_address_normalized);

CREATE TABLE wallet_sync_states (
    wallet_id BIGINT PRIMARY KEY REFERENCES wallet_accounts(id) ON DELETE CASCADE,
    status TEXT NOT NULL DEFAULT 'idle' CHECK (status IN ('idle', 'running', 'failed')),
    checkpoint_block BIGINT,
    checkpoint_timestamp TIMESTAMPTZ,
    lookback_minutes INTEGER NOT NULL DEFAULT 60 CHECK (lookback_minutes BETWEEN 1 AND 10080),
    last_started_at TIMESTAMPTZ,
    last_completed_at TIMESTAMPTZ,
    last_error TEXT,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE wallet_address_labels (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    chain_network_id BIGINT NOT NULL REFERENCES chain_networks(id),
    address TEXT NOT NULL,
    address_normalized TEXT NOT NULL,
    display_name TEXT NOT NULL CHECK (length(btrim(display_name)) BETWEEN 1 AND 120),
    kind TEXT NOT NULL CHECK (kind IN ('customer', 'self', 'exchange', 'merchant', 'contract', 'other')),
    note TEXT NOT NULL DEFAULT '' CHECK (length(note) <= 2000),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ledger_id, chain_network_id, address_normalized)
);

CREATE INDEX wallet_address_labels_ledger_kind
    ON wallet_address_labels (ledger_id, kind, lower(display_name));

CREATE TABLE chain_movement_row_links (
    movement_id BIGINT PRIMARY KEY REFERENCES chain_asset_movements(id) ON DELETE CASCADE,
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE RESTRICT,
    link_kind TEXT NOT NULL DEFAULT 'automatic' CHECK (link_kind IN ('automatic')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

ALTER TABLE background_jobs DROP CONSTRAINT background_jobs_kind_check;
ALTER TABLE background_jobs ADD CONSTRAINT background_jobs_kind_check
    CHECK (kind IN ('formula_recalc', 'csv_import', 'csv_export', 'wallet_backfill', 'wallet_sync'));

CREATE TRIGGER chain_networks_set_updated_at BEFORE UPDATE ON chain_networks
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER assets_set_updated_at BEFORE UPDATE ON assets
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER chain_settings_set_updated_at BEFORE UPDATE ON chain_settings
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER wallet_accounts_set_updated_at BEFORE UPDATE ON wallet_accounts
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER wallet_sync_states_set_updated_at BEFORE UPDATE ON wallet_sync_states
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER wallet_address_labels_set_updated_at BEFORE UPDATE ON wallet_address_labels
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER chain_transactions_set_updated_at BEFORE UPDATE ON chain_transactions
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
