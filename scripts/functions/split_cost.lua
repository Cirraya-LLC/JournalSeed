return {
  name = "split_cost",
  version = "1.0.0",
  description = "按人数精确平分一笔金额，使用 half-even 舍入上下文。",
  params = {
    { name = "amount", type = "number", label = "总金额", required = true },
    { name = "people", type = "number", label = "人数", required = true },
    { name = "description", type = "text", label = "说明", required = false }
  },
  run = function(params)
    local share = dec(params.amount) / dec(params.people)
    return {
      amount = tostring(share),
      description = params.description or "平分金额",
      cells = { calculation = "split_cost@1.0.0" }
    }
  end
}
