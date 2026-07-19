#pragma once

#include "journalseed/application/journal_service.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace journalseed::api {

class ApiController final : public std::enable_shared_from_this<ApiController> {
  public:
    explicit ApiController(std::shared_ptr<application::JournalService> service);

    void register_routes();
    void publish_event(std::string event_name, std::string json_data);

  private:
    struct EventState;

    std::shared_ptr<application::JournalService> service_;
    std::shared_ptr<EventState> events_;
};

}  // namespace journalseed::api
