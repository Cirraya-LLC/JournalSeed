return {
  name = "quick_expense",
  version = "1.0.0",
  description = "把正数金额转换为支出，并补齐常用说明。",
  params = {
    { name = "amount", type = "number", label = "支出金额", required = true },
    { name = "description", type = "text", label = "说明", required = true }
  },
  run = function(params)
    local amount = dec(params.amount)
    return {
      amount = tostring(-amount),
      description = params.description
    }
  end
}
