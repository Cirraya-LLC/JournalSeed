-- JournalSeed PostgreSQL 18 schema. Public identifiers are UUIDv7 while joins
-- and indexes use compact BIGINT identities.

CREATE TABLE IF NOT EXISTS schema_migrations (
    version BIGINT PRIMARY KEY,
    name TEXT NOT NULL,
    checksum TEXT NOT NULL,
    applied_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE TABLE app_users (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    username TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    role TEXT NOT NULL DEFAULT 'admin' CHECK (role = 'admin'),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK (length(btrim(username)) BETWEEN 1 AND 80)
);

CREATE UNIQUE INDEX app_users_username_unique ON app_users (lower(username));
CREATE UNIQUE INDEX app_users_single_admin ON app_users (role) WHERE role = 'admin';

CREATE TABLE sessions (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    user_id BIGINT NOT NULL REFERENCES app_users(id) ON DELETE CASCADE,
    token_hash BYTEA NOT NULL UNIQUE,
    csrf_token_hash BYTEA NOT NULL,
    expires_at TIMESTAMPTZ NOT NULL,
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE INDEX sessions_active_lookup ON sessions (token_hash, expires_at);

CREATE TABLE ledgers (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    archived_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp()
);

CREATE UNIQUE INDEX ledgers_active_name_unique
    ON ledgers (lower(name)) WHERE archived_at IS NULL;

CREATE TABLE accounts (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    opening_balance NUMERIC(38, 2) NOT NULL DEFAULT 0,
    system_code TEXT CHECK (system_code IN
        ('unallocated', 'income_clearing', 'expense_clearing', 'opening_balance')),
    archived_at TIMESTAMPTZ,
    deleted_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ledger_id, system_code)
);

CREATE UNIQUE INDEX accounts_active_name_unique
    ON accounts (ledger_id, lower(name))
    WHERE archived_at IS NULL AND deleted_at IS NULL AND system_code IS NULL;

CREATE TABLE categories (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 120),
    direction TEXT NOT NULL CHECK (direction IN ('income', 'expense')),
    system_code TEXT CHECK (system_code IN ('unallocated_income', 'unallocated_expense')),
    archived_at TIMESTAMPTZ,
    deleted_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ledger_id, system_code)
);

CREATE UNIQUE INDEX categories_active_name_unique
    ON categories (ledger_id, direction, lower(name))
    WHERE archived_at IS NULL AND deleted_at IS NULL AND system_code IS NULL;

CREATE TABLE journal_rows (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    occurred_on DATE NOT NULL DEFAULT CURRENT_DATE,
    description TEXT NOT NULL DEFAULT '',
    kind TEXT NOT NULL CHECK (kind IN ('entry', 'transfer', 'note')),
    amount NUMERIC(38, 2) NOT NULL,
    account_id BIGINT REFERENCES accounts(id),
    category_id BIGINT REFERENCES categories(id),
    transfer_account_id BIGINT REFERENCES accounts(id),
    created_by BIGINT NOT NULL REFERENCES app_users(id),
    revision BIGINT NOT NULL DEFAULT 1 CHECK (revision > 0),
    deleted_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    CHECK ((kind = 'note' AND amount = 0 AND transfer_account_id IS NULL)
        OR (kind = 'entry' AND amount <> 0 AND transfer_account_id IS NULL)
        OR (kind = 'transfer' AND amount > 0 AND account_id IS NOT NULL
            AND transfer_account_id IS NOT NULL AND account_id <> transfer_account_id)),
    CHECK (kind <> 'entry' OR category_id IS NOT NULL)
);

CREATE INDEX journal_rows_ledger_date_cursor
    ON journal_rows (ledger_id, occurred_on DESC, public_id DESC)
    WHERE deleted_at IS NULL;
CREATE INDEX journal_rows_ledger_amount_cursor
    ON journal_rows (ledger_id, amount, public_id)
    WHERE deleted_at IS NULL;
CREATE INDEX journal_rows_recycle_bin
    ON journal_rows (ledger_id, deleted_at DESC) WHERE deleted_at IS NOT NULL;

CREATE TABLE postings (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    account_id BIGINT NOT NULL REFERENCES accounts(id),
    category_id BIGINT REFERENCES categories(id),
    signed_amount NUMERIC(38, 2) NOT NULL CHECK (signed_amount <> 0),
    line_number SMALLINT NOT NULL CHECK (line_number IN (1, 2)),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (row_id, line_number)
);

CREATE INDEX postings_account_balance ON postings (account_id, row_id) INCLUDE (signed_amount);
CREATE INDEX postings_category_stats ON postings (category_id, row_id) INCLUDE (signed_amount)
    WHERE category_id IS NOT NULL;

CREATE OR REPLACE FUNCTION check_row_postings_balanced() RETURNS TRIGGER
LANGUAGE plpgsql AS $$
DECLARE
    checked_row_id BIGINT := COALESCE(NEW.row_id, OLD.row_id);
    row_kind TEXT;
    posting_count INTEGER;
    posting_sum NUMERIC(38, 2);
BEGIN
    SELECT kind INTO row_kind FROM journal_rows WHERE id = checked_row_id;
    IF row_kind IS NULL THEN
        RETURN NULL;
    END IF;

    SELECT count(*), COALESCE(sum(signed_amount), 0)
      INTO posting_count, posting_sum
      FROM postings WHERE row_id = checked_row_id;

    IF row_kind = 'note' AND posting_count <> 0 THEN
        RAISE EXCEPTION 'note row % must not have postings', checked_row_id;
    ELSIF row_kind <> 'note' AND (posting_count <> 2 OR posting_sum <> 0) THEN
        RAISE EXCEPTION 'financial row % must have two balanced postings', checked_row_id;
    END IF;
    RETURN NULL;
END;
$$;

CREATE CONSTRAINT TRIGGER postings_balance_guard
AFTER INSERT OR UPDATE OR DELETE ON postings
DEFERRABLE INITIALLY DEFERRED
FOR EACH ROW EXECUTE FUNCTION check_row_postings_balanced();

CREATE TABLE ledger_columns (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    system_key TEXT CHECK (system_key IN ('date', 'description', 'account', 'category', 'amount')),
    name TEXT NOT NULL CHECK (length(btrim(name)) BETWEEN 1 AND 80),
    value_type TEXT NOT NULL CHECK (value_type IN
        ('text', 'number', 'date', 'boolean', 'option', 'relation', 'formula', 'money')),
    position INTEGER NOT NULL CHECK (position >= 0),
    width INTEGER NOT NULL DEFAULT 160 CHECK (width BETWEEN 72 AND 720),
    decimal_places SMALLINT CHECK (decimal_places BETWEEN 0 AND 18),
    formula_source TEXT,
    formula_result_type TEXT,
    formula_dependencies JSONB NOT NULL DEFAULT '[]'::jsonb,
    deleted_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    UNIQUE (ledger_id, system_key),
    UNIQUE (ledger_id, position),
    CHECK ((system_key = 'amount' AND value_type = 'money' AND deleted_at IS NULL)
        OR system_key IS DISTINCT FROM 'amount')
);

CREATE INDEX ledger_columns_active_order ON ledger_columns (ledger_id, position)
    WHERE deleted_at IS NULL;

CREATE TABLE column_options (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    label TEXT NOT NULL CHECK (length(btrim(label)) BETWEEN 1 AND 80),
    color TEXT NOT NULL DEFAULT 'neutral',
    position INTEGER NOT NULL DEFAULT 0,
    archived_at TIMESTAMPTZ,
    UNIQUE (column_id, label)
);

CREATE TABLE text_cells (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    value TEXT NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id)
) PARTITION BY HASH (column_id);

CREATE TABLE number_cells (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    value NUMERIC(38, 18) NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id)
) PARTITION BY HASH (column_id);

CREATE TABLE date_cells (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    value DATE NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id)
) PARTITION BY HASH (column_id);

CREATE TABLE boolean_cells (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    value BOOLEAN NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id)
) PARTITION BY HASH (column_id);

CREATE TABLE option_cells (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    value BIGINT NOT NULL REFERENCES column_options(id),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id)
) PARTITION BY HASH (column_id);

CREATE TABLE relation_cells (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    related_row_id BIGINT REFERENCES journal_rows(id),
    related_account_id BIGINT REFERENCES accounts(id),
    related_category_id BIGINT REFERENCES categories(id),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id),
    CHECK (num_nonnulls(related_row_id, related_account_id, related_category_id) = 1)
) PARTITION BY HASH (column_id);

DO $$
DECLARE
    parent_name TEXT;
    partition_number INTEGER;
BEGIN
    FOREACH parent_name IN ARRAY ARRAY[
        'text_cells', 'number_cells', 'date_cells', 'boolean_cells',
        'option_cells', 'relation_cells'
    ] LOOP
        FOR partition_number IN 0..15 LOOP
            EXECUTE format(
                'CREATE TABLE %I PARTITION OF %I FOR VALUES WITH (MODULUS 16, REMAINDER %s)',
                parent_name || '_p' || lpad(partition_number::text, 2, '0'),
                parent_name,
                partition_number
            );
        END LOOP;
    END LOOP;
END;
$$;

CREATE INDEX text_cells_filter ON text_cells (column_id, value text_pattern_ops, row_id);
CREATE INDEX number_cells_filter ON number_cells (column_id, value, row_id);
CREATE INDEX date_cells_filter ON date_cells (column_id, value, row_id);
CREATE INDEX boolean_cells_filter ON boolean_cells (column_id, value, row_id);
CREATE INDEX option_cells_filter ON option_cells (column_id, value, row_id);
CREATE INDEX relation_cells_row_filter ON relation_cells (column_id, related_row_id, row_id);
CREATE INDEX relation_cells_account_filter ON relation_cells (column_id, related_account_id, row_id);
CREATE INDEX relation_cells_category_filter ON relation_cells (column_id, related_category_id, row_id);

CREATE TABLE formula_revisions (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    ledger_id BIGINT NOT NULL REFERENCES ledgers(id) ON DELETE CASCADE,
    revision BIGINT NOT NULL CHECK (revision > 0),
    status TEXT NOT NULL CHECK (status IN ('building', 'active', 'retired', 'failed')),
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    activated_at TIMESTAMPTZ,
    UNIQUE (ledger_id, revision)
);

CREATE UNIQUE INDEX formula_revisions_one_active
    ON formula_revisions (ledger_id) WHERE status = 'active';

CREATE TABLE formula_results (
    row_id BIGINT NOT NULL REFERENCES journal_rows(id) ON DELETE CASCADE,
    column_id BIGINT NOT NULL REFERENCES ledger_columns(id) ON DELETE CASCADE,
    revision_id BIGINT NOT NULL REFERENCES formula_revisions(id) ON DELETE CASCADE,
    text_value TEXT,
    number_value NUMERIC(38, 18),
    date_value DATE,
    boolean_value BOOLEAN,
    error_code TEXT,
    error_message TEXT,
    overridden BOOLEAN NOT NULL DEFAULT FALSE,
    computed_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    PRIMARY KEY (row_id, column_id, revision_id),
    CHECK (error_code IS NOT NULL OR
        num_nonnulls(text_value, number_value, date_value, boolean_value) = 1)
);

CREATE INDEX formula_results_read ON formula_results (revision_id, row_id, column_id);

CREATE TABLE background_jobs (
    id BIGINT GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    public_id UUID NOT NULL DEFAULT uuidv7() UNIQUE,
    ledger_id BIGINT REFERENCES ledgers(id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK (kind IN ('formula_recalc', 'csv_import', 'csv_export')),
    status TEXT NOT NULL DEFAULT 'queued' CHECK
        (status IN ('queued', 'running', 'completed', 'failed', 'cancelled')),
    payload JSONB NOT NULL DEFAULT '{}'::jsonb,
    progress_done BIGINT NOT NULL DEFAULT 0 CHECK (progress_done >= 0),
    progress_total BIGINT NOT NULL DEFAULT 0 CHECK (progress_total >= 0),
    checkpoint JSONB NOT NULL DEFAULT '{}'::jsonb,
    error JSONB,
    locked_by TEXT,
    locked_at TIMESTAMPTZ,
    cancel_requested_at TIMESTAMPTZ,
    created_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT clock_timestamp(),
    finished_at TIMESTAMPTZ
);

CREATE INDEX background_jobs_claim
    ON background_jobs (status, created_at, id)
    WHERE status IN ('queued', 'running');

CREATE OR REPLACE FUNCTION set_updated_at() RETURNS TRIGGER
LANGUAGE plpgsql AS $$
BEGIN
    NEW.updated_at = clock_timestamp();
    RETURN NEW;
END;
$$;

CREATE TRIGGER app_users_set_updated_at BEFORE UPDATE ON app_users
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER ledgers_set_updated_at BEFORE UPDATE ON ledgers
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER accounts_set_updated_at BEFORE UPDATE ON accounts
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER categories_set_updated_at BEFORE UPDATE ON categories
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER journal_rows_set_updated_at BEFORE UPDATE ON journal_rows
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER ledger_columns_set_updated_at BEFORE UPDATE ON ledger_columns
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
CREATE TRIGGER background_jobs_set_updated_at BEFORE UPDATE ON background_jobs
    FOR EACH ROW EXECUTE FUNCTION set_updated_at();
