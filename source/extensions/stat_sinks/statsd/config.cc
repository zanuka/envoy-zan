#include "source/extensions/stat_sinks/statsd/config.h"

#include <memory>

#include "envoy/config/metrics/v3/stats.pb.h"
#include "envoy/config/metrics/v3/stats.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/common/network/resolver_impl.h"
#include "source/extensions/stat_sinks/common/statsd/statsd.h"

namespace Envoy {
namespace Extensions {
namespace StatSinks {
namespace Statsd {

absl::StatusOr<Stats::SinkPtr>
StatsdSinkFactory::createStatsSink(const Protobuf::Message& config,
                                   Server::Configuration::ServerFactoryContext& server) {

  const auto& statsd_sink =
      MessageUtil::downcastAndValidate<const envoy::config::metrics::v3::StatsdSink&>(
          config, server.messageValidationContext().staticValidationVisitor());
  switch (statsd_sink.statsd_specifier_case()) {
  case envoy::config::metrics::v3::StatsdSink::StatsdSpecifierCase::kAddress: {
    auto address_or_error = Network::Address::resolveProtoAddress(statsd_sink.address());
    RETURN_IF_NOT_OK_REF(address_or_error.status());
    Network::Address::InstanceConstSharedPtr address = address_or_error.value();
    ENVOY_LOG(debug, "statsd UDP ip address: {}", address->asString());
    return std::make_unique<Common::Statsd::UdpStatsdSink>(server.threadLocal(), std::move(address),
                                                           false, statsd_sink.prefix());
  }
  case envoy::config::metrics::v3::StatsdSink::StatsdSpecifierCase::kTcpClusterName:
    ENVOY_LOG(debug, "statsd TCP cluster: {}", statsd_sink.tcp_cluster_name());
    return Common::Statsd::TcpStatsdSink::create(server.localInfo(), statsd_sink.tcp_cluster_name(),
                                                 server.threadLocal(), server.clusterManager(),
                                                 server.scope(), statsd_sink.prefix());
  case envoy::config::metrics::v3::StatsdSink::StatsdSpecifierCase::STATSD_SPECIFIER_NOT_SET:
    break; // Fall through to PANIC
  }
  PANIC("unexpected statsd specifier case num");
}

ProtobufTypes::MessagePtr StatsdSinkFactory::createEmptyConfigProto() {
  return std::make_unique<envoy::config::metrics::v3::StatsdSink>();
}

std::string StatsdSinkFactory::name() const { return StatsdName; }

/**
 * Static registration for the statsd sink factory. @see RegisterFactory.
 */
LEGACY_REGISTER_FACTORY(StatsdSinkFactory, Server::Configuration::StatsSinkFactory, "envoy.statsd");

} // namespace Statsd
} // namespace StatSinks
} // namespace Extensions
} // namespace Envoy
