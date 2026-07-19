#!/usr/bin/env bash
set -euo pipefail

readonly BASE_URL="${JOURNALSEED_BASE_URL:-http://127.0.0.1:8080}"
readonly ADMIN_USERNAME="${JOURNALSEED_TEST_USERNAME:-admin}"
readonly ADMIN_PASSWORD="${JOURNALSEED_TEST_PASSWORD:-JournalSeed-Test-2026!}"

work_directory="$(mktemp -d)"
readonly work_directory
readonly cookie_jar="${work_directory}/cookies.txt"
readonly response_file="${work_directory}/response.json"
trap 'rm -rf "${work_directory}"' EXIT

csrf_token=""

api_request() {
  local method="$1"
  local path="$2"
  local expected_status="$3"
  local payload="${4:-}"
  local arguments=(
    --silent
    --show-error
    --request "${method}"
    --cookie "${cookie_jar}"
    --cookie-jar "${cookie_jar}"
    --output "${response_file}"
    --write-out '%{http_code}'
  )

  if [[ -n "${payload}" ]]; then
    arguments+=(--header 'Content-Type: application/json' --data "${payload}")
  fi
  if [[ "${method}" != "GET" && -n "${csrf_token}" ]]; then
    arguments+=(--header "X-JournalSeed-CSRF: ${csrf_token}")
  fi

  local actual_status
  actual_status="$(curl "${arguments[@]}" "${BASE_URL}${path}")"
  if [[ "${actual_status}" != "${expected_status}" ]]; then
    printf 'Expected HTTP %s for %s %s, received %s\n' \
      "${expected_status}" "${method}" "${path}" "${actual_status}" >&2
    cat "${response_file}" >&2
    return 1
  fi
}

json_value() {
  jq -r "$@" "${response_file}"
}

assert_json() {
  local expected="$1"
  shift

  local actual
  if ! actual="$(json_value "$@")"; then
    printf 'JSON assertion could not evaluate: jq %q\n' "$*" >&2
    cat "${response_file}" >&2
    return 1
  fi
  if [[ "${actual}" != "${expected}" ]]; then
    printf 'Expected JSON value %q, received %q for jq %q\n' \
      "${expected}" "${actual}" "$*" >&2
    cat "${response_file}" >&2
    return 1
  fi
}

assert_equal() {
  local expected="$1"
  local actual="$2"
  local context="$3"
  if [[ "${actual}" != "${expected}" ]]; then
    printf 'Expected %s %q, received %q\n' \
      "${context}" "${expected}" "${actual}" >&2
    cat "${response_file}" >&2
    return 1
  fi
}

api_request GET /health 200
assert_json "ok" '.status'

api_request GET /api/v1/setup/status 200
setup_required="$(json_value '.required')"
if [[ "${setup_required}" == "true" ]]; then
  api_request POST /api/v1/setup 201 "$(jq -nc \
    --arg username "${ADMIN_USERNAME}" \
    --arg password "${ADMIN_PASSWORD}" \
    '{username:$username,password:$password,ledgerName:"初始账本"}')"
else
  api_request POST /api/v1/auth/login 200 "$(jq -nc \
    --arg username "${ADMIN_USERNAME}" \
    --arg password "${ADMIN_PASSWORD}" \
    '{username:$username,password:$password}')"
fi
csrf_token="$(json_value '.csrfToken')"

readonly run_id="$(date +%s)-$$"
api_request POST /api/v1/ledgers 201 "$(jq -nc \
  --arg name "集成验收-${run_id}" '{name:$name}')"
ledger_id="$(json_value '.id')"

api_request POST "/api/v1/ledgers/${ledger_id}/accounts" 201 \
  '{"name":"日常账户","openingBalance":"0.00"}'
primary_account_id="$(json_value '.id')"
api_request POST "/api/v1/ledgers/${ledger_id}/accounts" 201 \
  '{"name":"储蓄账户","openingBalance":"0.00"}'
savings_account_id="$(json_value '.id')"

api_request POST "/api/v1/ledgers/${ledger_id}/categories" 201 \
  '{"name":"工资","direction":"income"}'
income_category_id="$(json_value '.id')"
api_request POST "/api/v1/ledgers/${ledger_id}/categories" 201 \
  '{"name":"餐饮","direction":"expense"}'
expense_category_id="$(json_value '.id')"

api_request POST "/api/v1/ledgers/${ledger_id}/columns" 201 \
  '{"name":"商户","type":"text","formulaDependencies":[]}'
merchant_column_id="$(json_value '.id')"
api_request POST "/api/v1/ledgers/${ledger_id}/columns" 201 \
  '{"name":"已核对","type":"boolean","formulaDependencies":[]}'
checked_column_id="$(json_value '.id')"

api_request PATCH "/api/v1/columns/${merchant_column_id}" 200 \
  '{"name":"交易对方","width":220}'
assert_json "交易对方" '.name'
assert_json "220" '.width'

income_payload="$(jq -nc \
  --arg account "${primary_account_id}" \
  --arg category "${income_category_id}" \
  --arg merchant_column "${merchant_column_id}" \
  --arg checked_column "${checked_column_id}" \
  '{date:"2026-07-19",description:"七月工资",kind:"entry",amount:"1250.50",
    accountId:$account,categoryId:$category,transferAccountId:null,
    cells:{($merchant_column):"JournalSeed",($checked_column):true}}')"
api_request POST "/api/v1/ledgers/${ledger_id}/rows" 201 "${income_payload}"
income_row_id="$(json_value '.id')"
assert_json "1250.50" '.amount'
assert_json "JournalSeed" ".cells[\"${merchant_column_id}\"]"

expense_payload="$(jq -nc \
  --arg account "${primary_account_id}" \
  --arg category "${expense_category_id}" \
  '{date:"2026-07-19",description:"午餐",kind:"entry",amount:"-85.25",
    accountId:$account,categoryId:$category,transferAccountId:null,cells:{}}')"
api_request POST "/api/v1/ledgers/${ledger_id}/rows" 201 "${expense_payload}"
expense_row_id="$(json_value '.id')"

transfer_payload="$(jq -nc \
  --arg source "${primary_account_id}" \
  --arg destination "${savings_account_id}" \
  '{date:"2026-07-19",description:"转入储蓄",kind:"transfer",amount:"200.00",
    accountId:$source,categoryId:null,transferAccountId:$destination,cells:{}}')"
api_request POST "/api/v1/ledgers/${ledger_id}/rows" 201 "${transfer_payload}"
assert_json "transfer" '.kind'

api_request POST "/api/v1/ledgers/${ledger_id}/rows" 201 \
  '{"date":"2026-07-19","description":"核对完成","kind":"note","amount":"0.00","accountId":null,"categoryId":null,"transferAccountId":null,"cells":{}}'
note_row_id="$(json_value '.id')"
assert_json "note" '.kind'

api_request GET "/api/v1/ledgers/${ledger_id}/summary" 200
assert_json "1165.25" '.balance'
assert_json "1250.50" '.income'
assert_json "-85.25" '.expense'
assert_json "4" '.rowCount'

api_request GET "/api/v1/ledgers/${ledger_id}/rows?limit=2&sort=amount:desc" 200
assert_json "2" '.items | length'
assert_json "true" '.hasMore'
cursor="$(json_value '.nextCursor')"
api_request GET "/api/v1/ledgers/${ledger_id}/rows?limit=2&sort=amount:desc&cursor=${cursor}" 200
assert_json "2" '.items | length'

api_request DELETE "/api/v1/rows/${note_row_id}" 204
api_request GET "/api/v1/ledgers/${ledger_id}/rows?recycled=true" 200
assert_json "true" --arg id "${note_row_id}" '.items | any(.id == $id)'
api_request POST "/api/v1/rows/${note_row_id}/restore" 204

api_request DELETE "/api/v1/columns/${merchant_column_id}" 204
api_request GET "/api/v1/ledgers/${ledger_id}/columns?recycled=true" 200
assert_json "true" --arg id "${merchant_column_id}" 'any(.id == $id)'
api_request POST "/api/v1/columns/${merchant_column_id}/restore" 204

api_request GET /api/v1/functions 200
assert_json "true" 'any(.name == "quick_expense")'
api_request POST /api/v1/functions/quick_expense/invoke 200 \
  '{"amount":"9.90","description":"函数预览"}'
assert_json "-9.90" '.amount'

wrong_csrf_status="$(curl --silent --show-error \
  --request DELETE \
  --cookie "${cookie_jar}" \
  --header 'X-JournalSeed-CSRF: incorrect' \
  --output "${response_file}" \
  --write-out '%{http_code}' \
  "${BASE_URL}/api/v1/rows/${expense_row_id}")"
assert_equal "403" "${wrong_csrf_status}" "wrong-CSRF HTTP status"

api_request POST /api/v1/auth/logout 204
csrf_token=""
api_request GET /api/v1/auth/session 401

printf 'JournalSeed API smoke passed for ledger %s (income row %s).\n' \
  "${ledger_id}" "${income_row_id}"
