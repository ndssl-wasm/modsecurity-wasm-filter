// NOLINT(namespace-envoy)
#include <stdio.h>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>

//#include "common/http/utility.h"
#include "modsecurity/modsecurity.h"
#include "modsecurity/rule_message.h"
#include "modsecurity/rules_set.h"
#include "proxy_wasm_intrinsics.h"
//#include "extensions/common/wasm/ext/envoy_proxy_wasm_api.h"
#include "absl/strings/str_cat.h"
#include "extensions/common/wasm/json_util.h"
#include "rules.h"
#include "utils.h"

using ::nlohmann::json;
using ::Wasm::Common::JsonArrayIterate;
using ::Wasm::Common::JsonGetField;
using ::Wasm::Common::JsonObjectIterate;
using ::Wasm::Common::JsonValueAs;

class ExampleRootContext : public RootContext {
 public:
  explicit ExampleRootContext(uint32_t id, std::string_view root_id)
      : RootContext(id, root_id) {}

  bool onStart(size_t /* vm_configuration_size */) override;
  bool onConfigure(size_t /* configuration_size */) override;

  /**
   * This static function will be called by modsecurity and internally invoke
   * logCb filter's method
   */
  static void logCb(void *data, const void *ruleMessagev);

  bool updateRules();

  // get config
  const std::string &rules_inline() const { return rules_inline_; }

  std::shared_ptr<modsecurity::ModSecurity> modsec() const { return modsec_; }
  std::shared_ptr<modsecurity::RulesSet> modsec_rules() const {
    return modsec_rules_;
  }

 private:
  // rules config data from root context configurations
  std::string rules_inline_;
  std::string rules_service_;
  bool update_flag_;

  // share modsecurity obj
  std::shared_ptr<modsecurity::ModSecurity> modsec_;
  std::shared_ptr<modsecurity::RulesSet> modsec_rules_;

  // unit: second
  std::string duration_time_;

  // URL path which configured in the rule server
  std::string namespace_;
  std::string pod_name_;
  std::string path_;
};

class ExampleContext : public Context {
 public:
  explicit ExampleContext(uint32_t id, RootContext *root) : Context(id, root) {}

  void onCreate() override;

  FilterHeadersStatus onRequestHeaders(uint32_t headers,
                                       bool end_of_stream) override;
  FilterDataStatus onRequestBody(size_t body_buffer_length,
                                 bool end_of_stream) override;
  FilterMetadataStatus onRequestMetadata(uint32_t) override;
  FilterTrailersStatus onRequestTrailers(uint32_t) override;

  FilterHeadersStatus onResponseHeaders(uint32_t headers,
                                        bool end_of_stream) override;
  FilterDataStatus onResponseBody(size_t body_buffer_length,
                                  bool end_of_stream) override;
  FilterMetadataStatus onResponseMetadata(uint32_t) override;
  FilterTrailersStatus onResponseTrailers(uint32_t) override;

  void onDone() override;
  void onLog() override;
  void onDelete() override;

 private:
  // rules config data from root context configurations
  std::string rules_inline_;

  // share modsecurity obj
  std::shared_ptr<modsecurity::Transaction> modsec_transaction_;

  FilterHeadersStatus getRequestHeadersStatus();
  FilterHeadersStatus getResponseHeadersStatus();
  FilterDataStatus getRequestStatus();
  FilterDataStatus getResponseStatus();

  /**
   * @return true if intervention of current transaction is disruptive, false
   * otherwise
   */
  bool intervention();

  struct ModSecurityStatus {
    ModSecurityStatus()
        : intervined(0), request_processed(0), response_processed(0) {}
    bool intervined;
    bool request_processed;
    bool response_processed;
  };

  ModSecurityStatus status_;
};

static RegisterContextFactory register_ExampleContext(
    CONTEXT_FACTORY(ExampleContext), ROOT_FACTORY(ExampleRootContext));

bool ExampleRootContext::onStart(size_t /* vm_configuration_size */) {
  LOG_TRACE("onStart");
  return true;
}

bool ExampleRootContext::onConfigure(size_t configuration_size) {
  LOG_WARN("onConfigure");

  modsecurity::wasm_data::register_data_map(&rule_data);

  modsec_.reset(new modsecurity::ModSecurity());
  modsec_->setConnectorInformation("ModSecurity-envoy v3.0.4 (ModSecurity)");
  modsec_->setServerLogCb(ExampleRootContext::logCb,
                          modsecurity::RuleMessageLogProperty |
                              modsecurity::IncludeFullHighlightLogProperty);
  modsec_rules_.reset(new modsecurity::RulesSet());

  /* get inline configurations */
  auto configuration_data = getBufferBytes(WasmBufferType::PluginConfiguration,
                                           0, configuration_size);

  auto result = ::Wasm::Common::JsonParse(configuration_data->view());
  if (!result.has_value()) {
    LOG_WARN(absl::StrCat("cannot parse plugin configuration JSON string: ",
                          configuration_data->view()));
    return false;
  }

  auto &configuration = result.value();

  if (!JsonArrayIterate(configuration, "rules", [&](const json &item) -> bool {
        auto json_rule_line = JsonValueAs<std::string>(item);
        if (json_rule_line.second != Wasm::Common::JsonParserResultDetail::OK) {
          return false;
        }

        std::string rule_line = json_rule_line.first.value();

        LOG_INFO("Rule line: " + rule_line);

        auto it = rules.find(rule_line);
        if (it == rules.end()) {  // 如果不是文件而是命令
          int rulesLoaded = modsec_rules_->load(rule_line.c_str());
          if (rulesLoaded == -1) {
            LOG_ERROR(std::string("Failed to load rules"));
            return false;
          } else {
            LOG_WARN(std::string("Loaded plain text rules: ") +
                     std::to_string(rulesLoaded));
            return true;
          };
        } else {  // 如果是文件
          int rulesLoaded = modsec_rules_->load(it->second.c_str());
          if (rulesLoaded == -1) {
            LOG_ERROR(std::string("Failed to load rules"));
            return false;
          } else {
            LOG_WARN(std::string("Loaded rules from file: ") +
                     std::to_string(rulesLoaded));
            return true;
          };
        }
        return true;
      })) {
    LOG_WARN("failed to parse configuration for ip_blacklist.");
    return false;
  }

  rule_data.clear();
  rules.clear();

  return true;
}

void ExampleContext::onCreate() {
  LOG_WARN(std::string("onCreate " + std::to_string(id())));

  // modsecurity initializing
  ExampleRootContext *root = dynamic_cast<ExampleRootContext *>(this->root());
  modsec_transaction_.reset(new modsecurity::Transaction(
      root->modsec().get(), root->modsec_rules().get(), this));
}

FilterHeadersStatus ExampleContext::onRequestHeaders(uint32_t /* headers */,
                                                     bool end_of_stream) {
  LOG_INFO(
      "************************ onRequestHeaders ***************************");
  if (status_.intervined || status_.request_processed) {
    LOG_INFO("Processed");
    return getRequestHeadersStatus();
  }

  // modsecurity processConnection
  std::string remote_addr;
  int remote_port;
  std::string local_addr;
  int local_port;
  getValue({"source", "address"}, &remote_addr);
  getValue({"source", "port"}, &remote_port);
  getValue({"destination", "address"}, &local_addr);
  getValue({"destination", "port"}, &local_port);
  LOG_INFO(std::string("source address: ") + remote_addr +
           std::string(", dest address: ") + local_addr);
  modsec_transaction_->processConnection(
      split(remote_addr, ":")[0].c_str(), remote_port,
      split(local_addr, ":")[0].c_str(), local_port);
  if (intervention()) {
    return FilterHeadersStatus::StopIteration;
  }

  // modsecurity processURI
  std::string path = getRequestHeader(":path")->toString();
  std::string method = getRequestHeader(":method")->toString();
  std::string protocol;
  getValue({"request", "protocol"}, &protocol);
  modsec_transaction_->processURI(path.c_str(), method.c_str(),
                                  protocol.c_str());
  if (intervention()) {
    return FilterHeadersStatus::StopIteration;
  }

  // modsecurity processRequestHeaders
  auto result = getRequestHeaderPairs();
  auto pairs = result->pairs();
  for (auto &p : pairs) {
    modsec_transaction_->addRequestHeader(std::string(p.first),
                                          std::string(p.second));
  }
  modsec_transaction_->processRequestHeaders();
  LOG_INFO(std::string("modsecurity processRequestHeaders done"));

  if (end_of_stream) {
    LOG_INFO(std::string("request processed"));
    status_.request_processed = true;
  }
  if (intervention()) {
    LOG_INFO(std::string("stop iteration"));
    return FilterHeadersStatus::StopIteration;
  }
  LOG_INFO(std::string("getRequestHeadersStatus"));
  return getRequestHeadersStatus();
}

FilterDataStatus ExampleContext::onRequestBody(size_t body_buffer_length,
                                               bool end_of_stream) {
  LOG_INFO(
      "************************ onRequestBody ***************************");
  if (status_.intervined || status_.request_processed) {
    LOG_INFO("Processed");
    return getRequestStatus();
  }
  auto body =
      getBufferBytes(WasmBufferType::HttpRequestBody, 0, body_buffer_length);
  LOG_INFO(std::string(body->view()));

  // size_t responseLen = modsec_transaction_->getResponseBodyLength();
  std::vector<unsigned char> data(body->data(), body->data() + body->size());
  const unsigned char *dataptr = data.data();
  if (modsec_transaction_->appendRequestBody(dataptr, body->size()) == false) {
    LOG_INFO(
        "ModSecurityFilter::onRequestBody appendRequestBody reached limit");
    if (intervention()) {
      return FilterDataStatus::StopIterationNoBuffer;
    }
    // Otherwise set to process response
    end_of_stream = true;
  }

  if (end_of_stream) {
    LOG_INFO(std::string("request processed"));
    status_.request_processed = true;
    modsec_transaction_->processRequestBody();
    LOG_INFO("ModSecurityFilter::onRequestBody processRequestBody done");
  }
  if (intervention()) {
    return FilterDataStatus::StopIterationNoBuffer;
  }
  return getRequestStatus();
}

FilterHeadersStatus ExampleContext::onResponseHeaders(uint32_t /* headers */,
                                                      bool end_of_stream) {
  LOG_INFO(
      "************************ onResponseHeaders ***************************");
  if (status_.intervined || status_.response_processed) {
    return getResponseHeadersStatus();
  }

  auto headers = getResponseHeaderPairs();
  auto pairs = headers->pairs();
  for (auto &p : pairs) {
    LOG_INFO(std::string(p.first) + std::string(" -> ") +
             std::string(p.second));
    modsec_transaction_->addResponseHeader(std::string(p.first),
                                           std::string(p.second));
  }
  int response_code;
  std::string protocol;
  getValue({"response", "code"}, &response_code);
  getValue({"request", "protocol"}, &protocol);
  // TODO(luyao): get response protocol
  LOG_INFO("modsecurity processResponseHeaders start");
  modsec_transaction_->processResponseHeaders(response_code, protocol.c_str());
  LOG_INFO("modsecurity processResponseHeaders done");

  if (end_of_stream) {
    LOG_INFO(std::string("response processed"));
    status_.response_processed = true;
  }
  if (intervention()) {
    LOG_INFO(std::string("stop iteration"));
    return FilterHeadersStatus::StopIteration;
  }
  LOG_INFO(std::string("getResponseHeadersStatus"));
  return getResponseHeadersStatus();
}

FilterDataStatus ExampleContext::onResponseBody(size_t body_buffer_length,
                                                bool end_of_stream) {
  LOG_INFO(
      "************************ onResponseBody ***************************");
  if (status_.intervined || status_.response_processed) {
    LOG_INFO("Processed");
    return getResponseStatus();
  }
  auto body =
      getBufferBytes(WasmBufferType::HttpResponseBody, 0, body_buffer_length);
  LOG_INFO(std::string(body->view()));

  // size_t responseLen = modsec_transaction_->getResponseBodyLength();
  std::vector<unsigned char> data(body->data(), body->data() + body->size());
  const unsigned char *dataptr = data.data();
  if (modsec_transaction_->appendResponseBody(dataptr, body->size()) == false) {
    LOG_INFO(
        "ModSecurityFilter::onResponseBody appendResponseBody reached limit");
    if (intervention()) {
      return FilterDataStatus::StopIterationNoBuffer;
    }
    // Otherwise set to process response
    end_of_stream = true;
  }
  if (end_of_stream) {
    status_.response_processed = true;
    modsec_transaction_->processResponseBody();
  }
  if (intervention()) {
    return FilterDataStatus::StopIterationNoBuffer;
  }
  return getResponseStatus();
}

FilterMetadataStatus ExampleContext::onRequestMetadata(uint32_t) {
  return FilterMetadataStatus::Continue;
}

FilterMetadataStatus ExampleContext::onResponseMetadata(uint32_t) {
  return FilterMetadataStatus::Continue;
}

FilterTrailersStatus ExampleContext::onRequestTrailers(uint32_t) {
  return FilterTrailersStatus::Continue;
}

FilterTrailersStatus ExampleContext::onResponseTrailers(uint32_t) {
  return FilterTrailersStatus::Continue;
}

void ExampleContext::onDone() {
  LOG_WARN(std::string("onDone " + std::to_string(id())));
}

void ExampleContext::onLog() {
  LOG_WARN(std::string("onLog " + std::to_string(id())));
}

void ExampleContext::onDelete() {
  LOG_WARN(std::string("onDelete " + std::to_string(id())));
}

bool ExampleContext::intervention() {
  if (!status_.intervined && modsec_transaction_->m_it.disruptive) {
    // status_.intervined must be set to true before sendLocalReply to avoid
    // reentrancy when encoding the reply
    status_.intervined = true;
    LOG_INFO("intervention");

    std::vector<std::pair<std::string, std::string>> pairs;
    if (modsec_transaction_->m_it.status == 302) {
      pairs.push_back(std::make_pair(
          std::string("location"), std::string(modsec_transaction_->m_it.url)));
    }
    sendLocalResponse(modsec_transaction_->m_it.status, "", "", pairs);
  }
  return status_.intervined;
}

FilterHeadersStatus ExampleContext::getRequestHeadersStatus() {
  if (status_.intervined) {
    LOG_INFO("StopIteration");
    return FilterHeadersStatus::StopIteration;
  }
  if (status_.request_processed) {
    LOG_INFO("Continue");
    return FilterHeadersStatus::Continue;
  }
  // If disruptive, hold until status_.request_processed, otherwise let the data
  // flow.
  return modsec_transaction_->m_it.disruptive
             ? FilterHeadersStatus::StopIteration
             : FilterHeadersStatus::Continue;
}

FilterHeadersStatus ExampleContext::getResponseHeadersStatus() {
  if (status_.intervined || status_.response_processed) {
    LOG_INFO("Continue");
    return FilterHeadersStatus::Continue;
  }
  // If disruptive, hold until status_.response_processed, otherwise let the
  // data flow.
  return modsec_transaction_->m_it.disruptive
             ? FilterHeadersStatus::StopIteration
             : FilterHeadersStatus::Continue;
}

FilterDataStatus ExampleContext::getRequestStatus() {
  if (status_.intervined) {
    LOG_INFO("StopIterationNoBuffer");
    return FilterDataStatus::StopIterationNoBuffer;
  }
  if (status_.request_processed) {
    LOG_INFO("Continue");
    return FilterDataStatus::Continue;
  }
  // If disruptive, hold until status_.request_processed, otherwise let the data
  // flow.
  return modsec_transaction_->m_it.disruptive
             ? FilterDataStatus::StopIterationAndBuffer
             : FilterDataStatus::Continue;
}

FilterDataStatus ExampleContext::getResponseStatus() {
  if (status_.intervined || status_.response_processed) {
    // If intervined, let encodeData return the localReply
    LOG_INFO("Continue");
    return FilterDataStatus::Continue;
  }
  // If disruptive, hold until status_.response_processed, otherwise let the
  // data flow.
  return modsec_transaction_->m_it.disruptive
             ? FilterDataStatus::StopIterationAndBuffer
             : FilterDataStatus::Continue;
}

void ExampleRootContext::logCb(void *data, const void *rulemessage) {
  const modsecurity::RuleMessage *ruleMessage =
      reinterpret_cast<const modsecurity::RuleMessage *>(rulemessage);

  if (ruleMessage == nullptr) {
    LOG_INFO("ruleMessage == nullptr");
    return;
  }

  LOG_INFO("Rule Id: " + std::to_string(ruleMessage->m_ruleId) +
           " Rule Phase: " + std::to_string(ruleMessage->m_phase));
  /*
  LOG_INFO("* {} action. {}",
                  // Note - since ModSecurity >= v3.0.3 disruptive actions do
  not invoke the callback
                  // see
  https://github.com/SpiderLabs/ModSecurity/commit/91daeee9f6a61b8eda07a3f77fc64bae7c6b7c36
                  ruleMessage->m_isDisruptive ? "Disruptive" : "Non-disruptive",
                  modsecurity::RuleMessage::log(ruleMessage));
  */
  std::ofstream outfile;
  outfile.open("/etc/modsecurity_audit.log");
  std::string filedata = "hello world";
  outfile << filedata << std::endl;

  std::string isDisruptive =
      ruleMessage->m_isDisruptive ? "Disruptive " : "Non-disruptive ";
  LOG_INFO(isDisruptive +
           std::string(modsecurity::RuleMessage::log(ruleMessage)));
}