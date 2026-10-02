-- Per-wallet accepted currencies, with optional conversion into the ledger's default asset.
--
-- token_rules is a JSON array of {contract, symbol, exchangeRate?, accountId?}. `contract` is
-- the asset key the sync compares against: the normalized contract address, or 'native' for
-- the chain's own coin. When exchangeRate is set, record_wallet_sync books the movement in
-- the default asset at amount * exchangeRate on accountId (or the ledger's unallocated
-- account) instead of on the wallet's per-token account.
--
-- accept_all_tokens keeps wallets created before this migration syncing every asset, which
-- is what they did until now. Wallets created from here on are written with it FALSE and an
-- explicit rule list, so spam tokens sent to the address no longer become journal rows,
-- assets and accounts.
ALTER TABLE wallet_accounts
    ADD COLUMN accept_all_tokens BOOLEAN NOT NULL DEFAULT TRUE,
    ADD COLUMN token_rules JSONB NOT NULL DEFAULT '[]'::jsonb
        CHECK (jsonb_typeof(token_rules) = 'array');

-- Bookkeeping starts when a wallet is added, not at its first-ever transfer: movements whose
-- block time is earlier than sync_from are not recorded. Existing wallets get -infinity so
-- they keep their full history; new ones default to the moment they are inserted.
ALTER TABLE wallet_accounts ADD COLUMN sync_from TIMESTAMPTZ;
UPDATE wallet_accounts SET sync_from = '-infinity';
ALTER TABLE wallet_accounts
    ALTER COLUMN sync_from SET DEFAULT clock_timestamp(),
    ALTER COLUMN sync_from SET NOT NULL;
